#pragma once

#include <cstdint>
#include <string>

#include "common/errors.h"

namespace minidb {

enum class TokenKind { Keyword, Ident, Integer, String, LParen, RParen, Comma, Semicolon, Star, Equals, End };

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;       // Keyword는 대문자, String은 디코드된 값
  int64_t intValue = 0;   // Integer
  SourcePos pos;
};

std::string describe(const Token& t);

}  // namespace minidb
