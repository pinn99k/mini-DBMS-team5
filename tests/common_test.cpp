#include <gtest/gtest.h>

#include <string>

#include "common/bytes.h"
#include "common/crc32.h"
#include "common/json_writer.h"

using namespace minidb;

static uint32_t crcOf(const std::string& s) {
  return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

TEST(Crc32, StandardCheckValue) { EXPECT_EQ(crcOf("123456789"), 0xCBF43926u); }

TEST(Crc32, KnownVectors) {
  EXPECT_EQ(crcOf(""), 0x00000000u);
  EXPECT_EQ(crcOf("a"), 0xE8B7BE43u);
  EXPECT_EQ(crcOf("The quick brown fox jumps over the lazy dog"), 0x414FA339u);
}

TEST(Crc32, Incremental) {
  const std::string a = "12345", b = "6789";
  uint32_t c = crc32(reinterpret_cast<const uint8_t*>(a.data()), a.size());
  c = crc32(reinterpret_cast<const uint8_t*>(b.data()), b.size(), c);
  EXPECT_EQ(c, 0xCBF43926u);
}

TEST(Bytes, LittleEndianRoundTrip) {
  std::vector<uint8_t> b;
  bytes::putU16(b, 0x1234);
  bytes::putU32(b, 0xA1B2C3D4u);
  bytes::putI64(b, -2);
  ASSERT_EQ(b.size(), 14u);
  EXPECT_EQ(b[0], 0x34);  // LSB 먼저
  EXPECT_EQ(b[2], 0xD4);
  EXPECT_EQ(bytes::getU16(b.data()), 0x1234);
  EXPECT_EQ(bytes::getU32(b.data() + 2), 0xA1B2C3D4u);
  EXPECT_EQ(bytes::getI64(b.data() + 6), -2);
}

TEST(Json, EscapesStrings) {
  std::string out;
  json::appendString(out, "a\"b\\c\nd\x01");
  EXPECT_EQ(out, "\"a\\\"b\\\\c\\nd\\u0001\"");
  out.clear();
  json::appendString(out, "한글");
  EXPECT_EQ(out, "\"한글\"");
}

TEST(Json, Values) {
  std::string out;
  json::appendValue(out, Value{int64_t{-5}});
  out += ',';
  json::appendValue(out, Value{std::string("x")});
  EXPECT_EQ(out, "-5,\"x\"");
}
