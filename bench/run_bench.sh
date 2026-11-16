#!/usr/bin/env bash
# 벤치마크 전체 실행 (컨테이너 안, /work에서):
#   cmake --preset release && cmake --build --preset release && bash bench/run_bench.sh
# 데이터는 /data 볼륨(ext4). 결과는 bench/results/ 에 남는다.
set -euo pipefail
B=build/release
OUT=bench/results
DATA=/data/bench
mkdir -p "$OUT" "$DATA"

echo "== env"
{
  echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs) x $(nproc)"
  echo "kernel: $(uname -r)"
  echo "compiler: $(g++ --version | head -1)"
  echo "fs: $(df -T /data | tail -1 | awk '{print $2}')"
} | tee "$OUT/env.txt"

echo "== P2: index lookup vs full scan"
"$B/minidb_bench" --dir "$DATA/index" --rows 10000,100000,1000000 --csv "$OUT/index.csv" | tee "$OUT/index.txt"

echo "== P3: lock variants (Table API, no HTTP)"
{
  echo "variant,workload,threads,ops_per_s,rows_written_per_s"
  "$B/minidb_bench_lock" --dir "$DATA/lock"
  "$B/minidb_bench_lock_exclusive" --dir "$DATA/lock"
} | tee "$OUT/lock.csv"

echo "== SQL path: 1M single-row INSERT statements via minidb run"
S="$DATA/sql"
rm -rf "$S"
mkdir -p "$S/data"
printf 'name VARCHAR(32)\nage INT\n' > "$S/data/t.schema"
awk 'BEGIN { for (i = 1; i <= 1000000; i++) printf "INSERT INTO t VALUES (%cname_%d%c, %d);\n", 39, i, 39, i % 100 }' > "$S/insert.sql"
/usr/bin/time -v "$B/minidb" run --data "$S/data" "$S/insert.sql" > /dev/null 2> "$OUT/sql_run.txt"
grep -E "Elapsed|Maximum resident" "$OUT/sql_run.txt" | tee "$OUT/sql_run_summary.txt"
ls -l "$S/insert.sql" "$S/data/t.tbl" | awk '{print $5, $9}' | tee -a "$OUT/sql_run_summary.txt"

echo "== Streaming: SELECT * over 1M rows through the server"
PORT=18080
"$B/minidb" serve --data "$S/data" --port "$PORT" --workers 2 2> "$OUT/serve.log" &
PID=$!
for _ in $(seq 1 100); do curl -sf "http://127.0.0.1:$PORT/health" > /dev/null && break; sleep 0.1; done
before=$(grep VmHWM /proc/$PID/status | awk '{print $2}')
start=$(date +%s.%N)
curl -s -X POST --data-binary 'SELECT * FROM t' "http://127.0.0.1:$PORT/query" > "$DATA/stream.ndjson"
end=$(date +%s.%N)
after=$(grep VmHWM /proc/$PID/status | awk '{print $2}')
kill -TERM $PID
wait $PID
{
  echo "lines: $(wc -l < "$DATA/stream.ndjson")"
  echo "bytes: $(stat -c %s "$DATA/stream.ndjson")"
  echo "last: $(tail -1 "$DATA/stream.ndjson")"
  echo "seconds: $(echo "$end - $start" | bc 2>/dev/null || awk "BEGIN{print $end - $start}")"
  echo "server VmHWM before query: ${before} kB"
  echo "server VmHWM after query:  ${after} kB"
} | tee "$OUT/stream.txt"
