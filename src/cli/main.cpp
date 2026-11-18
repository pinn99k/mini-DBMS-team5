// minidb run   [--data DIR] [--bail] <file.sql>
// minidb dump  [--data DIR] <table>
// minidb serve [--data DIR] [--port 8080] [--workers N] [--queue 1024]
// 종료 코드: 0 성공, 1 실패한 문장 있음(또는 dump 손상 발견), 2 사용법 오류·시작 실패

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cli/dump.h"
#include "cli/table_printer.h"
#include "common/errors.h"
#include "engine/executor.h"
#include "server/serve.h"
#include "sql/parser.h"
#include "storage/database.h"

using namespace minidb;

namespace {

int usage() {
  std::cerr << "usage:\n"
               "  minidb run   [--data DIR] [--bail] <file.sql>\n"
               "  minidb dump  [--data DIR] <table>\n"
               "  minidb serve [--data DIR] [--port 8080] [--workers N] [--queue 1024]\n";
  return 2;
}

void printError(const std::string& file, const DbError& e, SourcePos fallback) {
  SourcePos p = e.pos.value_or(fallback);
  std::cerr << file << ':' << p.line << ':' << p.col << ": " << e.typeName() << ": " << e.what() << '\n';
}

int runScript(const std::string& dataDir, const std::string& file, bool bail) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    std::cerr << "cannot read " << file << '\n';
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string source = ss.str();

  std::unique_ptr<Database> db;
  try {
    db = Database::open(dataDir);
  } catch (const DbError& e) {
    std::cerr << "minidb: " << e.typeName() << ": " << e.what() << '\n';
    return 2;
  }
  Executor exec(*db);
  Parser parser(source);
  bool failed = false;

  for (;;) {
    std::optional<Statement> st;
    try {
      st = parser.next();
    } catch (const ParseError& e) {
      printError(file, e, SourcePos{});
      failed = true;
      if (bail) break;
      parser.skipToNextStatement();
      continue;
    }
    if (!st) break;

    try {
      std::visit(
          [&](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, InsertStmt>) {
              auto ids = exec.insert({s});
              std::cout << "INSERT " << ids.size() << " (id=" << ids.front() << ")\n";
            } else if constexpr (std::is_same_v<T, SelectStmt>) {
              TablePrinter printer(std::cout);
              exec.select(s, printer);
            } else if constexpr (std::is_same_v<T, ExplainStmt>) {
              std::cout << exec.explain(s.select) << '\n';
            } else {  // UPDATE·DELETE: 문장마다 따로 커밋
              const uint64_t n = exec.modify({*st}).front();
              std::cout << (std::is_same_v<T, UpdateStmt> ? "UPDATE " : "DELETE ") << n << '\n';
            }
          },
          *st);
    } catch (const DbError& e) {
      std::cout.flush();
      printError(file, e, positionOf(*st));
      failed = true;
      if (bail) break;
    }
  }

  std::cout.flush();
  try {
    db->syncAll();
  } catch (const DbError& e) {
    std::cerr << "minidb: fsync failed: " << e.what() << '\n';
    return 2;
  }
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  std::string dataDir = "./data";
  bool bail = false;
  ServerConfig cfg;
  std::vector<std::string> positional;

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
      return argv[++i];
    };
    try {
      if (a == "--data") {
        dataDir = value();
      } else if (a == "--bail") {
        bail = true;
      } else if (a == "--port") {
        cfg.port = std::stoi(value());
      } else if (a == "--workers") {
        cfg.workers = static_cast<size_t>(std::stoul(value()));
      } else if (a == "--queue") {
        cfg.queue = static_cast<size_t>(std::stoul(value()));
      } else if (a.rfind("--", 0) == 0) {
        std::cerr << "unknown option " << a << '\n';
        return usage();
      } else {
        positional.push_back(a);
      }
    } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return usage();
    }
  }

  if (cmd == "run" && positional.size() == 1) return runScript(dataDir, positional[0], bail);
  if (cmd == "dump" && positional.size() == 1) {
    try {
      return dumpTable(dataDir, positional[0], std::cout);
    } catch (const DbError& e) {
      std::cerr << "minidb: " << e.typeName() << ": " << e.what() << '\n';
      return 2;
    }
  }
  if (cmd == "serve" && positional.empty()) {
    if (cfg.workers == 0 || cfg.queue == 0) return usage();
    return runServe(dataDir, cfg);
  }
  return usage();
}
