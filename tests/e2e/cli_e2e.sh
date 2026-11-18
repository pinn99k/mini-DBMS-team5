#!/usr/bin/env bash
# CLI 종단 간 테스트: run / dump / 에러 출력 / 종료 코드 / 재시작 후 id
set -euo pipefail
BIN="$1"
L="${MINIDB_LAUNCHER:-}"
D="$(mktemp -d)"
trap 'rm -rf "$D"' EXIT
fail() { echo "FAIL: $*"; echo "--- stdout"; cat "$D/out" 2>/dev/null || true; echo "--- stderr"; cat "$D/err" 2>/dev/null || true; exit 1; }

mkdir "$D/data"
printf 'name VARCHAR(8)\nage INT\n' > "$D/data/users.schema"

cat > "$D/a.sql" <<'EOF'
-- 첫 실행
INSERT INTO users VALUES ('kim', 25);
insert into USERS values ('x', 1);
INSERT INTO users (age, name) VALUES (30, 'it''s;ok');
SELECT * FROM users WHERE name = 'kim';
EXPLAIN SELECT * FROM users WHERE id = 2;
SELECT nope FROM users;
SELECT FROM users;
SELECT name FROM users WHERE id = 2;
EOF

set +e
$L "$BIN" run --data "$D/data" "$D/a.sql" > "$D/out" 2> "$D/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "expected exit 1 (some statements failed), got $rc"
grep -q '^INSERT 1 (id=1)$' "$D/out" || fail "first insert"
grep -q '^INSERT 1 (id=2)$' "$D/out" || fail "second insert"
grep -q "a.sql:3:13: SchemaError: unknown table 'USERS'" "$D/err" || fail "unknown table error position"
grep -q "a.sql:7:8: SchemaError: unknown column 'nope'" "$D/err" || fail "unknown column error position"
grep -q "a.sql:8:8: ParseError: expected column name" "$D/err" || fail "parse error position"
grep -q '^IndexScan(users, id=2)$' "$D/out" || fail "explain"
grep -q "it's;ok" "$D/out" || fail "string with quote and semicolon"
grep -q '^(1 row)$' "$D/out" || fail "row count footer"

# 재시작 후 id가 이어진다
echo "INSERT INTO users VALUES ('lee', 41); SELECT * FROM users;" > "$D/b.sql"
$L "$BIN" run --data "$D/data" "$D/b.sql" > "$D/out" 2> "$D/err" || fail "second run"
grep -q '^INSERT 1 (id=3)$' "$D/out" || fail "id continues after restart"
grep -q '^(3 rows)$' "$D/out" || fail "three rows after restart"

# --bail은 첫 에러에서 멈춘다
printf "SELECT * FROM nope;\nINSERT INTO users VALUES ('z', 1);\n" > "$D/c.sql"
set +e
$L "$BIN" run --bail --data "$D/data" "$D/c.sql" > "$D/out" 2> "$D/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "bail exit code"
grep -q 'INSERT' "$D/out" && fail "bail should stop before the insert"

# dump
$L "$BIN" dump --data "$D/data" users > "$D/out" 2> "$D/err" || fail "dump"
grep -q '^3 frames, 3 records, 3 live, ok$' "$D/out" || fail "dump summary"
grep -q "name='lee'" "$D/out" || fail "dump record"

# 꼬리 손상: 잘라내고 경고, .torn 보관
size=$(stat -c %s "$D/data/users.tbl")
truncate -s $((size - 3)) "$D/data/users.tbl"
set +e
$L "$BIN" dump --data "$D/data" users > "$D/out" 2>&1
rc=$?
set -e
[ "$rc" -eq 1 ] && grep -q 'TAIL torn' "$D/out" || fail "dump should report torn tail"
echo "SELECT * FROM users;" > "$D/d.sql"
$L "$BIN" run --data "$D/data" "$D/d.sql" > "$D/out" 2> "$D/err" || fail "run after torn tail"
grep -q 'torn tail' "$D/err" || fail "torn tail warning"
grep -q '^(2 rows)$' "$D/out" || fail "two rows remain"
[ -f "$D/data/users.tbl.torn" ] || fail ".torn file"

# 중간 손상: 시작 거부 (종료 코드 2)
printf '\xff' | dd of="$D/data/users.tbl" bs=1 seek=24 conv=notrunc status=none
set +e
$L "$BIN" run --data "$D/data" "$D/d.sql" > "$D/out" 2> "$D/err"
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "mid corruption should refuse to start (exit 2), got $rc"
grep -q 'refusing to start' "$D/err" || fail "mid corruption message"

# UPDATE·DELETE (decisions #20): 출력, 재시작 후 유지, dump의 update/delete 레코드
mkdir "$D/data2"
printf 'name VARCHAR(8)\nage INT\n' > "$D/data2/users.schema"
cat > "$D/u.sql" <<'EOF'
INSERT INTO users VALUES ('kim', 25);
INSERT INTO users VALUES ('lee', 30);
INSERT INTO users VALUES ('kim', 40);
UPDATE users SET age = 26 WHERE id = 1;
UPDATE users SET name = 'park' WHERE name = 'kim';
DELETE FROM users WHERE id = 2;
DELETE FROM users WHERE id = 2;
UPDATE users SET id = 9;
EOF
set +e
$L "$BIN" run --data "$D/data2" "$D/u.sql" > "$D/out" 2> "$D/err"
rc=$?
set -e
[ "$rc" -eq 1 ] || fail "expected exit 1 (SET id fails), got $rc"
grep -q '^UPDATE 1$' "$D/out" || fail "update by id"
grep -q '^UPDATE 2$' "$D/out" || fail "update by filter"
grep -q '^DELETE 1$' "$D/out" || fail "delete"
grep -q '^DELETE 0$' "$D/out" || fail "delete of deleted id"
grep -q "u.sql:8:18: SchemaError: column 'id' cannot be updated" "$D/err" || fail "SET id error"
echo "SELECT * FROM users; INSERT INTO users VALUES ('choi', 1);" > "$D/v.sql"
$L "$BIN" run --data "$D/data2" "$D/v.sql" > "$D/out" 2> "$D/err" || fail "run after update/delete"
grep -q '^(2 rows)$' "$D/out" || fail "two rows after delete and restart"
grep -q 'park' "$D/out" || fail "updated name survives restart"
grep -q '^INSERT 1 (id=4)$' "$D/out" || fail "id not reused after delete"
$L "$BIN" dump --data "$D/data2" users > "$D/out" 2> "$D/err" || fail "dump after update/delete"
grep -q ' update id=1 ' "$D/out" || fail "dump update record"
grep -q ' delete id=2$' "$D/out" || fail "dump delete record"
grep -q ' records, 3 live, ok$' "$D/out" || fail "dump live count"

echo "cli_e2e: ok"
