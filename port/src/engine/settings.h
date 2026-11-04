#pragma once
#include "script/vm.h"

namespace regs::engine {

// A number from MAIN.$settings: the game's settings.script variables plus the port's graphics options
// (regsViewDistance, regsDetailDistance; see port/data/metro/ui/menus/graphics.script). def when MAIN, the settings
// object or the variable is missing, or the value is not a number.
float settingNumber(script::VM& vm, const char16_t* name, float def);

}  // namespace regs::engine
