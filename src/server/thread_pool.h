#pragma once

// 고정 워커 + 크기 제한 큐. 가득 차면 submit이 false (서버는 503). shutdown은 큐를 비운 뒤 join.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace minidb {

class ThreadPool {
 public:
  ThreadPool(size_t workers, size_t queueCapacity);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // 받아들이면 fn을 옮겨 가고 true. 큐가 가득 찼거나 종료 중이면 fn을 건드리지 않고 false.
  bool submit(std::function<void()>&& fn);
  void shutdown();  // 새 작업 거절 → 남은 작업을 끝까지 실행 → 워커 join. 여러 번 불러도 된다.

  size_t queued() const;

 private:
  void workerLoop();

  const size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace minidb
