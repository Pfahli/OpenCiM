#include "script/compiler.h"

#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include "core/text.h"
#include "script/parser.h"

namespace regs::script {

int Code::lineAt(uint32_t pc) const {
  int line = 0;
  for (auto& [p, l] : lines) {
    if (p > pc) break;
    line = l;
  }
  return line;
}

uint32_t Code::intern(const std::u16string& s) {
  auto it = stringIndex.find(s);
  if (it != stringIndex.end()) return it->second;
  uint32_t i = uint32_t(strings.size());
  strings.push_back(s);
  stringIndex.emplace(s, i);
  return i;
}

namespace {

struct CompileError : std::runtime_error {
  int line;
  CompileError(const std::string& m, int l) : std::runtime_error(m), line(l) {}
};

// Locals of one activation: the top-level code, a function, an event handler or a macro. Thread bodies compile in the
// enclosing activation; at run time the new thread gets a copy of its locals (docs/script-runtime/semantics.md §5.1).
// $name resolution follows the engine's single source-order pass (§1.1): in Top and Macro mode it is always a script
// variable; in Function and Handler mode it is a local if declared, else a script variable if that name is already in
// the script's variable map at this point of the file, else a new implicit local.
enum class Mode { Top, Function, Handler, Macro };

struct Activation {
  Mode mode = Mode::Top;
  std::unordered_map<std::u16string, uint16_t> locals;
  uint16_t count = 0;

  uint16_t declare(const std::u16string& name) {
    auto it = locals.find(name);
    if (it != locals.end()) return it->second;
    locals.emplace(name, count);
    return count++;
  }
  int find(const std::u16string& name) const {
    auto it = locals.find(name);
    return it == locals.end() ? -1 : it->second;
  }
};

struct Loop {
  std::vector<size_t> breaks;     // jump operand positions to patch to the exit
  std::vector<size_t> continues;  // ... to the continue target
  int ctxDepth = 0;               // context-stack depth inside the loop (foreach iterator included)
};

class Compiler {
 public:
  explicit Compiler(Ref<Code> code) : c_(std::move(code)) {}

  void program(const Node* p) {
    Activation top;
    top.mode = Mode::Top;
    act_ = &top;
    for (auto& s : p->kids) stmt(s.get());
    op(Opc::End);
    c_->topLocals = top.count;
  }

  // An expression fragment (state-machine guard) ends with HaltValue; a block fragment (action) with End.
  // Both compile in top-level mode, so $name is the owning script's variable.
  FragmentInfo fragment(const Node* p, bool expression) {
    Activation a;
    a.mode = Mode::Top;
    act_ = &a;
    line_ = 0;
    FragmentInfo info;
    info.entry = uint32_t(pc());
    if (expression) {
      if (p->kids.size() != 1 || p->kid(0)->kind != N::ExprStmt) fail("Expression fragment expected", p);
      expr(p->kid(0)->kid(0));
      op(Opc::HaltValue);
    } else {
      for (auto& s : p->kids) stmt(s.get());
      op(Opc::End);
    }
    info.nlocals = a.count;
    return info;
  }

 private:
  Ref<Code> c_;
  Activation* act_ = nullptr;
  std::vector<Loop> loops_;
  int ctxDepth_ = 0;
  int line_ = 0;
  bool forceGlobal_ = false;
  bool inThread_ = false;

  static bool isDigits(const std::u16string& s) {
    if (s.empty()) return false;
    for (char16_t c : s)
      if (c < '0' || c > '9') return false;
    return true;
  }

  // Resolves $name: a local slot, or -1 for the script variable (registered in the script's variable map).
  int resolveVar(const Node* n, bool forceScript) {
    const std::u16string& name = n->name;
    if (isDigits(name)) {  // $0, $1, ...: parameters / macro captures
      if (act_->mode == Mode::Top) fail("Parameter \"$" + utf16ToUtf8(name) + "\" not allowed here!", n);
      int idx = std::stoi(utf16ToUtf8(name));
      while (act_->count <= idx) act_->declare(u"\x1F" + utf8ToUtf16(std::to_string(act_->count)));
      return idx;
    }
    if (!forceScript && (act_->mode == Mode::Function || act_->mode == Mode::Handler)) {
      int slot = act_->find(name);
      if (slot >= 0) return slot;
      if (!c_->hasScriptVar(name)) return act_->declare(name);  // implicit local
    } else if (!forceScript && act_->mode == Mode::Top) {
      int slot = act_->find(name);  // explicit 'local' at top level
      if (slot >= 0) return slot;
    }
    c_->addScriptVar(name);
    return -1;
  }

