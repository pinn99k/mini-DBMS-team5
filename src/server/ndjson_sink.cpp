#include "server/ndjson_sink.h"

#include "common/json_writer.h"

namespace minidb {

std::string errorJson(const std::string& type, const std::string& message, std::optional<SourcePos> pos) {
  std::string out = "{\"ok\":false,\"error\":{\"type\":";
  json::appendString(out, type);
  out += ",\"message\":";
  json::appendString(out, message);
  if (pos) out += ",\"line\":" + std::to_string(pos->line) + ",\"col\":" + std::to_string(pos->col);
  out += "}}";
  return out;
}

void NdjsonSink::begin(const std::vector<Column>& columns) {
  buf_ += "{\"columns\":[";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i) buf_ += ',';
    json::appendString(buf_, columns[i].name);
  }
  buf_ += "]}\n";
  maybeFlush();
}

bool NdjsonSink::row(const Row& row) {
  if (!ok_) return false;
  buf_ += '[';
  for (size_t i = 0; i < row.size(); ++i) {
    if (i) buf_ += ',';
    json::appendValue(buf_, row[i]);
  }
  buf_ += "]\n";
  maybeFlush();
  return ok_;
}

void NdjsonSink::end(uint64_t rowCount) {
  buf_ += "{\"ok\":true,\"rowCount\":" + std::to_string(rowCount) + "}\n";
}

void NdjsonSink::fail(const std::string& type, const std::string& message) {
  buf_ += errorJson(type, message);
  buf_ += '\n';
}

void NdjsonSink::maybeFlush() {
  if (buf_.size() >= kFlushBytes) flush();
}

bool NdjsonSink::flush() {
  if (ok_ && !buf_.empty()) ok_ = write_(buf_.data(), buf_.size());
  buf_.clear();
  return ok_;
}

}  // namespace minidb
