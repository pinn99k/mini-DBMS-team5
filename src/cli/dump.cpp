#include "cli/dump.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "cli/table_printer.h"
#include "common/bytes.h"
#include "common/crc32.h"
#include "common/errors.h"
#include "storage/file_io.h"
#include "storage/frame_codec.h"
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

  uint64_t pos = frame::kFileHeaderSize;
  uint64_t frames = 0, records = 0;
  std::vector<uint8_t> buf;
  while (pos < size) {
    const uint64_t remain = size - pos;
    auto stop = [&](const char* what, bool tail) {
      out << "frame #" << frames << " @" << pos << ": " << what << " -> " << (tail ? "TAIL torn" : "MID corruption")
          << " (" << remain << " bytes from here to end)\n";
      return 1;
    };
    if (remain < frame::kFrameHeaderSize) return stop("incomplete frame header", true);
    uint8_t fh[frame::kFrameHeaderSize];
    io.pread(fd, fh, sizeof fh, pos);
    const uint32_t frameLen = bytes::getU32(fh);
    const uint32_t count = bytes::getU32(fh + 4);
    if (frameLen < frame::kMinFrameLen || frameLen > frame::kMaxFrameLen) return stop("frame_len out of range", false);
    const uint64_t total = frame::totalSize(frameLen);
    if (total > remain) return stop("incomplete frame", true);
    buf.resize(frameLen + 4u);
    io.pread(fd, buf.data(), buf.size(), pos + 4);
    const bool crcOk = crc32(buf.data(), frameLen) == bytes::getU32(buf.data() + frameLen);
    out << "frame #" << frames << " @" << pos << " len=" << frameLen << " records=" << count
        << " crc=" << (crcOk ? "ok" : "BAD") << "\n";
    if (!crcOk) return stop("CRC mismatch", total == remain);

    size_t off = 4;
    uint32_t seen = 0;
    while (off + 4 <= frameLen) {
      const uint32_t plen = bytes::getU32(buf.data() + off);
      if (plen > frameLen - off - 4) return stop("payload_len out of range", false);
      out << "  @" << (pos + 4 + off);
      try {
        printRow(out, schema, frame::decodePayload(schema, buf.data() + off + 4, plen));
      } catch (const StorageError& e) {
        out << " <" << e.what() << ">\n";
        return stop("bad record", false);
      }
      out << "\n";
      ++seen;
      off += 4u + plen;
    }
    if (off != frameLen || seen != count) return stop("record_count/frame_len mismatch", false);
    ++frames;
    records += seen;
    pos += total;
  }
  out << frames << " frames, " << records << " records, ok\n";
  return 0;
}

}  // namespace minidb
