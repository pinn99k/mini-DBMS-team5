#include <gtest/gtest.h>

#include "sql/lexer.h"
#include "sql/parser.h"

using namespace minidb;

namespace {

std::vector<Token> lexAll(const std::string& s) {
  Lexer lx(s);
  std::vector<Token> out;
  for (;;) {
    Token t = lx.next();
    if (t.kind == TokenKind::End) return out;
    out.push_back(t);
  }
}

Statement parseOne(const std::string& s) {
  Parser p(s);
  auto st = p.next();
  EXPECT_TRUE(st);
  EXPECT_FALSE(p.next());
  return *st;
}

SourcePos parseErrorPos(const std::string& s) {
  Parser p(s);
  try {
    while (p.next()) {
    }
  } catch (const ParseError& e) {
    return e.pos.value_or(SourcePos{});
  }
  ADD_FAILURE() << "expected ParseError for: " << s;
  return {};
}

}  // namespace

// ---- Lexer ----------------------------------------------------------------------------------

TEST(LexerTest, CommaAndSemicolonInsideStringAreNotSeparators) {
  auto t = lexAll("'a,b;c'");
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].kind, TokenKind::String);
  EXPECT_EQ(t[0].text, "a,b;c");
}

TEST(LexerTest, DoubledQuoteEscape) {
  auto t = lexAll("'it''s' ''");
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].text, "it's");
  EXPECT_EQ(t[1].text, "");
}

TEST(LexerTest, MixedCaseKeywordsAndCaseSensitiveIdents) {
  auto t = lexAll("SeLeCt Name fRoM Users");
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[0].kind, TokenKind::Keyword);
  EXPECT_EQ(t[0].text, "SELECT");
  EXPECT_EQ(t[1].kind, TokenKind::Ident);
  EXPECT_EQ(t[1].text, "Name");
  EXPECT_EQ(t[2].text, "FROM");
}

TEST(LexerTest, UnterminatedStringReportsStartPosition) {
  Lexer lx("SELECT *\n  FROM t WHERE name = 'kim");
  for (int i = 0; i < 7; ++i) lx.next();  // SELECT * FROM t WHERE name =
  try {
    lx.next();
    FAIL();
  } catch (const ParseError& e) {
    ASSERT_TRUE(e.pos);
    EXPECT_EQ(e.pos->line, 2u);
    EXPECT_EQ(e.pos->col, 23u);
  }
}

TEST(LexerTest, IntegersIncludingLimits) {
  auto t = lexAll("0 -5 9223372036854775807 -9223372036854775808");
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[1].intValue, -5);
  EXPECT_EQ(t[2].intValue, INT64_MAX);
  EXPECT_EQ(t[3].intValue, INT64_MIN);
  EXPECT_THROW(lexAll("9223372036854775808"), ParseError);
  EXPECT_THROW(lexAll("-9223372036854775809"), ParseError);
}

TEST(LexerTest, CommentsAndPositions) {
  auto t = lexAll("-- comment\n  SELECT -- trailing\n*");
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].pos.line, 2u);
  EXPECT_EQ(t[0].pos.col, 3u);
  EXPECT_EQ(t[1].kind, TokenKind::Star);
  EXPECT_EQ(t[1].pos.line, 3u);
}

TEST(LexerTest, RejectsInvalidUtf8AndStrayCharacters) {
  EXPECT_THROW(lexAll("'\xff'"), ParseError);
  EXPECT_THROW(lexAll("'\xc0\x80'"), ParseError);  // 비최소 인코딩
  EXPECT_NO_THROW(lexAll("'한글 ok'"));
  EXPECT_THROW(lexAll("SELECT @"), ParseError);
}

// ---- Parser ---------------------------------------------------------------------------------

TEST(ParserTest, InsertWithAndWithoutColumns) {
  auto a = std::get<InsertStmt>(parseOne("INSERT INTO users VALUES ('kim', 25);"));
  EXPECT_EQ(a.table, "users");
  EXPECT_TRUE(a.columns.empty());
  ASSERT_EQ(a.values.size(), 2u);
  EXPECT_EQ(std::get<std::string>(a.values[0].value), "kim");

  auto b = std::get<InsertStmt>(parseOne("insert into users (age, name) values (3, 'x')"));
  EXPECT_EQ(b.columns, (std::vector<std::string>{"age", "name"}));
  EXPECT_EQ(std::get<int64_t>(b.values[0].value), 3);
}

TEST(ParserTest, SelectVariants) {
  auto a = std::get<SelectStmt>(parseOne("SELECT * FROM t;"));
  EXPECT_TRUE(a.columns.empty());
  EXPECT_FALSE(a.where);

  auto b = std::get<SelectStmt>(parseOne("SELECT id, name FROM t WHERE name = 'a;b'"));
  EXPECT_EQ(b.columns, (std::vector<std::string>{"id", "name"}));
  ASSERT_TRUE(b.where);
  EXPECT_EQ(b.where->column, "name");
  EXPECT_EQ(std::get<std::string>(b.where->value.value), "a;b");

  auto c = std::get<ExplainStmt>(parseOne("EXPLAIN SELECT * FROM t WHERE id = 5"));
  EXPECT_EQ(std::get<int64_t>(c.select.where->value.value), 5);
}

TEST(ParserTest, MultipleStatementsAndEmptyOnes) {
  Parser p(";; SELECT * FROM a; ; INSERT INTO b VALUES (1);SELECT * FROM c");
  int n = 0;
  while (p.next()) ++n;
  EXPECT_EQ(n, 3);
}

TEST(ParserTest, SyntaxErrorsCarryLineAndCol) {
  SourcePos p = parseErrorPos("SELECT * FROM t;\nSELECT FROM t;");
  EXPECT_EQ(p.line, 2u);
  EXPECT_EQ(p.col, 8u);

  p = parseErrorPos("INSERT INTO t VALUES (1, );");
  EXPECT_EQ(p.col, 26u);
  parseErrorPos("SELECT * FROM t WHERE a > 1");  // '=' 만 지원
  parseErrorPos("SELECT * FROM t WHERE a = b");  // 오른쪽은 리터럴만
  parseErrorPos("DELETE FROM t");
  parseErrorPos("SELECT * FROM t extra");
}

TEST(ParserTest, RecoversAtNextStatementAfterError) {
  Parser p("SELECT FROM t; INSERT INTO t VALUES (1); SELECT @ FROM t; SELECT * FROM u;");
  std::vector<std::string> ok;
  for (;;) {
    try {
      auto st = p.next();
      if (!st) break;
      ok.push_back(std::holds_alternative<InsertStmt>(*st) ? "insert" : std::get<SelectStmt>(*st).table);
    } catch (const ParseError&) {
      p.skipToNextStatement();
    }
  }
  EXPECT_EQ(ok, (std::vector<std::string>{"insert", "u"}));
}

TEST(ParserTest, UnterminatedStringStopsTheRest) {
  Parser p("SELECT * FROM t WHERE a = 'oops; SELECT * FROM u;");
  EXPECT_THROW(p.next(), ParseError);
  p.skipToNextStatement();
  EXPECT_FALSE(p.next());
}
