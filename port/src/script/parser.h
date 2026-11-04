#pragma once
#include <string>

#include "script/ast.h"

namespace regs::script {

struct ParseResult {
  NodePtr program;     // null on error
  std::string error;   // "line:col: message"
  int line = 0, col = 0;
};

ParseResult parse(const std::u16string& source);

}  // namespace regs::script
