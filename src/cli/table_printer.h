#pragma once

// CLI sink. 폭은 스키마에서 나오므로(INT 20, VARCHAR(n) n, 최대 64) 버퍼링 없이 행을 바로 출력한다.

#include <ostream>
#include <vector>

#include "engine/row_sink.h"

namespace minidb {

class TablePrinter : public RowSink {
 public:
  explicit TablePrinter(std::ostream& out) : out_(out) {}
  void begin(const std::vector<Column>& columns) override;
  bool row(const Row& row) override;
  void end(uint64_t rowCount) override;

 private:
  std::ostream& out_;
  std::vector<Column> columns_;
  std::vector<size_t> widths_;
};

std::string escapeForDisplay(const std::string& s);

}  // namespace minidb
