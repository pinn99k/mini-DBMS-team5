> **보관 문서** (2026-11-15 Cowork와 정한 큰 설계). 수정하지 않는다. 현재 기준은 `docs/architecture.md`, 이후 결정은 `docs/decisions.md`.

# Phase 1 Brief — SQL processor (CLI + file storage)

Read `docs/briefs/common.md` first.

## Scope

- `INSERT`, `SELECT` from a .sql file via CLI (`minidb run <file>`). REPL optional.
- `minidb dump <table>`: human-readable dump of a table file (frames and records).
- Schema and table assumed to exist. No CREATE TABLE.

## On-disk format (fixed)

```
data/<table>.schema   text, one column per line: "<name> INT" | "<name> VARCHAR(<n>)"
                      hidden first column `id` (i64) is NOT listed
data/<table>.tbl      [header 8B][frame]...
header  = magic "MDB1"(4) | version u16 = 1 | reserved u16 = 0
frame   = frame_len u32 | record_count u32 | record × record_count | crc32 u32
          frame_len = byte length of (record_count + records)
          crc32 covers the same bytes as frame_len
record  = payload_len u32 | payload
payload = id i64 | fields in schema order
          INT → i64 ; VARCHAR → len u16 + UTF-8 bytes (len ≤ n)
record offset = position of the record's payload_len (inside its frame).
limits: payload_len ≤ 64 KiB, frame_len ≤ 4 MiB, record_count ≥ 1.
```

- One INSERT statement = one frame with 1 record. Batches (Phase 3) = one frame with N records.
- Frame is the unit of atomicity: either the whole frame is on disk and valid, or it is discarded.

Open-time check (scan frame by frame from header):
- magic/version mismatch → StorageError, refuse start.
- frame incomplete or CRC mismatch, and its declared end ≥ file end → tail torn: ftruncate to frame start, warn, continue.
- frame invalid with bytes after it → mid corruption: StorageError, refuse start.
- records inside a valid frame must consume exactly frame_len and match record_count, else mid corruption.
- Reads after open trust bytes already verified (no per-read CRC).
## Fixed rules

- Grammar: `INSERT INTO t [(cols)] VALUES (v, ...);` / `SELECT * | cols FROM t [WHERE col = v];`
- Keywords case-insensitive. Strings in single quotes, `''` escape. Split statements by tokens, not `split(";")`.
- Types: INT (i64), VARCHAR(n). No NULL. WHERE: single `=` only.
- id: assigned by engine. User-supplied `id` → SchemaError.
  `nextId` = max id seen at open + 1 (empty → 1). Advance only after the frame write succeeds. Never reuse. `INT64_MAX` → StorageError.
- Insert = write one frame. Write failure → ftruncate back to previous size, throw; nextId unchanged.
- SELECT results are pushed row by row to a sink. CLI sink prints each row immediately.
  Column widths come from the schema (VARCHAR(n) → n, INT → 20), so no buffering is needed for alignment.
- Durability: `write` per frame, `fsync` at end of CLI run. New file → fsync file + parent directory.
- Error in one statement: print with line:col, continue (`--bail` stops at first error).

## Suggested milestone order

1. Record/frame encoding + CRC32
2. Table file: append frame / read(offset) / scan(end) / open-time check
3. Schema loading + validation
4. Lexer → Parser → AST
5. Executor with row sink (full scan only, access-path seam kept)
6. CLI (`run`, `dump`), streaming aligned output

## Done criteria

- Lexer: `,` `;` inside strings, `''`, mixed-case keywords, unterminated string (error with line:col).
- Parser/validation: syntax error, value/column count mismatch, type mismatch, VARCHAR overflow, id supplied.
- Storage: insert→select round-trip; tail-torn frame truncated; mid corruption refused; bad magic refused;
  frame with wrong record_count refused.
- Atomic write: injected write failure leaves file size and nextId unchanged.
- id: continues after restart; failed insert leaves no gap.
- SELECT output starts before the scan finishes (sink receives rows incrementally).
