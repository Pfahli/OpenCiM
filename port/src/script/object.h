#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/code.h"
#include "script/value.h"

namespace regs::script {

class VM;

// Native callables. 'self' is null for global functions.
using NativeFn = std::function<Value(VM& vm, Object* self, int argc, const Value* args)>;

// Field types follow the engine's registrars (docs/native-api.md): bool 2, int 3, float 4, str 5, wstr 6,
// custom 7, var 8, bit 9.
struct FieldInfo {
  std::u16string name;
  int type = 0;
  bool readOnly = false;
  std::function<Value(VM&, Object*)> get;
  std::function<void(VM&, Object*, const Value&)> set;
};

struct MethodInfo {
  std::u16string name;
  int minArgs = 0, maxArgs = 0;
  NativeFn fn;
};

struct EventInfo {
  std::u16string name;
  int id = 0;
  bool filtered = false;  // descriptor flag 1: handlers are keyed by a filter string (keyDown::["Enter"])
};

struct ClassInfo {
  std::string name;
  const ClassInfo* parent = nullptr;
  // creates instances of native classes that need their own state or tick (nearest ancestor with one is used)
  std::function<Ref<Object>(const ClassInfo*)> factory;
  U16Map<FieldInfo> fields;
  U16Map<MethodInfo> methods;
  U16Map<EventInfo> events;

  const FieldInfo* findField(std::u16string_view n) const;
  const MethodInfo* findMethod(std::u16string_view n) const;
  const EventInfo* findEvent(std::u16string_view n) const;
  bool isA(const ClassInfo* other) const;
};

// A bound event handler (BIND_EVENT / BIND_EVENT_F).
struct EventBinding {
  Ref<Code> code;
  int handler = -1;         // index into code->handlers
  Ref<Object> self;         // 'this' inside the handler: the script containing the event statement
  std::u16string filter;    // filter key (filter.toString()) for filtered events
  bool hasFilter = false;
  uint64_t order = 0;       // binding order: deterministic stand-in for the engine's record-address order
};

// Script/engine object (gsObjectVariant > gsScript > gsModule ...). Every engine object can carry script variables,
// script functions (from the code it was loaded from), bound events and children.
class Object : public HeapObj {
 public:
  explicit Object(const ClassInfo* cls) : cls(cls) {}
  ~Object() override;

  const ClassInfo* cls;
  uint64_t serial = 0;            // creation number: stands in for the engine's object address (ordering, text)
  Ref<Code> code;                 // the script this object was compiled from (functions, script variables)
  std::u16string scriptPath;      // virtual path of that script
  std::u16string baseDir;         // directory for relative resource paths
  Object* parent = nullptr;
  std::vector<Ref<Object>> children;
  bool destroyed = false;
  uint32_t flags = 0xffff0005;    // gsModule bit fields at +0x6c: enabled 1, visible 2, audible 4 (ctor 0x100a1e80)
  std::u16string threadGroup;     // group for threads started by this object's code

  bool enabled() const { return flags & 1; }
  bool visible() const { return flags & 2; }

  // $variables in creation order (exportVariables writes them in this order)
  std::vector<std::pair<std::u16string, Value>> vars;
  U16Map<size_t> varIndex;
  std::map<std::u16string, std::vector<EventBinding>> events;
  std::vector<Ref<Object>> boundTargets;  // objects this script bound handlers on (unbound when it is destroyed)
  // storage for native fields that have no implementation yet (generated API defaults)
  U16Map<Value> fieldValues;

  Value* findVar(std::u16string_view name);
  Value& var(const std::u16string& name);  // creates unset when missing
  bool removeVar(const std::u16string& name);

  void addChild(const Ref<Object>& c);
  void removeChild(Object* c);

  // per-frame update (gsMoveable slot 0). The default ticks the enabled children in order (FUN_100a0670).
  virtual void tick(VM& vm, float dt);
  virtual void onDestroy(VM& vm) {}
};

// Script function / native function / bound native method.
class FuncObj : public HeapObj {
 public:
  enum class Kind { Script, Native };
  Kind kind = Kind::Native;
  Ref<Code> code;      // Script
  int index = -1;      // Script: function index in code
  Ref<Object> owner;   // Script: 'this'; Native method: self
  const MethodInfo* method = nullptr;  // Native method
  NativeFn native;     // Native global function
  std::u16string name;
  int minArgs = 0, maxArgs = 255;
};

class MacroObj : public HeapObj {
 public:
  Ref<Code> code;
  uint32_t entry = 0;
  int nlocals = 0;
  Ref<Object> self;
  std::vector<Value> captured;  // $0 ... $n
};

}  // namespace regs::script
