#pragma once
// 테이블 파일의 프레임 검증 스캔 (architecture §5.3). 손상 판정 규칙의 유일한 구현이다.
// Table::recover(자르기·거부)와 minidb dump(출력만)가 같이 쓴다 (decisions #18).

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "common/value.h"
#include "storage/file_io.h"
#include "storage/schema.h"

namespace minidb::frame {

enum class ScanStatus { Ok, TornTail, MidCorruption };

// 레코드 종류는 바이트에 적지 않고 추론한다 (architecture §5.2, decisions #20)
//   payload 8 B → Delete, 전체 레코드 + 지금까지 본 최대 id보다 큼 → Insert, 그 밖 → Update
enum class RecordKind { Insert, Update, Delete };

struct ScanResult {
  ScanStatus status = ScanStatus::Ok;
  uint64_t at = 0;       // TornTail·MidCorruption: 문제 프레임의 시작. TornTail이면 여기서 자른다
  std::string why;       // 판정 이유
  uint64_t frames = 0;   // 정상 프레임 수
  uint64_t records = 0;  // 정상 프레임에 든 레코드 수
  int64_t lastId = 0;    // 정상 프레임에서 본 최대 id (지운 id 포함, 빈 테이블은 0)
  // 살아 있는 (id, 최신 레코드 위치), id 오름차순. Ok·TornTail이면 정상 프레임까지의 상태
  std::vector<std::pair<int64_t, uint64_t>> live;
};

struct ScanCallbacks {
  // 길이·CRC 검사를 통과한 프레임마다, 그 프레임의 레코드보다 먼저
  std::function<void(uint64_t pos, uint32_t frameLen, uint32_t recordCount)> onFrame;
  // 판정된 레코드마다. offset = 파일에서 payload_len의 위치. Delete면 row = {id}.
  // MidCorruption이면 문제 프레임의 앞쪽 레코드가 이미 전달됐을 수 있다.
  std::function<void(RecordKind kind, uint64_t offset, const Row& row)> onRecord;
};

// [파일 헤더 끝, size)를 앞에서부터 검사한다. 파일 헤더는 호출자가 확인한다.
// 아무것도 쓰거나 자르지 않는다. 읽기 실패는 StorageError.
ScanResult scanFrames(FileIO& io, int fd, uint64_t size, const Schema& schema, const ScanCallbacks& cb = {});

}  // namespace minidb::frame
