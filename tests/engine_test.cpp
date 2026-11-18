#include <gtest/gtest.h>

#include "engine/executor.h"
#include "sql/parser.h"
#include "testutil.h"

using namespace minidb;
using namespace minidb::test;

namespace {

class CollectSink : public RowSink {
 public:
  void begin(const std::vector<Column>& c) override {
    for (const auto& col : c) columns.push_back(col.name);
  }
  bool row(const Row& r) override {
    rows.push_back(r);
    return stopAfter == 0 || rows.size() < stopAfter;
  }
  void end(uint64_t n) override {
    ended = true;
    count = n;
  }
  std::vector<std::string> columns;
  std::vector<Row> rows;
  size_t stopAfter = 0;
  bool ended = false;
  uint64_t count = 0;
};

Statement parse(const std::string& sql) { return *Parser(sql).next(); }

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    writeFile(dir / "users.schema", "name VARCHAR(8)\nage INT\n");
    writeFile(dir / "items.schema", "label VARCHAR(4)\n");
    db = Database::open(dir.path(), io);
    exec = std::make_unique<Executor>(*db);
  }
  std::vector<int64_t> insert(const std::string& sql) { return exec->insert({std::get<InsertStmt>(parse(sql))}); }
  std::vector<uint64_t> modify(const std::vector<std::string>& sqls) {
    std::vector<Statement> batch;
    for (const auto& s : sqls) batch.push_back(parse(s));
    return exec->modify(batch);
  }
  CollectSink select(const std::string& sql) {
    CollectSink sink;
    exec->select(std::get<SelectStmt>(parse(sql)), sink);
    return sink;
  }
  SchemaError schemaError(const std::string& sql) {
    try {
      auto st = parse(sql);
      if (auto* i = std::get_if<InsertStmt>(&st)) exec->insert({*i});
      if (std::holds_alternative<SelectStmt>(st)) select(sql);
      if (std::holds_alternative<UpdateStmt>(st) || std::holds_alternative<DeleteStmt>(st)) exec->modify({st});
    } catch (const SchemaError& e) {
      return e;
    }
    ADD_FAILURE() << "expected SchemaError: " << sql;
    return SchemaError("none");
  }

  TempDir dir;
  FaultyFileIO io;
  std::unique_ptr<Database> db;
  std::unique_ptr<Executor> exec;
};

}  // namespace

TEST_F(EngineTest, InsertSelectRoundTripWithProjectionAndFilter) {
  EXPECT_EQ(insert("INSERT INTO users VALUES ('kim', 25)"), std::vector<int64_t>{1});
  EXPECT_EQ(insert("INSERT INTO users (age, name) VALUES (30, 'lee')"), std::vector<int64_t>{2});
  insert("INSERT INTO users VALUES ('kim', 40)");

  auto all = select("SELECT * FROM users");
  EXPECT_EQ(all.columns, (std::vector<std::string>{"id", "name", "age"}));
  ASSERT_EQ(all.rows.size(), 3u);
  EXPECT_EQ(all.rows[1], (Row{Value{int64_t{2}}, Value{std::string("lee")}, Value{int64_t{30}}}));
  EXPECT_TRUE(all.ended);

  auto kims = select("SELECT age, id FROM users WHERE name = 'kim'");
  EXPECT_EQ(kims.columns, (std::vector<std::string>{"age", "id"}));
  ASSERT_EQ(kims.rows.size(), 2u);
  EXPECT_EQ(kims.rows[1], (Row{Value{int64_t{40}}, Value{int64_t{3}}}));
  EXPECT_EQ(kims.count, 2u);

  auto byId = select("SELECT name FROM users WHERE id = 2");
  ASSERT_EQ(byId.rows.size(), 1u);
  EXPECT_EQ(byId.rows[0], (Row{Value{std::string("lee")}}));
  EXPECT_TRUE(select("SELECT * FROM users WHERE id = 99").rows.empty());
}

