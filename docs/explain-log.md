# explain-log

이해 검사 대기 목록. 형식: `날짜 | 대상 | 대기·통과·보완·건너뜀 | 메모`
대상 코드가 생긴 뒤(구현 후) 예측 질문으로 확인한다. 근거: `docs/architecture.md`.

## 위임 (D1~D4, "추천대로")
- 2026-11-16 | D1 `frame_len` 범위 검사 + `.torn` 보관 — 왜 범위 검사만으로 복사량이 프레임 하나 이하가 되나 (위임) | 대기 |
- 2026-11-16 | D2 `RowSink::row()`의 `bool` — false가 오면 실행기가 어디서 멈추고, `end()`는 호출되나 (위임) | 대기 |
- 2026-11-16 | D3 자르기 실패 시 테이블 쓰기 불가 — 남은 바이트가 재시작 때 왜 꼬리 손상으로 정리되나 (위임) | 대기 |
- 2026-11-16 | D4 8B 미만 파일 헤더 재작성 — 왜 그 크기에는 프레임이 있을 수 없나 (위임) | 대기 |

## 알려주기 (AI가 정한 작동 원리)
- 2026-11-16 | 공개 순서(바이트 → fileSize → index) — 락 밖에서 pread해도 안전한 이유 | 대기 |
- 2026-11-16 | `pwrite(off = fileSize)` 고정, `O_APPEND` 금지 — 쓰기·자르기가 연달아 실패하면 둘의 결과가 어떻게 달라지나 | 대기 |
- 2026-11-16 | 저장소 pull(`Cursor::next`) / 실행기 push(`sink.row`) — 연산자 트리 없이 루프 하나로 충분한 조건 | 대기 |
- 2026-11-16 | Parser 문장 단위 pull — 토큰 배열 전체를 만들지 않는 이유 | 대기 |
- 2026-11-16 | 시작 시 모든 테이블 열기 — 테이블 목록에 락이 필요 없어지는 이유와 대가 | 대기 |
- 2026-11-16 | 레코드 크기 상한을 스키마 로딩 때 검사 — 실행 중 `payload_len` 초과가 왜 불가능해지나 (AI, #10) | 대기 |
- 2026-11-16 | 문자열 UTF-8 검증을 Lexer에서 — P3 JSON 출력이 항상 유효해지는 이유 (AI, #10) | 대기 |

## 구현 후 (MVP)
- 2026-11-16 | `src/index/bplus_tree.h` NodeDeleter — 가상 소멸자 없이 `unique_ptr<Node>`로 리프·내부 노드를 지우면 무엇이 깨지고, deleter가 어떻게 막나 | 대기 |
- 2026-11-16 | `src/index/bplus_tree.h` Loader::finish — 마지막 리프·마지막 그룹을 균등 분할해도 최소 키 수가 보장되는 이유 | 대기 |
- 2026-11-16 | `src/server/http_server.cpp` 거절 레인 — `enqueue`가 void인데 어떻게 503을 보내고, 왜 연결을 그냥 버리면 안 되나 (AI, #12) | 대기 |
- 2026-11-16 | keep-alive 연결 = 워커 하나 — 워커 4개·클라이언트 8개면 무엇이 몇 초 기다리나 (architecture §19) | 대기 |
- 2026-11-16 | `TCP_NODELAY` — 응답을 두 번에 나눠 쓰면 왜 요청마다 40 ms가 붙나 (AI, #14) | 대기 |
- 2026-11-16 | `docs/bench.md` §5 — 락 밖에서 pread하면 shared_mutex가 이득이 없어지는 이유 | 대기 |

## 락 분리 (#19)
- 2026-11-17 | `src/storage/table.cpp` `Table::insert` — `pwrite`를 `mu_` 밖에서 해도 읽는 쪽이 반쯤 쓴 프레임을 못 보는 이유 | 대기 |
- 2026-11-17 | `src/storage/table.cpp` `Table::insert` — 쓰는 쪽이 `writeMu_`만 쥐고 `maxId_`·`fileSize_`를 읽어도 데이터 레이스가 아닌 이유 | 대기 |

## UPDATE·DELETE (#20~#24)
- 2026-11-17 | D5 로그 추가 + 종류 추론(#20) — 8 B payload가 왜 묘비로만 해석되고, 최대 id를 지우고 재시작해도 그 id가 왜 다시 안 쓰이나 (위임) | 대기 |
- 2026-11-17 | D6 id 순서 스캔 + maxId 상한(#21) — 파일 순서로 돌면 스캔 중 UPDATE된 행이 왜 빠지나, id 순서에서는 어떤 이상이 남나 (위임) | 대기 |
- 2026-11-17 | D7 B+tree 정석 삭제(#22) — 병합한 노드가 왜 용량을 넘지 않고, 지운 키가 경계값에 남는 경우를 어떻게 고치나 (위임) | 대기 |
- 2026-11-17 | D8 INSERT와 섞지 않기·`affectedRows` 배열(#23) — 섞으면 막 넣은 행을 지울 때 무엇을 정해야 하나 (위임) | 대기 |
- 2026-11-17 | `Table::modify` 대상 찾기를 `writeMu_` 안에서(#24) — 밖에서 찾으면 두 UPDATE가 어떻게 서로를 덮나 (AI) | 대기 |
- 2026-11-17 | 락 순서 `writeMu_` → `mu_` — `modify`가 커서를 돌릴 때 교착이 안 나는 이유 (AI) | 대기 |
- 2026-11-17 | 경보(M16) `src/index/bplus_tree.h` `rebalanceInternal` CC 13·`rebalanceLeaf`와 중복 블록 — 왼쪽 빌리기/오른쪽 빌리기/병합 3갈래가 리프·내부에 각각 있음(§9.6 그대로). 리프는 키·값만 옮기고 내부는 자식까지 옮겨 하나로 합치면 분기가 섞임. 왼쪽 회전에서 부모 경계값과 형제 첫 키가 각각 어디로 가나 | 대기 |
- 2026-11-18 | 경보(M17) `src/storage/table.cpp` `Table::modify` CC 22 → `stageEdits`(찾기·적용, 읽기만)·`commitPending`(쓰기·공개)로 나눔(§5.4b 두 단계). `stageEdits`가 실패하면 왜 파일·인덱스가 전혀 안 바뀌나 | 대기 |
- 2026-11-18 | 경보(M18) `src/sql/parser.cpp` `Parser::next` CC 11 — 문장 종류(INSERT·SELECT·EXPLAIN·UPDATE·DELETE) 키워드 분기 하나 증가. 종류가 더 늘면 첫 키워드 → 파서 함수 표로. 문장 끝의 `;`·End 검사는 왜 종류와 상관없이 한 곳에 있나 | 대기 |
- 2026-11-18 | 경보(M19) `src/server/query_handler.cpp` `QueryHandler::handle` CC 12 — 요청 분류(전부 INSERT / 전부 UPDATE·DELETE / SELECT·EXPLAIN 1개)가 늘어남. 분류를 따로 떼는 건 요청 종류가 더 늘 때. `UPDATE ...; SELECT ...`가 왜 400이고 아무것도 실행되지 않나 | 대기 |
