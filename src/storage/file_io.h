#pragma once

// 테이블 파일 I/O 이음새. 운영은 PosixFileIO, 테스트는 장애를 주입하는 하위 클래스를 쓴다.
// 실패는 모두 StorageError.

#include <cstddef>
#include <cstdint>
#include <string>

namespace minidb {

class FileIO {
 public:
  virtual ~FileIO() = default;
  virtual void pwriteAll(int fd, const void* buf, size_t n, uint64_t off) = 0;
  // n 바이트 또는 파일 끝까지 읽고 읽은 바이트 수를 돌려준다.
  virtual size_t pread(int fd, void* buf, size_t n, uint64_t off) = 0;
  virtual void ftruncate(int fd, uint64_t size) = 0;
  virtual void fsync(int fd) = 0;
};

class PosixFileIO : public FileIO {
 public:
  void pwriteAll(int fd, const void* buf, size_t n, uint64_t off) override;
  size_t pread(int fd, void* buf, size_t n, uint64_t off) override;
  void ftruncate(int fd, uint64_t size) override;
  void fsync(int fd) override;
};

FileIO& defaultFileIO();
std::string errnoMessage(const std::string& what);

}  // namespace minidb
