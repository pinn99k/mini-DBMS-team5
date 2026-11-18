#pragma once

// 문장 단위로 당겨 오는 파서. 파일 전체를 토큰 배열로 만들지 않는다 (architecture §6.3).
//
// script     := { statement? ';' } [ statement ]
// insert     := INSERT INTO ident [ '(' ident {',' ident} ')' ] VALUES '(' literal {',' literal} ')'
// select     := SELECT ( '*' | ident {',' ident} ) FROM ident [ WHERE ident '=' literal ]
// explain    := EXPLAIN select
// update     := UPDATE ident SET ident '=' literal {',' ident '=' literal} [ where ]
// delete     := DELETE FROM ident [ where ]
// where      := WHERE ident '=' literal

#include <optional>
#include <string_view>

#include "sql/ast.h"
#include "sql/lexer.h"

namespace minidb {

class Parser {
 public:
  explicit Parser(std::string_view source) : lex_(source) {}

  std::optional<Statement> next();  // 끝이면 nullopt. ParseError
  void skipToNextStatement();       // ParseError 뒤에 호출: 다음 ';' 뒤로 이동

 private:
  const Token& peek();
  Token take();
  Token expect(TokenKind kind, const char* what);
  void expectKeyword(const char* kw);
  bool isKeyword(const Token& t, const char* kw) const;
  [[noreturn]] void unexpected(const char* what);

  InsertStmt parseInsert();
  SelectStmt parseSelect();
  UpdateStmt parseUpdate();
  DeleteStmt parseDelete();
  std::optional<Condition> parseWhere();
  Literal parseLiteral();

  Lexer lex_;
  std::optional<Token> cur_;
};

}  // namespace minidb