TEST_F(EngineTest, ValidationErrors) {
  EXPECT_EQ(schemaError("INSERT INTO nope VALUES (1)").kind, SchemaError::Kind::UnknownTable);
  EXPECT_EQ(schemaError("SELECT * FROM nope").kind, SchemaError::Kind::UnknownTable);
  schemaError("INSERT INTO users VALUES ('kim')");                       // 개수 불일치
  schemaError("INSERT INTO users (name) VALUES ('kim')");                // 컬럼 빠짐
  schemaError("INSERT INTO users (name, age) VALUES ('kim')");           // 컬럼·값 개수
  schemaError("INSERT INTO users VALUES (25, 'kim')");                   // 타입 불일치
  schemaError("INSERT INTO users VALUES ('waytoolong', 1)");             // VARCHAR 초과
  schemaError("INSERT INTO users (id, name, age) VALUES (1, 'a', 2)");   // id 지정
  schemaError("INSERT INTO users (name, name) VALUES ('a', 'b')");       // 중복
  schemaError("SELECT nope FROM users");
  schemaError("SELECT * FROM users WHERE age = 'x'");
  schemaError("SELECT * FROM users WHERE id = 'x'");
  SchemaError e = schemaError("INSERT INTO users VALUES ('kim', 'x')");
  ASSERT_TRUE(e.pos);
  EXPECT_EQ(e.pos->col, 34u);  // 문제 값의 위치
  EXPECT_EQ(db->table("users").rowCount(), 0u);
}

TEST_F(EngineTest, BatchIsAtomicAndSingleTable) {
  auto a = std::get<InsertStmt>(parse("INSERT INTO users VALUES ('a', 1)"));
  auto bad = std::get<InsertStmt>(parse("INSERT INTO users VALUES ('b', 'x')"));
  auto other = std::get<InsertStmt>(parse("INSERT INTO items VALUES ('z')"));
  EXPECT_THROW(exec->insert({a, a, bad}), SchemaError);
  EXPECT_EQ(db->table("users").rowCount(), 0u);
  EXPECT_THROW(exec->insert({a, other}), SchemaError);
  EXPECT_EQ(db->table("items").rowCount(), 0u);
  EXPECT_EQ(exec->insert({a, a, a}), (std::vector<int64_t>{1, 2, 3}));
}

TEST_F(EngineTest, UpdateAndDeleteThroughBothAccessPaths) {
  insert("INSERT INTO users VALUES ('kim', 25)");
  insert("INSERT INTO users VALUES ('lee', 30)");
  insert("INSERT INTO users VALUES ('kim', 40)");
  EXPECT_EQ(modify({"UPDATE users SET age = 26 WHERE id = 1"}), std::vector<uint64_t>{1});      // IndexScan
  EXPECT_EQ(modify({"UPDATE users SET name = 'park' WHERE name = 'kim'"}), std::vector<uint64_t>{2});  // FullScan
  EXPECT_EQ(modify({"DELETE FROM users WHERE id = 2", "UPDATE users SET age = 1 WHERE name = 'lee'"}),
            (std::vector<uint64_t>{1, 0}));  // 뒤 문장은 앞 문장의 결과를 본다
  auto all = select("SELECT * FROM users");
  ASSERT_EQ(all.rows.size(), 2u);
  EXPECT_EQ(all.rows[0], (Row{Value{int64_t{1}}, Value{std::string("park")}, Value{int64_t{26}}}));
  EXPECT_EQ(all.rows[1], (Row{Value{int64_t{3}}, Value{std::string("park")}, Value{int64_t{40}}}));
  EXPECT_EQ(modify({"UPDATE users SET age = 0"}), std::vector<uint64_t>{2});  // WHERE 없음 = 모든 행
  EXPECT_EQ(modify({"DELETE FROM users"}), std::vector<uint64_t>{2});
  EXPECT_EQ(db->table("users").rowCount(), 0u);
  EXPECT_EQ(insert("INSERT INTO users VALUES ('new', 1)"), std::vector<int64_t>{4});  // id 재사용 없음
}

