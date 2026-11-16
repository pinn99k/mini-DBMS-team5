#include "storage/schema.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "common/errors.h"
#include "storage/frame_codec.h"

namespace minidb {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string upperNoSpace(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
         });
}

}  // namespace

bool isIdentifier(std::string_view s) {
  if (s.empty()) return false;
  auto head = static_cast<unsigned char>(s[0]);
  if (!(std::isalpha(head) || head == '_')) return false;
  return std::all_of(s.begin() + 1, s.end(), [](char c) {
    auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || u == '_';
  });
}

Schema Schema::load(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw SchemaError("cannot read schema file " + file.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), file.string());
}

Schema Schema::parse(std::string_view text, const std::string& source) {
  Schema schema;
  schema.columns_.push_back(Column{"id", ColumnType::Int, 0});

  size_t lineNo = 0;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = trim(text.substr(start, end - start));
    start = end + 1;
    ++lineNo;
    if (line.empty()) continue;

    auto fail = [&](const std::string& msg) -> SchemaError {
      return SchemaError(source + ":" + std::to_string(lineNo) + ": " + msg);
    };

    size_t nameEnd = 0;
    while (nameEnd < line.size() && !std::isspace(static_cast<unsigned char>(line[nameEnd])) &&
           line[nameEnd] != '(') {
      ++nameEnd;
    }
    std::string name(line.substr(0, nameEnd));
    std::string type = upperNoSpace(line.substr(nameEnd));
    if (!isIdentifier(name)) throw fail("invalid column name '" + name + "'");
    if (equalsIgnoreCase(name, "id")) throw fail("column 'id' is reserved (assigned by the engine)");
    if (schema.indexOf(name)) throw fail("duplicate column '" + name + "'");

    Column col{name, ColumnType::Int, 0};
    if (type == "INT") {
      col.type = ColumnType::Int;
    } else if (type.size() > 9 && type.compare(0, 8, "VARCHAR(") == 0 && type.back() == ')') {
      std::string digits = type.substr(8, type.size() - 9);
      if (digits.empty() || digits.size() > 5 ||
          !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        throw fail("invalid VARCHAR length '" + digits + "'");
      }
      unsigned long n = std::stoul(digits);
      if (n < 1 || n > 65535) throw fail("VARCHAR length must be 1..65535");
      col.type = ColumnType::Varchar;
      col.maxLen = static_cast<uint16_t>(n);
    } else {
      throw fail("unknown type '" + std::string(trim(line.substr(nameEnd))) + "' (expected INT or VARCHAR(n))");
    }
    schema.columns_.push_back(col);
  }

  if (schema.columns_.size() == 1) throw SchemaError(source + ": schema has no columns");
  if (schema.maxPayloadSize() > frame::kMaxPayload) {
    throw SchemaError(source + ": maximum record size " + std::to_string(schema.maxPayloadSize()) +
                      " bytes exceeds the 64 KiB record limit");
  }
  return schema;
}

std::optional<size_t> Schema::indexOf(std::string_view name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) return i;
  }
  return std::nullopt;
}

size_t Schema::maxPayloadSize() const {
  size_t total = 0;
  for (const Column& c : columns_) total += c.type == ColumnType::Int ? 8 : 2 + c.maxLen;
  return total;
}

}  // namespace minidb
