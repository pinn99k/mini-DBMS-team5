#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/errors.h"
#include "common/value.h"

namespace minidb {

struct Literal {
  Value value;
  SourcePos pos;
};

struct Condition {
  std::string column;
  SourcePos columnPos;
  Literal value;
};

struct InsertStmt {
  std::string table;
  SourcePos tablePos;
  std::vector<std::string> columns;  // 비면 스키마 순서
  std::vector<SourcePos> columnPos;
  std::vector<Literal> values;
  SourcePos pos;
};

struct SelectStmt {
  std::string table;
  SourcePos tablePos;
  std::vector<std::string> columns;  // 비면 *
  std::vector<SourcePos> columnPos;
  std::optional<Condition> where;
  SourcePos pos;
};

struct ExplainStmt {
  SelectStmt select;
  SourcePos pos;
};

struct Assignment {
  std::string column;
  SourcePos columnPos;
  Literal value;
};

struct UpdateStmt {
  std::string table;
  SourcePos tablePos;
  std::vector<Assignment> set;
  std::optional<Condition> where;  // 없으면 모든 행
  SourcePos pos;
};

struct DeleteStmt {
  std::string table;
  SourcePos tablePos;
  std::optional<Condition> where;  // 없으면 모든 행
  SourcePos pos;
};

using Statement = std::variant<InsertStmt, SelectStmt, ExplainStmt, UpdateStmt, DeleteStmt>;

inline SourcePos positionOf(const Statement& s) {
  return std::visit([](const auto& st) { return st.pos; }, s);
}

}  // namespace minidb
