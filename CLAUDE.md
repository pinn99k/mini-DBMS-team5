# minidb — CLAUDE.md

C++17 mini DBMS (Krafton Jungle). P1 SQL/CLI + file storage → P2 in-memory B+tree → P3 HTTP server + thread pool.

@ai-collab-rules/rules/ai-collab-coding-discipline.md
@ai-collab-rules/rules/ai-collab-change-summary.md
@ai-collab-rules/rules/ai-collab-quality-alarms.md

## Docs
- `docs/architecture.md` — single design doc, current state. Source of truth for every rule. `[고정]` = human-fixed.
- `docs/decisions.md` — append-only log (who/why). Never edit past rows; supersede with a new row.
- `docs/decisions-summary.md` — human/delegated decisions only, by topic, for the team. Copy of architecture; `[고정]` or human/delegated decision changes → update in the same commit.
- `docs/guide.md` — human learning guide (narrative). Design change → update the affected chapter in the same commit.
- `docs/assignment.md` — original assignment. Fixed.
- `docs/refactoring.md` — post-scope candidates. Don't implement during P1–P3.
- `docs/explain-log.md` — understanding checks. Add 1 row per delegated decision / AI principle the system relies on.
- `docs/archive/` — 2026-11-15 big design (briefs, old decisions). Frozen. History only; never cite as current.
- `CHANGELOG.md` — start at first milestone; one entry per milestone.
- Conflict: latest decisions row > architecture.md > archive. Code ≠ doc → fix one in the same commit.

## Roles
- Human: one-way-door decisions only.
- AI: detail design + logic + implementation + tests. Write the detail into architecture.md; notable → decisions row (정한 사람 AI) + explain-log 대기.
- `ai-collab-design-briefing.md` (question map, ask on core logic) is NOT applied here — superseded by decision #9.

## Stop and ask (options + trade-offs + pick, then wait)
- Any `[고정]` rule, on-disk format, recovery rules, lock strategy, write ordering, error→status mapping, response format.
- New external dependency (allowed: GoogleTest, cpp-httplib P3).
- New abstraction/pattern used across 2+ modules.
- After the answer: append decisions row, update architecture.md.

## Workflow
- Milestones: architecture §17. Tests green before next milestone. Next phase only after human confirms.
- Before coding a milestone: 1-line expected change (change-summary rule).
- Commits: one milestone or less per commit; update CHANGELOG.md on milestone completion.

## Build (inside container)
```
docker compose run --rm dev
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
- Presets: debug (ASan+UBSan), release (-O2, bench), tsan (P3; tests run under `setarch $(uname -m) -R`).
- Data for bench/durability tests: `/data` volume (ext4). Never on `/work` (9p bind mount).
- git runs on the Windows host, not in the container.

## Hard constraints (details: architecture §4)
- `-Wall -Wextra -Werror`. POSIX fd + pread/pwrite for table files, no fstream. LE byte-by-byte encoding, no struct memcpy.
- Only `Table` touches file + index + nextId + lock. Results stream row by row to RowSink; never materialize.
