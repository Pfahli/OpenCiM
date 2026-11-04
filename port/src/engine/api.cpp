#include "engine/api.h"

#include "core/log.h"
#include "core/text.h"

namespace regs::engine {

using namespace regs::script;

namespace {

Value defaultFor(int type) {
  switch (type) {
    case 2: case 9: return Value::boolean(false);
    case 3: return Value::integer(0);
    case 4: return Value::real(0.0f);
    case 5: case 6: return Value::string(std::u16string());
    default: return Value();
  }
}

Value coerce(int type, const Value& v) {
  switch (type) {
    case 2: case 9: return Value::boolean(v.toBool());
    case 3: return Value::integer(v.toInt());
    case 4: return Value::real(v.toFloat());
    case 5: case 6: return Value::string(v.toString());
    default: return v;
  }
}

}  // namespace

void addField(ClassInfo* c, const std::u16string& name, int type, bool readOnly) {
  FieldInfo f;
  f.name = name;
  f.type = type;
  f.readOnly = readOnly;
  f.get = [name, type](VM&, Object* o) {
    auto it = o->fieldValues.find(name);
    return it == o->fieldValues.end() ? defaultFor(type) : it->second;
  };
  f.set = [name, type](VM&, Object* o, const Value& v) { o->fieldValues[name] = coerce(type, v); };
  c->fields[name] = std::move(f);
}

void addMethod(ClassInfo* c, const std::u16string& name, int minArgs, int maxArgs) {
  MethodInfo m;
  m.name = name;
  m.minArgs = minArgs;
  m.maxArgs = maxArgs;
  std::string key = "native " + c->name + "." + utf16ToUtf8(name);
  m.fn = [key](VM& vm, Object*, int, const Value*) {
    ++vm.missing[key];
    return Value();
  };
  c->methods[name] = std::move(m);
}

void addEvent(ClassInfo* c, const std::u16string& name, int id, bool filtered) {
  c->events[name] = EventInfo{name, id, filtered};
}

void stubFunction(VM& vm, const std::u16string& name, int minArgs, int maxArgs) {
  std::string key = "native " + utf16ToUtf8(name);
  vm.defineFunction(name, minArgs, maxArgs, [key](VM& v, Object*, int, const Value*) {
    ++v.missing[key];
    return Value();
  });
}

void ctorFunction(VM& vm, const std::u16string& name, const char* cls, int minArgs, int maxArgs) {
  std::string clsName = cls;
  vm.defineFunction(name, minArgs, maxArgs, [clsName](VM& v, Object*, int argc, const Value* args) {
    const ClassInfo* ci = v.findClass(clsName);
    Object* parent = argc > 0 ? args[0].object() : nullptr;
    Ref<Object> o = v.newObject(ci, parent);
    return Value::heap(VT::Object, o.get());
  });
}

void implement(VM& vm, const char* cls, const std::u16string& method, NativeFn fn) {
  ClassInfo* c = vm.findClass(cls);
  if (!c) {
    LOG_WARN("implement: unknown class %s", cls);
    return;
  }
  auto it = c->methods.find(method);
  if (it == c->methods.end()) {
    MethodInfo m;
    m.name = method;
    m.minArgs = 0;
    m.maxArgs = 255;
    it = c->methods.emplace(method, std::move(m)).first;
  }
  it->second.fn = std::move(fn);
}

void implementGlobal(VM& vm, const std::u16string& name, NativeFn fn, int minArgs, int maxArgs) {
  if (Value* g = vm.findGlobal(name); g && g->func()) {
    FuncObj* f = g->func();
    f->native = std::move(fn);
    if (minArgs >= 0) f->minArgs = minArgs;
    if (maxArgs >= 0) f->maxArgs = maxArgs;
    return;
  }
  vm.defineFunction(name, minArgs < 0 ? 0 : minArgs, maxArgs < 0 ? 255 : maxArgs, std::move(fn));
}

}  // namespace regs::engine
