#pragma once

// 키워드는 대소문자 무시, 식별자는 구분. 문자열은 '...' + '' 이스케이프, UTF-8 검사.
// `--`부터 줄 끝까지 주석. 에러를 던지기 전에 문제 지점을 지나가므로 호출자가 이어서 읽을 수 있다.

#include <cstddef>
#include <string_view>

#include "sql/token.h"

namespace minidb {

class Lexer {
 public:
  explicit Lexer(std::string_view source) : src_(source) {}
  Token next();  // ParseError

 private:
  bool atEnd() const { return i_ >= src_.size(); }
  char peek(size_t k = 0) const { return i_ + k < src_.size() ? src_[i_ + k] : '\0'; }
  void advance();
  void skipSpaceAndComments();
  // 토큰 종류별. t.pos는 호출자가 채운 시작 위치
  Token lexWord(Token t);    // 키워드 또는 식별자
  Token lexNumber(Token t);  // 정수 (선택적 '-')
  Token lexString(Token t);  // '...' 문자열
  SourcePos here() const { return SourcePos{line_, col_}; }

  std::string_view src_;
  size_t i_ = 0;
  uint32_t line_ = 1;
  uint32_t col_ = 1;
};

bool isValidUtf8(std::string_view s);

}  // namespace minidb
