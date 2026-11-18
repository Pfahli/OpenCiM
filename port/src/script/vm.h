#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/code.h"
#include "script/lexer.h"
#include "script/object.h"
#include "script/value.h"

namespace regs {
class Vfs;
}

namespace regs::script {

// Runtime error (docs/script-runtime/semantics.md §9.1): ends only the thread that raised it.
struct ScriptError : std::runtime_error {
  int code;
  ScriptError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
};

// Engine error codes and texts (FUN_1001b6e0).
enum ErrorCode {
  kErrInvalidCommand = 0, kErrDivZero = 1, kErrNotAssignable = 2, kErrOperator = 3, kErrNotCallable = 4,
  kErrValueless = 5, kErrArrayIndex = 6, kErrMapIndex = 7, kErrNotIndexable = 8, kErrArrayAssign = 9,
  kErrNoReference = 10, kErrArgCount = 11, kErrEventTarget = 12, kErrUndefinedMember = 13, kErrDestroyed = 14,
  kErrInvalidArgs = 15, kErrMapInit = 16, kErrNotMap = 17, kErrCustom = 18,
};

// Locals of one activation.
struct Env : HeapObj {
  std::vector<Value> slots;
};

struct Frame {
  enum class Kind : uint8_t { Top, Function, Handler, Macro };
  Kind kind = Kind::Top;
  Ref<Code> code;
  uint32_t pc = 0;
  Ref<Object> self;
  Ref<Env> env;
  int argc = 0;
  size_t stackBase = 0;
  size_t ctxBase = 0;
};

// Context stack entry: a 'with' target or a foreach iterator (gsIndexVariant(coll, i)).
struct CtxEntry {
  Value value;
  bool iter = false;
  int64_t index = 0;  // reverse iteration uses -1, -2, ... (negative indices count from the end)
};

struct ThreadGroup;

class ThreadObj : public HeapObj {
 public:
  enum class State : uint8_t { Running = 1, Finished = 2, Sleeping = 3, Watching = 4 };
  State state = State::Sleeping;
  double wake = 0;
  ThreadGroup* group = nullptr;
  std::vector<Value> stack;
  std::vector<Frame> frames;
  std::vector<CtxEntry> ctx;
  Value result;
  uint64_t id = 0;
};

struct ThreadGroup {
  std::u16string name;
  double time = 0;
  float speed = 1.0f;
  int paused = 0;  // runs only when exactly 0; named groups start at 1 (semantics.md §5.2)
  std::vector<Ref<ThreadObj>> threads;
};

struct LoadOptions {
  std::u16string group;            // thread group for the top-level code ("" = default group)
  const ClassInfo* cls = nullptr;  // class of the created object (gsScript when null)
  Ref<Object> into;                // compile into an existing object (init.script into MAIN)
  bool keepInScriptList = true;    // load-created scripts stay alive until deleted
  // runs after the source compiled and before it is linked (state machines add their guard/action fragments)
  std::function<bool(Code& code, std::string* error)> beforeLink;
};

class VM {
 public:
  explicit VM(Vfs& vfs);
  ~VM();

  Vfs& vfs() { return vfs_; }

  // ---- registration ----
  void setGlobal(const std::u16string& name, const Value& v) { globals_[name] = v; }
  Value* findGlobal(std::u16string_view name);
  void defineFunction(const std::u16string& name, int minArgs, int maxArgs, NativeFn fn);
  ClassInfo* defineClass(const std::string& name, const std::string& parent);
  ClassInfo* findClass(const std::string& name);
  const ClassInfo* scriptClass() const { return scriptClass_; }
  bool isScriptObject(const Object* o) const { return o && o->cls->isA(scriptClass_); }
  bool isModule(const Object* o) const { return o && moduleClass_ && o->cls->isA(moduleClass_); }

  // ---- objects ----
  Ref<Object> newObject(const ClassInfo* cls, Object* parent);
  void destroy(Object* o);

