#include "server/http_server.h"

#include <httplib.h>

#include "server/ndjson_sink.h"
#include "server/thread_pool.h"

namespace minidb {
namespace {

thread_local bool tlRejecting = false;

constexpr size_t kRejectLaneWorkers = 1;
constexpr size_t kRejectLaneQueue = 4096;

// httplib::TaskQueue 어댑터: 정상 경로는 ThreadPool, 가득 차면 거절 레인.
class PoolTaskQueue : public httplib::TaskQueue {
 public:
  PoolTaskQueue(size_t workers, size_t queue, std::shared_ptr<std::atomic<uint64_t>> rejected)
      : pool_(workers, queue), rejectLane_(kRejectLaneWorkers, kRejectLaneQueue), rejected_(std::move(rejected)) {}

  void enqueue(std::function<void()> fn) override {
    if (pool_.submit(std::move(fn))) return;  // 거절되면 fn은 그대로 남아 있다
    rejected_->fetch_add(1);
    std::function<void()> rejectTask = [fn = std::move(fn)] {
      tlRejecting = true;
      fn();
      tlRejecting = false;
    };
    if (rejectLane_.submit(std::move(rejectTask))) return;
    // 거절 레인까지 가득: 소켓을 새게 둘 수는 없으므로 accept 스레드에서 바로 503을 보낸다
    tlRejecting = true;
    rejectTask();
    tlRejecting = false;
  }

  void shutdown() override {
    pool_.shutdown();
    rejectLane_.shutdown();
  }

 private:
  ThreadPool pool_;
  ThreadPool rejectLane_;
  std::shared_ptr<std::atomic<uint64_t>> rejected_;
};

const char* errorTypeFor(int status) {
  switch (status) {
    case 400: return "BadRequest";
    case 404: return "NotFound";
    case 405: return "MethodNotAllowed";
    case 413: return "PayloadTooLarge";
    case 503: return "Overloaded";
    default: return "HttpError";
  }
}

}  // namespace

HttpServer::HttpServer(const Database& db, ServerConfig cfg)
    : cfg_(std::move(cfg)),
      exec_(db),
      handler_(exec_),
      svr_(std::make_unique<httplib::Server>()),
      rejected_(std::make_shared<std::atomic<uint64_t>>(0)) {
  const size_t workers = cfg_.workers;
  const size_t queue = cfg_.queue;
  auto rejected = rejected_;
  svr_->new_task_queue = [workers, queue, rejected] { return new PoolTaskQueue(workers, queue, rejected); };

  svr_->set_read_timeout(cfg_.readTimeoutSec, 0);
  svr_->set_write_timeout(cfg_.writeTimeoutSec, 0);
  svr_->set_payload_max_length(cfg_.maxBodyBytes);
  svr_->set_keep_alive_max_count(100);
  // 응답을 헤더·본문으로 나눠 쓰므로 Nagle + 지연 ACK가 요청마다 ~40 ms를 더한다 (실측). 끈다.
  svr_->set_tcp_nodelay(true);

  svr_->set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
    if (!tlRejecting) return httplib::Server::HandlerResponse::Unhandled;
    res.status = 503;
    res.set_header("Connection", "close");
    res.set_content(errorJson("Overloaded", "server queue is full, retry later"), "application/json");
    return httplib::Server::HandlerResponse::Handled;
  });

  svr_->Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"ok\":true}", "application/json");
  });
  svr_->Post("/query", [this](const httplib::Request& req, httplib::Response& res) { handler_.handle(req, res); });

  // 라우팅 밖에서 난 에러(404 경로, 413 바디 초과 등)도 JSON으로
  svr_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) {
      res.set_content(errorJson(errorTypeFor(res.status), httplib::status_message(res.status)), "application/json");
    }
  });
}

HttpServer::~HttpServer() = default;

bool HttpServer::bind() {
  if (cfg_.port == 0) {
    port_ = svr_->bind_to_any_port(cfg_.host);
    return port_ > 0;
  }
  if (!svr_->bind_to_port(cfg_.host, cfg_.port)) return false;
  port_ = cfg_.port;
  return true;
}

void HttpServer::run() { svr_->listen_after_bind(); }

void HttpServer::stop() { svr_->stop(); }

void HttpServer::waitUntilReady() const { svr_->wait_until_ready(); }

}  // namespace minidb
