#pragma once
#include <string>

#include "script/ast.h"
#include "script/code.h"

namespace regs::script {

struct CompileResult {
  Ref<Code> code;     // null on error
  std::string error;  // "line: message"
  int line = 0;
};

// Compiles a parsed program. Top-level code starts at pc 0 and ends with End; functions, event handlers, thread and
// macro bodies follow. Bare identifiers are collected in code->links for the linker.
CompileResult compile(const Node* program, const std::u16string& path);

// Compiles a source fragment into an existing code object before it is linked (state-machine guards and actions,
// sub-chunks 0x05020101 / 0x05020102 of a .states transition). The code is appended after everything else.
struct FragmentInfo {
  uint32_t entry = 0;
  uint16_t nlocals = 0;
};
bool compileFragment(Code& code, const std::u16string& source, bool expression, FragmentInfo& out, std::string* error);

}  // namespace regs::script
