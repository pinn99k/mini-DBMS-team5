#pragma once

#include <filesystem>

#include "server/http_server.h"

namespace minidb {

// minidb serve: 시그널 처리, 1초 주기 fsync 스레드, 종료 순서 (architecture §11.5).
// 반환: 0 정상 종료, 1 fsync 실패로 중지, 2 시작 실패.
int runServe(const std::filesystem::path& dataDir, const ServerConfig& cfg);

}  // namespace minidb
