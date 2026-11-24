// Script VM: scheduler, calls, events, load and the interpreter loop. Behaviour follows
// docs/script-runtime/semantics.md (execution) and values.md (values); operators are in vm_ops.cpp.
#include "script/vm.h"
#include "core/env.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/text.h"
#include "script/compiler.h"
#include "script/parser.h"
#include "vfs/vfs.h"

namespace regs::script {

namespace {

std::string u8(std::u16string_view s) { return utf16ToUtf8(std::u16string(s)); }
Value plain(const Value& v) { return v.ref() ? v.ref()->get() : v; }

constexpr uint64_t kInstructionBudget = 200'000'000;  // per thread run; guards the port against hangs

}  // namespace

VM::VM(Vfs& vfs) : vfs_(vfs) {
  scriptClass_ = defineClass("gsScript", "");
  groups_.push_back(std::make_unique<ThreadGroup>());  // group 0: unnamed, unpaused
}

VM::~VM() = default;

Value* VM::findGlobal(std::u16string_view name) {
  auto it = globals_.find(name);
  return it == globals_.end() ? nullptr : &it->second;
}

void VM::defineFunction(const std::u16string& name, int minArgs, int maxArgs, NativeFn fn) {
  auto f = make<FuncObj>();
  f->kind = FuncObj::Kind::Native;
  f->native = std::move(fn);
  f->name = name;
  f->minArgs = minArgs;
  f->maxArgs = maxArgs;
  globals_[name] = Value::heap(VT::Function, f.get());
}

ClassInfo* VM::defineClass(const std::string& name, const std::string& parent) {
  auto& slot = classes_[name];
  if (!slot) {
    slot = std::make_unique<ClassInfo>();
    slot->name = name;
  }
  if (!parent.empty()) slot->parent = findClass(parent);
  if (name == "gsModule") moduleClass_ = slot.get();
  return slot.get();
}

ClassInfo* VM::findClass(const std::string& name) {
  auto it = classes_.find(name);
  return it == classes_.end() ? nullptr : it->second.get();
}

Ref<Object> VM::newObject(const ClassInfo* cls, Object* parent) {
  if (!cls) cls = scriptClass_;
  Ref<Object> o;
  for (const ClassInfo* c = cls; c; c = c->parent)
    if (c->factory) {
      o = c->factory(cls);
      break;
    }
  if (!o) o = make<Object>(cls);
  if (!isModule(o.get())) o->flags = 0;
  o->serial = nextSerial_++;
  if (parent) parent->addChild(o);
  return o;
}

void VM::tickChildren(Object* o, float dt) {
  // the list may change while ticking (children created or destroyed by handlers)
  auto kids = o->children;
  for (auto& c : kids)
    if (!c->destroyed && c->parent == o && c->enabled()) c->tick(*this, dt);
}

Value VM::runFragment(Object* self, uint32_t entry, uint16_t nlocals, bool expression) {
  if (!self || !self->code) return Value();
  ThreadObj* t = newThread(group(u""));
  Ref<ThreadObj> keep(t);
  Frame f;
  f.kind = expression ? Frame::Kind::Macro : Frame::Kind::Top;  // HaltValue ends an expression fragment
  f.code = self->code;
  f.pc = entry;
  f.self = self;
  f.env = make<Env>();
  f.env->slots.resize(nlocals);
  t->frames.push_back(std::move(f));
  runThread(t);
  return t->state == ThreadObj::State::Finished ? t->result : Value();
}

// Destroy sequence (semantics.md Â§8.4): onDestroy handlers, class hook, destroyed bit; modules destroy their children
// in reverse order and detach; scripts leave the script list and lose the handlers they bound elsewhere.
void VM::destroy(Object* o) {
  if (!o || o->destroyed) return;
  Ref<Object> keep(o);
  fireEvent(o, u"onDestroy", {});
  o->onDestroy(*this);
  o->destroyed = true;
  if (isModule(o)) {
    auto kids = o->children;
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) destroy(it->get());
  }
  if (o->parent) o->parent->removeChild(o);
  std::erase_if(scripts_, [o](const Ref<Object>& s) { return s.get() == o; });
  for (auto& target : o->boundTargets)
    for (auto& [name, list] : target->events)
      std::erase_if(list, [o](const EventBinding& b) { return b.self.get() == o; });
  o->boundTargets.clear();
  o->events.clear();
}

// ---------------------------------------------------------------------------------------------------------------
// load (semantics.md Â§6)

std::u16string VM::resolvePath(const std::u16string& path, const Object* caller) const {
  if (path.empty()) return path;
  if (path[0] == u'/') return path.substr(1);
  if (path.rfind(u"~/", 0) == 0) return path.substr(2);
  std::u16string base;
  if (caller) {
    const std::u16string& f = caller->scriptPath;
    size_t k = f.find_last_of(u"/\\");
    if (k != std::u16string::npos) base = f.substr(0, k + 1);
  }
  std::u16string p = path.rfind(u"./", 0) == 0 ? path.substr(2) : path;
  return base + p;
}

void VM::link(Code& code, Object* self) {
  code.linked.assign(code.links.size(), Value());
  code.linkFunction.assign(code.links.size(), -1);
  for (size_t i = 0; i < code.links.size(); ++i) {
    auto& name = code.links[i];
    auto fi = code.functionIndex.find(name);
    if (fi != code.functionIndex.end()) {
      code.linkFunction[i] = fi->second;
    } else if (Value* g = findGlobal(name)) {
      code.linked[i] = *g;
    } else {
      throw ScriptError(kErrCustom, "Linker error in \"" + u8(code.path) + "\" at line " + std::to_string(code.linkLines[i]) +
                                        ": Undefined identifier \"" + u8(name) + "\"!");
    }
  }
  code.isLinked = true;
}

