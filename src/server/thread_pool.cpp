#include "server/thread_pool.h"

#include <iostream>

namespace minidb {

ThreadPool::ThreadPool(size_t workers, size_t queueCapacity) : capacity_(queueCapacity) {
  workers_.reserve(workers);
  for (size_t i = 0; i < workers; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()>&& fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stopping_ || queue_.size() >= capacity_) return false;
    queue_.push_back(std::move(fn));
  }
  cv_.notify_one();
  return true;
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

size_t ThreadPool::queued() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

void ThreadPool::workerLoop() {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stopping_ && 큐가 빔
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      task();
    } catch (const std::exception& e) {
      std::cerr << "minidb: worker task threw: " << e.what() << '\n';
    } catch (...) {
      std::cerr << "minidb: worker task threw an unknown exception\n";
    }
  }
}

}  // namespace minidb