  // ---- emission ----
  std::vector<uint8_t>& bc() { return c_->bc; }
  size_t pc() const { return c_->bc.size(); }
  void at(const Node* n) {
    if (n && n->line && n->line != line_) {
      line_ = n->line;
      c_->lines.emplace_back(uint32_t(pc()), line_);
    }
  }
  void op(Opc o) { bc().push_back(uint8_t(o)); }
  void u8(uint8_t v) { bc().push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
  void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
  void f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); u32(u); }
  size_t jump(Opc o) {  // returns the operand position to patch
    op(o);
    size_t p = pc();
    u32(0);
    return p;
  }
  void patch(size_t operandPos, size_t target) {
    int32_t rel = int32_t(int64_t(target) - int64_t(operandPos + 4));
    std::memcpy(&bc()[operandPos], &rel, 4);
  }
  void jumpTo(Opc o, size_t target) { patch(jump(o), target); }
  void str(Opc o, const std::u16string& s) { op(o); u32(c_->intern(s)); }

  [[noreturn]] void fail(const std::string& m, const Node* n) { throw CompileError(m, n ? n->line : line_); }

  // ---- statements ----
  void stmt(const Node* n) {
    at(n);
    switch (n->kind) {
      case N::Block:
        for (auto& k : n->kids) stmt(k.get());
        break;
      case N::Empty:
        break;
      case N::ExprStmt:
        expr(n->kid(0));
        op(Opc::Pop);
        break;
      case N::If: {
        expr(n->kid(0));
        size_t jz = jump(Opc::Jz);
        stmt(n->kid(1));
        if (n->kids.size() > 2) {
          size_t jmp = jump(Opc::Jmp);
          patch(jz, pc());
          stmt(n->kid(2));
          patch(jmp, pc());
        } else {
          patch(jz, pc());
        }
        break;
      }
      case N::While: {
        size_t start = pc();
        expr(n->kid(0));
        size_t jz = jump(Opc::Jz);
        loopBody(n->kid(1), start, 0);
        jumpTo(Opc::Jmp, start);
        patch(jz, pc());
        endLoop(pc(), start);
        break;
      }
      case N::DoWhile: {
        size_t start = pc();
        loopBody(n->kid(0), 0, 0);
        size_t cont = pc();
        expr(n->kid(1));
        jumpTo(Opc::Jnz, start);
        endLoop(pc(), cont);
        break;
      }
      case N::For: {
        if (n->kid(0)->kind != N::Empty) {
          expr(n->kid(0));
          op(Opc::Pop);
        }
        size_t start = pc();
        expr(n->kid(1));
        size_t jz = jump(Opc::Jz);
        loopBody(n->kid(3), 0, 0);
        size_t cont = pc();
        if (n->kid(2)->kind != N::Empty) {
          expr(n->kid(2));
          op(Opc::Pop);
        }
        jumpTo(Opc::Jmp, start);
        patch(jz, pc());
        endLoop(pc(), cont);
        break;
      }
      case N::Loop: {
        size_t start = pc();
        loopBody(n->kid(0), 0, 0);
        jumpTo(Opc::Jmp, start);
        endLoop(pc(), start);
        break;
      }
      case N::Foreach:
      case N::ForeachReverse: {
        bool rev = n->kind == N::ForeachReverse;
        expr(n->kid(0));
        size_t exitJump = jump(rev ? Opc::ForeachRev : Opc::Foreach);
        ++ctxDepth_;
        size_t body = pc();
        loopBody(n->kid(1), 0, 0);
        size_t cont = pc();
        jumpTo(rev ? Opc::ForeachRevNext : Opc::ForeachNext, body);
        size_t exit = pc();
        patch(exitJump, exit);
        endLoop(exit, cont);
        op(Opc::CtxPop);
        --ctxDepth_;
        break;
      }
      case N::With:
        expr(n->kid(0));
        op(Opc::CtxPush);
        ++ctxDepth_;
        stmt(n->kid(1));
        op(Opc::CtxPop);
        --ctxDepth_;
        break;
      case N::Return:
        // legal only in functions and event handlers; in a handler it ends the thread and takes no value
        // (0x100067c0.c:907-945); in a thread body inside a function it ends that thread (engine behaviour untraced)
        if (act_->mode == Mode::Function && !inThread_) {
          if (n->kids.empty()) op(Opc::PushNone);
          else expr(n->kid(0));
          op(Opc::Return);
        } else if (act_->mode == Mode::Handler || (act_->mode == Mode::Function && inThread_)) {
          if (!n->kids.empty()) fail("Reserved word \"return\" not allowed here!", n);
          op(Opc::End);
        } else {
          fail("Reserved word \"return\" not allowed here!", n);
        }
        break;
      case N::Break:
      case N::Continue: {
        if (loops_.empty()) fail(n->kind == N::Break ? "'break' outside a loop" : "'continue' outside a loop", n);
        Loop& l = loops_.back();
        bool brk = n->kind == N::Break;
        // leave nested 'with' contexts; a 'break' out of foreach jumps to the iterator's CtxPop
        for (int d = ctxDepth_; d > l.ctxDepth; --d) op(Opc::CtxPop);
        (brk ? l.breaks : l.continues).push_back(jump(Opc::Jmp));
        break;
      }
      case N::Exit:
        op(Opc::Exit);
        break;
      case N::Wait:
        if (n->kids.empty()) {
          op(Opc::PushInt);
          u32(0);
        } else {
          expr(n->kid(0));
        }
        op(Opc::Wait);
        break;
      case N::WaitUntil:
      case N::WaitWhile: {
        size_t start = pc();
        expr(n->kid(0));
        size_t done = jump(n->kind == N::WaitUntil ? Opc::Jnz : Opc::Jz);
        op(Opc::Yield);
        jumpTo(Opc::Jmp, start);
        patch(done, pc());
        break;
      }
      case N::Pause:
      case N::Resume:
        expr(n->kid(0));
        op(n->kind == N::Pause ? Opc::Pause : Opc::Resume);
        break;
      case N::Echo:
        if (n->kids.empty()) op(Opc::PushNone);
        else expr(n->kid(0));
        op(Opc::Echo);
        break;
      case N::Assert:
        expr(n->kid(0));
        op(Opc::Assert);
        break;
      case N::Delete:
        expr(n->kid(0));
        op(Opc::Delete);
        break;
      case N::Export:
        expr(n->kid(0));
        op(Opc::Export);
        op(Opc::Pop);
        break;
      case N::Local:
        for (auto& k : n->kids) {
          const Node* target = k->kind == N::Assign ? k->kid(0) : k.get();
          if (target->kind != N::Var) fail("'local' needs a variable", k.get());
          act_->declare(target->name);
          if (k->kind == N::Assign) {
            expr(k.get());
            op(Opc::Pop);
          }
        }
        break;
      case N::Global:  // forces the script variable and adds it to the map (0x100067c0.c:698-724)
        for (auto& k : n->kids) {
          const Node* root = k.get();
          while (root->kind == N::Assign || root->kind == N::Member || root->kind == N::Index) root = root->kid(0);
          if (root->kind == N::Var) c_->addScriptVar(root->name);
          if (k->kind == N::Var) continue;  // declaration only
          forceGlobal_ = true;
          expr(k.get());
          forceGlobal_ = false;
          op(Opc::Pop);
        }
        break;
      case N::FunctionDecl:
        if (act_->mode != Mode::Top || inThread_) fail("Reserved word \"function\" not allowed here!", n);
        functionDecl(n);
        break;
      case N::EventDecl:
        eventDecl(n);
        break;
      default:
        fail(std::string("unexpected statement ") + nodeName(n->kind), n);
    }
  }