  // ---- scripts ----
  // load / load g :: path (semantics.md §6): always returns a script object (empty after any error).
  Value load(const std::u16string& path, const LoadOptions& opt = {});
  // load() in two halves, for source compiled once and run many times (the shader templates, engine/shaders.cpp):
  // compileSource parses and compiles (null after an error, reported as load reports it; no extensions applied),
  // loadCompiled creates the object, links, lets init set script variables, then runs the top-level code at once.
  Ref<Code> compileSource(const std::u16string& source, const std::u16string& path);
  Value loadCompiled(const Ref<Code>& code, const std::u16string& path, const LoadOptions& opt,
                     const std::function<void(Object*)>& init = {});
  // merges the extensions of a game script (<root><path> for each of extensionRoots, in order) into its source
  // before compiling (see vm.cpp)
  void extendSource(const std::u16string& path, std::u16string& source);
  // VFS folders holding script extensions, applied in this order: the port's own first, then each enabled mod's
  // (engine/mods.cpp adds "regs/mods/<id>/extend/")
  std::vector<std::string> extensionRoots{"regs/extend/"};
  std::u16string resolvePath(const std::u16string& path, const Object* caller) const;

  // ---- calls ----
  // Calls a function, method or macro from native code; a script call runs until it returns or blocks.
  Value call(const Value& callable, const std::vector<Value>& args);
  // Fires an event: every matching handler runs immediately in a new default-group thread (semantics.md §4.2).
  bool fireEvent(Object* target, const std::u16string& name, const std::vector<Value>& args, const Value* key = nullptr);
  bool hasHandler(Object* target, const std::u16string& name) const;

  // ---- values (strict, as the interpreter sees them) ----
  Value getMember(const Value& obj, std::u16string_view name);
  void setMember(const Value& obj, std::u16string_view name, const Value& v);
  bool truthy(const Value& v);
  int32_t toInt(const Value& v);
  float toFloat(const Value& v);
  std::u16string toStr(const Value& v);
  Value resolve(const Value& v);  // dereference; evaluates macros

  // Runs compiled fragment code of 'self' synchronously in a new default-group thread; an expression fragment
  // yields its value (state-machine guards), a block fragment yields unset.
  Value runFragment(Object* self, uint32_t entry, uint16_t nlocals, bool expression);

  // ---- threads ----
  ThreadGroup& group(const std::u16string& name);
  void runThreads(float dt);
  size_t threadCount() const;
  float frameDt() const { return dt_; }

  // ---- object tree ----
  void tickChildren(Object* o, float dt);  // FUN_100a0670: enabled children in order

  // ---- diagnostics ----
  [[noreturn]] void error(int code, const std::string& msg);
  [[noreturn]] void error(const std::string& msg) { error(kErrCustom, msg); }
  std::function<void(const std::string&)> output;       // echo sink
  std::function<void(const std::string&)> errorOutput;  // error sink (one call per line)
  uint64_t instructions = 0;
  std::string where() const;
  std::map<std::string, int> missing;  // calls of stubbed natives, counted

 private:
  void link(Code& code, Object* self);
  void runThread(ThreadObj* t);
  Value execute(ThreadObj* t);
  ThreadObj* newThread(ThreadGroup& g);
  void pushCall(ThreadObj* t, int argc);
  Value binop(Op op, const Value& a, const Value& b);
  Value unop(Op op, const Value& a);
  Value applyHandler(uint8_t h, Op op, const Value& l, const Value& r);
  Value update(Op baseOrIncDec, uint8_t upd, const Value& cur, const Value& rhs);
  Value indexGet(const Value& a, const Value& i);
  void indexSet(const Value& a, const Value& i, const Value& v);
  bool indexDefined(const Value& a, const Value& i);
  void storeSlot(Value& slot, const Value& v);
  Value makeRef(std::function<Value()> get, std::function<void(const Value&)> set);
  Value ctxCurrent(ThreadObj* t);
  void ctxStore(ThreadObj* t, const Value& v);
  int64_t iterCount(const Value& coll);
  Value evalMacro(MacroObj* m);
  void report(const std::string& msg, ThreadObj* t);

  Vfs& vfs_;
  U16Map<Value> globals_;
  std::map<std::string, std::unique_ptr<ClassInfo>> classes_;
  const ClassInfo* scriptClass_ = nullptr;
  const ClassInfo* moduleClass_ = nullptr;
  std::vector<Ref<Object>> scripts_;  // loaded scripts stay alive until deleted (DAT_101939fc)
  std::vector<std::unique_ptr<ThreadGroup>> groups_;
  ThreadObj* current_ = nullptr;
  float dt_ = 0;
  uint64_t nextThreadId_ = 1;
  uint64_t nextSerial_ = 1;
  uint64_t bindOrder_ = 0;
  int depth_ = 0;
};

}  // namespace regs::script
