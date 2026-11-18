# mini-dbms

**전원이 나가도 깨지지 않는 C++17 미니 DBMS.** SQL 처리기 → B+ 트리 인덱스 → 스레드 풀 API 서버까지, 크래프톤 정글 미니 프로젝트.

| 숫자 | 의미 |
|---|---|
| **1.9 µs vs 45.5 ms** | 100만 행에서 id 조회(B+ 트리) vs 다른 컬럼 조회(전체 스캔). 약 24,000배 |
| **+128 kB** | 100만 행(25.7 MB)을 스트리밍하는 동안 늘어난 서버 메모리 |
| **+63%** | 락 분리 후 혼합 부하(읽기 90%) 2스레드 처리량. 473k → 770k ops/s |

## 목차
1. 구조
2. Phase 1 — 파일에 어떻게 쓰나
3. Phase 2 — B+ 트리 인덱스
4. Phase 3 — API 서버와 동시성
5. 실제 DBMS와 비교
6. 만들면서 발견한 것
7. 한계
8. 테스트
9. 빌드와 실행
10. 문서

---

## 1. 구조

```
  .sql 파일 ─▶ minidb run ─┐
                           ├─▶ Lexer ─▶ Parser ─▶ Binder ─▶ Planner ─▶ Executor ─▶ RowSink
  HTTP POST ─▶ minidb serve┘   (토큰)   (AST)    (검증)    (경로 선택)  (실행 루프)   (CLI 표 / NDJSON)
                                                                         │
                                                                       Table ── .tbl 파일 (append-only)
                                                                         └──── B+ 트리 (id → 파일 위치)
```

- CLI와 HTTP 서버는 **같은 엔진**을 쓴다. 다른 건 결과를 받는 쪽(`RowSink`)뿐이다.
- 파일·인덱스·id·락은 **`Table`만** 만진다. 그 위 계층은 락을 모른다. 동시성 버그가 생길 곳을 한 클래스에 가뒀다.
- 결과는 모으지 않고 **한 행씩 흘려보낸다.** 그래서 메모리가 결과 크기와 무관하다.

## 2. Phase 1 — 파일에 어떻게 쓰나

### 왜 이 포맷인가

| 단계 | 방법 | 깨지는 상황 |
|---|---|---|
| 0 | 한 줄에 한 행 (CSV) | 값에 쉼표·줄바꿈이 들어가면 경계가 무너진다 |
| 1 | 앞에 길이를 붙인다 | 쓰다가 전원이 나가면 반쯤 쓴 행을 알아볼 수 없다 |
| 2 | 끝에 CRC를 붙인다 | INSERT 3개 배치 중 2개만 남는 "일부 커밋"이 생긴다 |
| 3 | **여러 행을 프레임 하나로 묶고 CRC 하나로 덮는다** | 프레임이 온전하면 커밋, 아니면 없던 일. **프레임 = 트랜잭션** |
| 4 | **append-only** — 쓴 바이트는 다시 안 바꾼다 | 이 성질 덕분에 복구와 동시성이 단순해진다 |

