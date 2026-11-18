#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <future>
#include <random>
#include <thread>
#include <tuple>

#include "common/bytes.h"
#include "common/crc32.h"
#include "storage/database.h"
#include "storage/frame_codec.h"
#include "storage/frame_scan.h"
#include "testutil.h"

using namespace minidb;
using namespace minidb::test;

namespace {

Schema usersSchema() { return Schema::parse("name VARCHAR(16)\nage INT\n", "users.schema"); }

std::vector<Value> user(const std::string& name, int64_t age) { return {Value{name}, Value{age}}; }

std::vector<Row> scanAll(const Table& t) {
  std::vector<Row> rows;
  auto c = t.scan();
  Row r;
  while (c.next(r)) rows.push_back(r);
  return rows;
}

class TableTest : public ::testing::Test {
 protected:
  std::unique_ptr<Table> open(FileIO& io = defaultFileIO()) {
    return Table::open(dir / "users.tbl", "users", usersSchema(), io);
  }
  std::filesystem::path tbl() const { return dir / "users.tbl"; }
  TempDir dir;
};

}  // namespace

// ---- Schema ---------------------------------------------------------------------------------

TEST(SchemaTest, ParsesColumnsWithHiddenId) {
  Schema s = Schema::parse("  name  VARCHAR ( 10 )\n\nage int\r\n", "t");
  ASSERT_EQ(s.columns().size(), 3u);
  EXPECT_EQ(s.columns()[0].name, "id");
  EXPECT_EQ(s.columns()[1].type, ColumnType::Varchar);
  EXPECT_EQ(s.columns()[1].maxLen, 10);
  EXPECT_EQ(s.columns()[2].type, ColumnType::Int);
  EXPECT_EQ(s.indexOf("age"), 2u);
  EXPECT_FALSE(s.indexOf("AGE"));  // 식별자는 대소문자 구분
}

