#pragma once

// 디스크 정수는 little-endian, 바이트 단위로 직렬화한다. 구조체 memcpy 금지 (architecture §4).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace minidb::bytes {

inline void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void putI64(std::vector<uint8_t>& out, int64_t v) { putU64(out, static_cast<uint64_t>(v)); }

inline void setU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

inline uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline int64_t getI64(const uint8_t* p) { return static_cast<int64_t>(getU64(p)); }

}  // namespace minidb::bytes
