#pragma once

// 접근 경로 선택 지점 (architecture §7.2). WHERE id = <int>면 IndexScan, 아니면 FullScan.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "engine/binder.h"

namespace minidb {

struct FullScan {
  std::optional<Predicate> filter;
};

struct IndexScan {
  int64_t id = 0;
};

using AccessPath = std::variant<FullScan, IndexScan>;

AccessPath choosePath(const BoundSelect& select);

// EXPLAIN 출력: IndexScan(t, id=5) | FullScan(t) | FullScan(t, filter name='kim')
std::string describePath(const AccessPath& path, const BoundSelect& select);

std::string sqlLiteral(const Value& v);

}  // namespace minidb
