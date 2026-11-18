#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "script/value.h"

namespace regs::script {

// ReGS bytecode. It mirrors the engine's stack machine (docs/vm-opcodes.md) but is not byte-compatible: scripts
// ship as source, so only behaviour has to match. Operands are little-endian and follow the opcode inline.
enum class Opc : uint8_t {
  Nop, Pop, Dup, Dup2,
  PushNone, PushNull, PushTrue, PushFalse,
  PushInt,      // i32
  PushFloat,    // f32
  PushStr,      // u32 string index
  PushLink,     // u32 link index (bare identifier resolved at link time)
  PushThis, PushCtx, PushDt,
  LoadLocal,    // u16 slot
  StoreLocal,   // u16 slot            v -> v
  LoadVar,      // u32 name            -> this.$name
  StoreVar,     // u32 name            v -> v
  GetMember,    // u32 name (with '$')  obj -> v
  SetMember,    // u32 name             obj v -> v
  GetIndex,     //                      a i -> v
  SetIndex,     //                      a i v -> v
  Append,       //                      a v -> v       (a[] = v)
  UpdLocal,     // u16 slot, u8 upd     [rhs] -> v
  UpdVar,       // u32 name, u8 upd     [rhs] -> v
  UpdMember,    // u32 name, u8 upd     obj [rhs] -> v
  UpdIndex,     // u8 upd               a i [rhs] -> v
  StoreCtx,     //                      v -> v        (current = v: writes the iterated element)
  UpdCtx,       // u8 upd               [rhs] -> v
  RefLocal,     // u16 slot             -> ref        ('@')
  RefVar,       // u32 name
  RefMember,    // u32 name             obj -> ref
  RefIndex,     //                      a i -> ref
  Unop,         // u8 op
  Binop,        // u8 op
  Call,         // u8 argc              f a1..an -> r
  Return,       //                      v ->
  Jmp, Jz, Jnz, // i32 offset relative to the end of the instruction
  MakeArray,    // u32 n
  MakeMap,      // u32 n (values; pairs)
  Keys, Values, Defined, Export,
  DefinedIndex, //                      a i -> bool   (defined($a[i]) never raises an index error)
  Wait,         // s ->
  Yield,        // sleep until the next frame (skip_frame, wait_until re-check)
  Thread,       // i32 body offset      -> thread     (body shares the current locals)
  ThreadIn,     // i32 body offset      group -> thread
  Pause, Resume, Echo, Assert, Delete,
  Load,         //                      path -> obj
  LoadIn,       //                      group path -> obj
  MakeMacro,    // u8 argc, u16 nlocals, i32 body offset    a1..an -> macro
  HaltValue,    //                      v ->   (end of a macro body)
  BindEvent,    // u32 handler          obj ->
  BindEventF,   // u32 handler          obj filter ->
  CtxPush,      //                      x ->
  CtxPop,
  Foreach,      // i32 exit offset      coll ->       (pushes an iterator on the context stack)
  ForeachNext,  // i32 body offset (negative)
  ForeachRev, ForeachRevNext,
  DefaultArg,   // u16 slot, i32 skip offset: if the argument was passed, jump over its default
  End,          // finish the thread
  Exit,         // finish the thread (exit statement)
};

// Update kinds for Upd* opcodes: compound assignment uses the binary operator index (Op::Add ...);
// increments/decrements use these values.
enum : uint8_t { kUpdPreInc = 0xF0, kUpdPreDec = 0xF1, kUpdPostInc = 0xF2, kUpdPostDec = 0xF3, kUpdAssignPlain = 0xF4 };

struct FunctionInfo {
  std::u16string name;
  uint32_t entry = 0;
  uint16_t nparams = 0;
  uint16_t minArgs = 0;  // nparams minus the number of parameters with defaults
  uint16_t nlocals = 0;
  int line = 0;
};

struct HandlerInfo {
  uint32_t entry = 0;
  uint16_t nparams = 0;
  uint16_t nlocals = 0;
  std::u16string event;  // member name the handler was bound to (for diagnostics)
  int line = 0;
};

struct Code : HeapObj {
  std::u16string path;
  std::vector<uint8_t> bc;
  std::vector<std::pair<uint32_t, int>> lines;  // (pc, line) at each line change
  std::vector<std::u16string> strings;
  std::unordered_map<std::u16string, uint32_t> stringIndex;
  std::vector<FunctionInfo> functions;
  U16Map<int> functionIndex;
  std::vector<HandlerInfo> handlers;
  std::vector<std::u16string> links;
  std::vector<int> linkLines;
  std::vector<Value> linked;      // filled by the linker: native functions, constants, global objects
  std::vector<int> linkFunction;  // filled by the linker: index of an own function (bound to 'this' at run time), -1
  bool isLinked = false;
  uint16_t topLocals = 0;      // locals of the top-level code

  // script variables known at compile time, in source order; created as null on the script object at load
  std::vector<std::u16string> scriptVars;
  std::unordered_map<std::u16string, uint32_t> scriptVarIndex;
  bool hasScriptVar(const std::u16string& n) const { return scriptVarIndex.count(n) != 0; }
  void addScriptVar(const std::u16string& n) {
    if (scriptVarIndex.emplace(n, uint32_t(scriptVars.size())).second) scriptVars.push_back(n);
  }

  int lineAt(uint32_t pc) const;
  uint32_t intern(const std::u16string& s);
};

std::string disassemble(const Code& code);

}  // namespace regs::script
