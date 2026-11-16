// P2 벤치마크: id 인덱스 조회 vs 다른 컬럼 전체 스캔 (architecture §10).
// minidb_bench --dir /data/bench [--rows 10000,100000,1000000] [--lookups 1000] [--scans 5] [--csv out.csv]
// 데이터는 엔진 API(Table::insert, 1000행 배치)로 만든다. 조회는 Executor 경로(바인딩·계획 포함)로 잰다.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "engine/executor.h"
#include "sql/parser.h"

using namespace minidb;
using Clock = std::chrono::steady_clock;

namespace {

class NullSink : public RowSink {
 public:
  void begin(const std::vector<Column>&) override {}
  bool row(const Row&) override {
    ++rows;
    return true;
  }
  void end(uint64_t) override {}
  uint64_t rows = 0;
};

double us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

double percentile(std::vector<double> v, double p) {
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

SelectStmt parseSelect(const std::string& sql) { return std::get<SelectStmt>(*Parser(sql).next()); }

std::vector<size_t> parseList(const std::string& s) {
  std::vector<size_t> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');) out.push_back(std::stoull(item));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dir = "/data/bench";
  std::vector<size_t> sizes = {10000, 100000, 1000000};
  size_t lookups = 1000, scans = 5;
  std::string csvPath;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i], v = argv[i + 1];
    if (a == "--dir") dir = v;
    else if (a == "--rows") sizes = parseList(v);
    else if (a == "--lookups") lookups = std::stoull(v);
    else if (a == "--scans") scans = std::stoull(v);
    else if (a == "--csv") csvPath = v;
  }

  std::ostringstream csv;
  csv << "rows,insert_s,file_mb,open_ms,index_median_us,index_p99_us,scan_median_ms,scan_rows,speedup\n";
  std::cout << "| rows | insert (s) | file (MB) | open+rebuild (ms) | id lookup median (µs) | p99 (µs) | "
               "full scan median (ms) | speedup |\n|---:|---:|---:|---:|---:|---:|---:|---:|\n";

  for (size_t n : sizes) {
    const auto d = dir / ("rows_" + std::to_string(n));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    std::ofstream(d / "t.schema") << "name VARCHAR(32)\nage INT\n";

    // 1. 데이터 생성: 1000행 배치
    auto t0 = Clock::now();
    {
      auto db = Database::open(d);
      Table& t = db->table("t");
      std::vector<std::vector<Value>> batch;
      for (size_t i = 0; i < n; i += 1000) {
        batch.clear();
        for (size_t j = i; j < std::min(n, i + 1000); ++j) {
          batch.push_back({Value{"name_" + std::to_string(j + 1)}, Value{static_cast<int64_t>(j % 100)}});
        }
        t.insert(batch);
      }
      db->syncAll();
    }
    const double insertS = us(Clock::now() - t0) / 1e6;
    const double fileMb = static_cast<double>(std::filesystem::file_size(d / "t.tbl")) / (1024.0 * 1024.0);

    // 2. 재시작: 복구 스캔 + 벌크 로드
    t0 = Clock::now();
    auto db = Database::open(d);
    const double openMs = us(Clock::now() - t0) / 1e3;
    Executor exec(*db);

    // 3. id 조회 (IndexScan), 고정 시드 무작위 id
    std::mt19937_64 rng(42);
    std::vector<SelectStmt> probes;
    for (size_t i = 0; i < lookups; ++i) {
      probes.push_back(parseSelect("SELECT * FROM t WHERE id = " + std::to_string(rng() % n + 1)));
    }
    NullSink warm;
    for (size_t i = 0; i < std::min<size_t>(100, probes.size()); ++i) exec.select(probes[i], warm);  // 워밍업
    std::vector<double> lookupUs;
    for (const auto& p : probes) {
      NullSink s;
      auto a = Clock::now();
      exec.select(p, s);
      lookupUs.push_back(us(Clock::now() - a));
      if (s.rows != 1) {
        std::cerr << "lookup returned " << s.rows << " rows\n";
        return 1;
      }
    }

    // 4. 다른 컬럼 조건 (FullScan), 가운데 값
    const SelectStmt scan = parseSelect("SELECT * FROM t WHERE name = 'name_" + std::to_string(n / 2) + "'");
    NullSink warmScan;
    exec.select(scan, warmScan);
    std::vector<double> scanMs;
    uint64_t scanRows = 0;
    for (size_t i = 0; i < scans; ++i) {
      NullSink s;
      auto a = Clock::now();
      exec.select(scan, s);
      scanMs.push_back(us(Clock::now() - a) / 1e3);
      scanRows = s.rows;
    }

    const double idxMed = percentile(lookupUs, 0.5), idxP99 = percentile(lookupUs, 0.99);
    const double scanMed = percentile(scanMs, 0.5);
    const double speedup = scanMed * 1000.0 / idxMed;
    std::cout << std::fixed << "| " << n << " | " << std::setprecision(2) << insertS << " | " << fileMb << " | "
              << std::setprecision(1) << openMs << " | " << std::setprecision(1) << idxMed << " | " << idxP99
              << " | " << std::setprecision(2) << scanMed << " | " << std::setprecision(0) << speedup << "x |\n";
    csv << n << ',' << insertS << ',' << fileMb << ',' << openMs << ',' << idxMed << ',' << idxP99 << ',' << scanMed
        << ',' << scanRows << ',' << speedup << '\n';
  }
  if (!csvPath.empty()) std::ofstream(csvPath) << csv.str();
  return 0;
}
