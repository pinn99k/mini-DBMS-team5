#pragma once

// AST → 테이블 포인터·컬럼 인덱스·검증된 값. 규칙은 architecture §7.1. 위반은 SchemaError.

#include <cstddef>
#include <optional>
#include <vector>

#include "common/value.h"
#include "sql/ast.h"
#include "storage/database.h"

namespace minidb {

struct BoundInsert {
  Table* table = nullptr;
  std::vector<Value> values;  // 스키마 순서, id 제외
};

struct BoundSelect {
  Table* table = nullptr;
  std::vector<size_t> projection;  // 출력 컬럼 → 스키마 컬럼 인덱스
  std::optional<Predicate> filter;
};

// UPDATE·DELETE (architecture §7.1). remove면 set은 비어 있다.
struct BoundModify {
  Table* table = nullptr;
  std::optional<Predicate> filter;
  std::vector<std::pair<size_t, Value>> set;
  bool remove = false;
};

BoundInsert bindInsert(const Database& db, const InsertStmt& stmt);
BoundSelect bindSelect(const Database& db, const SelectStmt& stmt);
BoundModify bindUpdate(const Database& db, const UpdateStmt& stmt);
BoundModify bindDelete(const Database& db, const DeleteStmt& stmt);

}  // namespace minidb
