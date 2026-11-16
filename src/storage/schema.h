#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/value.h"

namespace minidb {

// data/<table>.schema: 한 줄에 "<name> INT" | "<name> VARCHAR(<n>)". 숨은 id는 적지 않는다.
class Schema {
 public:
  static Schema load(const std::filesystem::path& file);                // SchemaError
  static Schema parse(std::string_view text, const std::string& source);  // SchemaError

  const std::vector<Column>& columns() const { return columns_; }  // [0] = 숨은 id
  std::optional<size_t> indexOf(std::string_view name) const;
  size_t maxPayloadSize() const;  // 레코드 하나의 최대 payload 바이트

 private:
  std::vector<Column> columns_;
};

bool isIdentifier(std::string_view s);

}  // namespace minidb
