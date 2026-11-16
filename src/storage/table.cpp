#include "storage/table.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <limits>

#include "common/bytes.h"
#include "common/crc32.h"
#include "common/errors.h"
#include "storage/frame_codec.h"

namespace minidb {
namespace {

// MINIDB_EXCLUSIVE_LOCK: 읽기도 unique_lock (벤치에서 shared_mutex와 비교용)
#ifdef MINIDB_EXCLUSIVE_LOCK
using ReadLock = std::unique_lock<std::shared_mutex>;
#else
using ReadLock = std::shared_lock<std::shared_mutex>;
#endif
using WriteLock = std::unique_lock<std::shared_mutex>;

constexpr size_t kReadChunk = 64 * 1024;

}  // namespace

void fsyncDirectory(const std::filesystem::path& dir, FileIO& io) {
  int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) throw StorageError(errnoMessage("open directory " + dir.string()));
  try {
    io.fsync(dfd);
  } catch (...) {
    ::close(dfd);
    throw;
  }
  ::close(dfd);
}

Table::Table(std::filesystem::path path, std::string name, Schema schema, FileIO& io, int fd)
    : path_(std::move(path)), name_(std::move(name)), schema_(std::move(schema)), io_(io), fd_(fd) {}

Table::~Table() { ::close(fd_); }

std::unique_ptr<Table> Table::open(const std::filesystem::path& tblPath, std::string name, Schema schema,
                                   FileIO& io) {
  const bool existed = std::filesystem::exists(tblPath);
  int fd = ::open(tblPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) throw StorageError(errnoMessage("open " + tblPath.string()));
  std::unique_ptr<Table> t(new Table(tblPath, std::move(name), std::move(schema), io, fd));

  struct stat st {};
  if (::fstat(fd, &st) != 0) throw StorageError(errnoMessage("fstat " + tblPath.string()));
  const auto size = static_cast<uint64_t>(st.st_size);

  if (size < frame::kFileHeaderSize) {
    // 새 파일이거나, 생성 중 헤더를 쓰다 끊긴 파일 (decisions #5). 프레임이 들어 있을 수 없는 크기다.
    if (existed && size > 0) {
      std::cerr << "warning: table '" << t->name_ << "': file shorter than header (" << size
                << " bytes), rewriting header\n";
    }
    auto header = frame::fileHeader();
    io.pwriteAll(fd, header.data(), header.size(), 0);
    io.fsync(fd);
    if (!existed) fsyncDirectory(tblPath.parent_path(), io);
    t->fileSize_ = frame::kFileHeaderSize;
    return t;
  }

  t->recover(size);
  return t;
}

// 복구 스캔 (architecture §5.3). 프레임마다 길이 범위·CRC·레코드 구성을 검사하고 인덱스를 벌크 로딩한다.
void Table::recover(uint64_t size) {
  auto corrupt = [&](uint64_t at, const std::string& why) {
    return StorageError("table '" + name_ + "': corruption at offset " + std::to_string(at) + ": " + why +
                        " (refusing to start)");
  };

  uint8_t header[frame::kFileHeaderSize];
  if (io_.pread(fd_, header, sizeof header, 0) != sizeof header || !frame::checkFileHeader(header)) {
    throw StorageError("table '" + name_ + "': bad magic or version in " + path_.string());
  }

  Index::Loader loader;
  std::vector<uint8_t> buf;
  uint64_t pos = frame::kFileHeaderSize;
  int64_t lastId = 0;
  uint64_t rows = 0;

  while (pos < size) {
    const uint64_t remain = size - pos;
    if (remain < frame::kFrameHeaderSize) {
      saveAndTruncateTail(pos, size);
      size = pos;
      break;
    }
    uint8_t fh[frame::kFrameHeaderSize];
    if (io_.pread(fd_, fh, sizeof fh, pos) != sizeof fh) throw StorageError("short read during recovery");
    const uint32_t frameLen = bytes::getU32(fh);
    const uint32_t count = bytes::getU32(fh + 4);
    if (frameLen < frame::kMinFrameLen || frameLen > frame::kMaxFrameLen) {
      throw corrupt(pos, "frame_len " + std::to_string(frameLen) + " out of range");  // decisions #2
    }
    const uint64_t total = frame::totalSize(frameLen);
    if (total > remain) {
      saveAndTruncateTail(pos, size);
      size = pos;
      break;
    }

    buf.resize(frameLen + 4u);  // record_count .. records .. crc32 (파일 위치 pos + 4부터)
    if (io_.pread(fd_, buf.data(), buf.size(), pos + 4) != buf.size()) {
      throw StorageError("short read during recovery");
    }
    if (crc32(buf.data(), frameLen) != bytes::getU32(buf.data() + frameLen)) {
      if (total == remain) {
        saveAndTruncateTail(pos, size);
        size = pos;
        break;
      }
      throw corrupt(pos, "CRC mismatch");
    }

    size_t off = 4;
    uint32_t seen = 0;
    while (off < frameLen) {
      if (frameLen - off < 4) throw corrupt(pos, "truncated record header inside frame");
      const uint32_t plen = bytes::getU32(buf.data() + off);
      if (plen > frame::kMaxPayload || plen > frameLen - off - 4) throw corrupt(pos, "payload_len out of range");
      Row r;
      try {
        r = frame::decodePayload(schema_, buf.data() + off + 4, plen);
      } catch (const StorageError& e) {
        throw corrupt(pos, e.what());
      }
      const int64_t id = std::get<int64_t>(r[0]);
      if (id <= lastId) throw corrupt(pos, "ids are not strictly ascending");
      loader.add(id, pos + 4 + off);  // record offset = 파일에서 payload_len의 위치
      lastId = id;
      ++seen;
      off += 4u + plen;
    }
    if (seen != count) throw corrupt(pos, "record_count mismatch");
    rows += seen;
    pos += total;
  }

  fileSize_ = size;
  maxId_ = lastId;
  rowCount_ = rows;
  index_ = loader.finish();
}

