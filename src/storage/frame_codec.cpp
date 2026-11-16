#include "storage/frame_codec.h"

#include <string>

#include "common/bytes.h"
#include "common/crc32.h"
#include "common/errors.h"

namespace minidb::frame {
namespace {

constexpr uint8_t kMagic[4] = {'M', 'D', 'B', '1'};

void encodePayload(const Schema& schema, const Row& row, std::vector<uint8_t>& out) {
  const auto& cols = schema.columns();
  if (row.size() != cols.size()) throw StorageError("encode: row has wrong number of fields");
  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i].type == ColumnType::Int) {
      bytes::putI64(out, std::get<int64_t>(row[i]));
    } else {
      const auto& s = std::get<std::string>(row[i]);
      if (s.size() > cols[i].maxLen) throw StorageError("encode: VARCHAR value exceeds column length");
      bytes::putU16(out, static_cast<uint16_t>(s.size()));
      out.insert(out.end(), s.begin(), s.end());
    }
  }
}

}  // namespace

std::vector<uint8_t> fileHeader() {
  std::vector<uint8_t> h(kMagic, kMagic + 4);
  bytes::putU16(h, kVersion);
  bytes::putU16(h, 0);
  return h;
}

bool checkFileHeader(const uint8_t* p) {
  for (int i = 0; i < 4; ++i) {
    if (p[i] != kMagic[i]) return false;
  }
  return bytes::getU16(p + 4) == kVersion;
}

std::vector<uint8_t> encode(const Schema& schema, const std::vector<Row>& rows,
                            std::vector<uint32_t>* recordOffsets) {
  std::vector<uint8_t> out(kFrameHeaderSize, 0);
  bytes::setU32(out.data() + 4, static_cast<uint32_t>(rows.size()));
  for (const Row& row : rows) {
    size_t at = out.size();
    if (recordOffsets) recordOffsets->push_back(static_cast<uint32_t>(at));
    out.resize(at + 4);
    encodePayload(schema, row, out);
    size_t payloadLen = out.size() - at - 4;
    if (payloadLen > kMaxPayload) throw StorageError("encode: record exceeds 64 KiB");
    bytes::setU32(out.data() + at, static_cast<uint32_t>(payloadLen));
  }
  const uint32_t frameLen = static_cast<uint32_t>(out.size() - 4);
  bytes::setU32(out.data(), frameLen);
  bytes::putU32(out, crc32(out.data() + 4, frameLen));
  return out;
}

Row decodePayload(const Schema& schema, const uint8_t* p, size_t len) {
  const auto& cols = schema.columns();
  Row row;
  row.reserve(cols.size());
  size_t pos = 0;
  auto need = [&](size_t n) {
    if (len - pos < n) throw StorageError("record payload is shorter than its schema");
  };
  for (const Column& c : cols) {
    if (c.type == ColumnType::Int) {
      need(8);
      row.emplace_back(bytes::getI64(p + pos));
      pos += 8;
    } else {
      need(2);
      uint16_t n = bytes::getU16(p + pos);
      pos += 2;
      if (n > c.maxLen) throw StorageError("VARCHAR length in record exceeds column '" + c.name + "'");
      need(n);
      row.emplace_back(std::string(reinterpret_cast<const char*>(p + pos), n));
      pos += n;
    }
  }
  if (pos != len) throw StorageError("record payload has trailing bytes");
  return row;
}

}  // namespace minidb::frame
