#include "server/serve.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "common/errors.h"
#include "storage/database.h"

namespace minidb {
namespace {

std::atomic<bool> gStop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler needs a lock-free flag");

extern "C" void onSignal(int) { gStop.store(true); }  // 핸들러는 플래그만 세운다

}  // namespace

int runServe(const std::filesystem::path& dataDir, const ServerConfig& cfg) {
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  std::signal(SIGPIPE, SIG_IGN);

  std::unique_ptr<Database> db;
  try {
    db = Database::open(dataDir);
  } catch (const DbError& e) {
    std::cerr << "minidb: " << e.typeName() << ": " << e.what() << '\n';
    return 2;
  }

  HttpServer server(*db, cfg);
  if (!server.bind()) {
    std::cerr << "minidb: cannot bind " << cfg.host << ':' << cfg.port << '\n';
    return 2;
  }
  std::thread listener([&] { server.run(); });
  server.waitUntilReady();
  std::cerr << "minidb: listening on " << cfg.host << ':' << server.port() << " (workers=" << cfg.workers
            << ", queue=" << cfg.queue << ", data=" << dataDir.string() << ")\n";

  // 1초마다 fsync. 실패하면 재시도하지 않고 서버를 멈춘다 (fail-stop).
  std::atomic<bool> fatal{false};
  std::mutex syncMu;
  std::condition_variable syncCv;
  bool stopSync = false;
  std::thread syncer([&] {
    std::unique_lock<std::mutex> lock(syncMu);
    while (!syncCv.wait_for(lock, std::chrono::seconds(1), [&] { return stopSync; })) {
      try {
        db->syncAll();
      } catch (const DbError& e) {
        std::cerr << "minidb: fatal: periodic fsync failed: " << e.what() << '\n';
        fatal.store(true);
        gStop.store(true);
        return;
      }
    }
  });

  while (!gStop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 종료 순서: 새 연결 거절 → 큐·처리 중 작업 비우기 → fsync → 닫기
  std::cerr << "minidb: shutting down\n";
  server.stop();
  listener.join();  // listen이 끝나며 task queue shutdown(큐 비우기 + join)까지 마친다
  {
    std::lock_guard<std::mutex> lock(syncMu);
    stopSync = true;
  }
  syncCv.notify_all();
  syncer.join();
  try {
    db->syncAll();
  } catch (const DbError& e) {
    std::cerr << "minidb: final fsync failed: " << e.what() << '\n';
    return 1;
  }
  std::cerr << "minidb: stopped\n";
  return fatal.load() ? 1 : 0;
}

}  // namespace minidb
