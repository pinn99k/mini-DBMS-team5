#include "storage/table.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <limits>

#include "common/bytes.h"
#include "common/errors.h"
#include "storage/frame_codec.h"
#include "storage/frame_scan.h"

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

// 복구 스캔 (architecture §5.3). 판정은 frame::scanFrames가 하고, 여기서는 결과대로 자르거나 거부하고
// 살아 있는 id 표(id 오름차순)로 인덱스를 벌크 로딩한다.
void Table::recover(uint64_t size) {
  uint8_t header[frame::kFileHeaderSize];
  if (io_.pread(fd_, header, sizeof header, 0) != sizeof header || !frame::checkFileHeader(header)) {
    throw StorageError("table '" + name_ + "': bad magic or version in " + path_.string());
  }

  const frame::ScanResult r = frame::scanFrames(io_, fd_, size, schema_);

  if (r.status == frame::ScanStatus::MidCorruption) {
    throw StorageError("table '" + name_ + "': corruption at offset " + std::to_string(r.at) + ": " + r.why +
                       " (refusing to start)");
  }
  if (r.status == frame::ScanStatus::TornTail) {
    saveAndTruncateTail(r.at, size);
    size = r.at;
  }

  Index::Loader loader;
  for (const auto& [id, offset] : r.live) loader.add(id, offset);
  fileSize_ = size;
  maxId_ = r.lastId;
  rowCount_ = r.live.size();
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
  // 쓰는 쪽끼리는 writeMu_로 줄 선다. fileSize_·maxId_·writable_을 바꾸는 건 이 락을 쥔 쓰는 쪽뿐이라
  // 여기서는 mu_ 없이 읽어도 된다. 읽는 쪽은 fileSize_ 너머를 안 읽으므로 pwrite 중인 바이트를 못 본다.
  std::lock_guard<std::mutex> writer(writeMu_);
  requireWritable();
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

  writeFrame(bytes);

  // 공개: 읽는 쪽이 기다리는 건 이 구간(인덱스 삽입)뿐이다. 공개 순서: fileSize → maxId → index
  WriteLock publish(mu_);
  const uint64_t frameStart = fileSize_;
  fileSize_ += bytes.size();
  maxId_ += n;
  rowCount_ += static_cast<uint64_t>(n);
  for (size_t i = 0; i < ids.size(); ++i) index_.insert(ids[i], frameStart + offsets[i]);
  dirty_.store(true);
  return ids;
}

void Table::requireWritable() const {
  if (!writable_) {
    throw StorageError("table '" + name_ + "' is read-only: an earlier write could not be rolled back");
  }
}

// 고정 오프셋 쓰기 (decisions #6, O_APPEND 금지). 실패하면 fileSize_로 자르고, 그것도 실패하면 쓰기 불가(#4).
void Table::writeFrame(const std::vector<uint8_t>& bytes) {
  try {
    io_.pwriteAll(fd_, bytes.data(), bytes.size(), fileSize_);
  } catch (const StorageError& e) {
    try {
      io_.ftruncate(fd_, fileSize_);
    } catch (const StorageError& e2) {
      WriteLock publish(mu_);
      writable_ = false;
      throw StorageError(std::string("write failed (") + e.what() + ") and rollback failed (" + e2.what() +
                         "); table '" + name_ + "' is now read-only");
    }
    throw;
  }
}

// UPDATE·DELETE (architecture §5.4b). 대상 찾기부터 쓰기까지 writeMu_ 안 → 잃어버린 갱신이 없다 (#24).
std::vector<uint64_t> Table::modify(const std::vector<Edit>& edits) {
  std::lock_guard<std::mutex> writer(writeMu_);
  requireWritable();
  Pending pending;
  std::vector<uint64_t> counts = stageEdits(edits, pending);
  if (!pending.empty()) commitPending(pending);
  return counts;
}