// Port extensions of game scripts (port/data/regs/extend/<path>, from the port's data overlay): the part before a
// line "//@base" is compiled before the original source and the rest after it, in the same object; a line
// "//@rename <name> <newName>" renames a function definition of the original so the extension can wrap it. The
// game's file is used as installed, so only port-written code ships. Error lines of the original shift by the
// prefix's line count.
// Mods (port/docs/mod-manager.md §3.3) chain: each root of extensionRoots is applied to the result of the previous
// one, so a mod's prefix comes before the port's and its suffix after it, and its renames see the accumulated source
// (a mod renaming f wraps the port's f where the port already wrapped it).
namespace {
void applyExtension(const std::u16string& extText, std::u16string& source) {
  std::u16string ext = u"\n" + extText, prefix, suffix = ext;  // directives start a line
  if (size_t base = ext.find(u"\n//@base"); base != std::u16string::npos) {
    prefix = ext.substr(0, base);
    suffix = ext.substr(base);
  }
  for (size_t at = ext.find(u"\n//@rename "); at != std::u16string::npos; at = ext.find(u"\n//@rename ", at + 1)) {
    ++at;  // past the newline
    size_t end = ext.find(u'\n', at);
    std::u16string line = ext.substr(at + 10, end == std::u16string::npos ? std::u16string::npos : end - at - 10);
    size_t sp = line.find(u' ');
    if (sp == std::u16string::npos) continue;
    std::u16string from = line.substr(0, sp), to = line.substr(sp + 1);
    while (!to.empty() && (to.back() == u'\r' || to.back() == u' ')) to.pop_back();
    std::u16string def = u"function " + from + u"(";
    if (size_t f = source.find(def); f != std::u16string::npos) source.replace(f, def.size(), u"function " + to + u"(");
  }
  source = prefix + u"\n" + source + u"\n" + suffix;
}
}  // namespace

void VM::extendSource(const std::u16string& path, std::u16string& source) {
  for (const std::string& root : extensionRoots) {
    std::vector<uint8_t> data;
    if (vfs_.read(root + u8(path), data)) applyExtension(decodeTextFile(data), source);
  }
}

Value VM::load(const std::u16string& rawPath, const LoadOptions& opt) {
  Object* caller = current_ && !current_->frames.empty() ? current_->frames.back().self.get() : nullptr;
  std::u16string path = resolvePath(rawPath, caller);
  Ref<Object> obj = opt.into ? opt.into : newObject(opt.cls ? opt.cls : scriptClass_, nullptr);
  if (!opt.into && opt.keepInScriptList) scripts_.push_back(obj);
  obj->scriptPath = path;
  // compile: every load compiles again (no cache); any error leaves an empty script
  std::vector<uint8_t> data;
  if (!vfs_.read(u8(path), data)) {
    report("File \"" + u8(path) + "\" not found!", nullptr);
    return Value::heap(VT::Object, obj.get());
  }
  std::u16string source = decodeTextFile(data);
  extendSource(path, source);
  auto pr = parse(source);
  if (!pr.program) {
    report("Parser error in \"" + u8(path) + "\" at line " + std::to_string(pr.line) + ": " + pr.error, nullptr);
    return Value::heap(VT::Object, obj.get());
  }
  auto cr = compile(pr.program.get(), path);
  if (!cr.code) {
    report("Compiler error in \"" + u8(path) + "\" at line " + cr.error, nullptr);
    return Value::heap(VT::Object, obj.get());
  }
  if (opt.beforeLink) {
    std::string err;
    if (!opt.beforeLink(*cr.code, &err)) {
      report("Compiler error in \"" + u8(path) + "\": " + err, nullptr);
      return Value::heap(VT::Object, obj.get());
    }
  }
  try {
    link(*cr.code, obj.get());
  } catch (const ScriptError& e) {
    report(e.what(), nullptr);
    return Value::heap(VT::Object, obj.get());
  }
  obj->code = cr.code;
  for (auto& n : cr.code->scriptVars)  // script variables start as null (semantics.md Â§1.3)
    if (!obj->findVar(n)) obj->var(n) = Value::null();
  // run the top-level code now, in the given group; if it blocks it continues under the scheduler
  ThreadObj* t = newThread(group(opt.group));
  Frame f;
  f.kind = Frame::Kind::Top;
  f.code = cr.code;
  f.self = obj;
  f.env = make<Env>();
  f.env->slots.resize(cr.code->topLocals);
  t->frames.push_back(std::move(f));
  runThread(t);
  return Value::heap(VT::Object, obj.get());
}

Ref<Code> VM::compileSource(const std::u16string& source, const std::u16string& path) {
  auto pr = parse(source);
  if (!pr.program) {
    report("Parser error in \"" + u8(path) + "\" at line " + std::to_string(pr.line) + ": " + pr.error, nullptr);
    return {};
  }
  auto cr = compile(pr.program.get(), path);
  if (!cr.code) {
    report("Compiler error in \"" + u8(path) + "\" at line " + cr.error, nullptr);
    return {};
  }
  return cr.code;
}

