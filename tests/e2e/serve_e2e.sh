#!/usr/bin/env bash
# 서버 종단 간 테스트: curl로 질의, 부하 중 SIGTERM → 처리 중 요청 완료 → 재시작 후 커밋된 행 존재
set -euo pipefail
BIN="$1"
L="${MINIDB_LAUNCHER:-}"
D="$(mktemp -d)"
PORT=$((20000 + RANDOM % 20000))
URL="http://127.0.0.1:$PORT"
PID=""
cleanup() { [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null || true; rm -rf "$D"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- server log"; cat "$D/server.log" 2>/dev/null || true; exit 1; }

mkdir "$D/data"
printf 'name VARCHAR(16)\nage INT\n' > "$D/data/users.schema"

start() {
  $L "$BIN" serve --data "$D/data" --port "$PORT" --workers 4 > "$D/server.log" 2>&1 &
  PID=$!
  for _ in $(seq 1 100); do
    curl -sf "$URL/health" > /dev/null 2>&1 && return 0
    sleep 0.1
  done
  fail "server did not start"
}
q() { curl -s -X POST --data-binary "$1" "$URL/query"; }

start
[ "$(curl -s "$URL/health")" = '{"ok":true}' ] || fail "health"
[ "$(q "INSERT INTO users VALUES ('kim', 25)")" = '{"ok":true,"insertedIds":[1]}' ] || fail "insert"
q "SELECT * FROM users" | tail -1 | grep -q '"ok":true,"rowCount":1' || fail "select"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary "SELECT * FROM nope" "$URL/query")
[ "$code" = 404 ] || fail "unknown table status $code"
# UPDATE·DELETE 배치 (decisions #23)
q "INSERT INTO users VALUES ('tmp', 1)" > /dev/null
[ "$(q "UPDATE users SET age = 26 WHERE id = 1; DELETE FROM users WHERE id = 2")" = '{"ok":true,"affectedRows":[1,1]}' ] || fail "update/delete batch"
q "SELECT age FROM users WHERE id = 1" | grep -q '^\[26\]$' || fail "updated value"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary "INSERT INTO users VALUES ('a', 1); DELETE FROM users" "$URL/query")
[ "$code" = 400 ] || fail "mixed insert/delete status $code"

# 부하: 4개 클라이언트가 10행 배치를 계속 보낸다. 중간에 SIGTERM.
batch=""
for i in $(seq 1 10); do batch+="INSERT INTO users VALUES ('load', $i);"; done
for c in 1 2 3 4; do
  (
    for _ in $(seq 1 400); do
      r=$(curl -s -X POST --data-binary "$batch" "$URL/query" 2>/dev/null || true)
      case "$r" in *'"ok":true'*) echo ok >> "$D/committed.$c" ;; *) break ;; esac
    done
  ) &
done
sleep 1
kill -TERM "$PID"
wait "$PID" || fail "server exit code $?"
PID=""
wait  # 클라이언트 루프
grep -q 'shutting down' "$D/server.log" || fail "graceful shutdown log"
committed=$(cat "$D"/committed.* 2>/dev/null | wc -l)
[ "$committed" -gt 0 ] || fail "no batches committed before SIGTERM"

# 재시작: 성공 응답을 받은 배치는 모두 남아 있어야 한다. 배치는 전부 아니면 전무 → 행 수는 1 + 10의 배수
start
rows=$(q "SELECT * FROM users" | tail -1 | sed -E 's/.*"rowCount":([0-9]+).*/\1/')
expected_min=$((1 + committed * 10))
[ "$rows" -ge "$expected_min" ] || fail "rows after restart $rows < committed $expected_min"
[ $(( (rows - 1) % 10 )) -eq 0 ] || fail "partial batch visible: $rows rows"
kill -TERM "$PID"
wait "$PID" || fail "second shutdown"
PID=""
echo "serve_e2e: ok (committed batches=$committed, rows=$rows)"
