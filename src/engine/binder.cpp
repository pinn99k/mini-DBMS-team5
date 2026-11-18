#include "engine/binder.h"

#include <string>

#include "common/errors.h"

namespace minidb {
namespace {

Table& lookup(const Database& db, const std::string& name, SourcePos pos) {
  Table* t = db.find(name);
  if (!t) throw SchemaError("unknown table '" + name + "'", pos, SchemaError::Kind::UnknownTable);
  return *t;
}

const char* typeName(const Column& c) { return c.type == ColumnType::Int ? "INT" : "VARCHAR"; }

Value checkValue(const Column& col, const Literal& lit) {
  const bool isInt = std::holds_alternative<int64_t>(lit.value);
  if (col.type == ColumnType::Int && !isInt) {
    throw SchemaError("type mismatch: column '" + col.name + "' is INT, got a string", lit.pos);
  }
  if (col.type == ColumnType::Varchar) {
    if (isInt) {
      throw SchemaError("type mismatch: column '" + col.name + "' is VARCHAR(" + std::to_string(col.maxLen) +
                            "), got an integer",
                        lit.pos);
    }
    const auto& s = std::get<std::string>(lit.value);
    if (s.size() > col.maxLen) {
      throw SchemaError("value too long for column '" + col.name + "' VARCHAR(" + std::to_string(col.maxLen) +
                            "): " + std::to_string(s.size()) + " bytes",
                        lit.pos);
    }
  }
  return lit.value;
}

size_t resolveColumn(const Table& t, const std::string& name, SourcePos pos) {
  auto idx = t.schema().indexOf(name);
  if (!idx) throw SchemaError("unknown column '" + name + "' in table '" + t.name() + "'", pos);
  return *idx;
}

// WHERE col = literal → Predicate. 타입이 다르면 SchemaError.
std::optional<Predicate> bindWhere(const Table& t, const std::optional<Condition>& where) {
  if (!where) return std::nullopt;
  const size_t idx = resolveColumn(t, where->column, where->columnPos);
  const Column& col = t.schema().columns()[idx];
  const bool isInt = std::holds_alternative<int64_t>(where->value.value);
  if ((col.type == ColumnType::Int) != isInt) {
    throw SchemaError(std::string("type mismatch in WHERE: column '") + col.name + "' is " + typeName(col) +
                          ", got " + (isInt ? "an integer" : "a string"),
                      where->value.pos);
  }
  return Predicate{idx, where->value.value};
}

}  // namespace

BoundInsert bindInsert(const Database& db, const InsertStmt& s) {
  Table& t = lookup(db, s.table, s.tablePos);
  const auto& cols = t.schema().columns();
  const size_t userCols = cols.size() - 1;
  BoundInsert out{&t, std::vector<Value>(userCols)};

  if (s.columns.empty()) {
    if (s.values.size() != userCols) {
      throw SchemaError("table '" + t.name() + "' has " + std::to_string(userCols) + " columns, got " +
                            std::to_string(s.values.size()) + " values",
                        s.values.empty() ? s.pos : s.values.front().pos);
    }
    for (size_t i = 0; i < userCols; ++i) out.values[i] = checkValue(cols[i + 1], s.values[i]);
    return out;
  }

  if (s.columns.size() != s.values.size()) {
    throw SchemaError(std::to_string(s.columns.size()) + " columns listed but " + std::to_string(s.values.size()) +
                          " values given",
                      s.pos);
  }
  std::vector<bool> seen(userCols, false);
  for (size_t j = 0; j < s.columns.size(); ++j) {
    if (s.columns[j] == "id") throw SchemaError("column 'id' is assigned by the engine", s.columnPos[j]);
    const size_t idx = resolveColumn(t, s.columns[j], s.columnPos[j]);
    if (seen[idx - 1]) throw SchemaError("column '" + s.columns[j] + "' listed twice", s.columnPos[j]);
    seen[idx - 1] = true;
    out.values[idx - 1] = checkValue(cols[idx], s.values[j]);
  }
  for (size_t i = 0; i < userCols; ++i) {
    if (!seen[i]) {
      throw SchemaError("missing value for column '" + cols[i + 1].name + "' (no NULL or default values)", s.pos);
    }
  }
  return out;
}

BoundSelect bindSelect(const Database& db, const SelectStmt& s) {
  Table& t = lookup(db, s.table, s.tablePos);
  BoundSelect out;
  out.table = &t;
  if (s.columns.empty()) {
    for (size_t i = 0; i < t.schema().columns().size(); ++i) out.projection.push_back(i);
  } else {
    for (size_t j = 0; j < s.columns.size(); ++j) out.projection.push_back(resolveColumn(t, s.columns[j], s.columnPos[j]));
  }
  out.filter = bindWhere(t, s.where);
  return out;
}

BoundModify bindUpdate(const Database& db, const UpdateStmt& s) {
  Table& t = lookup(db, s.table, s.tablePos);
  BoundModify out;
  out.table = &t;
  out.filter = bindWhere(t, s.where);
  std::vector<bool> seen(t.schema().columns().size(), false);
  for (const Assignment& a : s.set) {
    if (a.column == "id") throw SchemaError("column 'id' cannot be updated", a.columnPos);
    const size_t idx = resolveColumn(t, a.column, a.columnPos);
    if (seen[idx]) throw SchemaError("column '" + a.column + "' assigned twice", a.columnPos);
    seen[idx] = true;
    out.set.emplace_back(idx, checkValue(t.schema().columns()[idx], a.value));
  }
  return out;
}

BoundModify bindDelete(const Database& db, const DeleteStmt& s) {
  Table& t = lookup(db, s.table, s.tablePos);
  BoundModify out;
  out.table = &t;
  out.filter = bindWhere(t, s.where);
  out.remove = true;
  return out;
}

}  // namespace minidb
