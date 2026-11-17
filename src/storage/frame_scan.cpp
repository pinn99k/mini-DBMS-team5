#include "storage/frame_scan.h"

#include <vector>

#include "common/bytes.h"
#include "common/crc32.h"
#include "common/errors.h"
#include "storage/frame_codec.h"

namespace minidb::frame {

ScanResult scanFrames(FileIO& io, int fd, uint64_t size, const Schema& schema, const ScanCallbacks& cb) {
  ScanResult r;
  uint64_t pos = kFileHeaderSize;
  std::vector<uint8_t> buf;
  auto stop = [&](ScanStatus status, std::string why) {
    r.status = status;
    r.at = pos;
    r.why = std::move(why);
    return r;
  };

  while (pos < size) {
    const uint64_t remain = size - pos;
    if (remain < kFrameHeaderSize) return stop(ScanStatus::TornTail, "incomplete frame header");
    uint8_t fh[kFrameHeaderSize];
    if (io.pread(fd, fh, sizeof fh, pos) != sizeof fh) throw StorageError("short read during frame scan");
    const uint32_t frameLen = bytes::getU32(fh);
    const uint32_t count = bytes::getU32(fh + 4);
    if (frameLen < kMinFrameLen || frameLen > kMaxFrameLen) {  // 위치와 상관없이 중간 손상 (decisions #2)
      return stop(ScanStatus::MidCorruption, "frame_len " + std::to_string(frameLen) + " out of range");
    }
    const uint64_t total = totalSize(frameLen);
    if (total > remain) return stop(ScanStatus::TornTail, "incomplete frame");

    buf.resize(frameLen + 4u);  // record_count .. records .. crc32 (파일 위치 pos + 4부터)
    if (io.pread(fd, buf.data(), buf.size(), pos + 4) != buf.size()) {
      throw StorageError("short read during frame scan");
    }
    if (crc32(buf.data(), frameLen) != bytes::getU32(buf.data() + frameLen)) {
      return stop(total == remain ? ScanStatus::TornTail : ScanStatus::MidCorruption, "CRC mismatch");
    }
    if (cb.onFrame) cb.onFrame(pos, frameLen, count);

    size_t off = 4;
    uint32_t seen = 0;
    int64_t lastId = r.lastId;
    while (off < frameLen) {
      if (frameLen - off < 4) return stop(ScanStatus::MidCorruption, "truncated record header inside frame");
      const uint32_t plen = bytes::getU32(buf.data() + off);
      if (plen > kMaxPayload || plen > frameLen - off - 4) {
        return stop(ScanStatus::MidCorruption, "payload_len out of range");
      }
      Row row;
      try {
        row = decodePayload(schema, buf.data() + off + 4, plen);
      } catch (const StorageError& e) {
        return stop(ScanStatus::MidCorruption, e.what());
      }
      const int64_t id = std::get<int64_t>(row[0]);
      if (id <= lastId) return stop(ScanStatus::MidCorruption, "ids are not strictly ascending");
      if (cb.onRecord) cb.onRecord(pos + 4 + off, row);
      lastId = id;
      ++seen;
      off += 4u + plen;
    }
    if (seen != count) return stop(ScanStatus::MidCorruption, "record_count mismatch");

    r.lastId = lastId;
    r.records += seen;
    ++r.frames;
    pos += total;
  }
  return r;
}

}  // namespace minidb::frame
