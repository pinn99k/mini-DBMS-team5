# Changelog

마일스톤 단위로 덧붙인다. 설계 근거는 `docs/architecture.md`, 결정 이유는 `docs/decisions.md`.

## 0.2.0 — 2026-11-17 (UPDATE·DELETE)

결정 #20~#24. 설계는 `architecture.md` §5.4b·§5.5·§9.6, 해설은 `guide.md` 8장.

- **M16 B+tree 삭제·범위**: `erase`(형제에게 빌리기 → 병합 → 루트 축소, 지운 키가 경계값으로 남으면 고침), `assign`(값 바꾸기), `range(from, to, max)`(id 순서 순회). `std::map`과 비교하는 무작위 삽입·삭제 테스트(용량 3·4·128), 전부 지우기, guide 8-1 예제.
- **M17 저장소**: UPDATE = 같은 id의 전체 레코드, DELETE = id만 든 8 B 묘비를 덧붙인다(포맷·version 그대로). `frame::scanFrames`가 레코드 종류를 추론하고 살아 있는 id 표를 들고 가서, 없는 id·지운 id를 고치는 레코드를 중간 손상으로 판정한다(예전 "id 엄격한 오름차순" 대체). `Table::modify`(문장 순서대로 적용, 프레임 하나, 대상 찾기부터 `writeMu_` 안), 전체 스캔은 id 순서 커서(256개씩 짧은 락, 시작 시 maxId 상한, 흩어진 레코드는 따로 읽기). `minidb dump`가 `update`·`delete` 레코드와 `L live`를 출력. 테스트: 왕복·재시작, 최대 id 삭제 후 재사용 없음, 배치 순서, 수정 프레임 꼬리·중간 손상, 장애 주입, 프레임 상한, 잃어버린 갱신(동시성), 스캔 중 수정.
- **M18 SQL·엔진·CLI**: `UPDATE t SET c = v [, ...] [WHERE c = v]`, `DELETE FROM t [WHERE c = v]`. `UPDATE`·`SET`·`DELETE`는 예약어. Binder가 `SET id`·같은 컬럼 두 번·타입·VARCHAR 길이를 검사하고, 접근 경로는 SELECT와 같은 규칙(`WHERE id =` → 인덱스). `Executor::modify`(배치, 한 테이블, 원자적). CLI 출력 `UPDATE n`·`DELETE n`, 문장마다 커밋. `Table::modify`를 찾기·적용(`stageEdits`)과 쓰기·공개(`commitPending`)로 나눔.

## 0.1.2 — 2026-11-17 (락 분리)

- **락 분리**(#19): `Table`에 쓰는 쪽끼리의 순서용 `writeMu_`를 더하고, 인코딩·`pwrite`·`ftruncate`는 그 락만 쥐고 한다. `shared_mutex`는 공개 순간(`fileSize_`·`index_` 갱신)에만 독점으로 잡는다. 혼합 부하(읽기 90%) 2스레드 473k → 770k ops/s(+63%), 락 때문에 잠드는 횟수 작업 1,000개당 86 → 20회. 읽기 전용·쓰기 전용은 노이즈 범위 안에서 그대로.
- 회귀 테스트 `ReadersDoNotWaitForWriterIo`: 쓰는 쪽을 `pwrite` 안에서 멈춰 두고 읽기가 막히지 않는지 본다. 수정 전 코드에서 실패함을 확인했다.
- `minidb_bench_lock`에 "작업 1,000개당 자발적 문맥 교환" 열을 추가했다.
- `bench.md` §5의 "shared_mutex가 이기지 못한다"는 1회 측정의 노이즈였다. 정정을 달았다(5회 반복하면 읽기 전용 2스레드에서 shared가 약 1.8배 빠름).
- 분석 문서 `docs/lock-split.md`, 원자료 `bench/results/lock_split.csv`.

## 0.1.1 — 2026-11-17 (리팩터링 1차)

동작은 바꾸지 않았다. 바뀐 출력은 하나뿐이다: `minidb dump`가 CRC 불일치 프레임에 `crc=BAD` 줄을 따로 찍지 않고 판정 줄로만 알린다.

- **손상 판정을 한 곳으로**(#18): `frame::scanFrames`(`src/storage/frame_scan.*`)를 새로 만들고 `Table::recover`와 `minidb dump`가 같이 쓴다. 전에는 dump에 id 오름차순 검사와 payload 64 KiB 상한 검사가 빠져 있어서, 시작이 거부되는 파일을 dump는 ok로 보고할 수 있었다. 회귀 테스트를 추가했다.
- `Lexer::next`(CC 39)를 토큰 종류별 `lexWord`·`lexNumber`·`lexString`으로 나눴다.
- `QueryHandler::handle`(CC 18)을 `parseBody`·`respondInsert`·`respondExplain`·`respondSelect`로 나눴다.
- 문서 수정: architecture §5.3의 꼬리 자르기 상한 "4 MiB + 12"를 "4 MiB + 8"로 고쳤다(코드 `totalSize = 4 + frame_len + 4`).
- 침식도 0.56 → 0.38 (`docs/quality-trend.md`).

## 0.1.0 — 2026-11-16 (MVP)

M1~M15를 한 번에 구현했다. debug(ASan+UBSan)·tsan·release 프리셋에서 테스트 9개(gtest 7 + 셸 종단 간 2) 통과.

- **Phase 1**: CRC32 프레임 append-only 저장, 꼬리·중간 손상 판정과 `.torn` 보관, Lexer·Parser(문장 단위 pull), Binder·Planner·Executor(RowSink push), `minidb run`·`minidb dump`.
- **Phase 2**: `BPlusTree<Cap>`(SoA 고정 배열, bottom-up 분할, `validate()`), 열 때 벌크 로딩, IndexScan, `EXPLAIN`, `minidb_bench`.
- **Phase 3**: ThreadPool(크기 제한 큐) + cpp-httplib, 거절 레인 503, 원자 INSERT 배치, NDJSON 스트리밍, 1초 주기 fsync, SIGTERM 정상 종료, `minidb_bench_lock`.
- 측정: 100만 건에서 id 조회 1.9 µs vs 전체 스캔 45.5 ms. 100만 행 스트리밍 중 서버 메모리 +128 kB (`docs/bench.md`).
- 구현 중 발견: Nagle 지연(결정 #14), 연결당 워커 점유(architecture §19), WSL2 TSan ASLR(결정 #7).
