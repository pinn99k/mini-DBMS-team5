#pragma once

#include <cstddef>
#include <cstdint>

namespace minidb {

// CRC-32 (IEEE 802.3, 반사 다항식 0xEDB88320). crc32("123456789") == 0xCBF43926.
// 이어서 계산하려면 이전 결과를 crc로 넘긴다.
uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

}  // namespace minidb