  void loopBody(const Node* body, size_t, size_t) {
    loops_.push_back(Loop{{}, {}, ctxDepth_});
    stmt(body);
  }
  void endLoop(size_t exitTarget, size_t continueTarget) {
    Loop l = std::move(loops_.back());
    loops_.pop_back();
    for (size_t b : l.breaks) patch(b, exitTarget);
    for (size_t c : l.continues) patch(c, continueTarget);
  }

  // parameters: slots 0..n-1; a default runs when the slot is valueless (not passed, or passed unset)
  void prologue(const std::vector<Param>& params, const Node* where) {
    for (uint16_t i = 0; i < params.size(); ++i) {
      if (act_->find(params[i].name) >= 0) fail("Dublicate variable definition \"" + utf16ToUtf8(params[i].name) + "\"!", where);
      act_->declare(params[i].name);
    }
    for (uint16_t i = 0; i < params.size(); ++i) {
      if (!params[i].defaultValue) continue;
      op(Opc::DefaultArg);
      u16(i);
      size_t skip = pc();
      u32(0);
      expr(params[i].defaultValue.get());
      op(Opc::StoreLocal);
      u16(i);
      op(Opc::Pop);
      patch(skip, pc());
    }
  }

  // Functions compile where they appear (the engine's single pass), with a jump over the body in the top-level code;
  // calls above the definition still work because the linker binds names after the whole file is compiled.
  void functionDecl(const Node* n) {
    if (c_->functionIndex.count(n->name)) fail("Dublicate function definition \"" + utf16ToUtf8(n->name) + "\"!", n);
    int index = int(c_->functions.size());
    c_->functionIndex.emplace(n->name, index);
    FunctionInfo info;
    info.name = n->name;
    info.nparams = uint16_t(n->params.size());
    uint16_t defaults = 0;
    for (auto& p : n->params)
      if (p.defaultValue) ++defaults;
    info.minArgs = uint16_t(info.nparams - defaults);
    info.line = n->line;
    c_->functions.push_back(info);
    size_t over = jump(Opc::Jmp);
    Activation a;
    a.mode = Mode::Function;
    Activation* saved = act_;
    act_ = &a;
    auto savedLoops = std::move(loops_);
    loops_.clear();
    int savedCtx = ctxDepth_;
    ctxDepth_ = 0;
    c_->functions[size_t(index)].entry = uint32_t(pc());
    prologue(n->params, n);
    stmt(n->kid(0));
    op(Opc::PushNone);
    op(Opc::Return);
    c_->functions[size_t(index)].nlocals = a.count;
    act_ = saved;
    loops_ = std::move(savedLoops);
    ctxDepth_ = savedCtx;
    patch(over, pc());
  }

