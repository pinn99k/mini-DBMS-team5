#include "common/crc32.h"

#include <array>

namespace minidb {
namespace {

constexpr std::array<uint32_t, 256> makeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kTable = makeTable();

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = kTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

}  // namespace minidb
