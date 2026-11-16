#pragma once

// 결과를 행 단위로 받는 쪽. CLI와 서버는 이 구현만 다르다 (architecture §2 불변식 3).

#include <cstdint>
#include <vector>

#include "common/value.h"

namespace minidb {

class RowSink {
 public:
  virtual ~RowSink() = default;
  virtual void begin(const std::vector<Column>& columns) = 0;  // 출력 컬럼
  virtual bool row(const Row& row) = 0;                         // false = 그만 받겠다 (decisions #3)
  virtual void end(uint64_t rowCount) = 0;                      // 정상 종료에만 호출
};

}  // namespace minidb
