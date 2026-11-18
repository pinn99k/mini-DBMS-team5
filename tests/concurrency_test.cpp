// tsan 프리셋에서 데이터 레이스를 잡는 테스트들. debug(ASan)에서도 정확성 검사로 돈다.

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>

#include "engine/executor.h"
#include "server/thread_pool.h"
#include "sql/parser.h"
#include "testutil.h"

using namespace minidb;
using namespace minidb::test;

namespace {

InsertStmt insertStmt(const std::string& sql) { return std::get<InsertStmt>(*Parser(sql).next()); }

class CountingSink : public RowSink {
 public:
  void begin(const std::vector<Column>&) override {}
  bool row(const Row&) override { return true; }
  void end(uint64_t n) override { count = n; }
  uint64_t count = 0;
};

class ConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    writeFile(dir / "t.schema", "name VARCHAR(16)\nn INT\n");
    db = Database::open(dir.path());
    exec = std::make_unique<Executor>(*db);
  }
  TempDir dir;
  std::unique_ptr<Database> db;
  std::unique_ptr<Executor> exec;
};

}  // namespace

TEST_F(ConcurrencyTest, ParallelInsertsGetUniqueIds) {
  constexpr int kThreads = 8, kPerThread = 300;
  std::vector<std::vector<int64_t>> ids(kThreads);
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      auto stmt = insertStmt("INSERT INTO t VALUES ('w" + std::to_string(t) + "', 1)");
      for (int i = 0; i < kPerThread; ++i) ids[t].push_back(exec->insert({stmt}).front());
    });
  }
  for (auto& th : ts) th.join();

  std::set<int64_t> all;
  for (const auto& v : ids) all.insert(v.begin(), v.end());
  EXPECT_EQ(all.size(), static_cast<size_t>(kThreads * kPerThread));
  EXPECT_EQ(*all.begin(), 1);
  EXPECT_EQ(*all.rbegin(), kThreads * kPerThread);
  Table& table = db->table("t");
  EXPECT_EQ(table.rowCount(), static_cast<uint64_t>(kThreads * kPerThread));
  std::string why;
  EXPECT_TRUE(table.validateIndex(&why)) << why;

  db.reset();  // 재시작 후에도 그대로
  db = Database::open(dir.path());
  EXPECT_EQ(db->table("t").rowCount(), static_cast<uint64_t>(kThreads * kPerThread));
}

TEST_F(ConcurrencyTest, ConcurrentScansNeverSeePartialBatches) {
  constexpr int kBatch = 7;
  std::atomic<bool> stop{false};
  std::atomic<int> badScans{0}, scans{0}, lookups{0};
  auto stmt = insertStmt("INSERT INTO t VALUES ('batchrow', 2)");
  const std::vector<InsertStmt> batch(kBatch, stmt);
  auto select = std::get<SelectStmt>(*Parser("SELECT * FROM t").next());

  std::vector<std::thread> ts;
  for (int w = 0; w < 3; ++w) {
    ts.emplace_back([&] {
      for (int i = 0; i < 150; ++i) exec->insert(batch);
    });
  }
  for (int r = 0; r < 3; ++r) {
    ts.emplace_back([&] {
      while (!stop.load()) {
        CountingSink sink;
        exec->select(select, sink);
        if (sink.count % kBatch != 0) ++badScans;
        ++scans;
      }
    });
  }
  ts.emplace_back([&] {
    auto byId = std::get<SelectStmt>(*Parser("SELECT * FROM t WHERE id = 1").next());
    while (!stop.load()) {
      CountingSink sink;
      exec->select(byId, sink);
      ++lookups;
    }
  });
  for (int w = 0; w < 3; ++w) ts[static_cast<size_t>(w)].join();
  stop = true;
  for (size_t i = 3; i < ts.size(); ++i) ts[i].join();

  EXPECT_EQ(badScans.load(), 0) << "of " << scans.load() << " scans";
  EXPECT_GT(scans.load(), 0);
  EXPECT_EQ(db->table("t").rowCount(), 3u * 150u * kBatch);
  EXPECT_TRUE(db->table("t").validateIndex());
}

// UPDATE·DELETE (decisions #21 #24) ---------------------------------------------------------