  // event target::[filter](params) body  ->  target [filter] BindEvent(F) h; Jmp over; body; End
  void eventDecl(const Node* n) {
    const Node* target = n->kid(0);
    std::u16string eventName;
    if (target->kind == N::Member) {
      expr(target->kid(0));
      eventName = (target->i ? u"$" : u"") + target->name;
    } else if (target->kind == N::WithMember) {
      op(Opc::PushCtx);
      eventName = (target->i ? u"$" : u"") + target->name;
    } else {
      fail("event target must be a member", n);
    }
    op(Opc::PushStr);
    u32(c_->intern(eventName));
    const Node* filter = n->kid(2);
    if (filter) expr(filter);
    uint32_t h = uint32_t(c_->handlers.size());
    c_->handlers.push_back(HandlerInfo{0, uint16_t(n->params.size()), 0, eventName, n->line});
    op(filter ? Opc::BindEventF : Opc::BindEvent);
    u32(h);
    size_t over = jump(Opc::Jmp);
    // the handler gets a fresh local map: it cannot see the enclosing function's locals (0x100067c0.c:343-372)
    Activation a;
    a.mode = Mode::Handler;
    Activation* saved = act_;
    act_ = &a;
    auto savedLoops = std::move(loops_);
    loops_.clear();
    int savedCtx = ctxDepth_;
    ctxDepth_ = 0;
    bool savedThread = inThread_;
    inThread_ = false;
    c_->handlers[h].entry = uint32_t(pc());
    for (auto& p : n->params)
      if (p.defaultValue) fail("event parameters cannot have default values", n);
    prologue(n->params, n);
    stmt(n->kid(1));
    op(Opc::End);
    c_->handlers[h].nlocals = a.count;
    act_ = saved;
    loops_ = std::move(savedLoops);
    ctxDepth_ = savedCtx;
    inThread_ = savedThread;
    patch(over, pc());
  }

