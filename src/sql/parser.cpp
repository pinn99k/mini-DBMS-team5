#include "sql/parser.h"

namespace minidb {

const Token& Parser::peek() {
  if (!cur_) cur_ = lex_.next();
  return *cur_;
}

Token Parser::take() {
  Token t = peek();
  cur_.reset();
  return t;
}

bool Parser::isKeyword(const Token& t, const char* kw) const {
  return t.kind == TokenKind::Keyword && t.text == kw;
}

void Parser::unexpected(const char* what) {
  const Token& t = peek();
  throw ParseError(std::string("expected ") + what + ", got " + describe(t), t.pos);
}

Token Parser::expect(TokenKind kind, const char* what) {
  if (peek().kind != kind) unexpected(what);
  return take();
}

void Parser::expectKeyword(const char* kw) {
  if (!isKeyword(peek(), kw)) unexpected(kw);
  take();
}

std::optional<Statement> Parser::next() {
  while (peek().kind == TokenKind::Semicolon) take();  // 빈 문장
  if (peek().kind == TokenKind::End) return std::nullopt;

  Statement st;
  const Token& first = peek();
  if (isKeyword(first, "INSERT")) {
    st = parseInsert();
  } else if (isKeyword(first, "SELECT")) {
    st = parseSelect();
  } else if (isKeyword(first, "EXPLAIN")) {
    SourcePos pos = take().pos;
    if (!isKeyword(peek(), "SELECT")) unexpected("SELECT after EXPLAIN");
    st = ExplainStmt{parseSelect(), pos};
  } else if (isKeyword(first, "UPDATE")) {
    st = parseUpdate();
  } else if (isKeyword(first, "DELETE")) {
    st = parseDelete();
  } else {
    unexpected("INSERT, SELECT, EXPLAIN, UPDATE or DELETE");
  }

  if (peek().kind == TokenKind::Semicolon) {
    take();
  } else if (peek().kind != TokenKind::End) {
    unexpected("';'");
  }
  return st;
}

void Parser::skipToNextStatement() {
  for (;;) {
    try {
      const TokenKind k = peek().kind;
      if (k == TokenKind::End) return;
      take();
      if (k == TokenKind::Semicolon) return;
    } catch (const ParseError&) {
      cur_.reset();  // Lexer는 이미 문제 지점을 지나갔다
    }
  }
}

InsertStmt Parser::parseInsert() {
  InsertStmt s;
  s.pos = take().pos;  // INSERT
  expectKeyword("INTO");
  Token table = expect(TokenKind::Ident, "table name");
  s.table = table.text;
  s.tablePos = table.pos;

  if (peek().kind == TokenKind::LParen) {
    take();
    for (;;) {
      Token col = expect(TokenKind::Ident, "column name");
      s.columns.push_back(col.text);
      s.columnPos.push_back(col.pos);
      if (peek().kind != TokenKind::Comma) break;
      take();
    }
    expect(TokenKind::RParen, "')'");
  }

  expectKeyword("VALUES");
  expect(TokenKind::LParen, "'('");
  for (;;) {
    s.values.push_back(parseLiteral());
    if (peek().kind != TokenKind::Comma) break;
    take();
  }
  expect(TokenKind::RParen, "')'");
  return s;
}

SelectStmt Parser::parseSelect() {
  SelectStmt s;
  s.pos = take().pos;  // SELECT
  if (peek().kind == TokenKind::Star) {
    take();
  } else {
    for (;;) {
      Token col = expect(TokenKind::Ident, "column name or '*'");
      s.columns.push_back(col.text);
      s.columnPos.push_back(col.pos);
      if (peek().kind != TokenKind::Comma) break;
      take();
    }
  }
  expectKeyword("FROM");
  Token table = expect(TokenKind::Ident, "table name");
  s.table = table.text;
  s.tablePos = table.pos;
  s.where = parseWhere();
  return s;
}

UpdateStmt Parser::parseUpdate() {
  UpdateStmt s;
  s.pos = take().pos;  // UPDATE
  Token table = expect(TokenKind::Ident, "table name");
  s.table = table.text;
  s.tablePos = table.pos;
  expectKeyword("SET");
  for (;;) {
    Token col = expect(TokenKind::Ident, "column name");
    expect(TokenKind::Equals, "'='");
    s.set.push_back(Assignment{col.text, col.pos, parseLiteral()});
    if (peek().kind != TokenKind::Comma) break;
    take();
  }
  s.where = parseWhere();
  return s;
}

DeleteStmt Parser::parseDelete() {
  DeleteStmt s;
  s.pos = take().pos;  // DELETE
  expectKeyword("FROM");
  Token table = expect(TokenKind::Ident, "table name");
  s.table = table.text;
  s.tablePos = table.pos;
  s.where = parseWhere();
  return s;
}

std::optional<Condition> Parser::parseWhere() {
  if (!isKeyword(peek(), "WHERE")) return std::nullopt;
  take();
  Token col = expect(TokenKind::Ident, "column name");
  expect(TokenKind::Equals, "'=' (only single equality is supported)");
  return Condition{col.text, col.pos, parseLiteral()};
}

Literal Parser::parseLiteral() {
  const Token& t = peek();
  if (t.kind == TokenKind::Integer) {
    Token v = take();
    return Literal{Value{v.intValue}, v.pos};
  }
  if (t.kind == TokenKind::String) {
    Token v = take();
    return Literal{Value{std::move(v.text)}, v.pos};
  }
  unexpected("literal (integer or 'string')");
}

}  // namespace minidb
