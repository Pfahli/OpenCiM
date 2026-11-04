#include "script/parser.h"

#include <stdexcept>

#include "core/text.h"

namespace regs::script {

const char* nodeName(N n) {
  static const char* names[] = {
      "Int", "Float", "String", "Char", "True", "False", "Null", "This", "Current", "SkipFrame", "Var", "Ident",
      "WithMember", "Member", "Index", "Call", "Unary", "Postfix", "Binary", "Assign", "ArrayLit", "MapLit", "Keys",
      "Values", "Defined", "Load", "Macro", "Thread", "FunctionExpr", "Program", "Block", "Empty", "ExprStmt", "If",
      "While", "DoWhile", "For", "Foreach", "ForeachReverse", "Loop", "With", "Return", "Break", "Continue", "Exit",
      "Wait", "WaitUntil", "WaitWhile", "Pause", "Resume", "Echo", "Assert", "Delete", "Local", "Global", "Export",
      "FunctionDecl", "EventDecl"};
  return names[size_t(n)];
}

std::string dumpAst(const Node* n, int indent) {
  if (!n) return std::string(indent, ' ') + "nil\n";
  std::string s(indent, ' ');
  s += nodeName(n->kind);
  if (n->op != Op::None) s += std::string(" ") + opText(n->op);
  if (!n->name.empty()) s += " '" + utf16ToUtf8(n->name) + "'";
  if (n->kind == N::Int || n->kind == N::Char) s += " " + std::to_string(n->i);
  if (n->kind == N::Float) s += " " + std::to_string(n->f);
  if ((n->kind == N::Member || n->kind == N::WithMember) && n->i) s += " $";
  for (auto& p : n->params) s += " param:" + utf16ToUtf8(p.name) + (p.defaultValue ? "=" : "");
  s += "\n";
  for (auto& p : n->params)
    if (p.defaultValue) s += dumpAst(p.defaultValue.get(), indent + 4);
  for (auto& k : n->kids) s += dumpAst(k.get(), indent + 2);
  return s;
}

namespace {

struct ParseError : std::runtime_error {
  int line, col;
  ParseError(const std::string& m, int l, int c) : std::runtime_error(m), line(l), col(c) {}
};

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : t_(std::move(toks)) {}

  NodePtr program() {
    auto p = node(N::Program);
    while (cur().kind != Tok::End) p->kids.push_back(statement());
    return p;
  }

 private:
  std::vector<Token> t_;
  size_t pos_ = 0;

  const Token& cur() const { return t_[pos_]; }
  const Token& peek(size_t k = 1) const { return t_[std::min(pos_ + k, t_.size() - 1)]; }
  void advance() {
    if (pos_ + 1 < t_.size()) ++pos_;
  }
  NodePtr node(N k) const { return std::make_unique<Node>(k, cur().line); }

  [[noreturn]] void fail(const std::string& msg) const {
    std::string where = describe(cur());
    throw ParseError(msg + " (at " + where + ")", cur().line, cur().col);
  }
  static std::string describe(const Token& t) {
    switch (t.kind) {
      case Tok::End: return "end of file";
      case Tok::Ident: return "'" + utf16ToUtf8(t.text) + "'";
      case Tok::Keyword: return std::string("keyword '") + kwText(t.kw) + "'";
      case Tok::Var: return "'$" + utf16ToUtf8(t.text) + "'";
      case Tok::Int: return "integer";
      case Tok::Float: return "float";
      case Tok::String: return "string";
      case Tok::Char: return "char literal";
      case Tok::Operator: return std::string("'") + opText(t.op) + "'";
      case Tok::LParen: return "'('";
      case Tok::RParen: return "')'";
      case Tok::LBracket: return "'['";
      case Tok::RBracket: return "']'";
      case Tok::LBrace: return "'{'";
      case Tok::RBrace: return "'}'";
      case Tok::Comma: return "','";
      case Tok::Dot: return "'.'";
      case Tok::Semicolon: return "';'";
      case Tok::DoubleColon: return "'::'";
      case Tok::Error: return "lexical error: " + utf16ToUtf8(t.text);
    }
    return "?";
  }