  // ---- expressions ----
  void expr(const Node* n) {
    at(n);
    switch (n->kind) {
      case N::Int:
        op(Opc::PushInt);
        u32(uint32_t(n->i));  // literals wrap to 32 bits (0xFFFF0000 -> -65536)
        break;
      case N::Char:
        op(Opc::PushInt);
        u32(uint32_t(n->i & 0xFFFF));
        break;
      case N::Float:
        op(Opc::PushFloat);
        f32(float(n->f));
        break;
      case N::String:
        str(Opc::PushStr, n->name);
        break;
      case N::True: op(Opc::PushTrue); break;
      case N::False: op(Opc::PushFalse); break;
      case N::Null: op(Opc::PushNull); break;
      case N::This: op(Opc::PushThis); break;
      case N::Current: op(Opc::PushCtx); break;
      case N::SkipFrame:
        op(Opc::Yield);
        op(Opc::PushDt);
        break;
      case N::Var: {
        int slot = resolveVar(n, forceGlobal_);
        if (slot >= 0) {
          op(Opc::LoadLocal);
          u16(uint16_t(slot));
        } else {
          str(Opc::LoadVar, n->name);
        }
        break;
      }
      case N::Ident: {
        op(Opc::PushLink);
        u32(link(n->name, n->line));
        break;
      }
      case N::WithMember:
        op(Opc::PushCtx);
        str(Opc::GetMember, memberName(n));
        break;
      case N::Member: {
        bool saved = forceGlobal_;
        expr(n->kid(0));
        forceGlobal_ = saved;
        str(Opc::GetMember, memberName(n));
        break;
      }
      case N::Index:
        if (n->kids.size() < 2) fail("'[]' can only be assigned to", n);
        expr(n->kid(0));
        expr(n->kid(1));
        op(Opc::GetIndex);
        break;
      case N::Call: {
        expr(n->kid(0));
        if (n->kids.size() - 1 > 255) fail("Too many parameters for function", n);
        for (size_t i = 1; i < n->kids.size(); ++i) expr(n->kid(i));
        op(Opc::Call);
        u8(uint8_t(n->kids.size() - 1));
        break;
      }
      case N::Unary:
        switch (n->op) {
          case Op::PreInc: update(n->kid(0), kUpdPreInc, nullptr); break;
          case Op::PreDec: update(n->kid(0), kUpdPreDec, nullptr); break;
          case Op::Ref: reference(n->kid(0)); break;
          default:
            expr(n->kid(0));
            op(Opc::Unop);
            u8(uint8_t(n->op));
        }
        break;
      case N::Postfix:
        update(n->kid(0), n->op == Op::PostInc ? kUpdPostInc : kUpdPostDec, nullptr);
        break;
      case N::Binary:
        // the engine evaluates both operands and dispatches through the operator table (no short circuit)
        expr(n->kid(0));
        expr(n->kid(1));
        op(Opc::Binop);
        u8(uint8_t(n->op));
        break;
      case N::Assign:
        if (n->op == Op::Assign) {
          update(n->kid(0), kUpdAssignPlain, n->kid(1));
        } else {
          update(n->kid(0), compoundBase(n->op), n->kid(1));
        }
        break;
      case N::ArrayLit:
        for (auto& k : n->kids) expr(k.get());
        op(Opc::MakeArray);
        u32(uint32_t(n->kids.size()));
        break;
      case N::MapLit:
        if (n->kids.size() % 2) fail("map literal needs key/value pairs", n);
        for (auto& k : n->kids) expr(k.get());
        op(Opc::MakeMap);
        u32(uint32_t(n->kids.size()));
        break;
      case N::Keys: expr(n->kid(0)); op(Opc::Keys); break;
      case N::Values: expr(n->kid(0)); op(Opc::Values); break;
      case N::Defined:
        if (n->kid(0)->kind == N::Index && n->kid(0)->kids.size() == 2) {
          expr(n->kid(0)->kid(0));
          expr(n->kid(0)->kid(1));
          op(Opc::DefinedIndex);
        } else {
          expr(n->kid(0));
          op(Opc::Defined);
        }
        break;
      case N::Load:
        if (n->kids.size() > 1) {
          expr(n->kid(1));
          expr(n->kid(0));
          op(Opc::LoadIn);
        } else {
          expr(n->kid(0));
          op(Opc::Load);
        }
        break;
      case N::Macro: {
        for (size_t i = 1; i < n->kids.size(); ++i) expr(n->kid(i));
        uint8_t argc = uint8_t(n->kids.size() - 1);
        op(Opc::MakeMacro);
        u8(argc);
        size_t nlocalsPos = pc();
        u16(0);
        size_t body = pc();
        u32(0);
        size_t over = jump(Opc::Jmp);
        patch(body, pc());
        // macro mode: $name is always the script variable; captures are $0..$n (0x100067c0.c:869-874)
        Activation a;
        a.mode = Mode::Macro;
        for (int i = 0; i < argc; ++i) a.declare(u"\x1F" + utf8ToUtf16(std::to_string(i)));
        Activation* saved = act_;
        act_ = &a;
        auto savedLoops = std::move(loops_);
        loops_.clear();
        bool savedThread = inThread_;
        inThread_ = false;
        expr(n->kid(0));
        op(Opc::HaltValue);
        uint16_t nl = a.count;
        std::memcpy(&bc()[nlocalsPos], &nl, 2);
        act_ = saved;
        loops_ = std::move(savedLoops);
        inThread_ = savedThread;
        patch(over, pc());
        break;
      }
      case N::Thread: {
        if (n->kids.size() > 1) expr(n->kid(1));
        size_t body = jump(n->kids.size() > 1 ? Opc::ThreadIn : Opc::Thread);
        size_t over = jump(Opc::Jmp);
        patch(body, pc());
        auto savedLoops = std::move(loops_);
        loops_.clear();
        int savedCtx = ctxDepth_;
        ctxDepth_ = 0;
        bool savedThread = inThread_;
        inThread_ = true;
        stmt(n->kid(0));
        op(Opc::End);
        loops_ = std::move(savedLoops);
        ctxDepth_ = savedCtx;
        inThread_ = savedThread;
        patch(over, pc());
        break;
      }
      case N::FunctionExpr:
        fail("anonymous functions are not supported", n);
      default:
        fail(std::string("unexpected expression ") + nodeName(n->kind), n);
    }
  }

