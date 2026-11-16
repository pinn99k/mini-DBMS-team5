> **보관 문서** (2026-11-15 Cowork와 정한 큰 설계). 수정하지 않는다. 현재 기준은 `docs/architecture.md`, 이후 결정은 `docs/decisions.md`.

# Phase 3 Brief — API server (thread pool, atomic batches, streaming)

Read `docs/briefs/common.md` first. Builds on Phase 1 + 2 as-is.

## Scope

- HTTP server exposing the engine. Requests processed in parallel by a self-built thread pool.
- Atomic multi-INSERT batches. Streaming SELECT responses.

## Fixed rules — server

- cpp-httplib for HTTP; replace its pool via `new_task_queue` with our ThreadPool.
- ThreadPool: fixed workers, bounded queue; full → 503.
- CLI: `minidb serve --port 8080 --workers <N> --queue 1024 --data ./data`
  Defaults: port 8080, workers = `std::thread::hardware_concurrency()`, queue 1024.
- Max request body 1 MiB → 413. Read/write timeout 5 s. Keep-alive on.
- Endpoints: `POST /query` (body = SQL text), `GET /health` → 200 `{"ok":true}`.

## Fixed rules — request shape

- Allowed bodies (trailing `;` optional):
  - exactly one `SELECT`, or exactly one `EXPLAIN SELECT`, or
  - 1..1000 `INSERT` statements (batch). All statements must be INSERT.
- Empty body, mixed statement kinds, multiple SELECTs, > 1000 statements → 400, nothing executed.
- Batch may target one table only (multi-table batch → 400). Refactor candidate otherwise.

## Fixed rules — atomic INSERT batch

1. Parse and validate ALL statements first. Any ParseError/SchemaError → 400, nothing executed.
2. Acquire table unique_lock. Remember start fileSize and nextId.
3. Assign ids in order; encode all rows into ONE frame; single write.
4. Write failure → ftruncate to start size, restore nextId, release lock → 500. Nothing applied.
5. Success → update fileSize, advance nextId, insert all ids into index, release lock.
- Readers never observe a partial batch: fileSize moves only after the full frame is written, so every scan end is a frame boundary.

## Fixed rules — locking (unchanged from decisions)

- insert/batch: unique_lock for the whole operation.
- findById: shared_lock only for the tree lookup; read file after releasing.
- scan: shared_lock only to read fileSize as `end`; scan up to `end` without lock (snapshot).

## Fixed rules — responses

- Errors detected before any response byte: status + JSON
  `{"ok":false,"error":{"type":"...","message":"...","line":L,"col":C}}` (line/col when available).
- Status mapping: ParseError → 400; SchemaError → 404 if unknown table, else 400; StorageError → 500;
  queue full → 503; body too large → 413.
- INSERT batch → 200 `application/json` `{"ok":true,"insertedIds":[...]}` (sent after commit).
- EXPLAIN → 200 `application/json` `{"ok":true,"plan":"..."}`.
- SELECT → 200 `application/x-ndjson`, chunked, one JSON value per line:
  ```
  {"columns":["id",...]}
  [1,"kim",25]
  ...
  {"ok":true,"rowCount":N}                          ← success
  {"ok":false,"error":{"type":"...","message":"..."}} ← failure after streaming started
  ```
  Client rule: result valid only if the last line has `"ok":true`. Missing last line = failure.
- No row-count cap. Rows are serialized and sent as produced; memory independent of result size.
- Slow client: write timeout (5 s) closes the connection and frees the worker.

## Fixed rules — durability and shutdown

- Background thread fsyncs every 1 s. fsync failure → log + stop server (no retry).
- SIGINT/SIGTERM: handler only sets `std::atomic<bool>`. Main loop: stop accepting → drain queue + in-flight (incl. streams, bounded by timeouts) → fsync → close.

## Open decisions (propose in spec, wait)

- Concurrency benchmark design (global mutex vs shared_mutex throughput; batch vs single insert). Test stage.

## Suggested milestone order

1. ThreadPool standalone (submit, bounded reject, graceful shutdown)
2. Table locking + batch commit + concurrency tests under TSan
3. HTTP layer: request validation, JSON errors, batch endpoint, NDJSON streaming
4. fsync thread + signal handling + shutdown order

## Done criteria

- Concurrency: N threads × M inserts → total N×M, unique ids, `validate()` passes; TSan clean on mixed load.
- Batch atomicity:
  - one invalid statement in a batch → 400, row count unchanged.
  - injected write failure mid-batch → 500, file size and nextId unchanged.
  - file truncated in the middle of a batch frame → after restart the whole batch is gone.
  - while K-row batches commit concurrently, every concurrent full scan returns a row count that is a multiple of K.
- Streaming: SELECT over 1M rows completes with bounded memory (record peak RSS); injected read failure mid-stream ends with `"ok":false` line.
- Request shape: empty, mixed, multiple SELECT, > 1000 statements → 400; > 1 MiB → 413; queue full → 503.
- Unknown table → 404. `/health` → 200.
- Shutdown: SIGTERM under load completes in-flight requests; committed rows present after restart.
- Throughput comparison in `docs/bench.md` per approved design.
