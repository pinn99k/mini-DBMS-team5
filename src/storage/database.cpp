#include "storage/database.h"

#include <algorithm>

#include "common/errors.h"

namespace minidb {

std::unique_ptr<Database> Database::open(const std::filesystem::path& dataDir, FileIO& io) {
  if (!std::filesystem::is_directory(dataDir)) {
    throw StorageError("data directory not found: " + dataDir.string());
  }
  std::vector<std::filesystem::path> schemas;
  for (const auto& entry : std::filesystem::directory_iterator(dataDir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".schema") schemas.push_back(entry.path());
  }
  std::sort(schemas.begin(), schemas.end());

  auto db = std::unique_ptr<Database>(new Database());
  for (const auto& schemaPath : schemas) {
    std::string name = schemaPath.stem().string();
    if (!isIdentifier(name)) throw SchemaError("invalid table name '" + name + "' (from " + schemaPath.string() + ")");
    Schema schema = Schema::load(schemaPath);
    auto tblPath = schemaPath;
    tblPath.replace_extension(".tbl");
    db->tables_.emplace(name, Table::open(tblPath, name, std::move(schema), io));
  }
  return db;
}

Table* Database::find(std::string_view name) const {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : it->second.get();
}

Table& Database::table(std::string_view name) const {
  if (Table* t = find(name)) return *t;
  throw SchemaError("unknown table '" + std::string(name) + "'", std::nullopt, SchemaError::Kind::UnknownTable);
}

std::vector<std::string> Database::tableNames() const {
  std::vector<std::string> out;
  for (const auto& [name, _] : tables_) out.push_back(name);
  return out;
}

void Database::syncAll() {
  for (auto& [_, t] : tables_) t->sync();
}

}  // namespace minidb
