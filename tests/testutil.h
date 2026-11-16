#pragma once

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "common/errors.h"
#include "storage/file_io.h"

namespace minidb::test {

// 테스트마다 고유한 임시 디렉터리. 소멸 시 삭제.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("minidb-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

inline void writeFile(const std::filesystem::path& p, const std::string& content) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << content;
}

inline std::vector<uint8_t> readBytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void writeBytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// 장애 주입 FileIO. 카운터는 "앞으로 몇 번째 호출에서 실패할지" (0 = 끔).
class FaultyFileIO : public PosixFileIO {
 public:
  std::atomic<int> failWriteAfter{0};     // N번째 pwriteAll에서 절반만 쓰고 실패
  std::atomic<int> failTruncateAfter{0};  // N번째 ftruncate 실패
  std::atomic<int> failReadAfter{0};      // N번째 pread 실패
  std::atomic<int> failFsyncAfter{0};
  std::atomic<uint64_t> reads{0};

  void pwriteAll(int fd, const void* buf, size_t n, uint64_t off) override {
    if (trip(failWriteAfter)) {
      PosixFileIO::pwriteAll(fd, buf, n / 2, off);  // 찢어진 쓰기
      throw StorageError("injected write failure");
    }
    PosixFileIO::pwriteAll(fd, buf, n, off);
  }
  size_t pread(int fd, void* buf, size_t n, uint64_t off) override {
    ++reads;
    if (trip(failReadAfter)) throw StorageError("injected read failure");
    return PosixFileIO::pread(fd, buf, n, off);
  }
  void ftruncate(int fd, uint64_t size) override {
    if (trip(failTruncateAfter)) throw StorageError("injected truncate failure");
    PosixFileIO::ftruncate(fd, size);
  }
  void fsync(int fd) override {
    if (trip(failFsyncAfter)) throw StorageError("injected fsync failure");
    PosixFileIO::fsync(fd);
  }

 private:
  static bool trip(std::atomic<int>& counter) {
    int v = counter.load();
    while (v > 0) {
      if (counter.compare_exchange_weak(v, v - 1)) return v == 1;
    }
    return false;
  }
};

}  // namespace minidb::test
