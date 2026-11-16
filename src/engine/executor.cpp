#include "engine/executor.h"

#include <stdexcept>

#include "common/errors.h"

namespace minidb {
namespace {

Row project(const Row& row, const std::vector<size_t>& projection) {
  Row out;
  out.reserve(projection.size());
  for (size_t idx : projection) out.push_back(row[idx]);
  return out;
}

}  // namespace

std::vector<int64_t> Executor::insert(const std::vector<InsertStmt>& batch) const {
  if (batch.empty()) throw std::invalid_argument("insert: empty batch");
  std::vector<std::vector<Value>> rows;
  rows.reserve(batch.size());
  Table* table = nullptr;
  for (const InsertStmt& stmt : batch) {
    BoundInsert b = bindInsert(db_, stmt);  // 하나라도 실패하면 아무것도 쓰지 않는다
    if (table && b.table != table) throw SchemaError("a batch must target a single table", stmt.tablePos);
    table = b.table;
    rows.push_back(std::move(b.values));
  }
  return table->insert(rows);
}

PreparedSelect Executor::prepare(const SelectStmt& stmt) const {
  PreparedSelect p{bindSelect(db_, stmt), FullScan{}, {}};
  p.path = choosePath(p.bound);
  const auto& cols = p.bound.table->schema().columns();
  for (size_t idx : p.bound.projection) p.columns.push_back(cols[idx]);
  return p;
}

uint64_t Executor::run(const PreparedSelect& p, RowSink& sink) const {
  sink.begin(p.columns);
  uint64_t count = 0;

  if (const auto* ix = std::get_if<IndexScan>(&p.path)) {
    if (auto row = p.bound.table->findById(ix->id)) {
      if (!sink.row(project(*row, p.bound.projection))) return 0;
      count = 1;
    }
    sink.end(count);
    return count;
  }

  const auto& full = std::get<FullScan>(p.path);
  Table::Cursor cursor = p.bound.table->scan();
  Row row;
  while (cursor.next(row)) {
    if (full.filter && row[full.filter->column] != full.filter->value) continue;
    if (!sink.row(project(row, p.bound.projection))) return count;  // 받는 쪽이 멈춤
    ++count;
  }
  sink.end(count);
  return count;
}

std::string Executor::explain(const SelectStmt& stmt) const {
  PreparedSelect p = prepare(stmt);
  return describePath(p.path, p.bound);
}

}  // namespace minidb
