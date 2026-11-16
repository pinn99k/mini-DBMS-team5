#pragma once

// SELECT 결과를 NDJSON으로 흘려보내는 sink (architecture §11.4).
//   {"columns":[...]}  →  [v,...] 줄들  →  {"ok":true,"rowCount":N} | {"ok":false,"error":{...}}
// 16 KiB까지 모았다가 쓴다. 메모리는 결과 크기와 무관하다. 쓰기가 실패하면 row()가 false를 돌려
// 실행기가 스캔을 멈춘다 (decisions #3).

#include <cstddef>
#include <functional>
#include <string>

#include "common/errors.h"
#include "engine/row_sink.h"

namespace minidb {

class NdjsonSink : public RowSink {
 public:
  using Writer = std::function<bool(const char*, size_t)>;
  static constexpr size_t kFlushBytes = 16 * 1024;

  explicit NdjsonSink(Writer writer) : write_(std::move(writer)) {}

  void begin(const std::vector<Column>& columns) override;
  bool row(const Row& row) override;
  void end(uint64_t rowCount) override;
  void fail(const std::string& type, const std::string& message);

  bool flush();  // false = 클라이언트로 쓰기 실패
  bool ok() const { return ok_; }

 private:
  void maybeFlush();

  Writer write_;
  std::string buf_;
  bool ok_ = true;
};

std::string errorJson(const std::string& type, const std::string& message,
                      std::optional<SourcePos> pos = std::nullopt);

}  // namespace minidb
