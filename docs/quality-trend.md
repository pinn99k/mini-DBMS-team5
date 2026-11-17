# 품질 추세

단계가 끝날 때 한 줄씩 덧붙인다 (phase-audit). 수치는 경보로만 쓴다. 절대값보다 추세를 본다.

- LOC: `src/`의 `.cpp`·`.h` 전체 줄 수
- 장황성: 중복 줄 / 전체 줄 (jscpd)
- 침식도: CC 10 초과 함수의 mass 합 / 전체 mass 합, mass = CC × √NLOC (lizard)
- 참고값(외부 글, 미검증): 사람이 관리한 저장소 장황성 약 0.15, 침식도 약 0.31. AI 생성 코드는 약 2배.

| 날짜 | 단계 | LOC | 장황성 | 침식도 | 메모 |
|---|---|---|---|---|---|
| 2026-11-17 | MVP (P1~P3) 완료 | 3,272 | 0.003 | 0.56 | 상위: `Lexer::next` CC 39, `Table::recover`·`dumpTable` CC 19(같은 판정 규칙을 따로 구현), `QueryHandler::handle` CC 18 |
| 2026-11-17 | 리팩터링 1차 | 3,322 | 0.003 (재측정 안 함, 중복 블록 변경 없음) | 0.38 | 판정 공용 스캐너(#18), `Lexer::next`·`QueryHandler::handle` 분리. 남은 상위: `Schema::parse` CC 22, `main` CC 20, `scanFrames` CC 19 |
