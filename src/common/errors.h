#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace minidb {

// 1부터 센다. col은 바이트 단위.
struct SourcePos {
  uint32_t line = 0;
  uint32_t col = 0;
};

class DbError : public std::runtime_error {
 public:
  explicit DbError(const std::string& message, std::optional<SourcePos> where = std::nullopt)
      : std::runtime_error(message), pos(where) {}
  virtual const char* typeName() const = 0;

  std::optional<SourcePos> pos;
};

class ParseError : public DbError {
 public:
  using DbError::DbError;
  const char* typeName() const override { return "ParseError"; }
};

class SchemaError : public DbError {
 public:
  enum class Kind { UnknownTable, Other };

  explicit SchemaError(const std::string& message, std::optional<SourcePos> where = std::nullopt,
                       Kind k = Kind::Other)
      : DbError(message, where), kind(k) {}
  const char* typeName() const override { return "SchemaError"; }

  Kind kind;
};

class StorageError : public DbError {
 public:
  using DbError::DbError;
  const char* typeName() const override { return "StorageError"; }
};

}  // namespace minidb
