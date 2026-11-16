> **보관 문서** (2026-11-15 Cowork와 정한 큰 설계). 수정하지 않는다. 현재 기준은 `docs/architecture.md`, 이후 결정은 `docs/decisions.md`.

# Mini DBMS — Common Brief (for Claude Code)

Applies to every phase. Phase briefs: `docs/briefs/phase-N.md`.
Decision record (source of truth): `docs/decisions.md`. Conflict → decisions.md wins; report it.

## 1. Role split

- Human: one-way-door decisions, phase briefs (decisions + constraints + done criteria).
- Claude Code: overall architecture, module/file layout, interfaces, detailed spec, implementation, tests.

## 2. Working protocol

- Before phase 1 code: write `docs/architecture.md` (overall structure, layers, dependency direction, how phases 2 and 3 plug in). Wait for approval.
- Per phase: write `docs/specs/phase-N.md` BEFORE code. Wait for approval.
  Must contain: module/file changes, public interfaces, data flow, test plan mapped to done criteria, every choice not fixed by the brief.
- Implement milestone by milestone; tests green before the next.
- Do not start phase N+1 until phase N done criteria pass and human confirms.
- Follow project CLAUDE.md / ai-collab rules (change summary, quality alarms, explain-back).

### You decide (record in spec, no need to ask)
- Names, file splits, private helpers, internal data structures, test code, CMake setup.

### Stop and ask (options + trade-offs + your pick, then wait)
- Changes to on-disk format, lock strategy, write ordering, error→status mapping, response format.
- New external dependency. New abstraction/pattern used in 2+ places.
- Anything listed as "Open decision" in a phase brief.
- Anything contradicting decisions.md or a brief.
After human decides → append to decisions.md.

## 3. Global constraints

- C++17, Linux (WSL/Docker). CMake. GoogleTest.
- Allowed external deps: GoogleTest, cpp-httplib (Phase 3 only). Nothing else.
- Self-implemented: CRC32 (IEEE, reflected poly 0xEDB88320), JSON serializer (output only).
- Flags: `-Wall -Wextra -Werror`. Debug: ASan+UBSan. Release (-O2) for benchmarks. TSan target from Phase 3.
- Table file I/O: POSIX fd + `pread`/`pwrite`. No `fstream` for table files.
- On-disk integers: little-endian, serialized byte by byte. Never memcpy structs.

## 4. Structural invariants (architecture must satisfy)

- One component (call it `Table`) is the only code touching data file + index + nextId + lock together. Code above it never sees locks.
- Table insert API takes 1..N rows and commits them as ONE frame (all or nothing). Single insert = batch of 1.
- Query results flow row by row (Volcano-style): executor pushes each row to a caller-supplied sink as soon as it is produced. No component materializes a full result set. CLI and server are just different sinks.
- Parsed statement = `std::variant<...>` dispatched with `std::visit`.
- Errors = typed exceptions: `ParseError`, `SchemaError`, `StorageError`.
- Executor has an access-path choice point (full scan now, index later).
- A test-only fault-injection seam exists for file writes (needed for atomicity tests).
