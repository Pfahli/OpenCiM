#pragma once
#include <string>

#include "script/vm.h"

namespace regs::engine {

// Generated from docs/native-api.json (tools/gen-port-api.mjs): every class, field, method, event, global function
// and constant of the engine's script API. Methods and functions start as logging stubs; fields store their value;
// new_* constructors create an object of their class under the parent passed as the first argument.
void registerGeneratedApi(script::VM& vm);

// Hand-written implementations replace the stubs (engine/natives_*.cpp).
void registerCoreNatives(script::VM& vm);
void registerObjectNatives(script::VM& vm);  // clocks, timers, state machines, module flags
std::u16string currentStateName(script::Object* stateMachine);
void registerUiNatives(script::VM& vm);  // textures, colours, GUI controls, window helpers

// helpers used by the generated code and by implementations
void addField(script::ClassInfo* c, const std::u16string& name, int type, bool readOnly);
void addMethod(script::ClassInfo* c, const std::u16string& name, int minArgs, int maxArgs);
void addEvent(script::ClassInfo* c, const std::u16string& name, int id, bool filtered);
void stubFunction(script::VM& vm, const std::u16string& name, int minArgs, int maxArgs);
void ctorFunction(script::VM& vm, const std::u16string& name, const char* cls, int minArgs, int maxArgs);

// Replace a method's implementation (keeps its argument range).
void implement(script::VM& vm, const char* cls, const std::u16string& method, script::NativeFn fn);
// Replace a global function's implementation (keeps its argument range when it exists).
void implementGlobal(script::VM& vm, const std::u16string& name, script::NativeFn fn, int minArgs = -1, int maxArgs = -1);

}  // namespace regs::engine