  bool is(Tok k) const { return cur().kind == k; }
  bool isKw(Kw k) const { return cur().kind == Tok::Keyword && cur().kw == k; }
  bool isOp(Op o) const { return cur().kind == Tok::Operator && cur().op == o; }
  void expect(Tok k, const char* what) {
    if (!is(k)) fail(std::string("expected ") + what);
    advance();
  }
  bool accept(Tok k) {
    if (!is(k)) return false;
    advance();
    return true;
  }

  // ---- statements ----

  NodePtr block() {
    auto b = node(N::Block);
    expect(Tok::LBrace, "'{'");
    while (!is(Tok::RBrace)) {
      if (is(Tok::End)) fail("unterminated block");
      b->kids.push_back(statement());
    }
    advance();
    return b;
  }

  void endStatement() {
    if (accept(Tok::Semicolon)) return;
    // a statement may end without ';' right before '}' or at the end of the file
    if (is(Tok::RBrace) || is(Tok::End)) return;
    fail("expected ';'");
  }

  NodePtr parenExpr() {
    expect(Tok::LParen, "'('");
    auto e = expr();
    expect(Tok::RParen, "')'");
    return e;
  }

  NodePtr simpleStatement(N kind, bool hasOperand) {
    auto s = node(kind);
    advance();
    if (hasOperand && !is(Tok::Semicolon) && !is(Tok::RBrace)) s->kids.push_back(expr());
    endStatement();
    return s;
  }

  NodePtr statement() {
    const Token& t = cur();
    if (t.kind == Tok::Error) fail("lexical error");
    if (t.kind == Tok::LBrace) return block();
    if (t.kind == Tok::Semicolon) {
      auto s = node(N::Empty);
      advance();
      return s;
    }
    if (t.kind == Tok::Keyword) {
      switch (t.kw) {
        case Kw::If: {
          auto s = node(N::If);
          advance();
          s->kids.push_back(parenExpr());
          s->kids.push_back(statement());
          if (isKw(Kw::Else)) {
            advance();
            s->kids.push_back(statement());
          }
          return s;
        }
        case Kw::While: {
          auto s = node(N::While);
          advance();
          s->kids.push_back(parenExpr());
          s->kids.push_back(statement());
          return s;
        }
        case Kw::Do: {
          auto s = node(N::DoWhile);
          advance();
          s->kids.push_back(statement());
          if (!isKw(Kw::While)) fail("expected 'while'");
          advance();
          s->kids.push_back(parenExpr());
          endStatement();
          return s;
        }
        case Kw::For: {
          auto s = node(N::For);
          advance();
          expect(Tok::LParen, "'('");
          s->kids.push_back(is(Tok::Semicolon) ? node(N::Empty) : expr());
          expect(Tok::Semicolon, "';'");
          s->kids.push_back(is(Tok::Semicolon) ? node(N::True) : expr());
          expect(Tok::Semicolon, "';'");
          s->kids.push_back(is(Tok::RParen) ? node(N::Empty) : expr());
          expect(Tok::RParen, "')'");
          s->kids.push_back(statement());
          return s;
        }
        case Kw::Foreach:
        case Kw::ForeachReverse: {
          auto s = node(t.kw == Kw::Foreach ? N::Foreach : N::ForeachReverse);
          advance();
          s->kids.push_back(parenExpr());
          s->kids.push_back(statement());
          return s;
        }
        case Kw::Loop: {
          auto s = node(N::Loop);
          advance();
          s->kids.push_back(statement());
          return s;
        }
        case Kw::With: {
          auto s = node(N::With);
          advance();
          s->kids.push_back(parenExpr());
          s->kids.push_back(statement());
          return s;
        }
        case Kw::Return: return simpleStatement(N::Return, true);
        case Kw::Break: return simpleStatement(N::Break, false);
        case Kw::Continue: return simpleStatement(N::Continue, false);
        case Kw::Exit: return simpleStatement(N::Exit, false);
        case Kw::Wait: return simpleStatement(N::Wait, true);
        case Kw::Pause: return simpleStatement(N::Pause, true);
        case Kw::Resume: return simpleStatement(N::Resume, true);
        case Kw::Echo: return simpleStatement(N::Echo, true);
        case Kw::Assert: return simpleStatement(N::Assert, true);
        case Kw::Delete: return simpleStatement(N::Delete, true);
        case Kw::Export: return simpleStatement(N::Export, true);
        case Kw::WaitUntil:
        case Kw::WaitWhile: {
          auto s = node(t.kw == Kw::WaitUntil ? N::WaitUntil : N::WaitWhile);
          advance();
          s->kids.push_back(expr());  // parentheses are optional (metro/ui/ui.script:713)
          endStatement();
          return s;
        }
        case Kw::Local:
        case Kw::Global: {
          auto s = node(t.kw == Kw::Local ? N::Local : N::Global);
          advance();
          s->kids.push_back(expr());
          while (accept(Tok::Comma)) s->kids.push_back(expr());
          endStatement();
          return s;
        }
        case Kw::Function:
          if (peek().kind == Tok::Ident) return functionDecl();
          break;
        case Kw::Event: return eventDecl();
        default: break;
      }
    }
    auto s = node(N::ExprStmt);
    s->kids.push_back(expr());
    // thread/macro/function expressions that end in a block need no ';'
    if (pos_ > 0 && t_[pos_ - 1].kind == Tok::RBrace) {
      accept(Tok::Semicolon);
      return s;
    }
    endStatement();
    return s;
  }