  static std::u16string memberName(const Node* n) { return (n->i ? u"$" : u"") + n->name; }

  static uint8_t compoundBase(Op o) {
    switch (o) {
      case Op::AddAssign: return uint8_t(Op::Add);
      case Op::SubAssign: return uint8_t(Op::Sub);
      case Op::MulAssign: return uint8_t(Op::Mul);
      case Op::DivAssign: return uint8_t(Op::Div);
      case Op::ModAssign: return uint8_t(Op::Mod);
      case Op::AndAssign: return uint8_t(Op::BitAnd);
      case Op::OrAssign: return uint8_t(Op::BitOr);
      case Op::XorAssign: return uint8_t(Op::BitXor);
      case Op::ShlAssign: return uint8_t(Op::Shl);
      case Op::ShrAssign: return uint8_t(Op::Shr);
      case Op::UshrAssign: return uint8_t(Op::Ushr);
      default: return uint8_t(Op::Add);
    }
  }

  // assignment / compound assignment / ++ / -- on an lvalue; rhs may be null for ++/--
  void update(const Node* target, uint8_t upd, const Node* rhs) {
    switch (target->kind) {
      case N::Var: {
        int slot = resolveVar(target, forceGlobal_);  // the engine pushes the target reference before the value
        bool saved = forceGlobal_;
        forceGlobal_ = false;
        if (rhs) expr(rhs);
        forceGlobal_ = saved;
        if (slot >= 0) {
          if (upd == kUpdAssignPlain) {
            op(Opc::StoreLocal);
            u16(uint16_t(slot));
          } else {
            op(Opc::UpdLocal);
            u16(uint16_t(slot));
            u8(upd);
          }
        } else {
          if (upd == kUpdAssignPlain) {
            str(Opc::StoreVar, target->name);
          } else {
            str(Opc::UpdVar, target->name);
            u8(upd);
          }
        }
        break;
      }
      case N::Member:
      case N::WithMember: {
        if (target->kind == N::Member) expr(target->kid(0));
        else op(Opc::PushCtx);
        bool saved = forceGlobal_;
        forceGlobal_ = false;
        if (rhs) expr(rhs);
        forceGlobal_ = saved;
        if (upd == kUpdAssignPlain) {
          str(Opc::SetMember, memberName(target));
        } else {
          str(Opc::UpdMember, memberName(target));
          u8(upd);
        }
        break;
      }
      case N::Index: {
        expr(target->kid(0));
        bool saved = forceGlobal_;
        forceGlobal_ = false;
        if (target->kids.size() < 2) {  // a[] = v
          if (upd != kUpdAssignPlain || !rhs) fail("'[]' only supports plain assignment", target);
          expr(rhs);
          op(Opc::Append);
          forceGlobal_ = saved;
          break;
        }
        expr(target->kid(1));
        if (rhs) expr(rhs);
        forceGlobal_ = saved;
        if (upd == kUpdAssignPlain) {
          op(Opc::SetIndex);
        } else {
          op(Opc::UpdIndex);
          u8(upd);
        }
        break;
      }
      case N::Current:  // writes through the foreach iterator (game.script:574, scenario.script:83)
        if (rhs) expr(rhs);
        if (upd == kUpdAssignPlain) {
          op(Opc::StoreCtx);
        } else {
          op(Opc::UpdCtx);
          u8(upd);
        }
        break;
      default:
        fail(std::string("cannot assign to ") + nodeName(target->kind), target);
    }
  }

