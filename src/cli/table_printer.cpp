#include "cli/table_printer.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <string>

namespace minidb {
namespace {
constexpr size_t kIntWidth = 20;
constexpr size_t kMaxWidth = 64;
}  // namespace

std::string escapeForDisplay(const std::string& s) {
  std::string out;
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c < 0x20 || c == 0x7F) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    } else {
      out += ch;
    }
  }
  return out;
}

void TablePrinter::begin(const std::vector<Column>& columns) {
  columns_ = columns;
  widths_.clear();
  for (const Column& c : columns) {
    size_t w = c.type == ColumnType::Int ? kIntWidth : std::min<size_t>(c.maxLen, kMaxWidth);
    widths_.push_back(std::max(w, c.name.size()));
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    out_ << (i ? " | " : "") << std::left << std::setw(static_cast<int>(widths_[i])) << columns[i].name;
  }
  out_ << '\n';
  for (size_t i = 0; i < columns.size(); ++i) out_ << (i ? "-+-" : "") << std::string(widths_[i], '-');
  out_ << '\n';
}

bool TablePrinter::row(const Row& row) {
  for (size_t i = 0; i < row.size(); ++i) {
    out_ << (i ? " | " : "");
    if (const auto* v = std::get_if<int64_t>(&row[i])) {
      out_ << std::right << std::setw(static_cast<int>(widths_[i])) << *v;
    } else {
      out_ << std::left << std::setw(static_cast<int>(widths_[i])) << escapeForDisplay(std::get<std::string>(row[i]));
    }
  }
  out_ << '\n';
  return static_cast<bool>(out_);
}

void TablePrinter::end(uint64_t rowCount) {
  out_ << '(' << rowCount << (rowCount == 1 ? " row)" : " rows)") << '\n';
}

}  // namespace minidb
