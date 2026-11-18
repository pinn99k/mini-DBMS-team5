#pragma once

// Table: 데이터 파일 + 인덱스 + maxId + 락을 함께 만지는 유일한 컴포넌트 (architecture §2 불변식 1).
// 위 계층은 락을 모른다.
//
// 공개 순서 (§12): 파일 바이트 → fileSize_ → index_. 읽는 쪽은 오프셋을 이 둘에서만 얻으므로
// 락을 놓은 뒤에 pread해도 아직 없는 바이트를 가리킬 수 없다.
//
// 락 두 개 (§12, decisions #19):
//   writeMu_ — 쓰는 쪽끼리의 순서. 인코딩·pwrite·자르기는 이것만 쥐고 한다.
//   mu_      — 공개 상태(fileSize_·maxId_·rowCount_·index_·writable_). 쓰는 쪽은 공개하는 순간에만 독점으로 잡는다.
//   공개 상태를 바꾸는 건 두 락을 모두 쥔 쓰는 쪽뿐이다. 그래서 쓰는 쪽은 writeMu_만으로 그 값을 읽어도 된다.
//   락 순서는 언제나 writeMu_ → mu_. modify는 writeMu_를 쥔 채 커서를 돌린다(커서가 mu_ shared를 잠깐씩).
//
// UPDATE·DELETE도 덧붙이기만 한다(decisions #20). 공개된 바이트는 바뀌지 않는다.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
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

  static constexpr size_t kScanBatch = 256;

  // id 순서로 도는 커서 (decisions #21). 시작 시점의 maxId가 상한이다.
  // kScanBatch개씩 mu_ shared 안에서 (id, offset)을 복사하고, 레코드는 락 밖에서 읽는다.
  class Cursor {
   public:
    bool next(Row& out);

   private:
    friend class Table;
    Cursor(const Table& table, int64_t lastId);
    const uint8_t* need(uint64_t off, size_t n);

    const Table* table_;
    int64_t nextId_ = 1;   // 다음 배치의 시작 id
    int64_t lastId_;       // 상한 (스캔 시작 시점의 maxId)
    bool done_ = false;
    std::vector<std::pair<int64_t, uint64_t>> batch_;
    size_t batchPos_ = 0;
    uint64_t end_ = 0;      // 배치를 받은 순간의 fileSize. 배치의 레코드는 모두 이 안에 있다
    std::vector<uint8_t> buf_;  // 순차 구간 미리 읽기
    uint64_t bufStart_ = 0;
    std::vector<uint8_t> far_;  // 멀리 떨어진 레코드 하나 (수정된 행의 새 버전)
  };

  // UPDATE·DELETE 문장 하나 (Binder·Planner를 거친 상태, architecture §5.4b)
  struct Edit {
    std::optional<int64_t> id;                    // 있으면 그 id 한 행 (IndexScan)
    std::optional<Predicate> filter;              // 없으면 모든 행 (FullScan)
    std::vector<std::pair<size_t, Value>> set;    // 컬럼 인덱스 → 새 값
    bool remove = false;                          // true = DELETE
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
  // 문장을 순서대로 적용하고(뒤 문장은 앞 문장의 결과를 봄) 프레임 1개로 원자적 커밋. 문장별 영향 행 수.
  std::vector<uint64_t> modify(const std::vector<Edit>& edits);
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
  // modify의 두 단계 (architecture §5.4b). 둘 다 writeMu_ 안에서.
  using Pending = std::map<int64_t, std::optional<Row>>;  // id → 최종 상태 (nullopt = 지움)
  std::vector<uint64_t> stageEdits(const std::vector<Edit>& edits, Pending& pending) const;  // 찾기·적용
  void commitPending(Pending& pending);                                                       // 쓰기·공개
  void requireWritable() const;                        // writeMu_ 안에서
  void writeFrame(const std::vector<uint8_t>& bytes);  // writeMu_ 안에서. 실패하면 되돌리고 던짐
  Row readRecord(uint64_t offset) const;

  const std::filesystem::path path_;
  const std::string name_;
  const Schema schema_;
  FileIO& io_;
  const int fd_;

  mutable std::shared_mutex mu_;
  std::mutex writeMu_;  // 쓰는 쪽끼리의 순서. pwrite는 이 락만 쥐고 한다
  uint64_t fileSize_ = 0;
  int64_t maxId_ = 0;
  uint64_t rowCount_ = 0;
  bool writable_ = true;
  Index index_;
  std::atomic<bool> dirty_{false};
};

void fsyncDirectory(const std::filesystem::path& dir, FileIO& io);

}  // namespace minidb
