#include "server/query_handler.h"

#include <memory>
#include <vector>

#include "common/json_writer.h"
#include "server/ndjson_sink.h"
#include "sql/parser.h"

namespace minidb {
namespace {

constexpr const char* kJson = "application/json";

void sendError(httplib::Response& res, int status, const std::string& type, const std::string& message,
               std::optional<SourcePos> pos = std::nullopt) {
  res.status = status;
  res.set_content(errorJson(type, message, pos), kJson);
}

void sendError(httplib::Response& res, const DbError& e) {
  sendError(res, statusFor(e), e.typeName(), e.what(), e.pos);
}

// 1. 바디 전체를 파싱한다 (바디 ≤ 1 MiB라 AST를 모두 들고 있어도 된다). 아무것도 실행하지 않은 상태.
// 실패하면 res에 에러를 채우고 false.
bool parseBody(const std::string& body, std::vector<Statement>& stmts, httplib::Response& res) {
  try {
    Parser parser(body);
    while (auto st = parser.next()) {
      stmts.push_back(std::move(*st));
      if (stmts.size() > QueryHandler::kMaxStatements) {
        sendError(res, 400, "RequestError", "too many statements (max 1000)");
        return false;
      }
    }
  } catch (const ParseError& e) {
    sendError(res, e);
    return false;
  }
  if (stmts.empty()) {
    sendError(res, 400, "RequestError", "empty request");
    return false;
  }
  return true;
}

// 2~5. 원자 배치: 전부 바인딩 → 프레임 하나로 커밋 (Executor::insert → Table::insert)
void respondInsert(const Executor& exec, std::vector<Statement>& stmts, httplib::Response& res) {
  std::vector<InsertStmt> batch;
  batch.reserve(stmts.size());
  for (auto& s : stmts) batch.push_back(std::move(std::get<InsertStmt>(s)));
  const auto ids = exec.insert(batch);
  std::string body = "{\"ok\":true,\"insertedIds\":[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) body += ',';
    body += std::to_string(ids[i]);
  }
  body += "]}";
  res.set_content(body, kJson);
}

// UPDATE·DELETE 배치 (decisions #23): 전부 바인딩 → 프레임 하나로 커밋 (Executor::modify → Table::modify)
void respondModify(const Executor& exec, const std::vector<Statement>& stmts, httplib::Response& res) {
  const auto counts = exec.modify(stmts);
  std::string body = "{\"ok\":true,\"affectedRows\":[";
  for (size_t i = 0; i < counts.size(); ++i) {
    if (i) body += ',';
    body += std::to_string(counts[i]);
  }
  body += "]}";
  res.set_content(body, kJson);
}

void respondExplain(const Executor& exec, const ExplainStmt& ex, httplib::Response& res) {
  std::string body = "{\"ok\":true,\"plan\":";
  json::appendString(body, exec.explain(ex.select));
  body += "}";
  res.set_content(body, kJson);
}

// SELECT: 바인딩 에러는 prepare에서(첫 바이트 전) 상태 코드로. 스트리밍 중 에러는 NDJSON 마지막 줄로.
void respondSelect(const Executor& exec, const SelectStmt& stmt, httplib::Response& res) {
  auto prepared = std::make_shared<PreparedSelect>(exec.prepare(stmt));
  const Executor* ex = &exec;
  res.set_chunked_content_provider("application/x-ndjson", [ex, prepared](size_t, httplib::DataSink& sink) {
    NdjsonSink out([&sink](const char* d, size_t n) { return sink.write(d, n); });
    try {
      ex->run(*prepared, out);
    } catch (const DbError& e) {
      out.fail(e.typeName(), e.what());
    } catch (const std::exception& e) {
      out.fail("InternalError", e.what());
    }
    if (!out.flush()) return false;  // 클라이언트가 떠남
    sink.done();
    return true;
  });
}

}  // namespace

int statusFor(const DbError& e) {
  if (dynamic_cast<const ParseError*>(&e)) return 400;
  if (const auto* se = dynamic_cast<const SchemaError*>(&e)) {
    return se->kind == SchemaError::Kind::UnknownTable ? 404 : 400;
  }
  return 500;  // StorageError
}

void QueryHandler::handle(const httplib::Request& req, httplib::Response& res) const {
  std::vector<Statement> stmts;
  if (!parseBody(req.body, stmts, res)) return;

  size_t inserts = 0, modifies = 0;
  for (const auto& s : stmts) {
    inserts += std::holds_alternative<InsertStmt>(s) ? 1 : 0;
    modifies += std::holds_alternative<UpdateStmt>(s) || std::holds_alternative<DeleteStmt>(s) ? 1 : 0;
  }

  try {
    if (inserts == stmts.size()) return respondInsert(exec_, stmts, res);
    if (modifies == stmts.size()) return respondModify(exec_, stmts, res);
    if (stmts.size() != 1) {
      return sendError(res, 400, "RequestError",
                       "a request must be exactly one SELECT, one EXPLAIN, 1..1000 INSERTs, or 1..1000 "
                       "UPDATE/DELETEs");
    }
    if (const auto* ex = std::get_if<ExplainStmt>(&stmts[0])) return respondExplain(exec_, *ex, res);
    respondSelect(exec_, std::get<SelectStmt>(stmts[0]), res);
  } catch (const DbError& e) {
    sendError(res, e);
  } catch (const std::exception& e) {
    sendError(res, 500, "InternalError", e.what());
  }
}

}  // namespace minidb
