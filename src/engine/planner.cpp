#include "engine/planner.h"

namespace minidb {

AccessPath choosePath(const BoundSelect& select) {
  if (select.filter && select.filter->column == 0) {
    return IndexScan{std::get<int64_t>(select.filter->value)};
  }
  return FullScan{select.filter};
}

std::string sqlLiteral(const Value& v) {
  if (const auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  std::string out = "'";
  for (char c : std::get<std::string>(v)) {
    out += c;
    if (c == '\'') out += '\'';
  }
  return out + "'";
}

std::string describePath(const AccessPath& path, const BoundSelect& select) {
  const std::string& table = select.table->name();
  if (const auto* ix = std::get_if<IndexScan>(&path)) {
    return "IndexScan(" + table + ", id=" + std::to_string(ix->id) + ")";
  }
  const auto& full = std::get<FullScan>(path);
  if (!full.filter) return "FullScan(" + table + ")";
  const std::string& col = select.table->schema().columns()[full.filter->column].name;
  return "FullScan(" + table + ", filter " + col + "=" + sqlLiteral(full.filter->value) + ")";
}

}  // namespace minidb
