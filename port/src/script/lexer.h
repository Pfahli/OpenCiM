#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace regs::script {

// Operator indices follow the engine's lexer table at 0x10156450 (docs/vm-opcodes.md "Operator codes").
enum class Op : uint8_t {
  None = 0,
  Add = 0x01, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge, Shl, Shr, Ushr, AndAnd, OrOr, Not, BitAnd, BitOr, BitXor, BitNot,
  PreInc = 0x16, PreDec, PostInc, PostDec, Plus, Neg,
  Assign = 0x1C, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, AndAssign, OrAssign, XorAssign, ShlAssign,
  ShrAssign, UshrAssign,
  Question = 0x28, Colon, Ref, Count,
};

enum class OpKind : uint8_t { Binary, Assign, Postfix, Prefix };
struct OpInfo {
  int precedence;  // lower binds tighter
  OpKind kind;
};
OpInfo opInfo(Op op);
const char* opText(Op op);

enum class Tok : uint8_t {
  End, Ident, Keyword, Var, Int, Float, String, Char, Operator,
  LParen, RParen, LBracket, RBracket, LBrace, RBrace, Comma, Dot, Semicolon, DoubleColon, Error,
};

enum class Kw : uint8_t {
  None, Array, Assert, Break, Continue, Current, Defined, Delete, Do, Echo, Else, Event, Exit, Export, False, For,
  Foreach, ForeachReverse, Function, Global, If, Keys, Load, Local, Loop, Macro, Map, Null, Pause, Resume, Return,
  SkipFrame, This, Thread, True, Values, Wait, WaitUntil, WaitWhile, While, With,
};
const char* kwText(Kw kw);

struct Token {
  Tok kind = Tok::End;
  Kw kw = Kw::None;
  Op op = Op::None;  // for Tok::Operator: the binary/prefix form; the parser re-maps +,-,++,-- by position
  std::u16string text;  // identifier / variable name (without '$') / decoded string literal
  int64_t ival = 0;
  double fval = 0;
  int line = 0, col = 0;
};

// Tokenizes a whole source. On a lexical error the last token has kind Tok::Error and its text holds the message.
std::vector<Token> tokenize(const std::u16string& src);

}  // namespace regs::script
