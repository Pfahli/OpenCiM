#include "script/object.h"

#include <algorithm>

#include "script/code.h"
#include "script/vm.h"

namespace regs::script {

const FieldInfo* ClassInfo::findField(const std::u16string& n) const {
  for (auto* c = this; c; c = c->parent) {
    auto it = c->fields.find(n);
    if (it != c->fields.end()) return &it->second;
  }
  return nullptr;
}

const MethodInfo* ClassInfo::findMethod(const std::u16string& n) const {
  for (auto* c = this; c; c = c->parent) {
    auto it = c->methods.find(n);
    if (it != c->methods.end()) return &it->second;
  }
  return nullptr;
}

const EventInfo* ClassInfo::findEvent(const std::u16string& n) const {
  for (auto* c = this; c; c = c->parent) {
    auto it = c->events.find(n);
    if (it != c->events.end()) return &it->second;
  }
  return nullptr;
}

bool ClassInfo::isA(const ClassInfo* other) const {
  for (auto* c = this; c; c = c->parent)
    if (c == other) return true;
  return false;
}

Object::~Object() = default;

void Object::tick(VM& vm, float dt) { vm.tickChildren(this, dt); }

Value* Object::findVar(const std::u16string& name) {
  auto it = varIndex.find(name);
  return it == varIndex.end() ? nullptr : &vars[it->second].second;
}

Value& Object::var(const std::u16string& name) {
  auto it = varIndex.find(name);
  if (it != varIndex.end()) return vars[it->second].second;
  varIndex.emplace(name, vars.size());
  vars.emplace_back(name, Value());
  return vars.back().second;
}

bool Object::removeVar(const std::u16string& name) {
  auto it = varIndex.find(name);
  if (it == varIndex.end()) return false;
  size_t i = it->second;
  vars.erase(vars.begin() + ptrdiff_t(i));
  varIndex.erase(it);
  for (auto& [k, idx] : varIndex)
    if (idx > i) --idx;
  return true;
}

void Object::addChild(const Ref<Object>& c) {
  if (c->parent == this) return;
  if (c->parent) c->parent->removeChild(c.get());
  c->parent = this;
  children.push_back(c);
}

void Object::removeChild(Object* c) {
  auto it = std::find_if(children.begin(), children.end(), [&](const Ref<Object>& r) { return r.get() == c; });
  if (it == children.end()) return;
  c->parent = nullptr;
  children.erase(it);
}

}  // namespace regs::script
