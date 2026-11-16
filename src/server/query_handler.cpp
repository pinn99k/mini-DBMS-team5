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

}  // namespace

int statusFor(const DbError& e) {
  if (dynamic_cast<const ParseError*>(&e)) return 400;
  if (const auto* se = dynamic_cast<const SchemaError*>(&e)) {
    return se->kind == SchemaError::Kind::UnknownTable ? 404 : 400;
  }
  return 500;  // StorageError
}

void QueryHandler::handle(const httplib::Request& req, httplib::Response& res) const {
  // 1. 전부 파싱 (바디 ≤ 1 MiB라 AST를 모두 들고 있어도 된다). 아무것도 실행하지 않은 상태.
  std::vector<Statement> stmts;
  try {
    Parser parser(req.body);
    while (auto st = parser.next()) {
      stmts.push_back(std::move(*st));
      if (stmts.size() > kMaxStatements) {
        return sendError(res, 400, "RequestError", "too many statements (max 1000)");
      }
    }
  } catch (const ParseError& e) {
    return sendError(res, e);
  }
  if (stmts.empty()) return sendError(res, 400, "RequestError", "empty request");

  size_t inserts = 0;
  for (const auto& s : stmts) inserts += std::holds_alternative<InsertStmt>(s) ? 1 : 0;

  try {
    if (inserts == stmts.size()) {
      // 2~5. 원자 배치: 전부 바인딩 → 프레임 하나로 커밋 (Executor::insert → Table::insert)
      std::vector<InsertStmt> batch;
      batch.reserve(stmts.size());
      for (auto& s : stmts) batch.push_back(std::move(std::get<InsertStmt>(s)));
      const auto ids = exec_.insert(batch);
      std::string body = "{\"ok\":true,\"insertedIds\":[";
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i) body += ',';
        body += std::to_string(ids[i]);
      }
      body += "]}";
      res.set_content(body, kJson);
      return;
    }

    if (stmts.size() != 1) {
      return sendError(res, 400, "RequestError",
                       "a request must be exactly one SELECT, one EXPLAIN, or 1..1000 INSERTs");
    }

    if (const auto* ex = std::get_if<ExplainStmt>(&stmts[0])) {
      std::string body = "{\"ok\":true,\"plan\":";
      json::appendString(body, exec_.explain(ex->select));
      body += "}";
      res.set_content(body, kJson);
      return;
    }

    // SELECT: 바인딩 에러는 여기서(첫 바이트 전) 상태 코드로. 이후 에러는 마지막 줄로.
    auto prepared = std::make_shared<PreparedSelect>(exec_.prepare(std::get<SelectStmt>(stmts[0])));
    const Executor* exec = &exec_;
    res.set_chunked_content_provider("application/x-ndjson",
                                     [exec, prepared](size_t, httplib::DataSink& sink) {
                                       NdjsonSink out([&sink](const char* d, size_t n) { return sink.write(d, n); });
                                       try {
                                         exec->run(*prepared, out);
                                       } catch (const DbError& e) {
                                         out.fail(e.typeName(), e.what());
                                       } catch (const std::exception& e) {
                                         out.fail("InternalError", e.what());
                                       }
                                       if (!out.flush()) return false;  // 클라이언트가 떠남
                                       sink.done();
                                       return true;
                                     });
  } catch (const DbError& e) {
    sendError(res, e);
  } catch (const std::exception& e) {
    sendError(res, 500, "InternalError", e.what());
  }
}

}  // namespace minidb