// 1단계: 문장을 순서대로 현재 행에 적용해 pending에 모은다. 파일·공개 상태는 바꾸지 않는다.
// 뒤 문장은 pending을 먼저 본다(앞 문장의 결과). 프레임 상한을 넘으면 쓰기 전에 SchemaError.
std::vector<uint64_t> Table::stageEdits(const std::vector<Edit>& edits, Pending& pending) const {
  uint64_t frameLen = 4;  // record_count + 레코드들. 쓰기 전에 상한을 넘는지 본다
  auto recordLen = [&](const std::optional<Row>& r) {
    return 4 + (r ? frame::payloadSize(schema_, *r) : frame::kTombstonePayload);
  };

  // 현재 행 하나에 문장을 적용한다. 조건에 맞으면 true.
  auto apply = [&](const Edit& e, Row row) {
    if (e.filter && row[e.filter->column] != e.filter->value) return false;
    const int64_t id = std::get<int64_t>(row[0]);
    std::optional<Row> next;
    if (!e.remove) {
      for (const auto& [col, v] : e.set) row[col] = v;
      next = std::move(row);
    }
    auto [it, fresh] = pending.try_emplace(id);
    if (!fresh) frameLen -= recordLen(it->second);
    frameLen += recordLen(next);
    it->second = std::move(next);
    if (frameLen > frame::kMaxFrameLen) {
      throw SchemaError("too many rows changed in one request: encoded frame exceeds 4 MiB");
    }
    return true;
  };

  std::vector<uint64_t> counts;
  for (const Edit& e : edits) {
    uint64_t n = 0;
    if (e.id) {
      auto it = pending.find(*e.id);
      std::optional<Row> row = it != pending.end() ? it->second : findById(*e.id);
      if (row && apply(e, std::move(*row))) ++n;
    } else {
      Cursor c = scan();  // writeMu_ → mu_ (shared, 배치마다 잠깐)
      Row row;
      while (c.next(row)) {
        auto it = pending.find(std::get<int64_t>(row[0]));
        if (it != pending.end()) {
          if (!it->second) continue;  // 앞 문장이 지움
          row = *it->second;
        }
        if (apply(e, std::move(row))) ++n;
      }
    }
    counts.push_back(n);
  }
  return counts;
}

// 2단계: pending을 id 순서로 프레임 하나에 쓰고(지움 = 묘비) 공개한다.
void Table::commitPending(Pending& pending) {
  std::vector<Row> rows;
  rows.reserve(pending.size());
  for (auto& [id, r] : pending) rows.push_back(r ? std::move(*r) : Row{Value{id}});  // 지움 = 묘비
  std::vector<uint32_t> offsets;
  const std::vector<uint8_t> bytes = frame::encode(schema_, rows, &offsets);
  writeFrame(bytes);

  // 공개: fileSize → index (UPDATE = assign, DELETE = erase)
  WriteLock publish(mu_);
  const uint64_t frameStart = fileSize_;
  fileSize_ += bytes.size();
  for (size_t i = 0; i < rows.size(); ++i) {
    const int64_t id = std::get<int64_t>(rows[i][0]);
    if (rows[i].size() == 1) {
      index_.erase(id);
      --rowCount_;
    } else {
      index_.assign(id, frameStart + offsets[i]);
    }
  }
  dirty_.store(true);
}

Table::Cursor Table::scan() const {
  ReadLock lock(mu_);
  return Cursor(*this, maxId_);
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

Table::Cursor::Cursor(const Table& table, int64_t lastId) : table_(&table), lastId_(lastId) {}

// [off, off+n)이 순차 버퍼에 있으면 그 포인터. 없으면:
//   버퍼 끝에서 64 KiB 이상 앞(멀리 떨어진 레코드, 보통 수정된 행의 새 버전) → 그 바이트만 far_에 읽는다.
//   그 밖(순차 진행, 뒤로 돌아감) → off부터 최대 64 KiB를 순차 버퍼에 다시 읽는다.
// 수정 없는 테이블은 오프셋이 연속이라 예전처럼 순차 읽기다. 메모리는 64 KiB + 레코드 하나.
const uint8_t* Table::Cursor::need(uint64_t off, size_t n) {
  const uint64_t bufEnd = bufStart_ + buf_.size();
  if (off >= bufStart_ && off + n <= bufEnd) return buf_.data() + (off - bufStart_);
  if (off + n > end_) throw StorageError("scan: record extends past snapshot end");
  const bool far = !buf_.empty() && off >= bufEnd + kReadChunk;
  std::vector<uint8_t>& b = far ? far_ : buf_;
  const size_t len = far ? n : static_cast<size_t>(std::max<uint64_t>(n, std::min<uint64_t>(kReadChunk, end_ - off)));
  b.resize(len);
  if (table_->io_.pread(table_->fd_, b.data(), len, off) != len) throw StorageError("scan: short read");
  if (!far) bufStart_ = off;
  return b.data();
}

bool Table::Cursor::next(Row& out) {
  if (batchPos_ == batch_.size()) {
    batch_.clear();
    batchPos_ = 0;
    if (done_ || nextId_ > lastId_) return false;
    {
      ReadLock lock(table_->mu_);
      table_->index_.range(nextId_, lastId_, kScanBatch, batch_);
      end_ = table_->fileSize_;  // 인덱스가 가리키는 바이트는 공개 순서상 모두 이 안에 있다
    }
    if (batch_.empty()) return false;
    const int64_t last = batch_.back().first;
    if (last == lastId_) {
      done_ = true;  // lastId_ + 1이 넘칠 수 있어서 따로 표시
    } else {
      nextId_ = last + 1;
    }
  }
  const uint64_t off = batch_[batchPos_++].second;
  const uint32_t plen = bytes::getU32(need(off, 4));
  out = frame::decodePayload(table_->schema_, need(off + 4, plen), plen);
  return true;
}

}  // namespace minidb
