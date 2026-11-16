#include "common/json_writer.h"

#include <cstdio>

namespace minidb::json {

void appendString(std::string& out, std::string_view s) {
  out += '"';
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void appendValue(std::string& out, const Value& v) {
  if (const auto* i = std::get_if<int64_t>(&v)) {
    out += std::to_string(*i);
  } else {
    appendString(out, std::get<std::string>(v));
  }
}

}  // namespace minidb::json
