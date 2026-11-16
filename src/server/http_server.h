#pragma once

// cpp-httplib 위의 HTTP 계층. httplib의 기본 풀을 new_task_queue로 우리 ThreadPool로 바꾼다.
//
// 503 처리 (decisions #12): httplib 0.14의 TaskQueue::enqueue는 void라서 거절 신호가 없고,
// 신버전도 거절하면 응답 없이 소켓만 닫는다. 그래서 큐가 가득 차면 연결을 "거절 레인"(작은 전용 스레드)
// 으로 보내고, 그 스레드에서는 pre-routing 핸들러가 즉시 503 + Connection: close로 답한다.

#include <atomic>
#include <cstddef>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

#include "engine/executor.h"
#include "server/query_handler.h"

namespace httplib {
class Server;
}

namespace minidb {

struct ServerConfig {
  std::string host = "0.0.0.0";
  int port = 8080;  // 0이면 아무 빈 포트
  size_t workers = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 4;
  size_t queue = 1024;
  time_t readTimeoutSec = 5;
  time_t writeTimeoutSec = 5;
  size_t maxBodyBytes = 1024 * 1024;
};

class HttpServer {
 public:
  HttpServer(const Database& db, ServerConfig cfg);
  ~HttpServer();

  bool bind();  // 성공하면 port()가 실제 포트
  int port() const { return port_; }
  void run();   // 블록. stop() 후 큐와 처리 중인 작업이 끝나면 돌아온다
  void stop();
  void waitUntilReady() const;
  uint64_t rejectedCount() const { return rejected_->load(); }

 private:
  ServerConfig cfg_;
  Executor exec_;
  QueryHandler handler_;
  std::unique_ptr<httplib::Server> svr_;
  std::shared_ptr<std::atomic<uint64_t>> rejected_;
  int port_ = -1;
};

}  // namespace minidb
