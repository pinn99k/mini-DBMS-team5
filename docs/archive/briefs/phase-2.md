> **보관 문서** (2026-11-15 Cowork와 정한 큰 설계). 수정하지 않는다. 현재 기준은 `docs/architecture.md`, 이후 결정은 `docs/decisions.md`.

# Phase 2 Brief — B+ tree index

Read `docs/briefs/common.md` first. Builds on Phase 1 as-is.

## Scope

- In-memory B+ tree `id → record offset`. Used for `WHERE id = ?`. Other columns: full scan.
- `EXPLAIN SELECT ...`.
- 1M+ record benchmark: index lookup vs full scan.

## Fixed rules — integration

- Index rebuilt at open from the same frame-by-frame scan that does the format check.
- Rebuild uses bulk loading (ids are in file order = ascending). If ids are not strictly ascending during rebuild → StorageError.
- Leaf values = record offsets (u64, position of the record inside its frame), never full rows.
- Insert order inside the table component (per frame): frame write → fileSize update → nextId advance → index insert for every record in the frame.
- Executor picks IndexScan iff WHERE is `id = <int>`; otherwise FullScan.
- IndexScan and FullScan both push rows to the sink one at a time (same interface as Phase 1).
- `EXPLAIN` output: `IndexScan(<table>, id=<v>)` | `FullScan(<table>)` | `FullScan(<table>, filter <col>=<v>)`.

## Fixed rules — tree design

- Unique keys only (i64). No delete. No range query (refactor candidate).
- Node capacity = compile-time template parameter. Production 128. Tests instantiate 3 and 4.
- Node layout: fixed-size arrays inline in the node, keys and values in separate arrays (SoA).
  `std::vector` inside nodes is not allowed.
- Two node structs (leaf, internal) sharing a header with `isLeaf`. No virtual functions.
- Leaf: keys[], offsets[], count, `next` (non-owning raw pointer).
  Internal: keys[], children[] (count+1, `std::unique_ptr` ownership).
- Split timing: bottom-up. Record descent path in a stack. No parent pointers.
- Split policy at runtime: 50/50.
- Leaf split = copy-up (first key of right leaf copied to parent, stays in leaf).
  Internal split = push-up (middle key moves to parent, removed from both halves).

## validate() must check

- Keys strictly ascending inside every node.
- All leaves at the same depth.
- Key count bounds (root exempt from minimum).
- Every internal separator == min key of its right subtree.
- Leaf chain visits all keys in ascending order; count equals inserted count.

## Open decisions (propose in spec, wait)

- Benchmark harness: data generation path, what is timed, warm-up/repetitions, scale points, reporting format.
  (Expected: Release build, fixed seed, engine API for bulk data, SQL path measured separately.)

## Suggested milestone order

1. BPlusTree standalone + `validate()` (capacity 3/4 in tests)
2. Bulk load
3. Integrate into table component + rebuild at open + IndexScan + EXPLAIN
4. Benchmark → `docs/bench.md`

## Done criteria

- Randomized insert/find tests (capacity 3, 4, 128) pass `validate()` every N ops; cascading splits up to new root covered.
- Bulk-loaded tree passes `validate()` and equals insert-built tree on find results.
- After 1M inserts: random id lookups equal full-scan results; missing id returns empty.
- Multi-record frames: every record of the frame is findable by id after commit and after restart.
- Restart restores count and nextId; empty table works.
- EXPLAIN shows IndexScan for `id =`, FullScan otherwise.
- `docs/bench.md` produced per approved harness.
