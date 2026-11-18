#include "cli/dump.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli/table_printer.h"
#include "common/bytes.h"
#include "common/errors.h"
#include "storage/file_io.h"
#include "storage/frame_codec.h"
#include "storage/frame_scan.h"
#include "storage/schema.h"

namespace minidb {
namespace {

void printRow(std::ostream& out, const Schema& schema, const Row& row) {
  const auto& cols = schema.columns();
  for (size_t i = 0; i < row.size(); ++i) {
    out << ' ' << cols[i].name << '=';
    if (const auto* v = std::get_if<int64_t>(&row[i])) {
      out << *v;
    } else {
      out << '\'' << escapeForDisplay(std::get<std::string>(row[i])) << '\'';
    }
  }
}

}  // namespace

int dumpTable(const std::filesystem::path& dataDir, const std::string& table, std::ostream& out) {
  Schema schema = Schema::load(dataDir / (table + ".schema"));
  const auto path = dataDir / (table + ".tbl");
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw StorageError(errnoMessage("open " + path.string()));
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } closer{fd};
  FileIO& io = defaultFileIO();

  struct stat st {};
  if (::fstat(fd, &st) != 0) throw StorageError(errnoMessage("fstat"));
  const auto size = static_cast<uint64_t>(st.st_size);
  out << "file " << path.string() << " (" << size << " bytes)\n";

  uint8_t header[frame::kFileHeaderSize];
  if (size < frame::kFileHeaderSize || io.pread(fd, header, sizeof header, 0) != sizeof header) {
    out << "header: SHORT (" << size << " bytes)\n";
    return 1;
  }
  if (!frame::checkFileHeader(header)) {
    out << "header: BAD magic/version\n";
    return 1;
  }
  out << "header: MDB1 v" << bytes::getU16(header + 4) << "\n";

  // 판정 규칙은 Table::recover와 같은 frame::scanFrames (decisions #18). dump는 출력만 하고 자르지 않는다.
  uint64_t frameNo = 0;
  frame::ScanCallbacks cb;
  cb.onFrame = [&](uint64_t pos, uint32_t frameLen, uint32_t count) {
    out << "frame #" << frameNo++ << " @" << pos << " len=" << frameLen << " records=" << count << " crc=ok\n";
  };
  cb.onRecord = [&](frame::RecordKind kind, uint64_t offset, const Row& row) {
    out << "  @" << offset;
    if (kind == frame::RecordKind::Update) out << " update";
    if (kind == frame::RecordKind::Delete) out << " delete";
    printRow(out, schema, row);  // 묘비는 id만
    out << "\n";
  };
  const frame::ScanResult r = frame::scanFrames(io, fd, size, schema, cb);
  if (r.status == frame::ScanStatus::Ok) {
    out << r.frames << " frames, " << r.records << " records, " << r.live.size() << " live, ok\n";
    return 0;
  }
  const bool tail = r.status == frame::ScanStatus::TornTail;
  out << "frame #" << r.frames << " @" << r.at << ": " << r.why << " -> " << (tail ? "TAIL torn" : "MID corruption")
      << " (" << (size - r.at) << " bytes from here to end)\n";
  return 1;
}

}  // namespace minidb
