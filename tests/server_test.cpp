#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <sstream>
#include <thread>

#include "server/http_server.h"
#include "testutil.h"

using namespace minidb;
using namespace minidb::test;

namespace {

std::vector<std::string> lines(const std::string& body) {
  std::vector<std::string> out;
  std::istringstream in(body);
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

int rawConnect(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

class ServerTest : public ::testing::Test {
 protected:
  void start(ServerConfig cfg = {}) {
    writeFile(dir / "users.schema", "name VARCHAR(16)\nage INT\n");
    db = Database::open(dir.path(), io);
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    server = std::make_unique<HttpServer>(*db, cfg);
    ASSERT_TRUE(server->bind());
    thread = std::thread([this] { server->run(); });
    server->waitUntilReady();
  }
  void TearDown() override {
    if (server) {
      server->stop();
      thread.join();
    }
  }
  httplib::Result post(const std::string& body) {
    httplib::Client cli("127.0.0.1", server->port());
    cli.set_read_timeout(10, 0);
    return cli.Post("/query", body, "text/plain");
  }

  TempDir dir;
  FaultyFileIO io;
  std::unique_ptr<Database> db;
  std::unique_ptr<HttpServer> server;
  std::thread thread;
};

}  // namespace

TEST_F(ServerTest, HealthAndUnknownRoute) {
  start();
  httplib::Client cli("127.0.0.1", server->port());
  auto h = cli.Get("/health");
  ASSERT_TRUE(h);
  EXPECT_EQ(h->status, 200);
  EXPECT_EQ(h->body, "{\"ok\":true}");
  auto nf = cli.Get("/nope");
  ASSERT_TRUE(nf);
  EXPECT_EQ(nf->status, 404);
  EXPECT_NE(nf->body.find("\"ok\":false"), std::string::npos);
}

TEST_F(ServerTest, InsertBatchSelectAndExplain) {
  start();
  auto ins = post("INSERT INTO users VALUES ('kim', 25); INSERT INTO users VALUES ('lee', 30)");
  ASSERT_TRUE(ins);
  EXPECT_EQ(ins->status, 200);
  EXPECT_EQ(ins->body, "{\"ok\":true,\"insertedIds\":[1,2]}");

  auto sel = post("SELECT * FROM users");
  ASSERT_TRUE(sel);
  EXPECT_EQ(sel->status, 200);
  EXPECT_EQ(sel->get_header_value("Content-Type"), "application/x-ndjson");
  EXPECT_EQ(lines(sel->body), (std::vector<std::string>{"{\"columns\":[\"id\",\"name\",\"age\"]}", "[1,\"kim\",25]",
                                                       "[2,\"lee\",30]", "{\"ok\":true,\"rowCount\":2}"}));

  auto one = post("SELECT name FROM users WHERE id = 2;");
  ASSERT_TRUE(one);
  EXPECT_EQ(lines(one->body)[1], "[\"lee\"]");

  auto ex = post("EXPLAIN SELECT * FROM users WHERE id = 2");
  ASSERT_TRUE(ex);
  EXPECT_EQ(ex->body, "{\"ok\":true,\"plan\":\"IndexScan(users, id=2)\"}");
}

TEST_F(ServerTest, RequestShapeAndErrorStatuses) {
  start();
  auto status = [&](const std::string& body) {
    auto r = post(body);
    return r ? r->status : -1;
  };
  EXPECT_EQ(status(""), 400);
  EXPECT_EQ(status(" ;; "), 400);
  EXPECT_EQ(status("SELECT * FROM users; INSERT INTO users VALUES ('a', 1)"), 400);
  EXPECT_EQ(status("SELECT * FROM users; SELECT * FROM users"), 400);
  std::string many;
  for (int i = 0; i < 1001; ++i) many += "INSERT INTO users VALUES ('a', 1);";
  EXPECT_EQ(status(many), 400);
  EXPECT_EQ(db->table("users").rowCount(), 0u);  // 아무것도 실행 안 됨

  EXPECT_EQ(status("SELECT * FROM nope"), 404);
  EXPECT_EQ(status("INSERT INTO nope VALUES (1)"), 404);
  EXPECT_EQ(status("INSERT INTO users VALUES ('a', 'x')"), 400);

  auto pe = post("SELECT * FROM users WHERE");
  ASSERT_TRUE(pe);
  EXPECT_EQ(pe->status, 400);
  EXPECT_NE(pe->body.find("\"type\":\"ParseError\""), std::string::npos);
  EXPECT_NE(pe->body.find("\"line\":1,\"col\":26"), std::string::npos);

  // 1000개짜리 배치는 허용, 하나라도 틀리면 전체 거절
  std::string thousand;
  for (int i = 0; i < 1000; ++i) thousand += "INSERT INTO users VALUES ('a', 1);";
  EXPECT_EQ(status(thousand), 200);
  EXPECT_EQ(status(thousand.substr(0, thousand.size() - 34) + "INSERT INTO users VALUES ('a', 'bad');"), 400);
  EXPECT_EQ(db->table("users").rowCount(), 1000u);
}

TEST_F(ServerTest, BodyTooLargeIs413) {
  start();
  auto r = post(std::string(1024 * 1024 + 10, ' '));
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 413);
}

TEST_F(ServerTest, InjectedWriteFailureIs500AndNothingApplied) {
  start();
  ASSERT_EQ(post("INSERT INTO users VALUES ('a', 1)")->status, 200);
  const auto size = db->table("users").fileSize();
  io.failWriteAfter = 1;
  auto r = post("INSERT INTO users VALUES ('b', 2); INSERT INTO users VALUES ('c', 3)");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 500);
  EXPECT_EQ(db->table("users").fileSize(), size);
  EXPECT_EQ(post("INSERT INTO users VALUES ('d', 4)")->body, "{\"ok\":true,\"insertedIds\":[2]}");
}

