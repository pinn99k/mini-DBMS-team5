#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "storage/file_io.h"
#include "storage/table.h"

namespace minidb {

// 시작할 때 dataDir의 *.schema를 전부 연다. 열린 뒤 테이블 목록은 바뀌지 않으므로 락이 없다.
class Database {
 public:
  static std::unique_ptr<Database> open(const std::filesystem::path& dataDir, FileIO& io = defaultFileIO());

  Table& table(std::string_view name) const;  // 없으면 SchemaError{UnknownTable}
  Table* find(std::string_view name) const;
  std::vector<std::string> tableNames() const;
  void syncAll();

 private:
  std::map<std::string, std::unique_ptr<Table>, std::less<>> tables_;
};

}  // namespace minidb
