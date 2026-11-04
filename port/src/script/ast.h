#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "script/lexer.h"

namespace regs::script {

enum class N : uint8_t {
  // expressions
  Int, Float, String, Char, True, False, Null, This, Current, SkipFrame,
  Var,          // $name                      name
  Ident,        // name (function, native global, constant)
  WithMember,   // .name / .$name inside with  name, i = 1 when '$'
  Member,       // a.name / a.$name            kids[0] = a, name, i = 1 when '$'
  Index,        // a[b]; a[] (one kid) appends
  Call,         // f(args...)                 kids[0] = callee, kids[1..] = args
  Unary,        // prefix op                  op, kids[0]
  Postfix,      // postfix op                 op, kids[0]
  Binary,       // a op b
  Assign,       // a op= b
  ArrayLit,     // [a, b, ...] / array [...]
  MapLit,       // map [k, v, ...]
  Keys, Values, Defined,
  Load,         // load [group ::] path       kids[0] = path, kids[1] = group (optional)
  Macro,        // macro [(args)] :: expr      kids[0] = expr, kids[1..] = captured args
  Thread,       // thread [group] :: stmt      kids[0] = body, kids[1] = group (optional)
  FunctionExpr, // function (params) { }       params, kids[0] = body
  // statements
  Program, Block, Empty, ExprStmt, If, While, DoWhile, For, Foreach, ForeachReverse, Loop, With, Return, Break,
  Continue, Exit, Wait, WaitUntil, WaitWhile, Pause, Resume, Echo, Assert, Delete, Local, Global, Export,
  FunctionDecl,  // function name(params) { }  name, params, kids[0] = body
  EventDecl,     // event target::[ctx](params) { }  kids[0] = target, kids[1] = body, kids[2] = ctx (optional)
};
const char* nodeName(N n);

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Param {
  std::u16string name;
  NodePtr defaultValue;
};

struct Node {
  N kind;
  Op op = Op::None;
  std::u16string name;
  int64_t i = 0;
  double f = 0;
  int line = 0;
  std::vector<NodePtr> kids;
  std::vector<Param> params;

  Node(N k, int ln) : kind(k), line(ln) {}
  Node* kid(size_t n) const { return n < kids.size() ? kids[n].get() : nullptr; }
};

// S-expression dump for tests and debugging.
std::string dumpAst(const Node* n, int indent = 0);

}  // namespace regs::script
