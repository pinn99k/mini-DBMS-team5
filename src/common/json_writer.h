#pragma once

// JSON 직렬화 (출력만). 입력 문자열은 Lexer가 UTF-8 유효성을 이미 검사했다.

#include <string>
#include <string_view>

#include "common/value.h"

namespace minidb::json {

void appendString(std::string& out, std::string_view s);
void appendValue(std::string& out, const Value& v);

}  // namespace minidb::json
