# mini-dbms

C++17로 직접 만드는 미니 DBMS. 크래프톤 정글 미니 프로젝트.

## 목차
1. 무엇을 만드나
2. 진행 상황
3. 빌드와 실행
4. 문서

## 1. 무엇을 만드나

- **Phase 1**: SQL 처리기. `.sql` 파일의 `INSERT`/`SELECT`를 CLI로 실행하고, CRC32 프레임 단위의 append-only 바이너리 파일에 저장한다.
- **Phase 2**: 메모리 B+ 트리 인덱스(`id → 레코드 오프셋`), `EXPLAIN`, 100만 건 인덱스 조회 vs 전체 스캔 비교.
- **Phase 3**: HTTP API 서버. 직접 만든 스레드 풀, 원자적 INSERT 배치, NDJSON 스트리밍 SELECT.

설계의 중심은 크래시 일관성(프레임이 전부 남거나 전부 사라짐), 행 단위 스트리밍(결과 크기와 무관한 메모리), 락이 `Table` 안에만 있는 구조다.

## 2. 진행 상황

MVP 완료 (0.1.0, 2026-11-16). Phase 1~3 전부 구현, ASan·TSan 테스트 통과. 측정 결과는 `docs/bench.md`, 변경 내역은 `CHANGELOG.md`.

## 3. 빌드와 실행

Docker 개발 컨테이너에서 빌드한다.

```bash
docker compose run --rm dev
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

컨테이너 안에서 써 보기:

```bash
mkdir -p /data/demo && printf 'name VARCHAR(16)\nage INT\n' > /data/demo/users.schema
echo "INSERT INTO users VALUES ('kim', 25); SELECT * FROM users;" > /tmp/a.sql
./build/debug/minidb run --data /data/demo /tmp/a.sql
./build/debug/minidb serve --data /data/demo --port 8080 &
curl -s -X POST --data-binary "SELECT * FROM users WHERE id = 1" http://127.0.0.1:8080/query
```

## 4. 문서

- `docs/guide.md`: **처음 읽을 문서.** 동작 원리를 순서대로 따라가는 가이드
- `docs/architecture.md`: 설계 (단일 문서, 규칙 참조용)
- `docs/decisions.md`: 결정 로그
- `docs/assignment.md`: 과제 원문
- `docs/bench.md`: 벤치마크 결과
- `docs/refactoring.md`: 리팩터링 후보
- `docs/retro/`: 회고 (진행하며 겪은 문제·원인·해결)