TEST_F(ServerTest, ReadFailureMidStreamEndsWithOkFalse) {
  start();
  std::string batch;
  for (int i = 0; i < 1000; ++i) batch += "INSERT INTO users VALUES ('abcdefghijklmnop', 1);";
  for (int i = 0; i < 10; ++i) ASSERT_EQ(post(batch)->status, 200);
  io.failReadAfter = 3;  // 몇 번 읽은 뒤 실패
  auto r = post("SELECT * FROM users");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);  // 이미 스트리밍이 시작됨
  auto ls = lines(r->body);
  ASSERT_GT(ls.size(), 2u);
  EXPECT_EQ(ls.front().rfind("{\"columns\"", 0), 0u);
  EXPECT_NE(ls.back().find("\"ok\":false"), std::string::npos) << ls.back();
  EXPECT_NE(ls.back().find("StorageError"), std::string::npos);
}

TEST_F(ServerTest, QueueFullReturns503) {
  ServerConfig cfg;
  cfg.workers = 1;
  cfg.queue = 1;
  cfg.readTimeoutSec = 3;
  start(cfg);
  // 연결 A: 워커가 요청을 기다리며 붙잡힌다. 연결 B: 큐 한 칸을 채운다.
  int a = rawConnect(server->port());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  int b = rawConnect(server->port());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_GE(a, 0);
  ASSERT_GE(b, 0);

  const auto t0 = std::chrono::steady_clock::now();
  httplib::Client cli("127.0.0.1", server->port());
  cli.set_read_timeout(10, 0);
  auto r = cli.Get("/health");
  const auto waited = std::chrono::steady_clock::now() - t0;
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 503);
  EXPECT_NE(r->body.find("Overloaded"), std::string::npos);
  EXPECT_LT(waited, std::chrono::seconds(2));  // 큐에서 기다리지 않고 바로 거절
  EXPECT_GE(server->rejectedCount(), 1u);
  ::close(a);
  ::close(b);
}

TEST_F(ServerTest, ConcurrentClientsInsertAndRead) {
  ServerConfig cfg;
  cfg.workers = 4;
  start(cfg);
  constexpr int kClients = 8, kRequests = 40;
  std::atomic<int> failures{0};
  std::vector<std::thread> ts;
  for (int c = 0; c < kClients; ++c) {
    ts.emplace_back([&] {
      httplib::Client cli("127.0.0.1", server->port());
      cli.set_keep_alive(true);
      cli.set_read_timeout(30, 0);
      cli.set_tcp_nodelay(true);
      auto check = [&](const httplib::Result& r) {
        if (r && r->status == 200) return;
        if (failures++ == 0) {
          std::cerr << "first failure: " << (r ? "status " + std::to_string(r->status) + " " + r->body
                                               : "error " + httplib::to_string(r.error()))
                    << '\n';
        }
      };
      for (int i = 0; i < kRequests; ++i) {
        check(cli.Post("/query", "INSERT INTO users VALUES ('c', 1); INSERT INTO users VALUES ('d', 2)", "text/plain"));
        check(cli.Post("/query", "SELECT * FROM users WHERE id = 1", "text/plain"));
      }
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(db->table("users").rowCount(), static_cast<uint64_t>(kClients * kRequests * 2));
  EXPECT_TRUE(db->table("users").validateIndex());
}
