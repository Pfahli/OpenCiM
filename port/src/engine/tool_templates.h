#pragma once
// The map editor's templates (port): select an area, save it as a template, stamp it elsewhere (tool_templates.cpp).
#include "script/vm.h"

namespace regs::engine {

void registerTemplateTools(script::VM& vm);

}  // namespace regs::engine