TEST(SchemaTest, RejectsBadSchemas) {
  EXPECT_THROW(Schema::parse("", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("id INT", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("a INT\na INT", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("a FLOAT", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("a VARCHAR(0)", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("1a INT", "t"), SchemaError);
  EXPECT_THROW(Schema::parse("a VARCHAR(40000)\nb VARCHAR(40000)", "t"), SchemaError);  // > 64 KiB 레코드
}

// ---- Frame codec ----------------------------------------------------------------------------

TEST(FrameCodecTest, EncodeDecodeRoundTrip) {
  Schema s = usersSchema();
  std::vector<Row> rows = {{Value{int64_t{1}}, Value{std::string("kim")}, Value{int64_t{25}}},
                           {Value{int64_t{2}}, Value{std::string("")}, Value{int64_t{-1}}}};
  std::vector<uint32_t> offsets;
  auto f = frame::encode(s, rows, &offsets);
  const uint32_t frameLen = bytes::getU32(f.data());
  EXPECT_EQ(frame::totalSize(frameLen), f.size());
  EXPECT_EQ(bytes::getU32(f.data() + 4), 2u);
  EXPECT_EQ(crc32(f.data() + 4, frameLen), bytes::getU32(f.data() + 4 + frameLen));
  ASSERT_EQ(offsets.size(), 2u);
  EXPECT_EQ(offsets[0], 8u);
  for (size_t i = 0; i < 2; ++i) {
    const uint32_t plen = bytes::getU32(f.data() + offsets[i]);
    EXPECT_EQ(frame::decodePayload(s, f.data() + offsets[i] + 4, plen), rows[i]);
  }
}

TEST(FrameCodecTest, DecodeRejectsMalformedPayload) {
  Schema s = usersSchema();
  std::vector<uint8_t> p;
  bytes::putI64(p, 1);
  bytes::putU16(p, 3);
  p.insert(p.end(), {'a', 'b', 'c'});
  EXPECT_THROW(frame::decodePayload(s, p.data(), p.size()), StorageError);  // age 없음
  bytes::putI64(p, 7);
  EXPECT_NO_THROW(frame::decodePayload(s, p.data(), p.size()));
  p.push_back(0);
  EXPECT_THROW(frame::decodePayload(s, p.data(), p.size()), StorageError);  // 남는 바이트
}

// ---- Table: round trip, ids, restart ---------------------------------------------------------

TEST_F(TableTest, InsertThenScanRoundTrip) {
  auto t = open();
  EXPECT_EQ(t->insert({user("kim", 25)}), std::vector<int64_t>{1});
  EXPECT_EQ(t->insert({user("lee", 30), user("park", 41)}), (std::vector<int64_t>{2, 3}));
  auto rows = scanAll(*t);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[2], (Row{Value{int64_t{3}}, Value{std::string("park")}, Value{int64_t{41}}}));
  EXPECT_EQ(t->rowCount(), 3u);
}

TEST_F(TableTest, IdsContinueAfterRestartAndIndexIsRebuilt) {
  {
    auto t = open();
    for (int i = 0; i < 50; ++i) t->insert({user("u" + std::to_string(i), i)});
    t->insert({user("a", 1), user("b", 2), user("c", 3)});  // 레코드 3개짜리 프레임
  }
  auto t = open();
  EXPECT_EQ(t->rowCount(), 53u);
  EXPECT_EQ(t->maxId(), 53);
  EXPECT_TRUE(t->validateIndex());
  for (int64_t id = 1; id <= 53; ++id) {
    auto r = t->findById(id);
    ASSERT_TRUE(r) << id;
    EXPECT_EQ(std::get<int64_t>((*r)[0]), id);
  }
  EXPECT_EQ(std::get<std::string>((*t->findById(52))[1]), "b");
  EXPECT_FALSE(t->findById(54));
  EXPECT_EQ(t->insert({user("next", 0)}), std::vector<int64_t>{54});
}

TEST_F(TableTest, EmptyTableWorksAndSurvivesRestart) {
  { auto t = open(); }
  auto t = open();
  EXPECT_EQ(t->rowCount(), 0u);
  EXPECT_TRUE(scanAll(*t).empty());
  EXPECT_FALSE(t->findById(1));
  EXPECT_EQ(std::filesystem::file_size(tbl()), 8u);
}

// ---- Recovery rules (architecture §5.3) -------------------------------------------------------

TEST_F(TableTest, TornTailIsTruncatedAndSaved) {
  {
    auto t = open();
    t->insert({user("kim", 1)});
    t->insert({user("lee", 2)});
  }
  auto bytes = readBytes(tbl());
  const auto full = bytes.size();
  bytes.resize(full - 5);  // 마지막 프레임이 잘림
  writeBytes(tbl(), bytes);

  auto t = open();
  EXPECT_EQ(t->rowCount(), 1u);
  EXPECT_TRUE(std::filesystem::exists(dir / "users.tbl.torn"));
  EXPECT_LT(std::filesystem::file_size(tbl()), full - 5);
  EXPECT_EQ(t->insert({user("park", 3)}), std::vector<int64_t>{2});  // 잘린 id는 다시 쓴다 (커밋 안 됐으므로)
}

TEST_F(TableTest, PartialFrameHeaderAtTailIsTorn) {
  { open()->insert({user("kim", 1)}); }
  auto bytes = readBytes(tbl());
  bytes.insert(bytes.end(), {0x10, 0x00, 0x00});
  writeBytes(tbl(), bytes);
  auto t = open();
  EXPECT_EQ(t->rowCount(), 1u);
}

TEST_F(TableTest, CrcMismatchOnLastFrameIsTornTail) {
  {
    auto t = open();
    t->insert({user("kim", 1)});
    t->insert({user("lee", 2)});
  }
  auto bytes = readBytes(tbl());
  bytes[bytes.size() - 6] ^= 0xFF;  // 마지막 프레임 내용
  writeBytes(tbl(), bytes);
  auto t = open();
  EXPECT_EQ(t->rowCount(), 1u);
}

TEST_F(TableTest, MidCorruptionRefusesToStart) {
  {
    auto t = open();
    t->insert({user("kim", 1)});
    t->insert({user("lee", 2)});
  }
  auto bytes = readBytes(tbl());
  bytes[8 + 12] ^= 0xFF;  // 첫 프레임 내용
  writeBytes(tbl(), bytes);
  EXPECT_THROW(open(), StorageError);
}

TEST_F(TableTest, BadMagicRefusesToStart) {
  { open()->insert({user("kim", 1)}); }
  auto bytes = readBytes(tbl());
  bytes[0] = 'X';
  writeBytes(tbl(), bytes);
  EXPECT_THROW(open(), StorageError);
}

TEST_F(TableTest, WrongRecordCountRefusesToStart) {
  {
    auto t = open();
    t->insert({user("kim", 1)});
    t->insert({user("lee", 2)});
  }
  auto bytes = readBytes(tbl());
  // 첫 프레임의 record_count를 2로 바꾸고 CRC도 다시 계산 → CRC는 맞지만 구성이 틀림
  const uint32_t frameLen = bytes::getU32(&bytes[8]);
  bytes::setU32(&bytes[12], 2);
  bytes::setU32(&bytes[12 + frameLen], crc32(&bytes[12], frameLen));
  writeBytes(tbl(), bytes);
  EXPECT_THROW(open(), StorageError);
}

TEST_F(TableTest, FrameLenOutOfRangeRefusesEvenAtTail) {
  { open()->insert({user("kim", 1)}); }
  auto bytes = readBytes(tbl());
  bytes::setU32(&bytes[8], 0x7FFFFFFF);  // 유일한(=마지막) 프레임의 길이가 상한 밖 (decisions #2)
  writeBytes(tbl(), bytes);
  EXPECT_THROW(open(), StorageError);
}

TEST_F(TableTest, ScannerRejectsUpdateOfMissingIdLikeRecover) {
  // 예전에는 dump만 id 검사를 빠뜨려 "ok"라고 했다. 이제 판정은 scanFrames 하나다 (decisions #18).
  // id 2 다음의 id 1은 "이미 본 최대 id 이하 = UPDATE"로 판정되는데, 1은 넣은 적이 없다 (decisions #20).
  Schema s = usersSchema();
  std::vector<uint8_t> file = frame::fileHeader();
  const auto f = frame::encode(s, {{Value{int64_t{2}}, Value{std::string("a")}, Value{int64_t{1}}},
                                   {Value{int64_t{1}}, Value{std::string("b")}, Value{int64_t{2}}}});
  file.insert(file.end(), f.begin(), f.end());
  writeBytes(tbl(), file);

  const int fd = ::open(tbl().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  const auto r = frame::scanFrames(defaultFileIO(), fd, file.size(), s);
  ::close(fd);
  EXPECT_EQ(r.status, frame::ScanStatus::MidCorruption);
  EXPECT_EQ(r.at, frame::kFileHeaderSize);
  EXPECT_EQ(r.why, "update of a missing id 1");
  EXPECT_THROW(open(), StorageError);
}

// ---- UPDATE·DELETE (architecture §5.4b, decisions #20 #21 #24) --------------------------------

namespace {

Table::Edit setAge(std::optional<int64_t> id, int64_t age, std::optional<Predicate> filter = std::nullopt) {
  Table::Edit e;
  e.id = id;
  e.filter = std::move(filter);
  e.set = {{2, Value{age}}};
  return e;
}

Table::Edit del(std::optional<int64_t> id, std::optional<Predicate> filter = std::nullopt) {
  Table::Edit e;
  e.id = id;
  e.filter = std::move(filter);
  e.remove = true;
  return e;
}

Predicate nameIs(const std::string& n) { return Predicate{1, Value{n}}; }
int64_t ageOf(const Table& t, int64_t id) { return std::get<int64_t>((*t.findById(id))[2]); }

std::vector<int64_t> idsOf(const std::vector<Row>& rows) {
  std::vector<int64_t> out;
  for (const Row& r : rows) out.push_back(std::get<int64_t>(r[0]));
  return out;
}

frame::ScanResult scanFile(const std::filesystem::path& p) {
  const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  const auto r = frame::scanFrames(defaultFileIO(), fd, std::filesystem::file_size(p), usersSchema());
  ::close(fd);
  return r;
}

}  // namespace

TEST_F(TableTest, UpdateAndDeleteRoundTripAndSurviveRestart) {
  {
    auto t = open();
    t->insert({user("a", 1), user("b", 2), user("c", 3), user("d", 4), user("e", 5)});
    EXPECT_EQ(t->modify({setAge(2, 99)}), std::vector<uint64_t>{1});              // id 경로
    EXPECT_EQ(t->modify({del(3)}), std::vector<uint64_t>{1});
    EXPECT_EQ(t->modify({setAge(std::nullopt, 50, nameIs("e"))}), std::vector<uint64_t>{1});  // 필터 경로
    EXPECT_EQ(t->modify({del(3), setAge(42, 1)}), (std::vector<uint64_t>{0, 0}));  // 지운 id, 없는 id
    EXPECT_EQ(idsOf(scanAll(*t)), (std::vector<int64_t>{1, 2, 4, 5}));
    EXPECT_EQ(ageOf(*t, 2), 99);
    EXPECT_EQ(ageOf(*t, 5), 50);
    EXPECT_FALSE(t->findById(3));
    EXPECT_EQ(t->rowCount(), 4u);
    EXPECT_TRUE(t->validateIndex());
  }
  auto t = open();
  EXPECT_EQ(idsOf(scanAll(*t)), (std::vector<int64_t>{1, 2, 4, 5}));
  EXPECT_EQ(ageOf(*t, 2), 99);
  EXPECT_EQ(ageOf(*t, 5), 50);
  EXPECT_FALSE(t->findById(3));
  EXPECT_EQ(t->rowCount(), 4u);
  EXPECT_EQ(t->maxId(), 5);
  EXPECT_TRUE(t->validateIndex());
  EXPECT_EQ(t->insert({user("f", 6)}), std::vector<int64_t>{6});
}

TEST_F(TableTest, DeletingMaxIdDoesNotReuseItAfterRestart) {
  {
    auto t = open();
    t->insert({user("a", 1), user("b", 2), user("c", 3)});
    t->modify({del(3)});
    EXPECT_EQ(t->maxId(), 3);
  }
  auto t = open();
  EXPECT_EQ(t->maxId(), 3);  // 지운 id 3의 INSERT 레코드도 파일에 있다
  EXPECT_EQ(t->insert({user("d", 4)}), std::vector<int64_t>{4});
}

TEST_F(TableTest, BatchAppliesStatementsInOrderAsOneFrame) {
  auto t = open();
  t->insert({user("a", 1), user("b", 2), user("c", 3)});
  const auto before = scanFile(tbl()).frames;
  // UPDATE 1 → DELETE 1 → UPDATE 1(이미 지움 = 0행) → 모든 행 age=5(남은 2행)
  EXPECT_EQ(t->modify({setAge(1, 10), del(1), setAge(1, 20), setAge(std::nullopt, 5)}),
            (std::vector<uint64_t>{1, 1, 0, 2}));
  const auto r = scanFile(tbl());
  EXPECT_EQ(r.frames, before + 1);  // 배치 전체가 프레임 하나
  EXPECT_EQ(r.records, 3u + 3u);    // id마다 최종 상태 하나: 묘비 1 + 새 버전 2
  EXPECT_FALSE(t->findById(1));
  EXPECT_EQ(ageOf(*t, 2), 5);
  EXPECT_EQ(ageOf(*t, 3), 5);

  const auto size = std::filesystem::file_size(tbl());
  EXPECT_EQ(t->modify({setAge(std::nullopt, 9, nameIs("nobody"))}), std::vector<uint64_t>{0});
  EXPECT_EQ(std::filesystem::file_size(tbl()), size);  // 맞는 행이 없으면 아무것도 쓰지 않음
}

TEST_F(TableTest, TornModifyFrameDropsTheWholeBatch) {
  {
    auto t = open();
    t->insert({user("a", 1), user("b", 2), user("c", 3)});
    t->modify({setAge(1, 50), del(2)});
  }
  auto bytes = readBytes(tbl());
  bytes.resize(bytes.size() - 3);
  writeBytes(tbl(), bytes);
  auto t = open();
  EXPECT_EQ(ageOf(*t, 1), 1);  // UPDATE도 DELETE도 반영 안 됨
  EXPECT_TRUE(t->findById(2));
  EXPECT_EQ(t->rowCount(), 3u);
  EXPECT_TRUE(std::filesystem::exists(dir / "users.tbl.torn"));
}

TEST_F(TableTest, MidCorruptionInModifyFrameRefusesToStart) {
  uint64_t at = 0;
  {
    auto t = open();
    t->insert({user("a", 1), user("b", 2)});
    at = t->fileSize();
    t->modify({setAge(1, 50), del(2)});
    t->insert({user("c", 3)});
  }
  auto bytes = readBytes(tbl());
  bytes[at + 12] ^= 0xFF;  // 수정 프레임 내용
  writeBytes(tbl(), bytes);
  EXPECT_THROW(open(), StorageError);
}

TEST_F(TableTest, ScannerRejectsDeleteOfMissingOrDeletedId) {
  Schema s = usersSchema();
  auto fileWith = [&](const std::vector<std::vector<Row>>& frames) {
    std::vector<uint8_t> file = frame::fileHeader();
    for (const auto& rows : frames) {
      const auto f = frame::encode(s, rows);
      file.insert(file.end(), f.begin(), f.end());
    }
    writeBytes(tbl(), file);
  };
  const Row a{Value{int64_t{1}}, Value{std::string("a")}, Value{int64_t{1}}};
  const Row tomb1{Value{int64_t{1}}};
  const Row tomb2{Value{int64_t{2}}};

  fileWith({{a}, {tomb1}});  // 정상: 넣고 지움
  auto r = scanFile(tbl());
  EXPECT_EQ(r.status, frame::ScanStatus::Ok);
  EXPECT_TRUE(r.live.empty());
  EXPECT_EQ(r.lastId, 1);

  fileWith({{a}, {tomb2}});
  EXPECT_EQ(scanFile(tbl()).why, "delete of a missing id 2");
  EXPECT_THROW(open(), StorageError);

  fileWith({{a}, {tomb1}, {tomb1}});
  EXPECT_EQ(scanFile(tbl()).why, "delete of a missing id 1");

  fileWith({{a}, {tomb1}, {a}});  // 지운 id의 새 버전
  EXPECT_EQ(scanFile(tbl()).why, "update of a missing id 1");
}

TEST_F(TableTest, InjectedWriteFailureDuringModifyLeavesStateUnchanged) {
  FaultyFileIO io;
  auto t = open(io);
  t->insert({user("a", 1), user("b", 2)});
  const auto size = std::filesystem::file_size(tbl());

  io.failWriteAfter = 1;
  EXPECT_THROW(t->modify({setAge(1, 50), del(2)}), StorageError);
  EXPECT_EQ(std::filesystem::file_size(tbl()), size);
  EXPECT_EQ(ageOf(*t, 1), 1);
  EXPECT_TRUE(t->findById(2));
  EXPECT_EQ(t->rowCount(), 2u);
  EXPECT_TRUE(t->validateIndex());

  io.failWriteAfter = 1;
  io.failTruncateAfter = 1;
  EXPECT_THROW(t->modify({del(1)}), StorageError);
  EXPECT_FALSE(t->writable());
  EXPECT_THROW(t->modify({del(1)}), StorageError);
  t.reset();
  auto again = open();  // 찢어진 수정 프레임은 꼬리 손상으로 정리
  EXPECT_EQ(again->rowCount(), 2u);
  EXPECT_EQ(ageOf(*again, 1), 1);
}

TEST_F(TableTest, TooManyChangesIsSchemaErrorAndWritesNothing) {
  auto t = Table::open(dir / "big.tbl", "big", Schema::parse("blob VARCHAR(60000)", "big"), defaultFileIO());
  std::vector<std::vector<Value>> half(40, {Value{std::string(60000, 'a')}});
  t->insert(half);
  t->insert(half);  // 80행 × 60 KB = 4.8 MB > 4 MiB
  const auto size = std::filesystem::file_size(dir / "big.tbl");
  Table::Edit all;
  all.set = {{1, Value{std::string(60000, 'b')}}};
  EXPECT_THROW(t->modify({all}), SchemaError);
  EXPECT_EQ(std::filesystem::file_size(dir / "big.tbl"), size);
  Table::Edit drop;
  drop.remove = true;
  EXPECT_EQ(t->modify({drop}), std::vector<uint64_t>{80});  // 묘비는 12 B라 들어간다
  EXPECT_EQ(t->rowCount(), 0u);
}

TEST_F(TableTest, ScanAfterScatteredUpdatesIsInIdOrder) {
  auto t = open();
  for (int b = 0; b < 10; ++b) {
    std::vector<std::vector<Value>> batch;
    for (int i = 0; i < 300; ++i) batch.push_back(user("u", b * 300 + i));
    t->insert(batch);
  }
  std::vector<Table::Edit> edits;
  for (int64_t id = 2; id <= 3000; id += 2) edits.push_back(setAge(id, -id));  // 짝수 id의 새 버전은 파일 끝
  t->modify(edits);
  t->modify({del(7), del(3000)});
  const auto rows = scanAll(*t);
  ASSERT_EQ(rows.size(), 2998u);
  int64_t prev = 0;
  for (const Row& r : rows) {
    const int64_t id = std::get<int64_t>(r[0]);
    ASSERT_GT(id, prev);
    EXPECT_EQ(std::get<int64_t>(r[2]), id % 2 == 0 ? -id : id - 1);
    prev = id;
  }
}

// pwriteAll 안에서 멈추는 FileIO. 쓰기가 진행 중인 순간을 테스트에서 붙잡는다.
class BlockingWriteIO : public PosixFileIO {
 public:
  std::atomic<bool> block{false};
  std::promise<void> entered;
  std::shared_future<void> release;
  void pwriteAll(int fd, const void* buf, size_t n, uint64_t off) override {
    if (block.exchange(false)) {
      entered.set_value();
      release.wait();
    }
    PosixFileIO::pwriteAll(fd, buf, n, off);
  }
};

TEST_F(TableTest, ReadersDoNotWaitForWriterIo) {
  // 쓰는 쪽이 pwrite 중이어도 읽는 쪽은 막히지 않는다. 아직 공개 안 된 행은 안 보인다 (decisions #19).
  BlockingWriteIO io;
  std::promise<void> releaseWriter;
  io.release = releaseWriter.get_future().share();
  auto t = open(io);
  t->insert({user("kim", 1)});

  io.block = true;
  auto entered = io.entered.get_future();
  std::thread writer([&] { t->insert({user("lee", 2)}); });
  entered.wait();  // 쓰는 쪽이 pwriteAll 안에서 멈춰 있다

  auto reader = std::async(std::launch::async, [&] {
    return std::make_tuple(t->findById(1).has_value(), t->rowCount(), scanAll(*t).size(), t->maxId());
  });
  const bool done = reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  releaseWriter.set_value();
  writer.join();

  ASSERT_TRUE(done) << "reader blocked behind the writer's pwrite";
  const auto [found, count, scanned, maxId] = reader.get();
  EXPECT_TRUE(found);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(scanned, 1u);
  EXPECT_EQ(maxId, 1);
  EXPECT_EQ(t->rowCount(), 2u);
  EXPECT_TRUE(t->findById(2).has_value());
}

TEST_F(TableTest, ShortFileGetsHeaderRewritten) {
  writeBytes(tbl(), {'M', 'D', 'B'});  // 헤더를 쓰다 끊긴 파일 (decisions #5)
  auto t = open();
  EXPECT_EQ(t->rowCount(), 0u);
  EXPECT_EQ(std::filesystem::file_size(tbl()), 8u);
  EXPECT_EQ(t->insert({user("kim", 1)}), std::vector<int64_t>{1});
}

// ---- Atomic writes --------------------------------------------------------------------------

TEST_F(TableTest, InjectedWriteFailureLeavesSizeAndIdsUnchanged) {
  FaultyFileIO io;
  auto t = open(io);
  t->insert({user("kim", 1)});
  const auto size = std::filesystem::file_size(tbl());

  io.failWriteAfter = 1;
  EXPECT_THROW(t->insert({user("lee", 2), user("park", 3)}), StorageError);
  EXPECT_EQ(std::filesystem::file_size(tbl()), size);
  EXPECT_EQ(t->maxId(), 1);
  EXPECT_EQ(t->rowCount(), 1u);
  EXPECT_EQ(t->insert({user("choi", 4)}), std::vector<int64_t>{2});  // 빈 번호 없음

  t.reset();
  auto again = open();
  EXPECT_EQ(again->rowCount(), 2u);
}

TEST_F(TableTest, FailedRollbackMakesTableReadOnlyAndRestartRecovers) {
  FaultyFileIO io;
  auto t = open(io);
  t->insert({user("kim", 1)});
  io.failWriteAfter = 1;
  io.failTruncateAfter = 1;
  EXPECT_THROW(t->insert({user("lee", 2)}), StorageError);
  EXPECT_FALSE(t->writable());
  EXPECT_THROW(t->insert({user("park", 3)}), StorageError);
  EXPECT_EQ(scanAll(*t).size(), 1u);  // 읽기는 fileSize까지 정상
  EXPECT_TRUE(t->findById(1));

  t.reset();
  auto again = open();  // 찢어진 꼬리는 꼬리 손상으로 정리된다 (decisions #4)
  EXPECT_EQ(again->rowCount(), 1u);
  EXPECT_TRUE(std::filesystem::exists(dir / "users.tbl.torn"));
  EXPECT_EQ(again->insert({user("park", 3)}), std::vector<int64_t>{2});
}

TEST_F(TableTest, ScanSnapshotIgnoresLaterInserts) {
  auto t = open();
  t->insert({user("a", 1)});
  auto cursor = t->scan();
  t->insert({user("b", 2)});
  Row r;
  int n = 0;
  while (cursor.next(r)) ++n;
  EXPECT_EQ(n, 1);
}

TEST_F(TableTest, LargeRecordsSpanReadChunks) {
  auto t = Table::open(dir / "big.tbl", "big", Schema::parse("blob VARCHAR(60000)", "big"), defaultFileIO());
  for (int i = 0; i < 5; ++i) t->insert({{Value{std::string(60000, static_cast<char>('a' + i))}}});
  auto rows = scanAll(*t);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(std::get<std::string>(rows[4][1]), std::string(60000, 'e'));
}

TEST_F(TableTest, MillionRowsIndexMatchesScan) {
  // debug 프리셋(ASan)은 종료 시 LeakSanitizer가 이 테스트 뒤에 ~60초를 쓴다. tsan·release 프리셋에서 돈다.
  if (std::getenv("MINIDB_SKIP_LARGE")) GTEST_SKIP() << "large test skipped (MINIDB_SKIP_LARGE)";
  auto t = open();
  std::vector<std::vector<Value>> batch;
  for (int b = 0; b < 1000; ++b) {
    batch.clear();
    for (int i = 0; i < 1000; ++i) batch.push_back(user("n" + std::to_string(b * 1000 + i), i));
    t->insert(batch);
  }
  ASSERT_EQ(t->rowCount(), 1000000u);
  EXPECT_TRUE(t->validateIndex());

  std::mt19937_64 rng(42);
  std::vector<int64_t> probes;
  for (int i = 0; i < 200; ++i) probes.push_back(static_cast<int64_t>(rng() % 1000000) + 1);
  std::sort(probes.begin(), probes.end());
  size_t p = 0;
  auto c = t->scan();
  Row r;
  while (c.next(r) && p < probes.size()) {
    while (p < probes.size() && probes[p] == std::get<int64_t>(r[0])) {
      EXPECT_EQ(*t->findById(probes[p]), r);
      ++p;
    }
  }
  EXPECT_EQ(p, probes.size());
  EXPECT_FALSE(t->findById(1000001));
}

// ---- Database -------------------------------------------------------------------------------

TEST(DatabaseTest, OpensAllSchemasAndRejectsUnknownTable) {
  TempDir dir;
  writeFile(dir / "users.schema", "name VARCHAR(8)\n");
  writeFile(dir / "items.schema", "price INT\n");
  auto db = Database::open(dir.path());
  EXPECT_EQ(db->tableNames(), (std::vector<std::string>{"items", "users"}));
  EXPECT_TRUE(std::filesystem::exists(dir / "users.tbl"));
  try {
    db->table("nope");
    FAIL();
  } catch (const SchemaError& e) {
    EXPECT_EQ(e.kind, SchemaError::Kind::UnknownTable);
  }
  EXPECT_THROW(Database::open(dir / "missing"), StorageError);
}