  void paramList(Node* fn) {
    expect(Tok::LParen, "'('");
    if (!is(Tok::RParen)) {
      do {
        if (!is(Tok::Var)) fail("expected parameter");
        Param p;
        p.name = cur().text;
        advance();
        if (isOp(Op::Assign)) {
          advance();
          p.defaultValue = expr(15);
        }
        fn->params.push_back(std::move(p));
      } while (accept(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
  }

  NodePtr functionDecl() {
    auto f = node(N::FunctionDecl);
    advance();  // function
    f->name = cur().text;
    advance();
    paramList(f.get());
    f->kids.push_back(block());
    return f;
  }

  NodePtr eventDecl() {
    auto e = node(N::EventDecl);
    advance();  // event
    auto target = postfix();
    expect(Tok::DoubleColon, "'::'");
    NodePtr ctx;
    if (accept(Tok::LBracket)) {
      ctx = expr();
      expect(Tok::RBracket, "']'");
    }
    paramList(e.get());
    e->kids.push_back(std::move(target));
    e->kids.push_back(statement());  // usually a block; a single statement is allowed (metro/ui/ui.script:592)
    if (ctx) e->kids.push_back(std::move(ctx));
    return e;
  }

  // ---- expressions ----

  NodePtr expr(int maxPrec = 16) {
    auto left = unary();
    while (is(Tok::Operator)) {
      Op op = cur().op;
      if (op == Op::PreInc || op == Op::PreDec) break;  // a postfix ++/-- was already consumed by postfix()
      OpInfo info = opInfo(op);
      if (info.kind == OpKind::Prefix && op != Op::Add && op != Op::Sub && op != Op::BitAnd) break;
      if (info.kind != OpKind::Binary && info.kind != OpKind::Assign) break;
      if (info.precedence > maxPrec) break;
      advance();
      if (info.kind == OpKind::Assign) {
        auto a = std::make_unique<Node>(N::Assign, left->line);
        a->op = op;
        a->kids.push_back(std::move(left));
        a->kids.push_back(expr(info.precedence));
        left = std::move(a);
      } else {
        auto b = std::make_unique<Node>(N::Binary, left->line);
        b->op = op;
        b->kids.push_back(std::move(left));
        b->kids.push_back(expr(info.precedence - 1));
        left = std::move(b);
      }
    }
    return left;
  }

  NodePtr unary() {
    if (is(Tok::Operator)) {
      Op op = cur().op;
      Op pre = Op::None;
      switch (op) {
        case Op::Not: case Op::BitNot: case Op::PreInc: case Op::PreDec: case Op::Ref: case Op::Count: pre = op; break;
        case Op::Add: pre = Op::Plus; break;
        case Op::Sub: pre = Op::Neg; break;
        default: break;
      }
      if (pre != Op::None) {
        auto u = node(N::Unary);
        u->op = pre;
        advance();
        u->kids.push_back(unary());
        return u;
      }
    }
    if (is(Tok::Keyword) && (cur().kw == Kw::Keys || cur().kw == Kw::Values || cur().kw == Kw::Defined)) {
      auto u = node(cur().kw == Kw::Keys ? N::Keys : cur().kw == Kw::Values ? N::Values : N::Defined);
      advance();
      u->kids.push_back(unary());
      return u;
    }
    return postfix();
  }

  void memberName(Node* m) {
    if (is(Tok::Ident) || is(Tok::Keyword)) {
      m->name = cur().text;
      advance();
    } else if (is(Tok::Var)) {
      m->name = cur().text;
      m->i = 1;
      advance();
    } else {
      fail("expected member name");
    }
  }

  NodePtr postfix() {
    auto e = primary();
    for (;;) {
      if (is(Tok::Dot)) {
        advance();
        auto m = std::make_unique<Node>(N::Member, e->line);
        memberName(m.get());
        m->kids.push_back(std::move(e));
        e = std::move(m);
      } else if (is(Tok::LBracket)) {
        advance();
        auto ix = std::make_unique<Node>(N::Index, e->line);
        ix->kids.push_back(std::move(e));
        if (!is(Tok::RBracket)) ix->kids.push_back(expr());  // a[] (no index) appends: $c[] = x
        expect(Tok::RBracket, "']'");
        e = std::move(ix);
      } else if (is(Tok::LParen)) {
        advance();
        auto c = std::make_unique<Node>(N::Call, e->line);
        c->kids.push_back(std::move(e));
        if (!is(Tok::RParen)) {
          do c->kids.push_back(expr());
          while (accept(Tok::Comma));
        }
        expect(Tok::RParen, "')'");
        e = std::move(c);
      } else if (isOp(Op::PreInc) || isOp(Op::PreDec)) {
        auto p = std::make_unique<Node>(N::Postfix, e->line);
        p->op = cur().op == Op::PreInc ? Op::PostInc : Op::PostDec;
        advance();
        p->kids.push_back(std::move(e));
        e = std::move(p);
      } else {
        return e;
      }
    }
  }

  NodePtr listUntil(N kind, Tok close, const char* closeText) {
    auto l = node(kind);
    if (!is(close)) {
      do {
        if (is(close)) break;  // tolerate a trailing comma
        l->kids.push_back(expr());
      } while (accept(Tok::Comma));
    }
    expect(close, closeText);
    return l;
  }

  NodePtr primary() {
    const Token& t = cur();
    switch (t.kind) {
      case Tok::Int: {
        auto n = node(N::Int);
        n->i = t.ival;
        advance();
        return n;
      }
      case Tok::Float: {
        auto n = node(N::Float);
        n->f = t.fval;
        advance();
        return n;
      }
      case Tok::String: {
        auto n = node(N::String);
        n->name = t.text;
        advance();
        return n;
      }
      case Tok::Char: {
        auto n = node(N::Char);
        n->name = t.text;
        n->i = t.ival;
        advance();
        return n;
      }
      case Tok::Var: {
        auto n = node(N::Var);
        n->name = t.text;
        advance();
        return n;
      }
      case Tok::Ident: {
        auto n = node(N::Ident);
        n->name = t.text;
        advance();
        return n;
      }
      case Tok::Dot: {
        auto n = node(N::WithMember);
        advance();
        memberName(n.get());
        return n;
      }
      case Tok::LParen: {
        advance();
        auto e = expr();
        expect(Tok::RParen, "')'");
        return e;
      }
      case Tok::LBracket: {
        advance();
        return listUntil(N::ArrayLit, Tok::RBracket, "']'");
      }
      case Tok::Keyword:
        switch (t.kw) {
          case Kw::True: advance(); return std::make_unique<Node>(N::True, t.line);
          case Kw::False: advance(); return std::make_unique<Node>(N::False, t.line);
          case Kw::Null: advance(); return std::make_unique<Node>(N::Null, t.line);
          case Kw::This: { auto n = node(N::This); advance(); return n; }
          case Kw::Current: { auto n = node(N::Current); advance(); return n; }
          case Kw::SkipFrame: { auto n = node(N::SkipFrame); advance(); return n; }
          case Kw::Map: {
            int line = t.line;
            advance();
            expect(Tok::LBracket, "'[' after map");
            auto m = listUntil(N::MapLit, Tok::RBracket, "']'");
            m->line = line;
            return m;
          }
          case Kw::Array: {
            int line = t.line;
            advance();
            if (accept(Tok::LBracket)) {
              auto a = listUntil(N::ArrayLit, Tok::RBracket, "']'");
              a->line = line;
              return a;
            }
            auto a = std::make_unique<Node>(N::ArrayLit, line);
            return a;
          }
          case Kw::Load: {
            auto l = node(N::Load);
            advance();
            auto first = expr(15);
            if (accept(Tok::DoubleColon)) {
              l->kids.push_back(expr(15));
              l->kids.push_back(std::move(first));
            } else {
              l->kids.push_back(std::move(first));
            }
            return l;
          }
          case Kw::Macro: {
            auto m = node(N::Macro);
            advance();
            std::vector<NodePtr> args;
            if (accept(Tok::LParen)) {
              if (!is(Tok::RParen)) {
                do args.push_back(expr());
                while (accept(Tok::Comma));
              }
              expect(Tok::RParen, "')'");
            }
            expect(Tok::DoubleColon, "'::' after macro");
            m->kids.push_back(expr());
            for (auto& a : args) m->kids.push_back(std::move(a));
            return m;
          }
          case Kw::Thread: {
            auto th = node(N::Thread);
            advance();
            NodePtr group;
            if (!is(Tok::DoubleColon)) group = expr(15);
            expect(Tok::DoubleColon, "'::' after thread");
            th->kids.push_back(statement());
            if (group) th->kids.push_back(std::move(group));
            return th;
          }
          case Kw::Function: {
            auto f = node(N::FunctionExpr);
            advance();
            paramList(f.get());
            f->kids.push_back(block());
            return f;
          }
          default: break;
        }
        break;
      default: break;
    }
    fail("expected expression");
  }
};

}  // namespace

ParseResult parse(const std::u16string& source) {
  ParseResult r;
  auto toks = tokenize(source);
  if (!toks.empty() && toks.back().kind == Tok::Error) {
    r.line = toks.back().line;
    r.col = toks.back().col;
    r.error = std::to_string(r.line) + ":" + std::to_string(r.col) + ": " + utf16ToUtf8(toks.back().text);
    return r;
  }
  try {
    Parser p(std::move(toks));
    r.program = p.program();
  } catch (const ParseError& e) {
    r.line = e.line;
    r.col = e.col;
    r.error = std::to_string(e.line) + ":" + std::to_string(e.col) + ": " + e.what();
  }
  return r;
}

}  // namespace regs::script
