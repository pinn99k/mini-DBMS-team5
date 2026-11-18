#pragma once

// 실행기. 저장소에서 행을 당겨(Cursor::next) sink로 민다(RowSink::row).
// 연산자가 scan → filter → project로 고정이라 연산자 트리 없이 루프 하나다 (architecture §7.3).
// 상태가 없으므로 여러 스레드가 하나를 같이 써도 된다.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/binder.h"
#include "engine/planner.h"
#include "engine/row_sink.h"
#include "sql/ast.h"
#include "storage/database.h"

namespace minidb {

struct PreparedSelect {
  BoundSelect bound;
  AccessPath path;
  std::vector<Column> columns;  // 출력 컬럼
};

class Executor {
 public:
  explicit Executor(const Database& db) : db_(db) {}

  // 1..N개, 한 테이블. 전부 검증한 뒤 프레임 하나로 커밋 (원자적).
  std::vector<int64_t> insert(const std::vector<InsertStmt>& batch) const;

  // prepare에서 바인딩·계획 에러가 모두 난다. 서버는 응답 첫 바이트 전에 prepare를 부른다.
  PreparedSelect prepare(const SelectStmt& stmt) const;
  uint64_t run(const PreparedSelect& prepared, RowSink& sink) const;  // 보낸 행 수
  uint64_t select(const SelectStmt& stmt, RowSink& sink) const { return run(prepare(stmt), sink); }

  std::string explain(const SelectStmt& stmt) const;

  // UPDATE·DELETE 1..N개, 한 테이블. 전부 바인딩한 뒤 Table::modify 한 번 (원자적). 문장별 영향 행 수.
  // UpdateStmt·DeleteStmt가 아닌 문장이 섞이면 std::invalid_argument (호출자가 분류함).
  std::vector<uint64_t> modify(const std::vector<Statement>& batch) const;

 private:
  const Database& db_;
};

}  // namespace minidb
