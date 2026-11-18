#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace minidb {

enum class ColumnType { Int, Varchar };

struct Column {
  std::string name;
  ColumnType type = ColumnType::Int;
  uint16_t maxLen = 0;  // VARCHAR(n)의 n (바이트). INT는 0
};

using Value = std::variant<int64_t, std::string>;
using Row = std::vector<Value>;  // 엔진 안에서는 [0]이 항상 id

// WHERE col = value. Binder가 만들고 Planner(SELECT)와 Table::modify(UPDATE·DELETE)가 쓴다.
struct Predicate {
  size_t column = 0;  // 스키마 컬럼 인덱스 (0 = id)
  Value value;
};

}  // namespace minidb
