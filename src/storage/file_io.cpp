#include "storage/file_io.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/errors.h"

namespace minidb {

std::string errnoMessage(const std::string& what) { return what + ": " + std::strerror(errno); }

void PosixFileIO::pwriteAll(int fd, const void* buf, size_t n, uint64_t off) {
  const auto* p = static_cast<const char*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pwrite(fd, p + done, n - done, static_cast<off_t>(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw StorageError(errnoMessage("pwrite"));
    }
    if (r == 0) throw StorageError("pwrite: wrote 0 bytes");
    done += static_cast<size_t>(r);
  }
}

size_t PosixFileIO::pread(int fd, void* buf, size_t n, uint64_t off) {
  auto* p = static_cast<char*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::pread(fd, p + done, n - done, static_cast<off_t>(off + done));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw StorageError(errnoMessage("pread"));
    }
    if (r == 0) break;
    done += static_cast<size_t>(r);
  }
  return done;
}

void PosixFileIO::ftruncate(int fd, uint64_t size) {
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) throw StorageError(errnoMessage("ftruncate"));
}

void PosixFileIO::fsync(int fd) {
  if (::fsync(fd) != 0) throw StorageError(errnoMessage("fsync"));
}

FileIO& defaultFileIO() {
  static PosixFileIO io;
  return io;
}

}  // namespace minidb
