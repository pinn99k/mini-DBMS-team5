#pragma once

// POST /query 처리 (architecture §11.2~11.4).
// 허용: SELECT 1개 | EXPLAIN SELECT 1개 | INSERT 1..1000개 | UPDATE·DELETE 1..1000개(한 테이블, 원자 배치).
// 그 외(INSERT와 UPDATE·DELETE 섞기 포함) 400 (decisions #23).

#include <httplib.h>

#include "engine/executor.h"

namespace minidb {

class QueryHandler {
 public:
  static constexpr size_t kMaxStatements = 1000;

  explicit QueryHandler(const Executor& exec) : exec_(exec) {}
  void handle(const httplib::Request& req, httplib::Response& res) const;

 private:
  const Executor& exec_;
};

int statusFor(const DbError& e);

}  // namespace minidb
