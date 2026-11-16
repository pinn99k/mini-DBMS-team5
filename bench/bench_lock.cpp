// P3 벤치마크: 락 방식과 배치 크기에 따른 처리량 (architecture §10).
// 같은 소스를 두 번 빌드한다: minidb_bench_lock (shared_mutex) / minidb_bench_lock_exclusive (읽기도 unique_lock).
// HTTP를 빼고 Table API를 직접 불러 락 효과만 본다.
// minidb_bench_lock --dir /data/lockbench [--rows 100000] [--threads 1,2,4,8] [--ms 1500]

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "storage/database.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

namespace {

#ifdef MINIDB_EXCLUSIVE_LOCK
constexpr const char* kVariant = "exclusive";
#else
constexpr const char* kVariant = "shared";
#endif

std::vector<size_t> parseList(const std::string& s) {
  std::vector<size_t> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');) out.push_back(std::stoull(item));
  return out;
}

enum class Workload { ReadOnly, Mixed90_10, WriteSingle, WriteBatch100 };
const char* name(Workload w) {
  switch (w) {
    case Workload::ReadOnly: return "read-only (findById)";
    case Workload::Mixed90_10: return "mixed 90% read / 10% insert";
    case Workload::WriteSingle: return "insert 1 row";
    case Workload::WriteBatch100: return "insert 100-row batch";
  }
  return "";
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dir = "/data/lockbench";
  size_t rows = 100000, ms = 1500;
  std::vector<size_t> threads = {1, 2, 4, 8};
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i], v = argv[i + 1];
    if (a == "--dir") dir = v;
    else if (a == "--rows") rows = std::stoull(v);
    else if (a == "--threads") threads = parseList(v);
    else if (a == "--ms") ms = std::stoull(v);
  }
  dir /= kVariant;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "t.schema") << "name VARCHAR(32)\nage INT\n";

  auto db = Database::open(dir);
  Table& t = db->table("t");
  std::vector<std::vector<Value>> batch;
  for (size_t i = 0; i < rows; i += 1000) {
    batch.clear();
    for (size_t j = i; j < std::min(rows, i + 1000); ++j) batch.push_back({Value{std::string("seed")}, Value{int64_t{1}}});
    t.insert(batch);
  }
  const std::vector<std::vector<Value>> one = {{Value{std::string("w")}, Value{int64_t{2}}}};
  const std::vector<std::vector<Value>> hundred(100, one[0]);

  for (Workload w : {Workload::ReadOnly, Workload::Mixed90_10, Workload::WriteSingle, Workload::WriteBatch100}) {
    for (size_t n : threads) {
      std::atomic<bool> stop{false};
      std::atomic<uint64_t> ops{0}, rowsWritten{0};
      std::vector<std::thread> ts;
      for (size_t k = 0; k < n; ++k) {
        ts.emplace_back([&, k] {
          std::mt19937_64 rng(k + 1);
          uint64_t local = 0, written = 0;
          while (!stop.load(std::memory_order_relaxed)) {
            const bool write = w == Workload::WriteSingle || w == Workload::WriteBatch100 ||
                               (w == Workload::Mixed90_10 && rng() % 10 == 0);
            if (write) {
              written += t.insert(w == Workload::WriteBatch100 ? hundred : one).size();
            } else {
              (void)t.findById(static_cast<int64_t>(rng() % rows) + 1);
            }
            ++local;
          }
          ops += local;
          rowsWritten += written;
        });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      stop = true;
      for (auto& th : ts) th.join();
      const double secs = static_cast<double>(ms) / 1000.0;
      std::cout << kVariant << ',' << name(w) << ',' << n << ',' << static_cast<uint64_t>(ops / secs) << ','
                << static_cast<uint64_t>(rowsWritten / secs) << '\n';
    }
  }
  return 0;
}
