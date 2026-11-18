#pragma once

// 테이블 파일 포맷 (architecture §5.2, [고정])
//   header  = magic "MDB1"(4) | version u16 = 1 | reserved u16 = 0
//   frame   = frame_len u32 | record_count u32 | record × record_count | crc32 u32
//             frame_len = (record_count + records)의 길이, crc32도 같은 바이트를 덮는다
//   record  = payload_len u32 | payload(id i64 | 필드들)
//             payload_len = 8(id만)이면 묘비 = DELETE (decisions #20)

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/value.h"
#include "storage/schema.h"

namespace minidb::frame {

inline constexpr uint32_t kMaxPayload = 64 * 1024;
inline constexpr uint32_t kMaxFrameLen = 4 * 1024 * 1024;
inline constexpr uint32_t kMinFrameLen = 4 + 4 + 8;  // record_count + payload_len + id
inline constexpr size_t kFileHeaderSize = 8;
inline constexpr size_t kFrameHeaderSize = 8;  // frame_len + record_count
inline constexpr uint16_t kVersion = 1;
inline constexpr uint32_t kTombstonePayload = 8;  // 묘비 payload = id. 전체 레코드는 언제나 이보다 길다

inline constexpr uint64_t totalSize(uint32_t frameLen) { return 4ull + frameLen + 4ull; }

std::vector<uint8_t> fileHeader();
bool checkFileHeader(const uint8_t* header8);

// rows는 [0]에 id가 들어 있는 완전한 행, 또는 id 하나만 든 행(묘비). recordOffsets에는 각 레코드(payload_len)
// 위치를 프레임 시작 기준으로 넣는다.
std::vector<uint8_t> encode(const Schema& schema, const std::vector<Row>& rows,
                            std::vector<uint32_t>* recordOffsets = nullptr);

// encode가 이 행에 쓸 payload 길이 (묘비면 8).
size_t payloadSize(const Schema& schema, const Row& row);

// payload 하나를 스키마로 디코드한다. 길이가 맞지 않으면 StorageError.
Row decodePayload(const Schema& schema, const uint8_t* payload, size_t len);

}  // namespace minidb::frame