TEST_F(EngineTest, UpdateDeleteValidationIsAtomic) {
  insert("INSERT INTO users VALUES ('kim', 25)");
  EXPECT_EQ(schemaError("UPDATE nope SET age = 1").kind, SchemaError::Kind::UnknownTable);
  EXPECT_EQ(schemaError("DELETE FROM nope").kind, SchemaError::Kind::UnknownTable);
  schemaError("UPDATE users SET id = 5");               // id는 못 바꿈
  schemaError("UPDATE users SET age = 1, age = 2");     // 같은 컬럼 두 번
  schemaError("UPDATE users SET nope = 1");
  schemaError("UPDATE users SET age = 'x'");            // 타입
  schemaError("UPDATE users SET name = 'waytoolong'");  // VARCHAR 초과
  schemaError("UPDATE users SET age = 1 WHERE name = 3");
  schemaError("DELETE FROM users WHERE nope = 1");
  SchemaError e = schemaError("UPDATE users SET age = 1, id = 2");
  ASSERT_TRUE(e.pos);
  EXPECT_EQ(e.pos->col, 27u);
  // 배치 중 하나라도 틀리면 아무것도 안 바뀜, 여러 테이블이면 에러
  EXPECT_THROW(modify({"DELETE FROM users", "UPDATE users SET age = 'x'"}), SchemaError);
  EXPECT_THROW(modify({"DELETE FROM users", "DELETE FROM items"}), SchemaError);
  EXPECT_EQ(db->table("users").rowCount(), 1u);
  EXPECT_THROW(exec->modify({parse("SELECT * FROM users")}), std::invalid_argument);
}

TEST_F(EngineTest, ExplainChoosesAccessPath) {
  auto ex = [&](const std::string& sql) { return exec->explain(std::get<ExplainStmt>(parse(sql)).select); };
  EXPECT_EQ(ex("EXPLAIN SELECT * FROM users WHERE id = 5"), "IndexScan(users, id=5)");
  EXPECT_EQ(ex("EXPLAIN SELECT * FROM users"), "FullScan(users)");
  EXPECT_EQ(ex("EXPLAIN SELECT * FROM users WHERE name = 'o''k'"), "FullScan(users, filter name='o''k')");
  EXPECT_EQ(ex("EXPLAIN SELECT id FROM users WHERE age = -3"), "FullScan(users, filter age=-3)");
}

TEST_F(EngineTest, OutputStartsBeforeScanFinishes) {
  // 64 KiB 읽기 단위보다 훨씬 큰 테이블을 만든다
  std::vector<InsertStmt> batch;
  for (int i = 0; i < 1000; ++i) batch.push_back(std::get<InsertStmt>(parse("INSERT INTO users VALUES ('abcdefgh', 1)")));
  for (int f = 0; f < 20; ++f) exec->insert(batch);

  class FirstRowProbe : public RowSink {
   public:
    explicit FirstRowProbe(FaultyFileIO& io) : io_(io) {}
    void begin(const std::vector<Column>&) override { readsAtBegin = io_.reads.load(); }
    bool row(const Row&) override {
      if (rows++ == 0) readsAtFirstRow = io_.reads.load();
      return true;
    }
    void end(uint64_t) override { readsAtEnd = io_.reads.load(); }
    FaultyFileIO& io_;
    uint64_t readsAtBegin = 0, readsAtFirstRow = 0, readsAtEnd = 0;
    size_t rows = 0;
  } probe(io);
  exec->select(std::get<SelectStmt>(parse("SELECT * FROM users")), probe);
  EXPECT_EQ(probe.rows, 20000u);
  EXPECT_EQ(probe.readsAtFirstRow - probe.readsAtBegin, 1u);  // 첫 행은 첫 읽기 직후
  EXPECT_GT(probe.readsAtEnd - probe.readsAtBegin, 5u);
}

TEST_F(EngineTest, SinkReturningFalseStopsTheScan) {
  for (int i = 0; i < 10; ++i) insert("INSERT INTO users VALUES ('x', 1)");
  CollectSink sink;
  sink.stopAfter = 3;
  exec->select(std::get<SelectStmt>(parse("SELECT * FROM users")), sink);
  EXPECT_EQ(sink.rows.size(), 3u);
  EXPECT_FALSE(sink.ended);  // 멈추면 end()는 부르지 않는다
}

TEST_F(EngineTest, ReadFailureMidScanPropagates) {
  for (int i = 0; i < 5; ++i) insert("INSERT INTO users VALUES ('x', 1)");
  io.failReadAfter = 1;
  CollectSink sink;
  EXPECT_THROW(exec->select(std::get<SelectStmt>(parse("SELECT * FROM users")), sink), StorageError);
  EXPECT_FALSE(sink.ended);
}
