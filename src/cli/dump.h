#pragma once

#include <filesystem>
#include <ostream>
#include <string>

namespace minidb {

// 테이블 파일을 Table을 거치지 않고 직접 읽어 프레임·레코드를 출력한다. 읽기 전용, 자르지 않는다.
// 반환: 0 정상, 1 손상 발견.
int dumpTable(const std::filesystem::path& dataDir, const std::string& table, std::ostream& out);

}  // namespace minidb
