# Mini DBMS — 설계

전체 구조와 세부 명세를 담은 **단일 설계 문서**다. 시스템의 현재 모습만 적고, 바뀌면 덮어쓴다.

- 상태: MVP 구현 완료 (2026-11-16). M1~M15 전부 구현, debug(ASan+UBSan)·tsan·release 프리셋에서 테스트 9개 통과. 측정 결과는 `docs/bench.md`.
- 표기: `[고정]`은 사람이 정한 규칙이다(2026-11-15 큰 설계, 결정 로그 #0~#5). 바꾸려면 묻는다. 표시가 없는 것은 AI가 정한 세부이고, 구현하면서 고칠 수 있다(고치면 결정 로그에 남긴다).
- `[P1]` `[P2]` `[P3]`는 그 내용이 처음 들어가는 Phase다.

## 목차
0. 문서 안내
1. 목표와 범위
2. 한 장 요약
3. 모듈과 의존 방향
4. 공통 규칙
5. 저장소 `[P1]`
6. SQL `[P1]`
7. 실행 엔진 `[P1·P2]`
8. CLI `[P1]`
9. 인덱스 `[P2]`
10. 벤치마크 `[P2·P3]`
11. API 서버 `[P3]`
12. 동시성 모델
13. 에러 흐름
14. 테스트
15. 빌드와 개발 환경
16. 디렉터리 구조
17. 마일스톤
18. 차별점
19. 남은 위험

---

## 0. 문서 안내

| 문서 | 역할 | 수정 방식 |
|---|---|---|
| `docs/assignment.md` | 과제 원문 | 고정 |
| `docs/guide.md` | 배우는 사람용 해설. 처음 읽는 문서 (이 문서는 마지막) | 설계가 바뀌면 같이 고침 |
| `docs/architecture.md` (이 문서) | 현재 설계. 규칙의 유일한 원본 | 덮어씀 |
| `docs/decisions.md` | 결정 로그. 언제·누가·왜 | 덧붙이기만 |
| `docs/decisions-summary.md` | 사람·위임 결정만 주제별로 모은 팀 공유용 요약. 이 문서의 사본 | `[고정]`이나 사람·위임 결정이 바뀌면 같은 커밋에서 고침 |
| `docs/refactoring.md` | 리팩터링 후보, 다 만든 뒤 할 일 | 자유 |
| `docs/explain-log.md` | 이해 검사 대기 목록 | 덧붙이고 상태만 바꿈 |
| `CHANGELOG.md` | 버전별 변경 (첫 마일스톤부터) | 덧붙이기만 |
| `docs/archive/` | 2026-11-15 큰 설계 원본(Phase별 지시서, 이전 결정 기록) | 수정 안 함. 이 문서로 흡수됨 |

충돌하면 결정 로그의 가장 최근 항목이 이기고, 이 문서를 그에 맞게 고친다. 코드와 이 문서가 다르면 같은 커밋에서 둘 중 하나를 고친다.

## 1. 목표와 범위

과제(`assignment.md`): Phase 1 SQL 처리기(CLI + 파일 저장) → Phase 2 메모리 B+ 트리 인덱스 + 100만 건 성능 비교 → Phase 3 스레드 풀 API 서버. 뒤 Phase는 앞 Phase를 그대로 내부 엔진으로 쓴다.

범위 밖 `[고정]`: 페이지, 버퍼 풀, ARIES 복구, CREATE TABLE, NULL, DELETE/UPDATE. 트랜잭션은 INSERT 배치의 원자성까지만 다룬다. 실질 난이도는 Phase 3의 동시성이다.

## 2. 한 장 요약

```
            ┌───────────── 진입점 ─────────────┐
            │  cli (run / dump)   server (P3)  │   ← 받는 쪽(sink)만 다르다
            └───────────────┬──────────────────┘
                            │ Statement (std::variant)
            ┌───────────────▼──────────────────┐
  sql ────▶ │ engine: Binder → Planner → Executor │ ──push──▶ RowSink
            └───────────────┬──────────────────┘
                            │ Table API (락은 안 보임)
            ┌───────────────▼──────────────────┐
            │ storage: Database · Table · Schema │
            │          FrameCodec · FileIO      │ ──▶ index: BPlusTree (P2)
            └──────────────────────────────────┘
                     common: 에러 · Value · LE 바이트 · CRC32 · JSON(P3)
```

구조 불변식 `[고정]`:
1. **`Table`만** 데이터 파일·인덱스·nextId·락을 함께 만진다. 그 위는 락을 모른다.
2. **INSERT API는 행 1..N개를 프레임 하나로 커밋한다** (전부 아니면 전무). 한 문장 INSERT는 크기 1짜리 배치.
3. **결과는 행 단위로 흐른다** (Volcano 방식). 실행기는 행이 나오는 즉시 sink로 민다. 어느 계층도 결과 전체를 모으지 않는다. CLI와 서버는 sink만 다르다.
4. 파싱된 문장은 `std::variant<...>`이고 `std::visit`으로 처리한다.
5. 에러는 타입별 예외: `ParseError`, `SchemaError`, `StorageError`.
6. 실행기에 접근 경로 선택 지점이 있다 (지금은 전체 스캔, P2부터 인덱스).
7. 파일 쓰기에 테스트용 장애 주입 이음새가 있다.

Phase 3에서 바뀌는 것은 `Table` 내부와 새 `server/` 모듈뿐이다. 그 위 계층이 바뀌어야 하면 이 설계가 틀린 것이니 고친다.

## 3. 모듈과 의존 방향

| 모듈 | 책임 | 등장 |
|---|---|---|
| `common` | 에러 타입, `Value`/`Row`, little-endian 바이트 읽기·쓰기, CRC32 | P1 |
| `storage` | `Schema`, `FrameCodec`, `FileIO`, `Table`, `Database` | P1 |
| `sql` | Lexer → Parser → AST. 저장소를 모른다 | P1 |
| `engine` | `Binder`, `Planner`, `Executor`, `RowSink` | P1 |
| `cli` | `minidb run / dump / serve`, 정렬 출력 sink, dump | P1 |
| `index` | `BPlusTree<Cap>`, 벌크 로더. I/O·락 없음 | P2 |
| `server` | ThreadPool, HTTP 어댑터, 요청 검증, NDJSON/JSON sink, fsync 스레드, 종료 | P3 |

```
cli ─▶ server ─▶ engine ─▶ sql
         │         └──────▶ storage ─▶ index
         └──────────────────────────▶ common   (모두 common에 의존)
```

- 위에서 아래로만 의존한다. CMake 정적 라이브러리 하나가 모듈 하나이고, `target_link_libraries`에 없는 모듈의 헤더는 쓰지 않는다.
- `server`가 `storage`를 직접 부르는 곳은 `Database::syncAll()`(fsync 스레드) 하나뿐이다.

## 4. 공통 규칙

- `[고정]` C++17, Linux(Docker 컨테이너). CMake + GoogleTest. 플래그 `-Wall -Wextra -Werror`. Debug는 ASan+UBSan, 벤치는 Release(-O2), P3부터 TSan.
- `[고정]` 외부 의존성은 GoogleTest와 cpp-httplib(P3)뿐. CRC32(IEEE, 반사 다항식 `0xEDB88320`)와 JSON 직렬화(출력만)는 직접 구현한다.
- `[고정]` 테이블 파일 I/O는 POSIX fd + `pread`/`pwrite`. `fstream` 금지 (읽기 위치를 공유해서 멀티스레드에서 안전하지 않음).
- `[고정]` 디스크 정수는 little-endian, 바이트 단위로 직렬화한다. 구조체 `memcpy` 금지.

공통 타입:

```cpp
struct SourcePos { uint32_t line, col; };          // 1부터. col은 바이트 단위

class DbError : public std::runtime_error { public: std::optional<SourcePos> pos; };
class ParseError   : public DbError {};
class SchemaError  : public DbError { public: enum class Kind { UnknownTable, Other } kind; };
class StorageError : public DbError {};

enum class ColumnType { Int, Varchar };
struct Column { std::string name; ColumnType type; uint16_t maxLen; };   // maxLen은 VARCHAR만

using Value = std::variant<int64_t, std::string>;
using Row   = std::vector<Value>;        // 엔진 안에서는 [0]이 항상 id
```

`SchemaError::Kind`는 P3에서 404(없는 테이블)와 400을 가르는 데 쓴다.

## 5. 저장소 `[P1]`

### 5.1 스키마 파일

`[고정]` `data/<table>.schema`: 텍스트, 한 줄에 컬럼 하나. `<name> INT` 또는 `<name> VARCHAR(<n>)`. 숨은 첫 컬럼 `id`(i64)는 적지 않는다.

세부 규칙:
- 이름은 `[A-Za-z_][A-Za-z0-9_]*`, 대소문자를 구분한다(SQL 식별자도 대소문자 구분). 타입 키워드는 대소문자 무시.
- 빈 줄과 앞뒤 공백은 무시한다. 주석은 없다.
- 컬럼 이름 `id`, 중복 이름, 컬럼 0개 → `SchemaError`.
- `VARCHAR(n)`의 n은 1..65535 **바이트**(UTF-8 글자 수가 아님).
- 최대 레코드 크기 `8 + Σ(INT 8 | VARCHAR 2+n)`가 `payload_len` 상한(64 KiB)을 넘으면 `SchemaError`. 그래서 실행 중에는 레코드가 상한을 넘을 수 없다.
- `Schema::columns()`는 숨은 `id`를 [0]에 넣어 돌려준다. 파일에는 없다.

### 5.2 테이블 파일 포맷 `[고정]`

```
data/<table>.tbl  [header 8B][frame]...
header  = magic "MDB1"(4) | version u16 = 1 | reserved u16 = 0
frame   = frame_len u32 | record_count u32 | record × record_count | crc32 u32
          frame_len = (record_count + records)의 바이트 길이
          crc32는 frame_len과 같은 바이트를 덮는다
record  = payload_len u32 | payload
payload = id i64 | 스키마 순서의 필드들
          INT → i64 ; VARCHAR → len u16 + UTF-8 바이트 (len ≤ n)
record offset = 레코드의 payload_len 위치 (프레임 안)
상한: payload_len ≤ 64 KiB, frame_len ≤ 4 MiB, record_count ≥ 1
```

- 프레임이 원자성의 단위다. 디스크에 온전히 있고 유효하거나, 버려진다.
- 프레임 최소 길이(`frame_len` 하한) = 4(record_count) + 4(payload_len) + 8(id) = 16.
- 이 포맷은 append-only이고 공개된 바이트는 다시 바뀌지 않는다. 그래서 락 밖에서 읽을 수 있다(§12).

### 5.3 열기와 복구

`Database::open(dataDir)`이 모든 `*.schema`를 읽고 테이블마다 `Table::open`을 부른다. 열린 뒤 테이블 목록은 바뀌지 않는다.

```
Table::open(<t>.tbl)
  ├ 파일 없음 → 만들기 → 헤더 8B 쓰기 → fsync(파일) → fsync(디렉터리)
  ├ 8B 미만 → 생성 중 끊긴 파일. 헤더를 다시 쓰고 fsync                   (#5)
  └ 복구 스캔 (헤더부터 프레임 단위)
       magic·version 불일치 → StorageError, 시작 거부                     [고정]
       프레임 반복:
         남은 바이트 < 8 (frame_len·record_count도 못 읽음)  → 꼬리 손상
         frame_len ∉ [16, 4 MiB] → 위치와 상관없이 중간 손상             (#2)
         선언된 끝 > 파일 끝                              → 꼬리 손상
         CRC 불일치: 선언된 끝 == 파일 끝 → 꼬리 손상, 아니면 중간 손상   [고정]
         레코드들이 frame_len을 정확히 채우지 않거나 개수 ≠ record_count → 중간 손상 [고정]
         레코드가 스키마로 디코드되지 않음 (필드 길이 불일치, VARCHAR len > n) → 중간 손상
         정상 레코드마다 onRecord(id, offset)
       꼬리 손상 → 잘라낼 바이트를 <t>.tbl.torn(있으면 .torn.N)에 복사·fsync
                 → ftruncate(프레임 시작) → fsync → 경고 로그 → 정상 시작      (#2)
       중간 손상 → StorageError, 시작 거부                                 [고정]
```

- `frame_len` 범위 검사 때문에 꼬리 손상으로 잘라내는 양은 언제나 프레임 하나(≤ 4 MiB + 12) 이하다. 그래서 `.torn` 복사 비용이 작다.
- `onRecord` 콜백: P1은 `maxId`를 갱신하고, P2는 id가 엄격히 오름차순인지 검사하며(아니면 `StorageError`) 벌크 로더에 넣는다.
- `[고정]` `nextId = 본 것 중 최대 id + 1` (빈 테이블은 1). 레코드 수 + 1이 아니다.
- `[고정]` 열린 뒤의 읽기는 이미 검증된 바이트를 믿는다. 읽을 때마다 CRC를 다시 계산하지 않는다.

### 5.4 쓰기 (insert)

```
Table::insert(rows)                        // id 없는 행 1..N, Binder가 검증을 끝낸 상태
  [P3: unique_lock]
  쓰기 불가 상태 → StorageError                                         (#4)
  nextId + N - 1 > INT64_MAX → StorageError                             [고정]
  id 부여 (nextId, nextId+1, ...) → FrameCodec::encode
  frame_len > 4 MiB → SchemaError("batch too large")   // P3 배치에서만 가능
  pwriteAll(off = fileSize_)                                            (#6)
    실패 → ftruncate(fileSize_) → StorageError     (fileSize_, nextId_ 그대로)   [고정]
           ftruncate도 실패 → 쓰기 불가 표시 → StorageError             (#4)
    성공 → fileSize_ += len → nextId_ += N → [P2: 인덱스에 id 전부 삽입]   [고정 순서]
  ← 부여된 id 목록
```

- **`O_APPEND`를 쓰지 않고 오프셋을 고정한다** (#6). 쓰기가 실패하고 자르기까지 실패했을 때, `O_APPEND`면 다음 프레임이 쓰레기 바이트 뒤에 붙어 재시작 때 중간 손상이 된다.
- 쓰기 불가 상태에서도 남은 바이트는 정상 프레임의 앞부분이다. 그래서 재시작하면 꼬리 손상으로 깔끔하게 정리된다.
- `pwriteAll`은 짧은 쓰기를 이어 쓴다. `EINTR`이면 재시도한다.
- 실패한 INSERT는 id를 소모하지 않는다. id는 재사용하지 않는다 `[고정]`.

### 5.5 읽기

- `Table::scan()`: 호출 시점의 `fileSize_`를 끝 지점으로 고정한 `Cursor`를 돌려준다 `[고정]`. `Cursor::next(Row&)`는 프레임 하나를 버퍼로 `pread`하고 레코드를 하나씩 디코드한다. 스캔 하나가 쓰는 메모리는 테이블 크기와 무관하게 프레임 버퍼 하나(≤ 4 MiB)다.
- `Table::findById(id)` `[P2]`: 인덱스에서 오프셋을 찾고 → `payload_len`(4B) `pread` → payload `pread` → 디코드.
- `pread` 실패는 `StorageError`다. 스트리밍 중이면 P3에서 마지막 줄 `"ok":false`가 된다.

### 5.6 내구성 `[고정]`

- 프레임마다 `write`. 프로세스 크래시에는 손실이 없다. 정전 시에는 마지막 fsync 이후 분량을 잃을 수 있다. 어느 경우든 프레임은 전부 남거나 전부 사라진다.
- CLI: SQL 파일 실행이 끝나면 쓴 테이블만 `fsync`하고 종료한다.
- 서버: 백그라운드 스레드가 1초마다 `fsync`. 실패하면 재시도하지 않고 로그를 남긴 뒤 서버를 멈춘다(fail-stop).
- 새 파일은 파일과 디렉터리 모두 `fsync`.

### 5.7 인터페이스

```cpp
class Schema {
public:
  static Schema load(const std::filesystem::path& file);          // SchemaError
  const std::vector<Column>& columns() const;                     // [0] = id
  std::optional<size_t> indexOf(std::string_view name) const;
};

class FileIO {                     // 장애 주입 이음새. 실패는 모두 StorageError
public:
  virtual ~FileIO() = default;
  virtual void   pwriteAll(int fd, const void* buf, size_t n, uint64_t off) = 0;
  virtual size_t pread(int fd, void* buf, size_t n, uint64_t off) = 0;
  virtual void   ftruncate(int fd, uint64_t size) = 0;
  virtual void   fsync(int fd) = 0;
};                                 // 운영 구현: PosixFileIO

namespace frame {                  // FrameCodec
  std::vector<uint8_t> encode(const Schema&, const std::vector<Row>& rowsWithId);
  Row decodeRecord(const Schema&, const uint8_t* payload, size_t len);   // 실패 → StorageError
}

class Table {
public:
  static std::unique_ptr<Table> open(std::filesystem::path tbl, Schema schema, FileIO& io);
  const Schema& schema() const;
  std::vector<int64_t> insert(const std::vector<std::vector<Value>>& rows);
  Cursor scan() const;
  std::optional<Row> findById(int64_t id) const;    // P2
  uint64_t rowCount() const;
  void sync();
  class Cursor { public: bool next(Row& out); };
};

class Database {
public:
  static Database open(const std::filesystem::path& dataDir, FileIO& io);
  Table& table(std::string_view name);              // 없으면 SchemaError{UnknownTable}
  void syncAll();                                    // 쓴 테이블만
};
```

## 6. SQL `[P1]`

### 6.1 문법

`[고정]` 범위: `INSERT INTO t [(cols)] VALUES (v, ...);` / `SELECT * | cols FROM t [WHERE col = v];`, P2부터 `EXPLAIN SELECT ...`.

```
script     := { statement? ';' } [ statement ]          -- 마지막 ';'는 생략 가능, 빈 문장(;;)은 건너뜀
statement  := insert | select | explain
insert     := INSERT INTO ident [ '(' ident { ',' ident } ')' ] VALUES '(' literal { ',' literal } ')'
select     := SELECT ( '*' | ident { ',' ident } ) FROM ident [ WHERE ident '=' literal ]
explain    := EXPLAIN select                              -- P2
literal    := integer | string
integer    := [ '-' ] digit { digit }                     -- int64 범위 밖이면 ParseError
string     := "'" { char | "''" } "'"                     -- '' 는 작은따옴표 하나
```

### 6.2 Lexer

- `[고정]` 키워드는 대소문자를 무시한다. 문장은 토큰으로 나눈다(`split(";")` 금지). 그래서 문자열 안의 `,`와 `;`는 구분자가 아니다.
- 키워드: `INSERT INTO VALUES SELECT FROM WHERE EXPLAIN`. 식별자는 대소문자를 구분한다.
- `--`부터 줄 끝까지는 주석이다(`.sql` 파일 편의).
- 문자열 리터럴은 UTF-8 유효성을 검사한다. 잘못된 바이트는 `ParseError`. P3 JSON 출력이 항상 유효하도록 입구에서 막는 것이다.
- 닫히지 않은 따옴표는 `ParseError`(시작 위치 line:col)이고, 회복할 수 없으므로 파일의 나머지 실행을 중단한다.
- 토큰: `{kind, text, pos}`. 위치는 1부터, col은 바이트 단위.

### 6.3 Parser와 AST

```cpp
struct Literal     { Value value; SourcePos pos; };
struct Condition   { std::string column; Literal value; SourcePos pos; };
struct InsertStmt  { std::string table; std::vector<std::string> columns; std::vector<Literal> values; SourcePos pos; };
struct SelectStmt  { std::string table; std::vector<std::string> columns; /* 비면 * */
                     std::optional<Condition> where; SourcePos pos; };
struct ExplainStmt { SelectStmt select; };                                  // P2
using Statement = std::variant<InsertStmt, SelectStmt /*, ExplainStmt (P2) */>;

class Parser {
public:
  explicit Parser(std::string_view source);
  std::optional<Statement> next();   // 문장 하나씩 당겨 온다. 끝이면 nullopt. ParseError
  void skipToNextStatement();        // 문장 에러 후 다음 ';' 뒤로
};
```

- 문장 단위로 당겨 온다. 파일 전체를 토큰 배열로 만들지 않는다. 100만 줄 INSERT 파일(약 50 MB)을 토큰 배열로 만들면 수백 MB가 된다.
- `ExplainStmt`를 variant에 추가하면 `std::visit` 쪽에서 컴파일 에러가 나므로, 처리를 빠뜨릴 수 없다.

## 7. 실행 엔진 `[P1·P2]`

### 7.1 Binder: 검증 규칙

AST를 테이블 포인터·컬럼 인덱스·검증된 값으로 바꾼다. 아래 규칙을 어기면 모두 `SchemaError`(위치 포함)다.

| 상황 | 결과 |
|---|---|
| 없는 테이블 | `SchemaError{UnknownTable}` |
| 없는 컬럼 (INSERT 목록, SELECT 목록, WHERE) | `SchemaError` |
| INSERT에 `id` 지정 `[고정]` | `SchemaError` |
| 컬럼 목록 없는 INSERT | 값 개수 = 스키마 컬럼 수(id 제외), 스키마 순서 |
| 컬럼 목록 있는 INSERT | 모든 컬럼을 정확히 한 번씩 (NULL·기본값이 없으므로). 순서는 자유. 빠짐·중복은 에러 |
| 값 개수 ≠ 컬럼 개수 | `SchemaError` |
| 타입 불일치 (INT에 문자열, VARCHAR에 정수, WHERE 양쪽) | `SchemaError` |
| VARCHAR 바이트 길이 > n | `SchemaError` |
| SELECT 목록 | `id` 포함 가능, 중복 허용, `*` = id + 모든 컬럼 |

### 7.2 Planner: 접근 경로

```cpp
struct FullScan  { std::optional<Predicate> filter; };   // Predicate = {컬럼 인덱스, Value}
struct IndexScan { int64_t id; };                         // P2
using AccessPath = std::variant<FullScan, IndexScan>;
AccessPath choose(const BoundSelect&);
```

- P1은 항상 `FullScan`이다. 이것이 불변식 6의 선택 지점이다.
- `[고정]` P2부터 WHERE가 `id = <int>`일 때만 `IndexScan`, 나머지는 `FullScan`.
- `[고정]` EXPLAIN 출력: `IndexScan(<table>, id=<v>)` | `FullScan(<table>)` | `FullScan(<table>, filter <col>=<v>)`. 문자열 값은 SQL 리터럴 형태(`'kim'`)로 적는다.

### 7.3 Executor와 RowSink

```cpp
class RowSink {
public:
  virtual ~RowSink() = default;
  virtual void begin(const std::vector<Column>& columns) = 0;   // 출력 컬럼
  virtual bool row(const Row& row) = 0;                          // false = 그만 (#3)
  virtual void end(uint64_t rowCount) = 0;                       // 정상 종료에만
};

class Executor {
public:
  explicit Executor(Database& db);
  std::vector<int64_t> insert(const std::vector<InsertStmt>& batch);  // 1..N, 한 테이블, 원자적
  void select(const SelectStmt& stmt, RowSink& sink);
  std::string explain(const SelectStmt& stmt);                        // P2
};
```

- `insert`: 모든 문장을 바인딩한 뒤(하나라도 실패하면 아무것도 쓰지 않음) `Table::insert` 한 번. 테이블이 둘 이상이면 `SchemaError`(P3에서 400).
- `select`: `begin` → 행마다 필터 → 투영 → `sink.row`. false면 멈추고 `end` 없이 반환한다. 스캔이 끝나면 `end(count)`. 예외는 그대로 위로 올라가고 `end`는 부르지 않는다.
- 저장소는 당기고(`Cursor::next`) 실행기는 민다(`sink.row`). Volcano 원형은 연산자 트리 전체가 pull이다. 여기서는 연산자가 scan → filter → project로 고정이라 트리 없이 루프 하나로 줄였다. WHERE가 확장되면 트리로 바꾼다(`refactoring.md`).
- IndexScan도 같은 sink 인터페이스로 0개 또는 1개 행을 민다 `[고정]`.

## 8. CLI `[P1]`

```
minidb run   [--data DIR] [--bail] <file.sql>
minidb dump  [--data DIR] <table>
minidb serve [--port 8080] [--workers N] [--queue 1024] [--data DIR]     (P3)
```

- `--data` 기본값은 `./data`. 컨테이너에서는 `/data` 볼륨을 쓴다(§15).
- **run**: 문장을 하나씩 실행한다.
  - INSERT 출력: `INSERT 1 (id=5)`.
  - SELECT 출력: 헤더 → 구분선 → 행 → `(N rows)`.
  - `[고정]` 컬럼 폭은 스키마에서 나온다(`max(이름 길이, VARCHAR n | INT 20)`). 그래서 버퍼링 없이 행이 나오는 즉시 출력한다. VARCHAR 폭은 최대 64로 자른다. `VARCHAR(65535)`를 65535칸으로 채우지 않기 위해서이고, 64바이트를 넘는 값은 정렬이 어긋난다.
  - 문자열 안의 제어 문자(`\n` `\t` 등)는 이스케이프해서 정렬을 지킨다. 한글처럼 여러 바이트인 글자는 폭이 어긋날 수 있다(바이트 기준 폭).
- **에러** `[고정]`: `<file>:<line>:<col>: <Type>: <message>`로 출력하고 다음 문장으로 간다. `--bail`이면 첫 에러에서 멈춘다.
- **종료 코드**: 0 = 모두 성공, 1 = 실패한 문장이 있음, 2 = 사용법 오류나 시작 실패(복구 거부 등).
- **dump**: `Table`을 거치지 않고 파일을 직접 읽는 읽기 전용 도구다. 자르지 않는다. 헤더를 출력한 뒤 프레임마다 `frame #k @off len=… records=… crc=ok|BAD`를, 레코드마다 `  @off id=… <필드들>`을 출력한다. 첫 비정상 프레임에서 꼬리 손상인지 중간 손상인지 표시하고 멈춘다. 손상 테스트와 시연에 쓴다.

## 9. 인덱스 `[P2]`

### 9.1 설계 규칙 `[고정]`

- 메모리 B+ 트리, `id(i64) → record offset(u64)`. 유일 키, 삭제 없음, 범위 질의 없음.
- 노드 용량은 컴파일 시간 템플릿 인자. 운영 128, 테스트는 3과 4.
- 노드 안에 고정 크기 배열을 두고, 키 배열과 값 배열을 나눈다(SoA). 노드 안 `std::vector` 금지.
- 리프 구조체와 내부 노드 구조체를 따로 두고, 공통 헤더에 `isLeaf`를 둔다. 가상 함수 없음.
- 리프: `keys[]`, `offsets[]`, `count`, `next`(소유하지 않는 원시 포인터). 내부 노드: `keys[]`, `children[]`(count+1개, `std::unique_ptr` 소유).
- 분할은 bottom-up. 내려가는 경로를 스택에 기록하고 부모 포인터는 두지 않는다. 런타임 분할 정책은 반반.
- 리프 분할은 copy-up(오른쪽 리프의 첫 키를 부모에 복사, 리프에도 남음). 내부 노드 분할은 push-up(가운데 키를 부모로 옮기고 양쪽에서 뺀다).
- 열 때 벌크 로딩으로 다시 만든다(파일 순서 = id 오름차순).

### 9.2 수치 (Cap = 용량)

| | 최대 키 | 최소 키 (루트 제외) | 분할 시 |
|---|---|---|---|
| 리프 | Cap | ⌊(Cap+1)/2⌋ | Cap+1개 → 왼쪽 ⌊(Cap+1)/2⌋, 나머지 오른쪽 |
| 내부 | Cap (자식 Cap+1) | ⌊Cap/2⌋ | Cap+1개 → 왼쪽 ⌊Cap/2⌋, 가운데 1개는 위로, 나머지 오른쪽 |

- 예: Cap=3이면 리프 4 → 2|2, 내부 4 → 1|↑|2. Cap=4이면 리프 5 → 2|3, 내부 5 → 2|↑|2.
- 분할 직전에 Cap+1개를 담을 곳이 필요하다. 노드 배열을 Cap+1 크기로 둘지, 임시 버퍼를 쓸지는 구현에서 정한다.

### 9.3 벌크 로딩

- 오름차순 (id, offset)을 스트리밍으로 받는다. 리프를 Cap개씩 꽉 채우고 `next`로 잇는다. 위 레벨은 각 자식의 최소 키를 경계값으로 같은 방식으로 쌓는다.
- 레벨의 마지막 노드가 최소 키 수보다 적으면 바로 앞 노드와 균등하게 나눈다.
- 순차 삽입(반반 분할)은 리프가 약 50%만 차지만, 재시작하면 거의 100%로 다시 채워진다. 1M 건 기준 리프 약 8K개다.

### 9.4 validate()가 확인하는 것 `[고정]`

1. 모든 노드 안에서 키가 엄격히 오름차순
2. 모든 리프의 깊이가 같음
3. 키 개수가 §9.2 범위 안 (루트는 최솟값 예외)
4. 모든 내부 경계값 = 오른쪽 서브트리의 최소 키
5. 리프 연결을 따라가면 모든 키가 오름차순이고, 개수가 삽입 개수와 같음

### 9.5 통합

- `Table`이 `BPlusTree<128> index_`를 가진다. 복구 스캔의 `onRecord`가 벌크 로더를 채운다. `insert`는 성공한 뒤 마지막 단계에서 프레임의 모든 id를 넣는다(§5.4 순서).
- `Planner`가 `IndexScan`을 고르고, 실행기는 `Table::findById`를 부른다.

## 10. 벤치마크 `[P2·P3]`

P2 (인덱스 vs 전체 스캔). 실행 파일 `minidb_bench`, Release 빌드, 고정 시드.
- **데이터 생성**: 엔진 API(`Table::insert`)로 1000행짜리 배치를 반복한다. 스키마는 `name VARCHAR(32), age INT`. 크기는 10K / 100K / 1M (+ 선택 5M).
- **측정 대상**:
  - (a) `SELECT * FROM t WHERE id = k`, Executor + 버리는 sink, 무작위 k 1000개.
  - (b) `SELECT * FROM t WHERE name = '<중간 값>'`, 전체 스캔.
  - (c) 열기 시간(복구 스캔 + 벌크 로드).
  - (d) SQL 경로를 따로 잼: INSERT 100만 줄 `.sql`을 `minidb run`으로 실행(파싱 + 프레임 1M개 + 끝 fsync).
- **방법**: 워밍업 1회. (a)는 조회별 중앙값과 p99, (b)는 5회 반복 중앙값. 페이지 캐시가 데워진 상태임을 명시한다. 데이터는 `/data` 볼륨(ext4)에 둔다.
- **보고**: `docs/bench.md` 표 + 원본 CSV(`bench/results/*.csv`). 환경(CPU, 커널, 빌드 플래그)을 같이 적는다.

P3 (동시성). 실행 파일 `minidb_bench_lock`과 `minidb_bench_lock_exclusive`.
- 같은 소스를 두 번 빌드해 락 방식만 바꾼다. 두 번째는 `MINIDB_EXCLUSIVE_LOCK`을 켜서 읽기도 `unique_lock`을 쓴다.
- HTTP를 빼고 `Table` API를 직접 불러 락 효과만 본다.
- 작업 부하 4종: 읽기 전용(findById), 읽기 90% + 단건 INSERT 10%, 단건 INSERT, 100행 배치 INSERT. 스레드 1·2·4·8개로 각 1.5초.
- 보고: `bench/results/lock.csv`, 해석은 `docs/bench.md` §5.
- 한 번에 실행: `bash bench/run_bench.sh` (P2 + P3 + SQL 경로 + 스트리밍 RSS).

## 11. API 서버 `[P3]`

### 11.1 구성 `[고정]`

- cpp-httplib의 기본 풀을 `new_task_queue`로 우리 `ThreadPool`로 바꾼다. 워커 수는 고정이고 큐 크기에 한계가 있다. 가득 차면 503.
- 기본값: 포트 8080, 워커 = `std::thread::hardware_concurrency()`, 큐 1024.
- 바디 최대 1 MiB(넘으면 413). 읽기·쓰기 타임아웃 5초. keep-alive 켬.
- 엔드포인트: `POST /query`(바디 = SQL 평문), `GET /health` → 200 `{"ok":true}`.

```cpp
class ThreadPool {                       // httplib::TaskQueue 구현
public:
  ThreadPool(size_t workers, size_t queueCap);
  bool enqueue(std::function<void()> fn) override;   // 가득 차면 false
  void shutdown() override;                          // 새 작업 거절 → 큐 비우기 → join
};
```

### 11.2 요청 형태 `[고정]`

- 허용하는 바디(마지막 `;`는 선택): `SELECT` 정확히 1개, `EXPLAIN SELECT` 정확히 1개, `INSERT` 1~1000개(배치, 전부 INSERT).
- 빈 바디, 종류가 섞인 요청, SELECT 여러 개, 1000개 초과 → 400, 아무것도 실행하지 않음.
- 배치는 테이블 하나만 대상으로 한다(여러 테이블이면 400).
- 분류는 `Parser::next()`로 전부 파싱한 뒤에 한다. 바디가 1 MiB 이하라 AST를 모두 들고 있어도 된다.

### 11.3 원자 배치 `[고정]`

1. 모든 문장을 파싱·검증한다. `ParseError`·`SchemaError`가 하나라도 나면 400, 아무것도 실행하지 않음.
2. 테이블 unique_lock을 잡고 시작 `fileSize`와 `nextId`를 기억한다.
3. 순서대로 id를 부여하고, 모든 행을 프레임 하나로 인코딩해 한 번에 쓴다.
4. 쓰기 실패 → 시작 크기로 `ftruncate`, `nextId` 복원, 락 해제 → 500. 아무것도 반영되지 않음.
5. 성공 → `fileSize` 갱신, `nextId` 증가, 모든 id를 인덱스에 삽입, 락 해제.

읽는 쪽은 배치의 일부만 보는 일이 없다. `fileSize`는 프레임 전체를 쓴 뒤에만 움직이므로, 모든 스캔의 끝 지점이 프레임 경계다. 이 흐름은 `Executor::insert` + `Table::insert`(§5.4)가 이미 하는 일이다. 서버가 따로 구현하지 않는다.

### 11.4 응답 `[고정]`

- 첫 바이트 전에 난 에러: 상태 코드 + `{"ok":false,"error":{"type":"...","message":"...","line":L,"col":C}}` (line/col은 있을 때만).
- 상태 코드 매핑: `ParseError` 400, `SchemaError` 404(없는 테이블)·400(그 외), `StorageError` 500, 큐 가득 503, 바디 초과 413.
- INSERT 배치 → 200 `{"ok":true,"insertedIds":[...]}` (커밋 후 전송).
- EXPLAIN → 200 `{"ok":true,"plan":"..."}`.
- SELECT → 200 `application/x-ndjson`, chunked, 한 줄에 JSON 값 하나:
  ```
  {"columns":["id",...]}
  [1,"kim",25]
  ...
  {"ok":true,"rowCount":N}                              ← 성공
  {"ok":false,"error":{"type":"...","message":"..."}}   ← 스트리밍 시작 후 실패
  ```
  클라이언트 규칙: 마지막 줄이 `"ok":true`여야 결과가 유효하다. 마지막 줄이 없으면 실패다.
- 행 수 상한 없음. 행은 나오는 대로 직렬화해 보내고, 메모리는 결과 크기와 무관하다.
- 느린 클라이언트: 쓰기 타임아웃(5초)이 나면 `DataSink::write`가 실패하고 → `NdjsonSink::row`가 false를 돌려주고 → 실행기가 멈추고 → 워커가 풀려난다.
- 직렬화는 `common/json_writer`가 한다(문자열 이스케이프, 정수). 입력 UTF-8은 Lexer가 이미 검증했다.
- NDJSON은 16 KiB까지 모았다가 chunk 하나로 쓴다 (결정 #13). 행마다 chunk를 쓰면 행마다 시스템 콜과 chunk 헤더가 붙는다. 버퍼 크기가 고정이라 메모리는 여전히 결과 크기와 무관하다(실측: `docs/bench.md` §4).
- 예외가 아닌 에러의 `type`: 요청 형태 위반 `RequestError`, 큐 가득 `Overloaded`, 바디 초과 `PayloadTooLarge`, 없는 경로 `NotFound`, 그 밖의 예외 `InternalError`.

### 11.5 내구성과 종료 `[고정]`

- 백그라운드 스레드가 1초마다 `Database::syncAll()`. 실패하면 로그를 남기고 서버를 멈춘다.
- SIGINT/SIGTERM: 핸들러는 `std::atomic<bool>`만 세운다. 메인 루프가 이 순서로 진행한다: 새 연결 거절(`svr.stop()`) → 큐와 처리 중인 작업 비우기(`ThreadPool::shutdown`, 스트림은 타임아웃으로 상한) → fsync → 닫기.
- `listen()`은 별도 스레드에서 돌고, 메인 스레드는 플래그를 100 ms마다 확인한다.

### 11.6 구현하며 정한 것

- **cpp-httplib는 Ubuntu apt 패키지(0.14.3)** (결정 #11). 헤더 전용이 아니라 `.so`로 컴파일된 판이라, .so를 빌드할 때 쓴 define(`CPPHTTPLIB_OPENSSL_SUPPORT` 등)을 pkg-config로 받아 똑같이 켠다. 이 define이 다르면 클래스 레이아웃이 어긋난다(ODR 위반).
- **503은 "거절 레인"으로 보낸다** (결정 #12).
  - 0.14.3의 `TaskQueue::enqueue`는 `void`라 거절할 방법이 없다. 신버전은 `bool`을 돌려주지만, 거절하면 응답 없이 소켓만 닫는다.
  - 그래서 `ThreadPool::submit`이 실패하면 그 연결을 작은 전용 스레드(거절 레인, 워커 1개, 큐 4096)로 넘긴다. 그 스레드에서는 thread_local 플래그가 켜져 있어서, pre-routing 핸들러가 요청 본문을 읽지 않고 바로 `503 + Connection: close`를 보낸다.
  - 거절 레인까지 가득 차면 accept 스레드에서 직접 503을 보낸다. 연결을 그냥 버리면 소켓이 새기 때문이다.
- **`TCP_NODELAY`를 켠다** (결정 #14). httplib는 응답을 헤더와 본문 두 번에 나눠 쓴다. 이때 Nagle 알고리즘과 상대의 지연 ACK가 맞물려 요청마다 약 40 ms가 붙었다. 켜기 전후로 동시성 테스트가 14 s → 7 s(서버만 켬) → 0.22 s(클라이언트도 켬)가 됐다.

## 12. 동시성 모델

P1·P2에는 락이 없다. P3에서 `Table` 안에 `std::shared_mutex` 하나를 추가한다 `[고정]`.

| 상태 | 쓰기 | 읽기 |
|---|---|---|
| 파일 바이트 | insert (unique) | scan·findById (락 밖에서 pread) |
| `fileSize_` | insert 성공 후 (unique) | scan 시작 시 한 번 (shared) |
| `nextId_`, 쓰기 불가 플래그 | insert (unique) | insert만 |
| `index_` | insert (unique) | findById는 트리 조회 동안만 (shared), 파일 읽기는 락 해제 후 |
| `Database` 테이블 목록 | 시작 시 한 번 | 이후 불변 → 락 없음 |

락 밖에서 파일을 읽어도 안전한 이유:
- **공개 순서** `[고정]`: 바이트 → `fileSize_` → `index_`. 읽는 쪽은 오프셋을 이 둘에서만 얻으므로, 아직 없는 바이트를 가리킬 수 없다.
- 실패 시 자르기는 공개된 `fileSize_` 뒤쪽만 건드린다. 그 바이트를 본 읽기 쪽은 없다.
- `pread`/`pwrite`는 위치를 공유하지 않으므로, 같은 fd를 여러 스레드가 써도 된다.
- DELETE가 없어서 공개된 바이트는 바뀌지 않는다. DELETE를 넣으면 이 전제가 깨진다.
- fsync 스레드는 `fsync(fd)`만 부른다. `pwrite`와 동시에 불러도 된다. fd는 fsync 스레드를 join한 뒤에 닫는다.

## 13. 에러 흐름

| 발생 | 예외 | CLI | 서버 |
|---|---|---|---|
| Lexer·Parser | `ParseError` | 출력 후 다음 문장. 닫히지 않은 따옴표는 파일 끝까지 중단 | 400 |
| Binder | `SchemaError` | 출력 후 다음 문장 | 404 / 400 |
| 시작 시 복구 | `StorageError` | 종료 코드 2 | 시작 거부 |
| 실행 중 I/O | `StorageError` | 출력 후 다음 문장 | 첫 바이트 전 500 / 스트리밍 중 `"ok":false` |

예외를 잡는 곳은 진입점(CLI 루프, 서버 핸들러) 두 곳뿐이다. 중간 계층은 잡아서 다시 던지지 않는다.

## 14. 테스트

### 14.1 이음새

- **`FaultyFileIO`**: N번째 `pwriteAll` 실패, 짧게 쓰고 실패, `ftruncate` 실패, N번째 `pread` 실패를 흉내 낸다.
- **"출력이 스캔이 끝나기 전에 시작된다"**: `pread` 횟수를 세는 `FileIO` + 첫 `row()`에서 그 횟수를 기록하는 sink. 첫 행 시점의 읽기 횟수가 전체 프레임 수보다 작으면 통과.
- **손상 파일**: 엔진으로 쓴 파일의 바이트를 직접 고친다(꼬리 자르기, CRC 뒤집기, record_count·frame_len 바꾸기). 바이트를 처음부터 만드는 생성기도 둔다.
- **B+tree**: `BPlusTree<3>`, `<4>`, `<128>`을 typed test로 돌리고, N번 연산마다 `validate()`.
- **서버**: httplib 클라이언트로 실제 포트에 요청한다. 동시성 테스트는 `tsan` 프리셋으로 돌린다.

### 14.2 완료 조건

| Phase | 조건 |
|---|---|
| P1 | Lexer: 문자열 안의 `,` `;`, `''`, 키워드 대소문자 섞기, 닫히지 않은 문자열(line:col) |
| P1 | 검증: 문법 오류, 값·컬럼 개수 불일치, 타입 불일치, VARCHAR 초과, id 지정 |
| P1 | 저장소: insert→select 왕복, 꼬리 손상 프레임 잘림(+ `.torn` 생성), 중간 손상 거부, magic 불일치 거부, record_count 불일치 거부, `frame_len` 범위 밖 거부, 8B 미만 파일 복구 |
| P1 | 원자 쓰기: 쓰기 실패를 주입해도 파일 크기·nextId 그대로. 자르기까지 실패하면 쓰기 불가 + 재시작 후 정상 |
| P1 | id: 재시작 후 이어짐, 실패한 INSERT 뒤 빈 번호 없음 |
| P1 | SELECT 출력이 스캔이 끝나기 전에 시작됨. `row()`가 false를 돌려주면 스캔이 멈춤 |
| P2 | 무작위 insert/find(용량 3, 4, 128), N번마다 `validate()`, 루트까지 연쇄 분할 |
| P2 | 벌크 로드 트리가 `validate()`를 통과하고, 삽입으로 만든 트리와 find 결과가 같음 |
| P2 | 100만 건 후 무작위 id 조회 결과 = 전체 스캔 결과, 없는 id는 빈 결과 |
| P2 | 여러 레코드짜리 프레임의 모든 id가 커밋 후와 재시작 후에 찾아짐 |
| P2 | 재시작 후 건수·nextId 복원, 빈 테이블, EXPLAIN이 `id =`엔 IndexScan, 나머지엔 FullScan |
| P2 | `docs/bench.md` (§10) |
| P3 | 스레드 N × INSERT M → 총 N×M건, id 유일, `validate()` 통과, 혼합 부하에서 TSan 경고 없음 |
| P3 | 배치: 잘못된 문장 하나 → 400 + 건수 그대로 / 쓰기 실패 주입 → 500 + 크기·nextId 그대로 / 배치 프레임 중간에서 잘린 파일 → 재시작 후 배치 전체가 사라짐 / K행 배치가 동시에 커밋되는 동안 모든 스캔의 행 수가 K의 배수 |
| P3 | 스트리밍: 100만 행 SELECT가 일정한 메모리로 끝남(최대 RSS 기록), 중간 읽기 실패 시 `"ok":false` 줄 |
| P3 | 요청 형태: 빈 바디·혼합·SELECT 여러 개·1000개 초과 → 400, 1 MiB 초과 → 413, 큐 가득 → 503, 없는 테이블 → 404, `/health` → 200 |
| P3 | SIGTERM을 부하 중에 보내도 처리 중이던 요청이 끝나고, 커밋된 행이 재시작 후 남아 있음 |
| P3 | 처리량 비교 `docs/bench.md` (§10) |

## 15. 빌드와 개발 환경

- **Docker 개발 컨테이너** (`docker/Dockerfile`, `compose.yaml`, 결정 #1): Ubuntu 24.04, gcc 13.3, CMake 3.28, Ninja 1.11, gdb, GoogleTest(apt, `find_package(GTest)`), `time`(최대 RSS 측정용).
  ```
  docker compose run --rm dev
  cmake --preset debug && cmake --build --preset debug && ctest --preset debug
  ```
- **저장 위치**: 소스(`/work`)는 바인드 마운트(9p)이고, 빌드 산출물(`/work/build`)과 테이블 데이터(`/data`)는 named volume(ext4)이다.
  - 9p는 느리고 fsync 의미가 다르다. 벤치와 내구성 테스트는 반드시 ext4에서 돌린다.
  - 덤으로 볼트에 산출물과 대용량 `.tbl`이 쌓이지 않는다.
- **프리셋**: `debug`(ASan+UBSan, `-O1`), `release`(-O2), `tsan`(RelWithDebInfo).
  - debug는 ASan 권장대로 `-O1`이다. `-O0`이면 100만 건 테스트가 2배 넘게 느리다.
  - debug 테스트 프리셋은 `MINIDB_SKIP_LARGE=1`로 100만 건 저장소 테스트를 건너뛴다. 테스트 자체는 19초인데, 종료 시 LeakSanitizer 검사가 60초를 더 쓴다. 같은 테스트는 tsan(4초)과 release에서 돈다.
- **TSan은 ASLR을 끄고 실행한다** (결정 #7). WSL2 커널 6.6은 `vm.mmap_rnd_bits=32`라서, 그대로 실행하면 TSan이 `unexpected memory mapping`으로 죽는다(10번 중 9번). `setarch $(uname -m) -R`을 쓰면 10번 중 10번 통과하고 일부러 넣은 레이스도 잡았다. `tsan` 프리셋이 ctest 실행기에 이 래퍼를 건다. 이 때문에 compose에 `seccomp:unconfined`가 있다.
- **커밋 전 검사**: ai-collab 훅은 Windows 호스트의 git에서 돈다. 컨테이너에는 git이 없다.

## 16. 디렉터리 구조

```
CMakeLists.txt  CMakePresets.json  compose.yaml  CLAUDE.md  CHANGELOG.md
docker/Dockerfile
src/
  common/   errors.h  value.h  bytes.h  crc32.{h,cpp}                    (P3: json_writer.*)
  storage/  schema.*  file_io.*  frame_codec.*  table.*  database.*
  sql/      token.h  lexer.*  ast.h  parser.*
  engine/   row_sink.h  binder.*  planner.*  executor.*
  index/    bplus_tree.h  bulk_loader.h                                  (P2)
  server/   thread_pool.*  http_server.*  query_handler.*  ndjson_sink.*  (P3)
  cli/      main.cpp  table_printer.*  dump.*
tests/      모듈별 *_test.cpp, testutil/ (FaultyFileIO, 손상 파일 생성기, 임시 디렉터리)
bench/      minidb_bench, results/                                       (P2)
docs/       architecture.md  decisions.md  decisions-summary.md  refactoring.md  explain-log.md  assignment.md  bench.md  archive/
```

## 17. 마일스톤

각 마일스톤이 끝나면 테스트가 통과해야 다음으로 간다. Phase 경계에서는 사람이 확인한다.

| # | 내용 | 완료 조건 (§14.2) |
|---|---|---|
| M1 | CMake·프리셋·`common`(바이트, CRC32, 에러) | CRC32 표준 벡터 |
| M2 | `FrameCodec` + `Schema` | 왕복, 스키마 에러 |
| M3 | `Table`: 열기·복구·insert·scan, `FileIO`·`FaultyFileIO` | P1 저장소·원자 쓰기·id |
| M4 | Lexer → Parser → AST | P1 Lexer·문법 |
| M5 | Binder·Planner·Executor·RowSink | P1 검증·스트리밍 |
| M6 | CLI `run`·`dump` | 종단 간 `.sql` 실행 |
| M7 | `BPlusTree` + `validate()` | P2 무작위 |
| M8 | 벌크 로드 | P2 벌크 |
| M9 | Table 통합, IndexScan, EXPLAIN | P2 나머지 |
| M10 | 벤치 → `docs/bench.md` | P2 벤치 |
| M11 | ThreadPool | 거절·정상 종료 |
| M12 | Table 락 + 배치 + TSan | P3 동시성·배치 |
| M13 | HTTP 계층·NDJSON | P3 요청·응답·스트리밍 |
| M14 | fsync 스레드·시그널·종료 | P3 종료 |
| M15 | 처리량 비교 | P3 벤치 |

## 18. 차별점

과제가 요구한 "다른 팀과의 차별점" 후보. 모두 설계에 이미 들어 있다.
- **크래시 일관성**: CRC 프레임 + 꼬리·중간 손상 판정 + `.torn` 보관. 장애 주입으로 테스트한다.
- **원자 배치**: 프레임 하나 = 커밋 하나. 동시 스캔이 배치의 일부만 보지 않음을 테스트로 증명한다.
- **스트리밍 결과**: CLI와 서버 모두 결과 크기와 무관한 메모리. 서버는 NDJSON으로 실패를 마지막 줄에 알린다.
- **관찰 도구**: `EXPLAIN`, `minidb dump`.
- **측정**: 인덱스 vs 스캔, 락 방식별 처리량, 최대 RSS.

## 19. 남은 위험

- **워커 하나가 연결 하나를 붙잡는다.** httplib는 연결 단위로 작업을 큐에 넣고, keep-alive 연결이면 그 연결이 끝날 때까지(최대 keep-alive 타임아웃 5초, 요청 100개) 같은 워커가 처리한다.
  - 그래서 동시에 열린 keep-alive 연결이 워커 수보다 많으면, 나머지 연결은 요청이 없어도 큐에서 기다린다.
  - 테스트에서 클라이언트 8개 · 워커 4개일 때, 뒤쪽 4개가 클라이언트 읽기 타임아웃(5초)에 걸려 실패했다.
  - 대책 후보: keep-alive 타임아웃 단축, 요청 단위 디스패치(이벤트 루프). `refactoring.md`에 있다.
- **거절 레인은 스레드 1개**다. 503을 받을 클라이언트가 요청을 아주 느리게 보내면 거절 레인이 읽기 타임아웃(5초)만큼 막히고, 그동안 다른 거절도 늦어진다.
- 순차 삽입 시 리프 충전율 50%: 1M 건이면 인덱스 메모리가 약 2배다. 재시작하면 회복된다. 맨 오른쪽 분할 특수 처리는 `refactoring.md`에 있다.
- CLI 출력 폭이 바이트 기준이라 여러 바이트 글자는 정렬이 어긋난다. 의도한 단순화다.
