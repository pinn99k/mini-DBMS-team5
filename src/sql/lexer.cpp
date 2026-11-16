#include "sql/lexer.h"

#include <array>
#include <cctype>
#include <limits>

namespace minidb {
namespace {

constexpr std::array<const char*, 7> kKeywords = {"INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE", "EXPLAIN"};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::string describe(const Token& t) {
  switch (t.kind) {
    case TokenKind::Keyword: return "keyword " + t.text;
    case TokenKind::Ident: return "identifier '" + t.text + "'";
    case TokenKind::Integer: return "integer " + std::to_string(t.intValue);
    case TokenKind::String: return "string literal";
    case TokenKind::LParen: return "'('";
    case TokenKind::RParen: return "')'";
    case TokenKind::Comma: return "','";
    case TokenKind::Semicolon: return "';'";
    case TokenKind::Star: return "'*'";
    case TokenKind::Equals: return "'='";
    case TokenKind::End: return "end of input";
  }
  return "token";
}

bool isValidUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      const auto cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    // 최소 길이 인코딩, 서로게이트 제외, 최대 U+10FFFF
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += len;
  }
  return true;
}

void Lexer::advance() {
  if (atEnd()) return;
  if (src_[i_] == '\n') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  ++i_;
}

Token Lexer::next() {
  // 공백과 주석
  for (;;) {
    while (!atEnd() && std::isspace(static_cast<unsigned char>(peek()))) advance();
    if (peek() == '-' && peek(1) == '-') {
      while (!atEnd() && peek() != '\n') advance();
      continue;
    }
    break;
  }

  Token t;
  t.pos = here();
  if (atEnd()) return t;  // End

  const char c = peek();
  auto single = [&](TokenKind k) {
    t.kind = k;
    t.text = std::string(1, c);
    advance();
    return t;
  };
  switch (c) {
    case '(': return single(TokenKind::LParen);
    case ')': return single(TokenKind::RParen);
    case ',': return single(TokenKind::Comma);
    case ';': return single(TokenKind::Semicolon);
    case '*': return single(TokenKind::Star);
    case '=': return single(TokenKind::Equals);
    default: break;
  }

  if (isIdentStart(c)) {
    size_t start = i_;
    while (!atEnd() && isIdentChar(peek())) advance();
    t.text = std::string(src_.substr(start, i_ - start));
    std::string upper;
    for (char ch : t.text) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    t.kind = TokenKind::Ident;
    for (const char* kw : kKeywords) {
      if (upper == kw) {
        t.kind = TokenKind::Keyword;
        t.text = upper;
      }
    }
    return t;
  }

  if (isDigit(c) || (c == '-' && isDigit(peek(1)))) {
    const bool negative = c == '-';
    if (negative) advance();
    // 음수는 |INT64_MIN| = INT64_MAX + 1까지 허용
    const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + (negative ? 1u : 0u);
    uint64_t v = 0;
    bool overflow = false;
    size_t start = i_;
    while (!atEnd() && isDigit(peek())) {
      const auto d = static_cast<uint64_t>(peek() - '0');
      if (v > (limit - d) / 10) overflow = true;
      if (!overflow) v = v * 10 + d;
      advance();
    }
    if (overflow) {
      throw ParseError("integer literal out of range: " + std::string(negative ? "-" : "") +
                           std::string(src_.substr(start, i_ - start)),
                       t.pos);
    }
    t.kind = TokenKind::Integer;
    t.intValue = negative ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
    t.text = std::to_string(t.intValue);
    return t;
  }

  if (c == '\'') {
    advance();
    std::string value;
    for (;;) {
      if (atEnd()) throw ParseError("unterminated string literal", t.pos);
      const char ch = peek();
      if (ch == '\'') {
        if (peek(1) == '\'') {
          value += '\'';
          advance();
          advance();
          continue;
        }
        advance();
        break;
      }
      value += ch;
      advance();
    }
    if (!isValidUtf8(value)) throw ParseError("string literal is not valid UTF-8", t.pos);
    t.kind = TokenKind::String;
    t.text = std::move(value);
    return t;
  }

  advance();  // 문제 문자를 지나간 뒤 던진다
  throw ParseError(std::string("unexpected character '") + c + "'", t.pos);
}

}  // namespace minidb