  void reference(const Node* target) {
    switch (target->kind) {
      case N::Var: {
        int slot = resolveVar(target, false);
        if (slot >= 0) {
          op(Opc::RefLocal);
          u16(uint16_t(slot));
        } else {
          str(Opc::RefVar, target->name);
        }
        break;
      }
      case N::Member:
        expr(target->kid(0));
        str(Opc::RefMember, memberName(target));
        break;
      case N::WithMember:
        op(Opc::PushCtx);
        str(Opc::RefMember, memberName(target));
        break;
      case N::Index:
        expr(target->kid(0));
        expr(target->kid(1));
        op(Opc::RefIndex);
        break;
      default:
        // '@' of a non-lvalue just yields the value
        expr(target);
    }
  }

  uint32_t link(const std::u16string& name, int line) {
    for (uint32_t i = 0; i < c_->links.size(); ++i)
      if (c_->links[i] == name) return i;
    c_->links.push_back(name);
    c_->linkLines.push_back(line);
    return uint32_t(c_->links.size() - 1);
  }
};

}  // namespace

CompileResult compile(const Node* program, const std::u16string& path) {
  CompileResult r;
  auto code = make<Code>();
  code->path = path;
  try {
    Compiler c(code);
    c.program(program);
    r.code = code;
  } catch (const CompileError& e) {
    r.line = e.line;
    r.error = std::to_string(e.line) + ": " + e.what();
  }
  return r;
}

bool compileFragment(Code& code, const std::u16string& source, bool expression, FragmentInfo& out, std::string* error) {
  std::u16string src = source.substr(0, source.find(u'\0'));
  // an expression fragment is parsed as one expression statement
  auto pr = parse(expression ? u"(" + src + u");" : src);
  if (!pr.program) {
    if (error) *error = std::string(expression ? "Expression" : "Block") + " fragment: " + pr.error;
    return false;
  }
  try {
    Compiler c{Ref<Code>(&code)};  // the caller holds a reference, so this one never frees the code
    out = c.fragment(pr.program.get(), expression);
  } catch (const CompileError& e) {
    if (error) *error = std::string(expression ? "Expression" : "Block") + " fragment: " + e.what();
    return false;
  }
  return true;
}

}  // namespace regs::script
