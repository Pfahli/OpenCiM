#include "script/lexer.h"

#include <cmath>
#include <cstring>

#include "core/text.h"

namespace regs::script {

OpInfo opInfo(Op op) {
  switch (op) {
    case Op::Add: case Op::Sub: return {6, OpKind::Binary};
    case Op::Mul: case Op::Div: case Op::Mod: return {5, OpKind::Binary};
    case Op::Eq: case Op::Ne: return {9, OpKind::Binary};
    case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: return {8, OpKind::Binary};
    case Op::Shl: case Op::Shr: case Op::Ushr: return {7, OpKind::Binary};
    case Op::AndAnd: return {13, OpKind::Binary};
    case Op::OrOr: return {14, OpKind::Binary};
    case Op::BitAnd: return {10, OpKind::Binary};
    case Op::BitOr: return {12, OpKind::Binary};
    case Op::BitXor: return {11, OpKind::Binary};
    case Op::Not: case Op::BitNot: case Op::PreInc: case Op::PreDec: case Op::Plus: case Op::Neg: return {3, OpKind::Prefix};
    case Op::PostInc: case Op::PostDec: return {2, OpKind::Postfix};
    case Op::Question: case Op::Colon: return {15, OpKind::Binary};
    case Op::Ref: case Op::Count: return {1, OpKind::Prefix};
    default:
      if (op >= Op::Assign && op <= Op::UshrAssign) return {16, OpKind::Assign};
      return {99, OpKind::Binary};
  }
}

const char* opText(Op op) {
  static const char* names[] = {"?", "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">=", "<<", ">>", ">>>", "&&",
                                "||", "!", "&", "|", "^", "~", "++", "--", "++", "--", "+", "-", "=", "+=", "-=", "*=",
                                "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>=", "?", ":", "@", "#"};
  size_t i = size_t(op);
  return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

namespace {

struct KwName { const char* text; Kw kw; };
const KwName kKeywords[] = {
    {"array", Kw::Array}, {"assert", Kw::Assert}, {"break", Kw::Break}, {"continue", Kw::Continue},
    {"current", Kw::Current}, {"defined", Kw::Defined}, {"delete", Kw::Delete}, {"do", Kw::Do}, {"echo", Kw::Echo},
    {"else", Kw::Else}, {"event", Kw::Event}, {"exit", Kw::Exit}, {"export", Kw::Export}, {"false", Kw::False},
    {"for", Kw::For}, {"foreach", Kw::Foreach}, {"foreach_reverse", Kw::ForeachReverse}, {"function", Kw::Function},
    {"global", Kw::Global}, {"if", Kw::If}, {"keys", Kw::Keys}, {"load", Kw::Load}, {"local", Kw::Local},
    {"loop", Kw::Loop}, {"macro", Kw::Macro}, {"map", Kw::Map}, {"null", Kw::Null}, {"pause", Kw::Pause},
    {"resume", Kw::Resume}, {"return", Kw::Return}, {"skip_frame", Kw::SkipFrame}, {"this", Kw::This},
    {"thread", Kw::Thread}, {"true", Kw::True}, {"values", Kw::Values}, {"wait", Kw::Wait},
    {"wait_until", Kw::WaitUntil}, {"wait_while", Kw::WaitWhile}, {"while", Kw::While}, {"with", Kw::With},
};

// longest match first
struct OpName { const char* text; Op op; };
const OpName kOps[] = {
    {">>>=", Op::UshrAssign}, {">>>", Op::Ushr}, {"<<=", Op::ShlAssign}, {">>=", Op::ShrAssign},
    {"==", Op::Eq}, {"!=", Op::Ne}, {"<=", Op::Le}, {">=", Op::Ge}, {"<<", Op::Shl}, {">>", Op::Shr},
    {"&&", Op::AndAnd}, {"||", Op::OrOr}, {"++", Op::PreInc}, {"--", Op::PreDec}, {"+=", Op::AddAssign},
    {"-=", Op::SubAssign}, {"*=", Op::MulAssign}, {"/=", Op::DivAssign}, {"%=", Op::ModAssign},
    {"&=", Op::AndAssign}, {"|=", Op::OrAssign}, {"^=", Op::XorAssign},
    {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Mod}, {"<", Op::Lt}, {">", Op::Gt},
    {"!", Op::Not}, {"&", Op::BitAnd}, {"|", Op::BitOr}, {"^", Op::BitXor}, {"~", Op::BitNot}, {"=", Op::Assign},
    {"?", Op::Question}, {":", Op::Colon}, {"@", Op::Ref}, {"#", Op::Count},
};

bool isIdentStart(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char16_t c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }

}  // namespace

const char* kwText(Kw kw) {
  for (auto& k : kKeywords)
    if (k.kw == kw) return k.text;
  return "?";
}

std::vector<Token> tokenize(const std::u16string& s) {
  std::vector<Token> out;
  size_t i = 0, n = s.size();
  int line = 1;
  size_t lineStart = 0;
  auto error = [&](const std::string& msg) {
    Token t;
    t.kind = Tok::Error;
    t.text = utf8ToUtf16(msg);
    t.line = line;
    t.col = int(i - lineStart) + 1;
    out.push_back(std::move(t));
    return out;
  };
  while (true) {
    // whitespace and comments
    while (i < n) {
      char16_t c = s[i];
      if (c == '\n') { ++line; lineStart = ++i; }
      else if (c == ' ' || c == '\t' || c == '\r' || c == 0xFEFF || c == '\f' || c == '\v' || c == 0) ++i;
      else if (c == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') ++i; }
      else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
          if (s[i] == '\n') { ++line; lineStart = i + 1; }
          ++i;
        }
        if (i + 1 >= n) return error("unterminated comment");
        i += 2;
      } else break;
    }
    Token t;
    t.line = line;
    t.col = int(i - lineStart) + 1;
    if (i >= n) {
      t.kind = Tok::End;
      out.push_back(std::move(t));
      return out;
    }
    char16_t c = s[i];
    if (isIdentStart(c)) {
      size_t b = i;
      while (i < n && isIdentChar(s[i])) ++i;
      t.text = s.substr(b, i - b);
      t.kind = Tok::Ident;
      std::string ascii;
      for (char16_t ch : t.text) ascii += static_cast<char>(ch);  // identifiers are ASCII
      for (auto& k : kKeywords)
        if (ascii == k.text) { t.kind = Tok::Keyword; t.kw = k.kw; break; }
    } else if (c == '$') {
      size_t b = ++i;
      while (i < n && isIdentChar(s[i])) ++i;
      t.kind = Tok::Var;
      t.text = s.substr(b, i - b);
    } else if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(s[i + 1]))) {
      size_t b = i;
      if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        uint64_t v = 0;
        size_t d = i;
        while (i < n) {
          char16_t h = s[i];
          int x = isDigit(h) ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
          if (x < 0) break;
          v = v * 16 + uint64_t(x);
          ++i;
        }
        if (i == d) return error("bad hex literal");
        t.kind = Tok::Int;
        t.ival = int64_t(v);
      } else {
        bool isFloat = false;
        while (i < n && isDigit(s[i])) ++i;
        if (i < n && s[i] == '.' && !(i + 1 < n && s[i + 1] == '.')) {
          isFloat = true;
          ++i;
          while (i < n && isDigit(s[i])) ++i;
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
          size_t save = i++;
          if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
          if (i < n && isDigit(s[i])) {
            isFloat = true;
            while (i < n && isDigit(s[i])) ++i;
          } else {
            i = save;
          }
        }
        std::string num;
        for (size_t k = b; k < i; ++k) num += static_cast<char>(s[k]);  // ASCII digits only
        if (isFloat) {
          t.kind = Tok::Float;
          t.fval = std::strtod(num.c_str(), nullptr);
        } else {
          t.kind = Tok::Int;
          t.ival = std::strtoll(num.c_str(), nullptr, 10);
        }
        if (i < n && (s[i] == 'f' || s[i] == 'F') && !(i + 1 < n && isIdentChar(s[i + 1]))) {
          ++i;  // tolerate a C-style float suffix
          t.kind = Tok::Float;
          t.fval = isFloat ? t.fval : double(t.ival);
        }
      }
      if (i < n && isIdentStart(s[i])) return error("bad number literal");
    } else if (c == '"' || c == '\'') {
      char16_t q = c;
      ++i;
      std::u16string v;
      while (true) {
        if (i >= n) return error("unterminated string");
        char16_t d = s[i++];
        if (d == '\n') {  // string literals may span lines (metro/strings/es.strings)
          ++line;
          lineStart = i;
        }
        if (d == q) break;
        if (d == '\\' && i < n) {
          char16_t e = s[i++];
          switch (e) {
            case 'n': v += u'\n'; break;
            case 't': v += u'\t'; break;
            case 'r': v += u'\r'; break;
            case '0': v += char16_t(0); break;
            case '\\': v += u'\\'; break;
            case '"': v += u'"'; break;
            case '\'': v += u'\''; break;
            case 'u': {  // \uXXXX: what the value export writes for code units ≥ 0x80 (values.md §7), e.g. in saves
              char16_t code = 0;
              int digits = 0;
              for (; digits < 4 && i < n; ++digits, ++i) {
                char16_t h = s[i];
                int x = h >= '0' && h <= '9' ? h - '0' : h >= 'A' && h <= 'F' ? h - 'A' + 10 : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
                if (x < 0) break;
                code = char16_t(code * 16 + x);
              }
              if (digits == 4) {
                v += code;
              } else {  // not an escape after all: keep the text as written
                v += u"\\u";
                v.append(s.substr(i - digits, digits));
              }
              break;
            }
            default: v += u'\\'; v += e; break;  // unknown escapes are kept verbatim (to be confirmed against the lexer)
          }
        } else {
          v += d;
        }
      }
      if (q == '"') {
        t.kind = Tok::String;
        t.text = std::move(v);
      } else {
        t.kind = Tok::Char;
        t.text = v;
        t.ival = v.empty() ? 0 : v[0];
      }
    } else {
      switch (c) {
        case '(': t.kind = Tok::LParen; ++i; break;
        case ')': t.kind = Tok::RParen; ++i; break;
        case '[': t.kind = Tok::LBracket; ++i; break;
        case ']': t.kind = Tok::RBracket; ++i; break;
        case '{': t.kind = Tok::LBrace; ++i; break;
        case '}': t.kind = Tok::RBrace; ++i; break;
        case ',': t.kind = Tok::Comma; ++i; break;
        case ';': t.kind = Tok::Semicolon; ++i; break;
        case '.': t.kind = Tok::Dot; ++i; break;
        default: {
          if (c == ':' && i + 1 < n && s[i + 1] == ':') {
            t.kind = Tok::DoubleColon;
            i += 2;
            break;
          }
          bool found = false;
          for (auto& o : kOps) {
            size_t len = std::strlen(o.text);
            if (i + len > n) continue;
            bool match = true;
            for (size_t k = 0; k < len; ++k)
              if (s[i + k] != char16_t(o.text[k])) { match = false; break; }
            if (match) {
              t.kind = Tok::Operator;
              t.op = o.op;
              i += len;
              found = true;
              break;
            }
          }
          if (!found) return error("unexpected character U+" + std::to_string(int(c)));
        }
      }
    }
    out.push_back(std::move(t));
  }
}

}  // namespace regs::script
