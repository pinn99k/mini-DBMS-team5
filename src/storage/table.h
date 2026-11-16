#pragma once

// Table: 데이터 파일 + 인덱스 + maxId + 락을 함께 만지는 유일한 컴포넌트 (architecture §2 불변식 1).
// 위 계층은 락을 모른다.
//
// 공개 순서 (§12): 파일 바이트 → fileSize_ → index_. 읽는 쪽은 오프셋을 이 둘에서만 얻으므로
// 락을 놓은 뒤에 pread해도 아직 없는 바이트를 가리킬 수 없다.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/value.h"
#include "index/bplus_tree.h"
#include "storage/file_io.h"
#include "storage/schema.h"

namespace minidb {

class Table {
 public:
  static constexpr int kIndexCapacity = 128;
  using Index = BPlusTree<kIndexCapacity>;

  // 스캔 시작 시점의 fileSize를 끝 지점으로 고정한 커서. 락 없이 읽는다.
  class Cursor {
   public:
    bool next(Row& out);

   private:
    friend class Table;
    Cursor(const Table& table, uint64_t end);
    const uint8_t* need(uint64_t off, size_t n);

    const Table* table_;
    uint64_t pos_;        // 다음 프레임 시작
    uint64_t end_;        // 스냅샷 끝 (항상 프레임 경계)
    uint64_t recOff_ = 0;  // 현재 프레임에서 다음 레코드 위치
    uint32_t remaining_ = 0;
    std::vector<uint8_t> buf_;
    uint64_t bufStart_ = 0;
  };

  static std::unique_ptr<Table> open(const std::filesystem::path& tblPath, std::string name,
                                     Schema schema, FileIO& io);
  ~Table();
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  const std::string& name() const { return name_; }
  const Schema& schema() const { return schema_; }

  // id 없는 행(스키마 순서, Binder가 검증 완료) 1..N개 → 프레임 1개로 원자적 커밋.
  std::vector<int64_t> insert(const std::vector<std::vector<Value>>& rows);
  Cursor scan() const;
  std::optional<Row> findById(int64_t id) const;

  uint64_t rowCount() const;
  uint64_t fileSize() const;
  int64_t maxId() const;
  bool writable() const;
  bool validateIndex(std::string* why = nullptr) const;

  void sync();  // 쓴 내용이 있으면 fsync

 private:
  Table(std::filesystem::path path, std::string name, Schema schema, FileIO& io, int fd);
  void recover(uint64_t size);
  void saveAndTruncateTail(uint64_t at, uint64_t size);
  Row readRecord(uint64_t offset) const;

  const std::filesystem::path path_;
  const std::string name_;
  const Schema schema_;
  FileIO& io_;
  const int fd_;

  mutable std::shared_mutex mu_;
  uint64_t fileSize_ = 0;
  int64_t maxId_ = 0;
  uint64_t rowCount_ = 0;
  bool writable_ = true;
  Index index_;
  std::atomic<bool> dirty_{false};
};

void fsyncDirectory(const std::filesystem::path& dir, FileIO& io);

}  // namespace minidb