Value VM::loadCompiled(const Ref<Code>& code, const std::u16string& path, const LoadOptions& opt,
                       const std::function<void(Object*)>& init) {
  // as load() from the link on: a new object (or opt.into), linked against the globals as they are now, run at once
  Ref<Object> obj = opt.into ? opt.into : newObject(opt.cls ? opt.cls : scriptClass_, nullptr);
  if (!opt.into && opt.keepInScriptList) scripts_.push_back(obj);
  obj->scriptPath = path;
  try {
    link(*code, obj.get());
  } catch (const ScriptError& e) {
    report(e.what(), nullptr);
    return Value::heap(VT::Object, obj.get());
  }
  obj->code = code;
  if (init) init(obj.get());
  for (auto& n : code->scriptVars)
    if (!obj->findVar(n)) obj->var(n) = Value::null();
  ThreadObj* t = newThread(group(opt.group));
  Frame f;
  f.kind = Frame::Kind::Top;
  f.code = code;
  f.self = obj;
  f.env = make<Env>();
  f.env->slots.resize(code->topLocals);
  t->frames.push_back(std::move(f));
  runThread(t);
  return Value::heap(VT::Object, obj.get());
}

// ---------------------------------------------------------------------------------------------------------------
// threads (semantics.md Â§5)

ThreadGroup& VM::group(const std::u16string& name) {
  if (name.empty()) return *groups_.front();
  for (auto& g : groups_)
    if (g->name == name) return *g;
  groups_.push_back(std::make_unique<ThreadGroup>());
  groups_.back()->name = name;
  groups_.back()->paused = 1;  // named groups start paused (0x1002ada0.c:47)
  return *groups_.back();
}

ThreadObj* VM::newThread(ThreadGroup& g) {
  auto t = make<ThreadObj>();
  t->group = &g;
  t->wake = 0;
  t->state = ThreadObj::State::Sleeping;
  t->id = nextThreadId_++;
  g.threads.push_back(t);
  return t.get();
}

size_t VM::threadCount() const {
  size_t n = 0;
  for (auto& g : groups_)
    for (auto& t : g->threads)
      if (t->state != ThreadObj::State::Finished) ++n;
  return n;
}

void VM::runThread(ThreadObj* t) {
  if (t->frames.empty() || (t->frames.front().self && t->frames.front().self->destroyed)) {
    t->state = ThreadObj::State::Finished;
    return;
  }
  ThreadObj* saved = current_;
  current_ = t;
  ++depth_;
  Ref<ThreadObj> keep(t);
  t->state = ThreadObj::State::Running;
  try {
    t->result = execute(t);
  } catch (const ScriptError& e) {
    report(e.what(), t);
    t->state = ThreadObj::State::Finished;
    t->frames.clear();
  }
  --depth_;
  current_ = saved;
}

void VM::runThreads(float dt) {
  dt_ = dt;
  if (depth_ != 0) return;
  static const bool debugGroups = regs::envVar("REGS_DEBUG_GROUPS") != nullptr;
  static int calls = 0;
  if (debugGroups && ++calls % 300 == 0)  // the thread groups' state, every 300 frames
    for (auto& g : groups_) {
      size_t sleeping = 0, other = 0;
      double wake = -1;
      for (auto& t : g->threads)
        if (t->state == ThreadObj::State::Sleeping) {
          ++sleeping;
          if (wake < 0 || t->wake < wake) wake = t->wake;
        } else {
          ++other;
          LOG_INFO("  thread in '%s': state %d, %zu frames", utf16ToUtf8(g->name).c_str(), int(t->state), t->frames.size());
        }
      LOG_INFO("group '%s': paused %d speed %.2f time %.2f, %zu threads (%zu sleeping, %zu other, next wake %.2f)",
               utf16ToUtf8(g->name).c_str(), g->paused, g->speed, g->time, g->threads.size(), sleeping, other, wake);
    }
  for (size_t gi = 0; gi < groups_.size(); ++gi) {
    ThreadGroup& g = *groups_[gi];
    if (g.paused != 0) continue;
    g.time += double(dt) * double(g.speed);
    for (int pass = 0; pass < 1000; ++pass) {
      // earliest wake first; FIFO among equal wakes
      ThreadObj* next = nullptr;
      for (auto& t : g.threads)
        if (t->state == ThreadObj::State::Sleeping && t->wake <= g.time && (!next || t->wake < next->wake)) next = t.get();
      if (!next || g.paused != 0) break;
      runThread(next);
    }
    std::erase_if(g.threads, [](const Ref<ThreadObj>& t) { return t->state == ThreadObj::State::Finished; });
  }
}

// ---------------------------------------------------------------------------------------------------------------
// calls (semantics.md Â§3)