과제 범위가 INSERT·SELECT뿐이라, 실제 DB가 쓰는 "페이지 + WAL" 두 겹 대신 **로그 자체를 테이블로** 썼다. 고칠 일이 없으면 페이지는 로그의 복사본일 뿐이기 때문이다. 나중에 넣은 UPDATE·DELETE도 같은 원칙을 지킨다: 제자리에서 고치지 않고 새 버전과 묘비(id만 든 레코드)를 덧붙인다(결정 #20).

### 실행

```
$ minidb run --data data demo.sql
INSERT 1 (id=1)
INSERT 1 (id=2)
INSERT 1 (id=3)
id                   | name             | age
---------------------+------------------+---------------------
                   1 | kim              |                   25
                   2 | lee              |                   31
                   3 | park             |                   28
(3 rows)
demo.sql:8:8: SchemaError: unknown column 'nope' in table 'users'
```

에러는 `파일:줄:칸`으로 위치를 짚고, 다음 문장을 계속 실행한다(`--bail`이면 멈춤).

### 파일 안을 직접 보기

```
$ minidb dump --data data users
header: MDB1 v1
frame #0 @8 len=29 records=1 crc=ok
  @16 id=1 name='kim' age=25
frame #1 @45 len=29 records=1 crc=ok
  @53 id=2 name='lee' age=31
frame #2 @82 len=30 records=1 crc=ok
  @90 id=3 name='park' age=28
3 frames, 3 records, ok
```

### 전원이 나가면: 꼬리는 자르고, 중간은 거부한다

마지막 프레임을 5바이트 잘라 "쓰다가 죽은" 상태를 만들고 다시 열면:

```
$ minidb run --data data select.sql
warning: table 'users': torn tail (33 bytes at offset 82) truncated, saved to data/users.tbl.torn
id                   | name             | age
---------------------+------------------+---------------------
                   1 | kim              |                   25
                   2 | lee              |                   31
(2 rows)
```

- **꼬리 손상**(마지막 프레임이 덜 써짐) = 커밋된 적 없는 데이터. 잘라내고 정상 시작한다.
- 잘라낸 바이트는 **`.torn` 파일로 보관**한다. 판정이 틀려도 데이터를 잃지 않는다.
- **중간 손상**(뒤에 정상 프레임이 더 있는데 앞이 깨짐) = 커밋된 데이터가 상한 것. 자동으로 고치지 않고 **시작을 거부**한다.
- 길이 필드가 깨져 "4 GB짜리 프레임"처럼 보이면, 꼬리로 오판해 파일 전체를 자를 수 있다. 그래서 길이가 16 B~4 MiB 밖이면 위치와 상관없이 중간 손상으로 본다. 덕분에 잘리는 양은 언제나 프레임 하나 이하다.
- 이 판정 규칙은 코드 한 곳(`frame::scanFrames`)에만 있고, 복구와 `dump`가 같이 쓴다.

## 3. Phase 2 — B+ 트리 인덱스

- 인덱스는 **id → 파일 오프셋**이다. 레코드는 파일에 두고, 위치만 메모리에 둔다.
- `WHERE id = 상수`면 인덱스를 타고, 그 밖은 전체 스캔한다. `EXPLAIN`으로 확인할 수 있다.

```
EXPLAIN SELECT * FROM users WHERE id = 2;        → IndexScan(users, id=2)
EXPLAIN SELECT * FROM users WHERE name = 'kim';  → FullScan(users, filter name='kim')
```

- 재시작하면 복구 스캔에서 id가 이미 오름차순으로 나온다. 그래서 하나씩 삽입하는 대신 **리프를 꽉 채워 아래에서 위로 한 번에 짓는다**(벌크 로딩). 100만 건 재시작에 136 ms.

| 행 수 | id 조회 (B+ 트리) | 전체 스캔 | 배율 |
|---:|---:|---:|---:|
| 10,000 | 1.3 µs | 0.61 ms | 464× |
| 100,000 | 1.7 µs | 4.13 ms | 2,491× |
| 1,000,000 | **1.9 µs** | **45.5 ms** | **24,361×** |

행이 100배가 되는 동안 조회는 거의 그대로고 스캔은 선형으로 는다. 조회 1.9 µs의 대부분은 트리 탐색이 아니라 파일 읽기(`pread`)다. 노드 하나에 키 128개라 100만 건에서도 높이가 3이다.

## 4. Phase 3 — API 서버와 동시성

### 요청과 응답

```
> POST /query   INSERT INTO users VALUES ('choi', 40); INSERT INTO users VALUES ('jung', 22);
{"ok":true,"insertedIds":[3,4]}                                         [200]

> POST /query   SELECT * FROM users
{"columns":["id","name","age"]}
[1,"kim",25]
[2,"lee",31]
[3,"choi",40]
[4,"jung",22]
{"ok":true,"rowCount":4}                                                [200]

> POST /query   SELECT * FROM nope
{"ok":false,"error":{"type":"SchemaError","message":"unknown table 'nope'","line":1,"col":15}}   [404]

> POST /query   INSERT ... ('ok', 1); INSERT ... ('toolongname_over16', 2);
{"ok":false,"error":{"type":"SchemaError","message":"value too long for column 'name' VARCHAR(16): 18 bytes",...}}   [400]
```

- **원자 배치**: INSERT 여러 개는 프레임 하나로 커밋된다. 마지막 예처럼 하나라도 틀리면 **아무것도 쓰지 않는다.**
- **스트리밍(NDJSON)**: SELECT는 한 줄에 한 행씩 보낸다. HTTP 상태 코드는 첫 줄과 함께 나가므로, 스트리밍 도중 에러는 **마지막 줄**(`"ok":false`)로 알린다. 클라이언트는 마지막 줄이 `"ok":true`일 때만 결과를 믿는다.

### 스레드 풀과 503

| 단계 | 방법 | 깨지는 상황 |
|---|---|---|
| 0 | 요청마다 스레드 생성 | 요청 1만 개 = 스레드 1만 개 |
| 1 | 고정 워커 + 무한 큐 | 큐가 끝없이 쌓이고 응답은 결국 타임아웃 |
| 2 | **크기 제한 큐, 가득 차면 즉시 503** | 클라이언트가 "바쁘니 나중에"를 바로 안다 |

쓴 HTTP 라이브러리(cpp-httplib 0.14)는 큐가 거절해도 신호를 주지 않는다. 그래서 거절할 연결을 전용 스레드("거절 레인")로 넘겨 503을 보낸다.

### 락: 읽는 쪽은 거의 기다리지 않는다

- 읽기는 락 없이 한다. 쓴 바이트는 안 바뀌니(append-only) 안전하다. UPDATE·DELETE도 덧붙이기라 이 전제가 유지된다.
- 전체 스캔은 인덱스를 id 순서로 256개씩 짧게 락 잡고 받아 와 읽는다. 시작 시점의 최대 id까지만 보므로 INSERT 배치는 **전부 보거나 하나도 안 본다.** 스캔 도중 커밋된 여러 행 UPDATE·DELETE는 일부만 보일 수 있다(READ COMMITTED 수준, 결정 #21).
- 쓰는 쪽도 파일에 쓰는 동안은 읽는 쪽을 막지 않는다. 쓰는 쪽끼리만 줄 서는 락(`writeMu_`)을 따로 두고, 공유 락은 **공개하는 순간**에만 잡는다.

| 혼합 부하 (읽기 90%), 2스레드 | 처리량 | 락 때문에 잠든 횟수 / 1,000 작업 |
|---|---:|---:|
| 락 분리 전 (`pwrite` 동안 독점 락) | 473k ops/s | 86 |
| **락 분리 후** | **770k ops/s** | **20** |

5회 반복 중앙값, vCPU 2개. 분석 전체: [`docs/lock-split.md`](docs/lock-split.md)

## 5. 실제 DBMS와 비교

| 영역 | 이 프로젝트 | PostgreSQL / InnoDB | 차이의 이유 |
|---|---|---|---|
| 저장 | 로그 = 테이블 (append-only) | 페이지 데이터 파일 + WAL | 수정이 없으면 페이지는 로그의 복사본 |
| 원자성 | 프레임 1개 = 커밋 | WAL 커밋 레코드 | 원리는 같다 |
| 복구 | 꼬리는 자르고 중간은 거부, `.torn` 보관 | 첫 손상 지점을 로그 끝으로 봄 | 우리는 로그가 유일한 사본이라 더 보수적 |
| 내구성 | 서버는 1초마다 fsync, 실패하면 정지 | 커밋마다 fsync + group commit | 처리량과 손실 범위(최대 1초)의 맞바꿈 |
| 동시성 | 파일 길이 스냅샷 + 락 밖 읽기 | 행 단위 MVCC + 행 락 | 최소한으로 줄인 MVCC |
| 인덱스 | 메모리 B+ 트리, 시작 시 벌크 로딩 | 디스크 B+ 트리 + 버퍼 풀 | 과제가 메모리 기반을 요구 |
| 실행 | 고정 루프 (scan → filter → project) | 연산자 트리 + 비용 기반 플래너 | JOIN·ORDER BY가 없으면 트리가 필요 없음 |

## 6. 만들면서 발견한 것

| 증상 | 원인 | 해결 |
|---|---|---|
| 요청마다 정확히 40 ms 지연 (640개에 14초) | Nagle 알고리즘과 지연 ACK가 서로를 기다림 | `TCP_NODELAY` → 0.22초 |
| 클라이언트 8 · 워커 4에서 요청 절반이 타임아웃 | keep-alive 연결 하나가 워커 하나를 끝까지 붙잡음 | 구조적 한계로 기록 (7절) |
| TSan이 10번 중 9번 시작하자마자 죽음 | WSL2 커널의 넓은 주소 무작위화(ASLR) 범위 | ASLR 끄고 실행 → 10/10 |
| "shared_mutex는 이득이 없다"는 벤치 결론 | 1.5초 측정을 한 번만 본 노이즈 (±25%) | 5회 반복 → 실제로는 1.8배 이득. 진짜 문제(락이 I/O를 덮음)를 찾아 락 분리 |
| 진단 도구가 "ok"라는데 서버는 시작을 거부 | `dump`와 복구가 판정 규칙을 따로 구현해 어긋남 | 판정을 `scanFrames` 하나로 통합 |

## 7. 한계

- **꼬리 손상 뒤 id 재사용.** 다음 id = 파일에 남은 최대 id + 1이다. 위 데모에서 잘린 id 3이 서버에서 'choi'에게 다시 발급됐다. 원인은 fsync 전에 응답한 것(1초 주기)이고, 고치려면 id를 묶음으로 예약해 따로 저장해야 한다.
- **keep-alive 연결 하나 = 워커 하나.** 열린 연결이 워커 수보다 많으면 나머지는 요청이 없어도 기다린다. 이벤트 루프로 요청 단위 배분이 필요하다.
- **쓰기는 테이블당 한 줄로 선다.** 파일 하나에 덧붙이기 때문이다. 여러 요청을 fsync 한 번으로 묶는 group commit이 다음 후보다.
- **파일이 줄지 않는다.** UPDATE·DELETE는 덧붙이기만 한다. 옛 버전을 걷어 내는 compaction이 없다.
- **한 요청이 바꿀 수 있는 행 수에 상한**(프레임 4 MiB). 100만 행의 `DELETE FROM t`는 400이다.
- 측정은 vCPU 2개 컨테이너에서 했다. 병렬성 수치는 코어가 많은 환경에서 다시 재야 한다.

## 8. 테스트

- 단위 테스트(GoogleTest) 7개 바이너리 + 셸 종단 간 테스트 2개(CLI, 서버).
- **장애 주입**: 쓰기 절반 실패, 되돌리기 실패, 읽기·fsync 실패를 주입하는 `FaultyFileIO`로 복구 규칙을 검증한다.
- 세 가지 빌드에서 모두 통과: debug(AddressSanitizer + UBSan), tsan(ThreadSanitizer), release.
- 동시성: 병렬 INSERT의 id 유일성, 스캔이 배치 일부만 보지 않음, 쓰기 I/O 중에도 읽기가 막히지 않음.

## 9. 빌드와 실행

Docker 개발 컨테이너에서 빌드한다. Windows·macOS 호스트에 툴체인이 없어도 된다.

```bash
docker compose run --rm dev
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

컨테이너 안에서 써 보기:

```bash
mkdir -p /data/demo && printf 'name VARCHAR(16)\nage INT\n' > /data/demo/users.schema
echo "INSERT INTO users VALUES ('kim', 25); SELECT * FROM users;" > /tmp/a.sql
./build/debug/minidb run  --data /data/demo /tmp/a.sql
./build/debug/minidb dump --data /data/demo users
./build/debug/minidb serve --data /data/demo --port 8080 &
curl -s -X POST --data-binary "SELECT * FROM users WHERE id = 1" http://127.0.0.1:8080/query
```

벤치마크: `bench/run_bench.sh`, 결과는 `docs/bench.md`.

## 10. 문서

| 문서 | 내용 |
|---|---|
| [`docs/guide.md`](docs/guide.md) | **처음 읽을 문서.** 동작 원리를 순서대로 따라가는 가이드 |
| [`docs/architecture.md`](docs/architecture.md) | 설계 (단일 문서, 규칙 참조용) |
| [`docs/decisions.md`](docs/decisions.md) · [`decisions-summary.md`](docs/decisions-summary.md) | 결정 로그 · 사람이 정한 결정 요약 |
| [`docs/bench.md`](docs/bench.md) · [`lock-split.md`](docs/lock-split.md) | 벤치마크 · 락 분리 분석 |
| [`docs/retro/`](docs/retro/) | 회고 (겪은 문제·원인·해결) |
| [`docs/refactoring.md`](docs/refactoring.md) · [`quality-trend.md`](docs/quality-trend.md) | 다음 후보 · 코드 품질 추세 |
| [`docs/assignment.md`](docs/assignment.md) | 과제 원문 |
| [`CHANGELOG.md`](CHANGELOG.md) | 변경 내역 |
