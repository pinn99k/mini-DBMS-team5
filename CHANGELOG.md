# Changelog

마일스톤 단위로 덧붙인다. 설계 근거는 `docs/architecture.md`, 결정 이유는 `docs/decisions.md`.

## 0.1.0 — 2026-11-16 (MVP)

M1~M15를 한 번에 구현했다. debug(ASan+UBSan)·tsan·release 프리셋에서 테스트 9개(gtest 7 + 셸 종단 간 2) 통과.

- **Phase 1**: CRC32 프레임 append-only 저장, 꼬리·중간 손상 판정과 `.torn` 보관, Lexer·Parser(문장 단위 pull), Binder·Planner·Executor(RowSink push), `minidb run`·`minidb dump`.
- **Phase 2**: `BPlusTree<Cap>`(SoA 고정 배열, bottom-up 분할, `validate()`), 열 때 벌크 로딩, IndexScan, `EXPLAIN`, `minidb_bench`.
- **Phase 3**: ThreadPool(크기 제한 큐) + cpp-httplib, 거절 레인 503, 원자 INSERT 배치, NDJSON 스트리밍, 1초 주기 fsync, SIGTERM 정상 종료, `minidb_bench_lock`.
- 측정: 100만 건에서 id 조회 1.9 µs vs 전체 스캔 45.5 ms. 100만 행 스트리밍 중 서버 메모리 +128 kB (`docs/bench.md`).
- 구현 중 발견: Nagle 지연(결정 #14), 연결당 워커 점유(architecture §19), WSL2 TSan ASLR(결정 #7).
