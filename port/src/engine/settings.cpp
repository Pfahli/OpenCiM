#include "engine/settings.h"

namespace regs::engine {

using namespace regs::script;

float settingNumber(VM& vm, const char16_t* name, float def) {
  Value* main = vm.findGlobal(u"MAIN");
  Object* m = main ? main->object() : nullptr;
  Value* s = m ? m->findVar(u"settings") : nullptr;
  Object* so = s ? vm.resolve(*s).object() : nullptr;
  Value* v = so ? so->findVar(name) : nullptr;
  if (!v) return def;
  Value x = vm.resolve(*v);
  if (x.type() == VT::Int || x.type() == VT::Float) return vm.toFloat(x);
  if (x.type() == VT::Bool) return x.toBool() ? 1.0f : 0.0f;
  return def;
}

}  // namespace regs::engine
