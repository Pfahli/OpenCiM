# GSystem script VM — bytecode reference

Reverse engineered from `GSystem.dll` (SHA-256 `1daf5be3…02602`, Cities in Motion 1.0.22) with Ghidra 12.1.4
pseudocode (`decompiled/GSystem-v2/functions/`; v1 in `decompiled/GSystem/` has the same addresses). Each opcode was read in the **interpreter** (`FUN_1002ae90`) and,
where noted, matched to the **compiler** (`FUN_100067c0` plus expression helpers) that emits it through
`FUN_100166d0` (opcode), `FUN_10016740` (opcode + u8), `FUN_100165b0` (raw operand bytes) and `FUN_100167c0`
(reserve a 4-byte jump slot to back-patch). Source-level evidence comes from shipped scripts under `extracted/`.
Mnemonics are mine; the binary has no names for them.

## Machine model

* **Code object** (built by the compiler): `+0x04..+0x08` link table (36-byte records: identifiers resolved by the
  linker), `+0x2c` global/script variable container, `+0x38` event table (16-byte records), `+0x50/+0x54/+0x58`
  bytecode buffer / length / capacity (starts at 256 bytes, doubles).
* **Thread** (one per coroutine): `+0x14` current frame, `+0x18` state, `+0x1c` thread-group index, `+0x20`
  wake-up time (double). States set by `FUN_1002a800`: **1** running, **2** finished, **3** sleeping until
  `+0x20`, **4** blocked until a watched variable changes.
* **Frame**: `[0]` executing script object (its `+0x14` is the code object), `[1]` program counter,
  `[2]` locals container, `[3]` operand-stack capacity, `[4]` stack depth, `[6]` locals base index,
  `[7]` locals count, `[8]` top of the context stack (`foreach` iterators and `with` targets).
* **Operand stack**: array of ref-counted variant pointers; grows on demand.
* **Dispatch**: fetch `op = code[pc++]`; `0xFF` ends the thread (state 2) before the switch; any byte above `0x3B`
  raises *"Invalid command (%d)!"* via the runtime-error formatter `FUN_1001b6e0`. Operands follow the opcode
  inline, little-endian. Jump offsets (`rel32`) are relative to the position of the offset field itself.

## Variant categories

The variant method at vtable `+0x18` returns an operator-dispatch category. Binary, prefix and postfix
operators look up a handler in the table at `DAT_101939f8` indexed by (left category, right category,
operator). A missing handler raises *"Operator %d not defined for operands!"*.

| Cat | Variants |
|---|---|
| 0 | valueless (the "unset" singleton), closures/macros |
| 1 | bool (`true`/`false` singletons) |
| 2 | int (`0` is a cached singleton) |
| 3 | float |
| 4 | string (UTF-16) |
| 5 | object-like: `null`, threads, engine objects |
| 6 | array |
| 9 | map |

## Opcodes

Stack notation: `a b → r` means `b` is the top before the instruction and `r` replaces both.