// 꼬리 손상: 잘라낼 바이트(≤ 프레임 하나)를 .torn 파일로 보관한 뒤 자른다 (decisions #2).
void Table::saveAndTruncateTail(uint64_t at, uint64_t size) {
  const uint64_t n = size - at;
  std::filesystem::path torn = path_;
  torn += ".torn";
  for (int i = 1; std::filesystem::exists(torn); ++i) {
    torn = path_;
    torn += ".torn." + std::to_string(i);
  }

  std::vector<uint8_t> data(n);
  if (io_.pread(fd_, data.data(), n, at) != n) throw StorageError("short read while saving torn tail");
  int tfd = ::open(torn.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (tfd < 0) throw StorageError(errnoMessage("create " + torn.string()));
  try {
    io_.pwriteAll(tfd, data.data(), data.size(), 0);
    io_.fsync(tfd);
  } catch (...) {
    ::close(tfd);
    throw;
  }
  ::close(tfd);
  fsyncDirectory(path_.parent_path(), io_);

  io_.ftruncate(fd_, at);
  io_.fsync(fd_);
  std::cerr << "warning: table '" << name_ << "': torn tail (" << n << " bytes at offset " << at
            << ") truncated, saved to " << torn.string() << "\n";
}

std::vector<int64_t> Table::insert(const std::vector<std::vector<Value>>& rows) {
  if (rows.empty()) throw StorageError("insert: no rows");
  WriteLock lock(mu_);
  if (!writable_) {
    throw StorageError("table '" + name_ + "' is read-only: an earlier write could not be rolled back");
  }
  const auto n = static_cast<int64_t>(rows.size());
  if (maxId_ > std::numeric_limits<int64_t>::max() - n) throw StorageError("id space exhausted");

  std::vector<Row> full;
  full.reserve(rows.size());
  std::vector<int64_t> ids;
  ids.reserve(rows.size());
  for (int64_t i = 0; i < n; ++i) {
    Row r;
    r.reserve(rows[static_cast<size_t>(i)].size() + 1);
    r.emplace_back(maxId_ + 1 + i);
    r.insert(r.end(), rows[static_cast<size_t>(i)].begin(), rows[static_cast<size_t>(i)].end());
    full.push_back(std::move(r));
    ids.push_back(maxId_ + 1 + i);
  }

  std::vector<uint32_t> offsets;
  std::vector<uint8_t> bytes = frame::encode(schema_, full, &offsets);
  if (bytes.size() - 8 > frame::kMaxFrameLen) {
    throw SchemaError("batch too large: encoded frame exceeds 4 MiB");
  }

  // 고정 오프셋 쓰기 (decisions #6, O_APPEND 금지)
  try {
    io_.pwriteAll(fd_, bytes.data(), bytes.size(), fileSize_);
  } catch (const StorageError& e) {
    try {
      io_.ftruncate(fd_, fileSize_);
    } catch (const StorageError& e2) {
      writable_ = false;  // decisions #4
      throw StorageError(std::string("write failed (") + e.what() + ") and rollback failed (" + e2.what() +
                         "); table '" + name_ + "' is now read-only");
    }
    throw;
  }

  // 공개 순서: fileSize → maxId → index
  const uint64_t frameStart = fileSize_;
  fileSize_ += bytes.size();
  maxId_ += n;
  rowCount_ += static_cast<uint64_t>(n);
  for (size_t i = 0; i < ids.size(); ++i) index_.insert(ids[i], frameStart + offsets[i]);
  dirty_.store(true);
  return ids;
}

Table::Cursor Table::scan() const {
  ReadLock lock(mu_);
  return Cursor(*this, fileSize_);
}

std::optional<Row> Table::findById(int64_t id) const {
  std::optional<uint64_t> off;
  {
    ReadLock lock(mu_);
    off = index_.find(id);
  }
  if (!off) return std::nullopt;
  return readRecord(*off);  // 락을 놓은 뒤 읽는다
}

Row Table::readRecord(uint64_t offset) const {
  uint8_t lenBuf[4];
  if (io_.pread(fd_, lenBuf, 4, offset) != 4) throw StorageError("short read of record header");
  const uint32_t plen = bytes::getU32(lenBuf);
  if (plen > frame::kMaxPayload) throw StorageError("payload_len out of range");
  std::vector<uint8_t> payload(plen);
  if (io_.pread(fd_, payload.data(), plen, offset + 4) != plen) throw StorageError("short read of record");
  return frame::decodePayload(schema_, payload.data(), plen);
}

uint64_t Table::rowCount() const {
  ReadLock lock(mu_);
  return rowCount_;
}

uint64_t Table::fileSize() const {
  ReadLock lock(mu_);
  return fileSize_;
}

int64_t Table::maxId() const {
  ReadLock lock(mu_);
  return maxId_;
}

bool Table::writable() const {
  ReadLock lock(mu_);
  return writable_;
}

bool Table::validateIndex(std::string* why) const {
  ReadLock lock(mu_);
  if (index_.size() != rowCount_) {
    if (why) *why = "index size != row count";
    return false;
  }
  return index_.validate(why);
}

void Table::sync() {
  if (!dirty_.exchange(false)) return;
  try {
    io_.fsync(fd_);
  } catch (...) {
    dirty_.store(true);
    throw;
  }
}

// ---- Cursor ---------------------------------------------------------------------------------

Table::Cursor::Cursor(const Table& table, uint64_t end)
    : table_(&table), pos_(frame::kFileHeaderSize), end_(end) {}

// [off, off+n)이 버퍼에 있으면 그 포인터, 없으면 off부터 최대 64 KiB(또는 n)를 다시 읽는다.
// 버퍼 크기는 max(64 KiB, 레코드 하나) → 테이블 크기와 무관하다.
const uint8_t* Table::Cursor::need(uint64_t off, size_t n) {
  if (off >= bufStart_ && off + n <= bufStart_ + buf_.size()) return buf_.data() + (off - bufStart_);
  if (off + n > end_) throw StorageError("scan: record extends past snapshot end");
  const size_t len = static_cast<size_t>(std::max<uint64_t>(n, std::min<uint64_t>(kReadChunk, end_ - off)));
  buf_.resize(len);
  if (table_->io_.pread(table_->fd_, buf_.data(), len, off) != len) throw StorageError("scan: short read");
  bufStart_ = off;
  return buf_.data();
}

bool Table::Cursor::next(Row& out) {
  while (remaining_ == 0) {
    if (pos_ >= end_) return false;
    const uint8_t* h = need(pos_, frame::kFrameHeaderSize);
    const uint32_t frameLen = bytes::getU32(h);
    remaining_ = bytes::getU32(h + 4);
    recOff_ = pos_ + frame::kFrameHeaderSize;
    pos_ += frame::totalSize(frameLen);
  }
  const uint32_t plen = bytes::getU32(need(recOff_, 4));
  const uint8_t* p = need(recOff_ + 4, plen);
  out = frame::decodePayload(table_->schema_, p, plen);
  recOff_ += 4u + plen;
  --remaining_;
  return true;
}

}  // namespace minidb