// stack: callee a1..an  ->  either a new frame (script function, macro) or the result (native)
void VM::pushCall(ThreadObj* t, int argc) {
  size_t base = t->stack.size() - size_t(argc) - 1;
  Value callee = plain(t->stack[base]);
  Value* args = t->stack.data() + base + 1;
  if (FuncObj* f = callee.func()) {
    if (f->kind == FuncObj::Kind::Script) {
      const FunctionInfo& fi = f->code->functions[size_t(f->index)];
      if (argc < fi.minArgs || argc > fi.nparams) error(kErrArgCount, "Wrong number of arguments (" + std::to_string(argc) + ")!");
      Frame fr;
      fr.kind = Frame::Kind::Function;
      fr.code = f->code;
      fr.pc = fi.entry;
      fr.self = f->owner;
      fr.env = make<Env>();
      fr.env->slots.resize(std::max<size_t>(fi.nlocals, size_t(argc)));
      for (int i = 0; i < argc; ++i) fr.env->slots[size_t(i)] = args[i];  // an '@' reference stays an alias
      fr.argc = argc;
      t->stack.resize(base);
      fr.stackBase = base;
      fr.ctxBase = t->ctx.size();  // the context value is not reset: the callee still sees 'with'/'current'
      t->frames.push_back(std::move(fr));
      return;
    }
    if (argc < f->minArgs || argc > f->maxArgs) error(kErrArgCount, "Wrong number of arguments (" + std::to_string(argc) + ")!");
    std::vector<Value> a(args, args + argc);
    Value r;
    if (f->method) {
      Object* self = f->owner.get();
      if (self && self->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
      r = f->method->fn(*this, self, argc, a.data());
    } else {
      r = f->native(*this, f->owner.get(), argc, a.data());
    }
    t->stack.resize(base);
    t->stack.push_back(std::move(r));
    return;
  }
  if (MacroObj* m = callee.macro()) {
    if (argc != 0) error(kErrArgCount, "Wrong number of arguments (" + std::to_string(argc) + ")!");
    Value r = evalMacro(m);
    t->stack.resize(base);
    t->stack.push_back(std::move(r));
    return;
  }
  error(kErrNotCallable, "Cannot call non-function variant!");
}

// Macro evaluation (semantics.md Â§7): a new default-group thread over the captures, run synchronously.
Value VM::evalMacro(MacroObj* m) {
  ThreadObj* t = newThread(group(u""));
  Frame fr;
  fr.kind = Frame::Kind::Macro;
  fr.code = m->code;
  fr.pc = m->entry;
  fr.self = m->self;
  fr.env = make<Env>();
  fr.env->slots.resize(std::max<size_t>(m->nlocals, m->captured.size()));
  for (size_t i = 0; i < m->captured.size(); ++i) fr.env->slots[i] = m->captured[i];
  t->frames.push_back(std::move(fr));
  Ref<ThreadObj> keep(t);
  runThread(t);
  return t->state == ThreadObj::State::Finished ? t->result : Value();
}

Value VM::call(const Value& callable, const std::vector<Value>& args) {
  Value c = plain(callable);
  if (FuncObj* f = c.func(); f && f->kind == FuncObj::Kind::Native) {
    std::vector<Value> a = args;
    if (f->method) return f->method->fn(*this, f->owner.get(), int(a.size()), a.data());
    return f->native(*this, f->owner.get(), int(a.size()), a.data());
  }
  if (MacroObj* m = c.macro()) return evalMacro(m);
  FuncObj* f = c.func();
  if (!f) return Value();
  ThreadObj* t = newThread(group(u""));
  Ref<ThreadObj> keep(t);
  t->stack.push_back(c);
  for (auto& a : args) t->stack.push_back(a);
  ThreadObj* saved = current_;
  current_ = t;
  try {
    pushCall(t, int(args.size()));
  } catch (const ScriptError& e) {
    report(e.what(), t);
    t->state = ThreadObj::State::Finished;
    current_ = saved;
    return Value();
  }
  current_ = saved;
  runThread(t);
  return t->state == ThreadObj::State::Finished ? t->result : Value();
}

// ---------------------------------------------------------------------------------------------------------------
// events (semantics.md Â§4)

bool VM::hasHandler(Object* target, const std::u16string& name) const {
  auto it = target->events.find(name);
  return it != target->events.end() && !it->second.empty();
}

bool VM::fireEvent(Object* target, const std::u16string& name, const std::vector<Value>& args, const Value* key) {
  if (!target || target->destroyed) return false;
  auto it = target->events.find(name);
  if (it == target->events.end() || it->second.empty()) return false;
  std::vector<EventBinding> bindings;
  std::u16string k = key ? plain(*key).toString() : u"";
  for (auto& b : it->second)
    if (!b.hasFilter || (key && b.filter == k)) bindings.push_back(b);
  if (bindings.empty()) return false;
  Ref<Object> keep(target);
  for (auto& b : bindings) {
    if (target->destroyed) break;
    const HandlerInfo& h = b.code->handlers[size_t(b.handler)];
    ThreadObj* t = newThread(group(u""));
    Ref<ThreadObj> keepT(t);
    Frame fr;
    fr.kind = Frame::Kind::Handler;
    fr.code = b.code;
    fr.pc = h.entry;
    fr.self = b.self;
    fr.env = make<Env>();
    fr.env->slots.resize(std::max<size_t>(h.nlocals, h.nparams));
    size_t n = std::min<size_t>(args.size(), h.nparams);
    for (size_t i = 0; i < n; ++i) fr.env->slots[i] = args[i];
    fr.argc = int(n);
    t->frames.push_back(std::move(fr));
    CtxEntry c;  // the firing object is the context value: 'current' and '.member' in the handler
    c.value = Value::heap(VT::Object, target);
    t->ctx.push_back(std::move(c));
    runThread(t);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// members (semantics.md Â§2.2, values.md Â§6)

Value VM::getMember(const Value& raw, std::u16string_view name) {
  Value v = resolve(raw);
  if (Object* o = v.object()) {
    bool var = !name.empty() && name[0] == u'$';
    if (var) {
      if (!isScriptObject(o)) error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
      if (o->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
      return plain(o->var(std::u16string(name.substr(1))));  // reading creates the variable (unset)
    }
    if (const FieldInfo* fi = o->cls->findField(name)) return fi->get ? fi->get(*this, o) : Value();
    if (const MethodInfo* mi = o->cls->findMethod(name)) {
      if (o->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
      auto f = make<FuncObj>();
      f->kind = FuncObj::Kind::Native;
      f->method = mi;
      f->owner = o;
      f->name = name;
      f->minArgs = mi->minArgs;
      f->maxArgs = mi->maxArgs;
      return Value::heap(VT::Function, f.get());
    }
    if (o->cls->findEvent(name)) return Value();  // event variants are only used as 'event' targets
    if (o->code) {
      auto fi = o->code->functionIndex.find(name);
      if (fi != o->code->functionIndex.end()) {
        if (o->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
        auto f = make<FuncObj>();
        f->kind = FuncObj::Kind::Script;
        f->code = o->code;
        f->index = fi->second;
        f->owner = o;  // obj.f(): 'this' is obj
        f->name = name;
        return Value::heap(VT::Function, f.get());
      }
    }
    error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
  }
  if (ThreadObj* th = v.thread()) {
    if (name == u"destroyed") return Value::boolean(th->state == ThreadObj::State::Finished);
  }
  error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
}

void VM::setMember(const Value& raw, std::u16string_view name, const Value& val) {
  Value v = resolve(raw);
  if (Object* o = v.object()) {
    if (o->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
    if (!name.empty() && name[0] == u'$') {
      if (!isScriptObject(o)) error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
      storeSlot(o->var(std::u16string(name.substr(1))), val);
      return;
    }
    if (const FieldInfo* fi = o->cls->findField(name)) {
      if (fi->readOnly) error(kErrNotAssignable, "Left side cannot be assigned!");
      if (fi->set) fi->set(*this, o, plain(val));
      return;
    }
    if (o->cls->findMethod(name) || (o->code && o->code->functionIndex.count(name)))
      error(kErrNotAssignable, "Left side cannot be assigned!");
    error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
  }
  error(kErrUndefinedMember, "Undefined member name \"" + u8(name) + "\"!");
}

Value VM::makeRef(std::function<Value()> get, std::function<void(const Value&)> set) {
  auto r = make<RefObj>();
  r->get = std::move(get);
  r->set = std::move(set);
  return Value::heap(VT::Ref, r.get());
}

// ---------------------------------------------------------------------------------------------------------------
// context stack (semantics.md Â§8)

int64_t VM::iterCount(const Value& coll) {
  if (ArrayObj* a = coll.array()) return int64_t(a->items.size());
  if (MapObj* m = coll.map()) return int64_t(m->entries.size());
  if (coll.type() == VT::String) return int64_t(coll.str().size());
  if (Object* o = coll.object(); o && isModule(o)) return int64_t(o->children.size());
  error(kErrNotIndexable, "Variant not indexable!");
}

Value VM::ctxCurrent(ThreadObj* t) {
  if (t->ctx.empty()) return Value();
  CtxEntry& c = t->ctx.back();
  if (!c.iter) return c.value;
  int64_t n = iterCount(c.value);
  int64_t k = c.index < 0 ? n + c.index : c.index;
  if (ArrayObj* a = c.value.array()) {
    if (k < 0 || k >= n) error(kErrArrayIndex, "Array index out of bounds (" + std::to_string(c.index) + ")!");
    return plain(a->items[size_t(k)]);
  }
  if (Object* o = c.value.object()) {
    if (k < 0 || k >= n) error(kErrArrayIndex, "Array index out of bounds (" + std::to_string(c.index) + ")!");
    return Value::heap(VT::Object, o->children[size_t(k)].get());
  }
  error(kErrNotIndexable, "Variant not indexable!");  // maps and strings have no positional element
}

void VM::ctxStore(ThreadObj* t, const Value& v) {
  if (t->ctx.empty()) error(kErrNotAssignable, "Left side cannot be assigned!");
  CtxEntry& c = t->ctx.back();
  if (!c.iter) error(kErrNotAssignable, "Left side cannot be assigned!");
  if (ArrayObj* a = c.value.array()) {
    int64_t n = int64_t(a->items.size());
    int64_t k = c.index < 0 ? n + c.index : c.index;
    if (k < 0 || k >= n) error(kErrArrayIndex, "Array index out of bounds (" + std::to_string(c.index) + ")!");
    storeSlot(a->items[size_t(k)], v);
    return;
  }
  error(kErrNotIndexable, "Variant not indexable!");
}

// ---------------------------------------------------------------------------------------------------------------
// errors (semantics.md Â§9)

void VM::error(int code, const std::string& msg) { throw ScriptError(code, msg); }

std::string VM::where() const {
  if (!current_ || current_->frames.empty()) return "<native>";
  const Frame& f = current_->frames.back();
  return "\"" + u8(f.code->path) + "\" at line " + std::to_string(f.code->lineAt(f.pc > 0 ? f.pc - 1 : 0));
}

void VM::report(const std::string& msg, ThreadObj* t) {
  auto sink = [this](const std::string& s) {
    if (errorOutput) errorOutput(s);
    else LOG_ERROR("%s", s.c_str());
  };
  if (!t || t->frames.empty()) {
    sink(msg);
    return;
  }
  auto line = [](const Frame& f) { return f.code->lineAt(f.pc > 0 ? f.pc - 1 : 0); };
  const Frame& top = t->frames.back();
  sink("Runtime error in \"" + u8(top.code->path) + "\" at line " + std::to_string(line(top)) + ": " + msg);
  for (size_t i = t->frames.size() - 1; i-- > 0;)
    sink("Called from \"" + u8(t->frames[i].code->path) + "\" at line " + std::to_string(line(t->frames[i])));
}

// ---------------------------------------------------------------------------------------------------------------
// interpreter

Value VM::execute(ThreadObj* t) {
  uint64_t budget = 0;
  auto& st = t->stack;
  while (!t->frames.empty()) {
    Frame& f = t->frames.back();
    const uint8_t* bc = f.code->bc.data();
    auto rd8 = [&]() { return bc[f.pc++]; };
    auto rd16 = [&]() { uint16_t v; std::memcpy(&v, bc + f.pc, 2); f.pc += 2; return v; };
    auto rd32 = [&]() { uint32_t v; std::memcpy(&v, bc + f.pc, 4); f.pc += 4; return v; };
    auto rdS = [&]() -> const std::u16string& { return f.code->strings[rd32()]; };
    auto pop = [&]() { Value v = std::move(st.back()); st.pop_back(); return v; };
    auto jumpRel = [&](int32_t rel) { f.pc = uint32_t(int64_t(f.pc) + rel); };
    auto finish = [&]() {
      t->state = ThreadObj::State::Finished;
      t->frames.clear();
    };
    if (++budget > kInstructionBudget) error("Script runs too long without waiting (possible endless loop)");
    ++instructions;

    Opc o = Opc(rd8());
    switch (o) {
      case Opc::Nop: break;
      case Opc::Pop: st.pop_back(); break;
      case Opc::Dup: st.push_back(st.back()); break;
      case Opc::Dup2: {
        Value a = st[st.size() - 2], b = st.back();
        st.push_back(a);
        st.push_back(b);
        break;
      }
      case Opc::PushNone: st.emplace_back(); break;
      case Opc::PushNull: st.push_back(Value::null()); break;
      case Opc::PushTrue: st.push_back(Value::boolean(true)); break;
      case Opc::PushFalse: st.push_back(Value::boolean(false)); break;
      case Opc::PushInt: st.push_back(Value::integer(int32_t(rd32()))); break;
      case Opc::PushFloat: {
        uint32_t u = rd32();
        float fl;
        std::memcpy(&fl, &u, 4);
        st.push_back(Value::real(fl));
        break;
      }
      case Opc::PushStr: st.push_back(Value::string(rdS())); break;
      case Opc::PushLink: {
        uint32_t i = rd32();
        int fi = f.code->linkFunction[i];
        if (fi >= 0) {
          auto fo = make<FuncObj>();
          fo->kind = FuncObj::Kind::Script;
          fo->code = f.code;
          fo->index = fi;
          fo->owner = f.self;  // own functions are bound to the script that owns the code
          fo->name = f.code->functions[size_t(fi)].name;
          st.push_back(Value::heap(VT::Function, fo.get()));
        } else {
          st.push_back(f.code->linked[i]);
        }
        break;
      }
      case Opc::PushThis: st.push_back(f.self ? Value::heap(VT::Object, f.self.get()) : Value::null()); break;
      case Opc::PushCtx: st.push_back(ctxCurrent(t)); break;
      case Opc::PushDt: st.push_back(Value::real(dt_)); break;
      case Opc::LoadLocal: st.push_back(plain(f.env->slots[rd16()])); break;
      case Opc::StoreLocal: {
        uint16_t s = rd16();
        storeSlot(f.env->slots[s], st.back());
        st.back() = plain(st.back());
        break;
      }
      case Opc::LoadVar: {
        const auto& n = rdS();
        Value* v = f.self ? f.self->findVar(n) : nullptr;
        st.push_back(v ? plain(*v) : Value());
        break;
      }
      case Opc::StoreVar: {
        const auto& n = rdS();
        if (f.self) storeSlot(f.self->var(n), st.back());
        st.back() = plain(st.back());
        break;
      }
      case Opc::GetMember: {
        const auto& n = rdS();
        Value obj = pop();
        st.push_back(getMember(obj, n));
        break;
      }
      case Opc::SetMember: {
        const auto& n = rdS();
        Value v = plain(pop());
        Value obj = pop();
        setMember(obj, n, v);
        st.push_back(v);
        break;
      }
      case Opc::GetIndex: {
        Value i = pop(), a = pop();
        st.push_back(indexGet(a, i));
        break;
      }
      case Opc::SetIndex: {
        Value v = plain(pop());
        Value i = pop(), a = pop();
        indexSet(a, i, v);
        st.push_back(v);
        break;
      }
      case Opc::Append: {
        Value v = plain(pop());
        Value a = plain(pop());
        ArrayObj* arr = a.array();
        if (!arr) error(kErrNotIndexable, "Variant not indexable!");
        arr->items.push_back(v);
        st.push_back(v);
        break;
      }
      case Opc::UpdLocal: case Opc::UpdVar: case Opc::UpdMember: case Opc::UpdIndex: case Opc::UpdCtx: {
        uint16_t slot = 0;
        const std::u16string* name = nullptr;
        if (o == Opc::UpdLocal) slot = rd16();
        else if (o == Opc::UpdVar || o == Opc::UpdMember) name = &rdS();
        uint8_t upd = rd8();
        Value rhs;
        if (upd < 0xF0) rhs = plain(pop());
        Value obj, idx, cur;
        switch (o) {
          case Opc::UpdLocal: cur = plain(f.env->slots[slot]); break;
          case Opc::UpdVar: { Value* v = f.self ? f.self->findVar(*name) : nullptr; cur = v ? plain(*v) : Value(); break; }
          case Opc::UpdMember: obj = pop(); cur = getMember(obj, *name); break;
          case Opc::UpdIndex: idx = pop(); obj = pop(); cur = indexGet(obj, idx); break;
          default: cur = ctxCurrent(t); break;
        }
        Value nv = update(Op(upd < 0xF0 ? upd : 0), upd, cur, rhs);
        Value result = (upd == kUpdPostInc || upd == kUpdPostDec) ? cur : nv;
        switch (o) {
          case Opc::UpdLocal: storeSlot(f.env->slots[slot], nv); break;
          case Opc::UpdVar: if (f.self) storeSlot(f.self->var(*name), nv); break;
          case Opc::UpdMember: setMember(obj, *name, nv); break;
          case Opc::UpdIndex: indexSet(obj, idx, nv); break;
          default: ctxStore(t, nv); break;
        }
        st.push_back(result);
        break;
      }
      case Opc::StoreCtx: {
        Value v = plain(st.back());
        ctxStore(t, v);
        st.back() = v;
        break;
      }
      case Opc::RefLocal: {
        Ref<Env> env = f.env;
        uint16_t s = rd16();
        // '@' of a variable that is itself an '@' alias (a parameter passed by reference) passes that reference on: a
        // reference to the local would end at the local (2026-11-24, owner's report: a line's new name was not kept,
        // linepanel renameLine → ui.script showInputDialog → showDialog hand the name down as @$value twice)
        if (env->slots[s].ref()) {
          st.push_back(env->slots[s]);
          break;
        }
        st.push_back(makeRef([env, s] { return plain(env->slots[s]); }, [env, s](const Value& v) { env->slots[s] = v; }));
        break;
      }
      case Opc::RefVar: {
        std::u16string n = rdS();
        Ref<Object> self = f.self;
        if (Value* cur = self ? self->findVar(n) : nullptr; cur && cur->ref()) {  // an alias: passed on (as RefLocal)
          st.push_back(*cur);
          break;
        }
        st.push_back(makeRef([self, n] { Value* v = self ? self->findVar(n) : nullptr; return v ? plain(*v) : Value(); },
                             [self, n](const Value& v) { if (self) self->var(n) = v; }));
        break;
      }
      case Opc::RefMember: {
        std::u16string n = rdS();
        Value obj = plain(pop());
        st.push_back(makeRef([this, obj, n] { return getMember(obj, n); }, [this, obj, n](const Value& v) { setMember(obj, n, v); }));
        break;
      }
      case Opc::RefIndex: {
        Value i = resolve(pop()), a = plain(pop());
        st.push_back(makeRef([this, a, i] { return indexGet(a, i); }, [this, a, i](const Value& v) { indexSet(a, i, v); }));
        break;
      }
      case Opc::Unop: {
        Op op = Op(rd8());
        Value a = pop();
        st.push_back(unop(op, a));
        break;
      }
      case Opc::Binop: {
        Op op = Op(rd8());
        Value b = pop(), a = pop();
        st.push_back(binop(op, a, b));
        break;
      }
      case Opc::Call: {
        int argc = rd8();
        pushCall(t, argc);
        // calling into a deleted script ends the thread silently (semantics.md Â§3)
        if (!t->frames.empty() && t->frames.back().self && t->frames.back().self->destroyed) {
          finish();
          return Value();
        }
        break;
      }
      case Opc::Return:
      case Opc::HaltValue: {
        Value v = plain(pop());
        size_t base = f.stackBase, ctxBase = f.ctxBase;
        bool macro = f.kind == Frame::Kind::Macro;
        t->frames.pop_back();
        st.resize(base);
        if (t->ctx.size() > ctxBase) t->ctx.resize(ctxBase);
        if (t->frames.empty() || macro) {
          t->state = ThreadObj::State::Finished;
          t->frames.clear();
          return v;
        }
        if (t->frames.back().self && t->frames.back().self->destroyed) {
          finish();
          return Value();
        }
        st.push_back(std::move(v));
        break;
      }
      case Opc::Jmp: { int32_t r = int32_t(rd32()); jumpRel(r); break; }
      case Opc::Jz: { int32_t r = int32_t(rd32()); if (!truthy(pop())) jumpRel(r); break; }
      case Opc::Jnz: { int32_t r = int32_t(rd32()); if (truthy(pop())) jumpRel(r); break; }
      case Opc::MakeArray: {
        uint32_t n = rd32();
        auto a = make<ArrayObj>();
        a->items.reserve(n);
        for (size_t i = st.size() - n; i < st.size(); ++i) a->items.push_back(plain(st[i]));
        st.resize(st.size() - n);
        st.push_back(Value::heap(VT::Array, a.get()));
        break;
      }
      case Opc::MakeMap: {
        uint32_t n = rd32();
        if (n % 2) error(kErrMapInit, "Initializing map with invalid number of arguments (" + std::to_string(n) + ")!");
        auto m = make<MapObj>();
        for (size_t i = st.size() - n; i < st.size(); i += 2) {
          Value key = plain(st[i]);
          if (Value* existing = m->find(key)) *existing = plain(st[i + 1]);  // later value wins, first key kept
          else m->at(key) = plain(st[i + 1]);
        }
        st.resize(st.size() - n);
        st.push_back(Value::heap(VT::Map, m.get()));
        break;
      }
      case Opc::Keys: case Opc::Values: {
        Value m = resolve(pop());
        MapObj* mp = m.map();
        if (!mp) error(kErrNotMap, "Variant is not a map!");
        auto out = make<ArrayObj>();
        for (auto& [k, v] : mp->entries) out->items.push_back(o == Opc::Keys ? k : plain(v));
        st.push_back(Value::heap(VT::Array, out.get()));
        break;
      }
      case Opc::Defined: st.back() = Value::boolean(!plain(st.back()).isNone()); break;
      case Opc::DefinedIndex: {
        Value i = pop(), a = pop();
        st.push_back(Value::boolean(indexDefined(a, i)));
        break;
      }
      case Opc::Export: st.back() = Value::string(plain(st.back()).exportSource()); break;
      case Opc::Wait: {
        float s = toFloat(pop());
        t->state = ThreadObj::State::Sleeping;
        t->wake = t->group->time + double(s);
        return Value();
      }
      case Opc::Yield:
        t->state = ThreadObj::State::Sleeping;
        t->wake = t->group->time + 1e-5;
        return Value();
      case Opc::Thread: case Opc::ThreadIn: {
        int32_t rel = int32_t(rd32());
        ThreadGroup* g = &group(u"");  // 'thread ::' always uses the default group
        if (o == Opc::ThreadIn) g = &group(toStr(pop()));
        ThreadObj* nt = newThread(*g);
        Frame nf;
        nf.kind = Frame::Kind::Top;
        nf.code = f.code;
        nf.pc = uint32_t(int64_t(f.pc) + rel);
        nf.self = f.self;
        nf.env = make<Env>(*f.env);  // a copy of the creator's locals (arrays/maps stay shared)
        nt->frames.push_back(std::move(nf));
        st.push_back(Value::heap(VT::Thread, nt));
        break;
      }
      case Opc::Pause: group(toStr(pop())).paused++; break;
      case Opc::Resume: group(toStr(pop())).paused--; break;  // not clamped
      case Opc::Echo: {
        std::string s = utf16ToUtf8(toStr(pop()));
        if (output) output(s);
        else LOG_INFO("%s", s.c_str());
        break;
      }
      case Opc::Assert:
        if (!truthy(pop())) report("Assert failed!", t);  // logged; execution continues
        break;
      case Opc::Delete: {
        Value v = resolve(pop());
        if (Object* ob = v.object()) {
          destroy(ob);
        } else if (ThreadObj* th = v.thread()) {
          th->state = ThreadObj::State::Finished;
          if (th == t) {
            finish();
            return Value();
          }
        } else if (!v.isNull()) {
          error(kErrValueless, "Cannot read valueless variant!");
        }
        if (t->frames.empty() || (t->frames.back().self && t->frames.back().self->destroyed)) {
          finish();
          return Value();
        }
        break;
      }
      case Opc::Load: case Opc::LoadIn: {
        Value path = pop();
        LoadOptions opt;
        if (o == Opc::LoadIn) opt.group = toStr(pop());
        std::u16string p = toStr(path);
        Value r = load(p, opt);
        t->stack.push_back(r);
        break;
      }
      case Opc::MakeMacro: {
        uint8_t argc = rd8();
        uint16_t nl = rd16();
        int32_t rel = int32_t(rd32());
        auto m = make<MacroObj>();
        m->code = f.code;
        m->entry = uint32_t(int64_t(f.pc) + rel);
        m->nlocals = nl;
        m->self = f.self;
        m->captured.assign(st.end() - argc, st.end());  // plain values are snapshots, '@' references stay live
        st.resize(st.size() - argc);
        st.push_back(Value::heap(VT::Macro, m.get()));
        break;
      }
      case Opc::BindEvent: case Opc::BindEventF: {
        uint32_t h = rd32();
        EventBinding b;
        Value filter;
        if (o == Opc::BindEventF) filter = resolve(pop());
        std::u16string name = pop().toString();
        Value target = resolve(pop());
        Object* ob = target.object();
        const EventInfo* ei = ob ? ob->cls->findEvent(name) : nullptr;
        if (!ei) error(kErrEventTarget, "Cannot set event to variant of this type!");
        if (ob->destroyed) error(kErrDestroyed, "Trying to use destroyed object!");
        if (ei->filtered != (o == Opc::BindEventF)) break;  // mismatch: silently not bound
        b.code = f.code;
        b.handler = int(h);
        b.self = f.self;
        b.hasFilter = ei->filtered;
        if (b.hasFilter) {
          std::u16string key = filter.toString();
          for (auto& c : key) c = char16_t(c & 0xFF);  // narrowed to bytes like the engine's std::string key
          b.filter = key;
        }
        auto& list = ob->events[name];
        bool dup = false;
        for (auto& e : list)
          if (e.code.get() == b.code.get() && e.handler == b.handler && e.self.get() == b.self.get() && e.filter == b.filter) dup = true;
        if (dup) break;  // re-executing the same event statement is a no-op
        b.order = ++bindOrder_;
        list.push_back(b);
        if (f.self && f.self.get() != ob) f.self->boundTargets.push_back(ob);
        break;
      }
      case Opc::CtxPush: {
        CtxEntry c;
        c.value = plain(pop());
        t->ctx.push_back(std::move(c));
        break;
      }
      case Opc::CtxPop:
        if (!t->ctx.empty()) t->ctx.pop_back();
        break;
      case Opc::Foreach: case Opc::ForeachRev: {
        int32_t rel = int32_t(rd32());
        CtxEntry c;
        c.iter = true;
        c.value = resolve(pop());
        int64_t n = iterCount(c.value);
        c.index = o == Opc::ForeachRev ? -1 : 0;
        t->ctx.push_back(std::move(c));
        if (n == 0) jumpRel(rel);
        break;
      }
      case Opc::ForeachNext: case Opc::ForeachRevNext: {
        int32_t rel = int32_t(rd32());
        CtxEntry& c = t->ctx.back();
        int64_t n = iterCount(c.value);  // re-read every step
        if (o == Opc::ForeachNext) {
          if (++c.index < n) jumpRel(rel);
        } else {
          if (--c.index >= -n) jumpRel(rel);  // -i == count || i > -count (0x1002ae90.c:788)
        }
        break;
      }
      case Opc::DefaultArg: {
        uint16_t slot = rd16();
        int32_t rel = int32_t(rd32());
        if (!plain(f.env->slots[slot]).isNone()) jumpRel(rel);  // default only for a valueless slot
        break;
      }
      case Opc::End: case Opc::Exit:
        finish();
        return Value();
      default:
        error(kErrInvalidCommand, "Invalid command (" + std::to_string(int(o)) + ")!");
    }
  }
  t->state = ThreadObj::State::Finished;
  return Value();
}

}  // namespace regs::script