| Op | Mnemonic | Operands | Stack | Meaning | Emitted for |
|---|---|---|---|---|---|
| 00 | NOP | – | – | no operation | (padding) |
| 01 | POP | – | `x →` | discard top | expression statements |
| 02 | POSTFIX | u8 op | `a → r` | postfix operator (`a++`, `a--`) | postfix ops |
| 03 | PREFIX | u8 op | `a → r` | prefix operator (`!a`, `-a`, `~a`, `++a`, `#a`, `@a`) | prefix ops |
| 04 | BINOP | u8 op | `a b → r` | binary operator (table below) | binary ops |
| 05 | PUSH_I32 | i32 | `→ int` | integer literal | every non-zero int literal (`FUN_10005500`) |
| 06 | PUSH_F32 | f32 | `→ float` | float literal | float literals |
| 07 | PUSH_STR | u8 n, n×u16 | `→ str` | short string literal (≤ 255 chars) | `"…"` |
| 08 | PUSH_LINK | i32 idx | `→ v` | push the linker-resolved object for identifier `idx` (script function, native function, constant) | bare identifiers |
| 09 | CALL | u8 argc | `f a1…an → r` | call `f` (variant method `+0x48`); may switch frames | `f(…)` |
| 0A | PUSH_U16 | u16 | `→ int` | 16-bit integer | separate literal helper `FUN_10005840`; most likely character literals `'c'` (**inferred**) |
| 0B | PUSH_GLOBAL | i32 id | `→ ref` | reference to script/global variable `id` | `$var`, `global` |
| 0C | ASSIGN | – | `ref v → ref` | `ref = v` (variant method `+0x44`); the reference stays on the stack | `=` (operator 0x1C), default-parameter init |
| 0D | PUSH_FALSE | – | `→ false` | | `false` |
| 0E | PUSH_TRUE | – | `→ true` | | `true` |
| 0F | PUSH_NULL | – | `→ null` | | `null` |
| 10 | PUSH_VOID | – | `→ valueless` | the "unset" value (category 0) | implicit `return`, empty `return` |
| 11 | PUSH_ZERO | – | `→ 0` | cached integer 0 | literal `0` |
| 12 | INDEX | – | `a i → a[i]` | element/entry reference (method `+0x60`) | `a[i]` |
| 13 | MAKE_ARRAY | i32 n | `v1…vn → arr` | array literal | `[a, b, …]` |
| 14 | JMP | rel32 | – | unconditional jump | `if/else`, loops, `break`, `continue`, skip inline bodies |
| 15 | JZ | rel32 | `c →` | jump if `c` is false | `if`, `while`, `for`, `wait_while` |
| 16 | WAIT | – | `s →` | sleep `s` seconds (thread state 3, wake = clock + s) | `wait s` |
| 17 | THREAD | rel32 | `→ thread` | spawn a coroutine at `pc+rel` sharing the current locals, default group | `thread { … }` |
| 18 | WAIT_WATCH | i32 n, n×i32 global ids, i32 m, m×i32 local ids, rel32 | – | register as watcher on the listed variables, jump back to the condition, block (state 4) | `wait_until` / `wait_while` |
| 19 | JNZ | rel32 | `c →` | jump if `c` is true | `do … while`, `wait_until` |
| 1A | RETURN | – | `… v → v` | return from a script function: pop locals, restore caller frame | `return` inside a function |
| 1B | PUSH_LOCAL | i32 idx | `→ ref` | reference to local variable (`frame[6]+idx`) | `local`, parameters |
| 1C | BIND_EVENT | i32 idx | `obj →` | bind event handler `idx` (event table) on `obj` (method `+0x54`) | `event obj::(…) { … }` |
| 1D | PUSH_THIS | – | `→ this` | current script object | `this` |
| 1E | BIND_EVENT_F | i32 idx | `obj filter →` | as 1C with a filter value | `event obj::[filter](…) { … }` |
| 1F | CTX_PUSH | – | `x →` | push `x` on the context stack | `with (x)` |
| 20 | CTX_POP | – | – | pop the context stack | end of `with`/`foreach`; before `return`/`exit` |
| 21 | PUSH_CTX | – | `→ v` | current context value (iterator's current element / `with` target) | `current`, leading `.member` |
| 22 | FOREACH | rel32 | `coll →` | push iterator at index 0; jump to `rel` if `coll` is empty | `foreach` |
| 23 | FOREACH_NEXT | rel32 | – | `index++`; jump back to body while `index < count` | `foreach` |
| 24 | ECHO | – | `x →` | print `x` (toString) to the script output | `echo` |
| 25 | GET_MEMBER | NUL-terminated ASCII name | `obj → v` | member by name (method `+0x58`); names keep their `$` | `.name`, `.$name` |
| 26 | DELETE | – | `x →` | destroy the object; ends the thread if it deleted its own script | `delete` |
| 27 | GET_MEMBER_DYN | – | `obj name → v` | member by computed name | dynamic member access |
| 28 | LOAD | – | `path → obj` | load/compile/run a script file, push its object | `load "…"` |
| 29 | HALT_VALUE | – | `v →` | finish the thread and hand `v` to the caller | end of a `macro` body |
| 2A | MAKE_MACRO | u8 argc, rel32 | `a1…an → macro` | closure over `argc` captured values and the code at `rel` | `macro :: …` |
| 2B | SKIP_FRAME | – | – | sleep until the next frame (wake = clock + 1e-5) | `skip_frame` |
| 2C | PUSH_DT | – | `→ float` | the delta time passed to the exported `runThreads(dt)` | `skip_frame` (it evaluates to dt) |
| 2D | THREAD_IN | rel32 | `group → thread` | as 17 but in the named thread group | `thread "game" :: { … }` (`metro/ui/announcement/announcement.script:27`) |
| 2E | PAUSE | – | `group →` | increment the group's pause count | `pause "group"` |
| 2F | RESUME | – | `group →` | decrement the group's pause count | `resume "group"` |
| 30 | DEFAULT_ARG | i32 idx, rel32 | `→ ref?` | if parameter `idx` was passed, jump over its default; else push its reference | `function f($a = …)` |
| 31 | APPEND_SLOT | – | `arr → ref` | reference to `arr[count]` (one past the end) | `$a[] = x` |
| 32 | FOREACH_REV | rel32 | `coll →` | iterator from the end; jump if empty | `foreach_reverse` |
| 33 | FOREACH_REV_NEXT | rel32 | – | `index--`; loop while elements remain | `foreach_reverse` |
| 34 | MAKE_MAP | i32 n | `k1 v1 … → map` | map literal (n values, pairs) | `map [k, v, …]` |
| 35 | KEYS | – | `m → arr` | keys of a map (method `+0x64`) | `keys` |
| 36 | VALUES | – | `m → arr` | values (method `+0x68`) | `values` |
| 37 | EXPORT | – | `v → str` | serialize a value as script source (method `+0x34`) | `export` |
| 38 | ASSERT | – | `c →` | runtime error *"Assert failed!"* if `c` is false | `assert` |
| 39 | DEFINED | – | `v → bool` | true unless valueless (method `+0x70`) | `defined` |
| 3A | PUSH_STR32 | i32 n, n×u16 | `→ str` | long string literal | strings > 255 chars (`FUN_10005710`) |
| 3B | LOAD_IN | – | `group path → obj` | load a script with its threads in a thread group | `load "group" :: path` |
| FF | END | – | – | finish the thread | end of top-level code, thread and event bodies, `return;` in an event handler, `exit` (a top-level `return` is a compile error, `script-runtime/semantics.md` §3) |

## Operator codes (operand of 02/03/04)

The lexer (`FUN_10016050` → `FUN_10015510`) produces operator indices; the table at `0x10156450` holds
`{u32 precedence, u32 kind}` per index (kind 0 binary, 1 assignment/right-associative, 2 postfix, 3 prefix;
a lower precedence number binds tighter). `+`, `-`, `++`, `--` carry two indices and the parser picks by position.

| Idx | Op | Kind | Prec | | Idx | Op | Kind | Prec |
|---|---|---|---|---|---|---|---|---|
| 01 | `+` | bin | 6 | | 17 | `--` (prefix) | pre | 3 |
| 02 | `-` | bin | 6 | | 18 | `++` (postfix) | post | 2 |
| 03 | `*` | bin | 5 | | 19 | `--` (postfix) | post | 2 |
| 04 | `/` | bin | 5 | | 1A | `+` (unary) | pre | 3 |
| 05 | `%` | bin | 5 | | 1B | `-` (unary) | pre | 3 |
| 06 | `==` | bin | 9 | | 1C | `=` (→ opcode 0C) | asg | 16 |
| 07 | `!=` | bin | 9 | | 1D | `+=` | asg | 16 |
| 08 | `<` | bin | 8 | | 1E | `-=` | asg | 16 |
| 09 | `<=` | bin | 8 | | 1F | `*=` | asg | 16 |
| 0A | `>` | bin | 8 | | 20 | `/=` | asg | 16 |
| 0B | `>=` | bin | 8 | | 21 | `%=` | asg | 16 |
| 0C | `<<` | bin | 7 | | 22 | `&=` | asg | 16 |
| 0D | `>>` | bin | 7 | | 23 | `\|=` | asg | 16 |
| 0E | `>>>` | bin | 7 | | 24 | `^=` | asg | 16 |
| 0F | `&&` | bin | 13 | | 25 | `<<=` | asg | 16 |
| 10 | `\|\|` | bin | 14 | | 26 | `>>=` | asg | 16 |
| 11 | `!` | pre | 3 | | 27 | `>>>=` | asg | 16 |
| 12 | `&` | bin | 10 | | 28 | `?` | bin | 15 |
| 13 | `\|` | bin | 12 | | 29 | `:` | bin | 15 |
| 14 | `^` | bin | 11 | | 2A | `@` (reference) | pre | 1 |
| 15 | `~` | pre | 3 | | 2B | `#` (count) | pre | 1 |
| 16 | `++` (prefix) | pre | 3 | | | | | |

* `?` and `:` are separate binary operators at the same precedence. `x : y` on its own yields `y` when `x` is
  unset (`$settings.$fps : false`). **Inferred** from precedence and usage, not from the handler code.
* `@x` makes a live reference (`setArg(0, @$stop.passengerCount)` in `metro/scripts/ui.script:463`);
  `#x` is the element count (`$h[#$h-7]` in `metro/ui/linepanel/linepanel.script:357`).
* Other tokens: `( ) , . ; [ ]` are punctuation; `::` is its own token (event, macro and `load … ::`);
  `"…"` strings, `'…'` character literals, `$name` variables; `//` and `/* */` comments.

## Compilation patterns (from the emit sites)

```
if (c) A else B      c; JZ L1; A; JMP L2; L1: B; L2:
while (c) A          L0: c; JZ L1; A; JMP L0; L1:
do A while (c)       L0: A; c; JNZ L0
foreach (x) A        x; FOREACH L1; L0: A; FOREACH_NEXT L0; L1: CTX_POP
with (x) A           x; CTX_PUSH; A; CTX_POP
wait_until (c)       L0: c; JNZ L1; WAIT_WATCH [vars of c] L0; L1:     (wait_while uses JZ)
thread { A }         THREAD L0; JMP L1; L0: A; END; L1:            (pushes the thread object)
thread "g" :: { A }  "g"; THREAD_IN L0; JMP L1; L0: A; END; L1:
event o::[f](p){A}   o; f; BIND_EVENT_F idx; JMP L1; A; END; L1:
macro :: e           a1..an; MAKE_MACRO n L0; JMP L1; L0: e; HALT_VALUE; L1:
skip_frame           SKIP_FRAME; PUSH_DT
function end         PUSH_VOID; RETURN
```

## Not yet verified

* The per-type operator handlers in `DAT_101939f8` (e.g. string `+`, the semantics of `?`/`:`), and what the
  native `+0x44/+0x48/+0x54/+0x58/+0x60` methods do for each engine class.
* The exact record layouts of the link table (36 B) and event table (16 B), and the linker's resolution order.
* `0x27` has no constant emit site; its compile path (computed member names) was inferred from the interpreter.
* Ghidra did not define small vtable-only methods as functions, so several variant methods
  (e.g. the category getters) were read from raw bytes (`tools/vtables.mjs`).