TEST_F(ConcurrencyTest, ConcurrentUpdatesOfDifferentColumnsLoseNothing) {
  // 레코드는 행 전체를 쓴다. 대상 찾기가 writeMu_ 밖이면 한쪽 변경이 다른 쪽에 덮인다(잃어버린 갱신).
  writeFile(dir / "p.schema", "a INT\nb INT\n");
  db.reset();
  db = Database::open(dir.path());
  Table& p = db->table("p");
  p.insert({{Value{int64_t{0}}, Value{int64_t{0}}}});
  constexpr int kIters = 400;
  auto bump = [&](size_t col) {
    for (int i = 1; i <= kIters; ++i) {
      Table::Edit e;
      e.id = 1;
      e.set = {{col, Value{int64_t{i}}}};
      ASSERT_EQ(p.modify({e}), std::vector<uint64_t>{1});
    }
  };
  std::thread ta(bump, 1), tb(bump, 2);
  ta.join();
  tb.join();
  const Row r = *p.findById(1);
  EXPECT_EQ(std::get<int64_t>(r[1]), kIters);
  EXPECT_EQ(std::get<int64_t>(r[2]), kIters);
}

TEST_F(ConcurrencyTest, ScansDuringUpdatesAndDeletesSeeEachIdOnceInOrder) {
  Table& t = db->table("t");
  std::vector<std::vector<Value>> rows(2000, {Value{std::string("r")}, Value{int64_t{0}}});
  t.insert(rows);
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0}, scans{0};

  std::thread writer([&] {
    for (int64_t i = 1; i <= 100; ++i) {
      Table::Edit up;  // 여러 행 UPDATE (필터 경로)
      up.filter = Predicate{2, Value{i - 1}};
      up.set = {{2, Value{i}}};
      Table::Edit gone;
      gone.id = i * 5;
      gone.remove = true;
      t.modify({up, gone});
      t.insert({{Value{std::string("new")}, Value{int64_t{-1}}}});
    }
  });
  std::vector<std::thread> readers;
  for (int k = 0; k < 2; ++k) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto c = t.scan();
        Row r;
        int64_t prev = 0;
        uint64_t n = 0;
        while (c.next(r)) {
          const int64_t id = std::get<int64_t>(r[0]);
          if (id <= prev) ++bad;  // 같은 id 두 번 또는 순서 뒤집힘
          prev = id;
          ++n;
        }
        if (n > 2300) ++bad;
        ++scans;
      }
    });
  }
  writer.join();
  stop = true;
  for (auto& th : readers) th.join();

  EXPECT_EQ(bad.load(), 0) << "of " << scans.load() << " scans";
  EXPECT_GT(scans.load(), 0);
  EXPECT_EQ(t.rowCount(), 2000u - 100u + 100u);
  std::string why;
  EXPECT_TRUE(t.validateIndex(&why)) << why;
  db.reset();  // 재시작 후 같은 상태
  db = Database::open(dir.path());
  EXPECT_EQ(db->table("t").rowCount(), 2000u);
  EXPECT_FALSE(db->table("t").findById(5));
}

TEST(ThreadPoolTest, RunsTasksAndRejectsWhenFull) {
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  std::atomic<int> ran{0};
  ThreadPool pool(1, 2);
  auto blocker = [&] {
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [&] { return release; });
    ++ran;
  };
  ASSERT_TRUE(pool.submit(blocker));  // 워커가 잡는다
  while (pool.queued() != 0) std::this_thread::yield();
  EXPECT_TRUE(pool.submit([&] { ++ran; }));
  EXPECT_TRUE(pool.submit([&] { ++ran; }));
  std::function<void()> extra = [&] { ++ran; };
  EXPECT_FALSE(pool.submit(std::move(extra)));  // 큐 2칸이 가득
  EXPECT_TRUE(static_cast<bool>(extra));          // 거절되면 fn은 그대로
  {
    std::lock_guard<std::mutex> l(m);
    release = true;
  }
  cv.notify_all();
  pool.shutdown();  // 남은 작업까지 실행
  EXPECT_EQ(ran.load(), 3);
  EXPECT_FALSE(pool.submit([] {}));  // 종료 후 거절
}

TEST(ThreadPoolTest, ShutdownDrainsQueue) {
  std::atomic<int> ran{0};
  {
    ThreadPool pool(2, 1000);
    for (int i = 0; i < 500; ++i) ASSERT_TRUE(pool.submit([&] { ++ran; }));
  }  // 소멸자 = shutdown
  EXPECT_EQ(ran.load(), 500);
}
