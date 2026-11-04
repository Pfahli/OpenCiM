# GSystem script runtime — execution semantics

Scope: how names resolve, how calls, events, threads, `load` and macros behave, and how errors are handled in the
script VM of `GSystem.dll` (Cities in Motion 1.0.22). Operators and value conversions are documented separately
(`operators.json` in this folder). Opcode numbers and mnemonics follow [`../vm-opcodes.md`](../vm-opcodes.md).

Evidence is cited as `0xADDR.c:line` (file in `decompiled/GSystem-v2/functions/`), `bytes@VA` (raw DLL bytes) or a
shipped script path under `extracted/`. Every rule carries a tag:

* **observed**: read directly from pseudocode or bytes;
* **corroborated**: an inference confirmed by shipped scripts;
* **inferred**: a consequence of the code that was not traced end to end.

Vtable slots are written as byte offsets (`+0x48` = slot 18).

---

## 0. Machine model recap (what the rules below refer to)

| Object | Layout (offsets) | Evidence |
|---|---|---|
| **gsObjectVariant** (base of every script-visible object) | `+4` refcount, `+8` destroy flags (2 = destroying, bit 0 = destroyed), `+0xC` per-object event-handler table (one container per event id), `+0x10` class info (member table, event descriptors 8 B each, event count at `+0x20`) | `0x1001f960.c:22-27`, `0x1001c2e0.c` |
| **gsScript** (0x3C B) | `+0x14` code object, `+0x18` `scriptFilename` (wstring), `+0x34` "kept alive by the global script list" flag, `+0x38` serial number. **No parent or children pointer.** | ctor `0x10017360.c`, `0x10019490.c:36-39` |
| **Code object** (0x5C B, one per script object) | `+0x04` link table (36 B records: name, …, `+0x1C` resolved variant, `+0x20` line), `+0x14` list of event bindings this script made, `+0x20` variable-name map `name→index` (size at `+0x28`), `+0x2C` variable storage (a `gsArrayVariant`), `+0x30/+0x34` function table (20 B: link index, nParams, nLocals, entry pc, nDefaults), `+0x38/+0x3C` event table (16 B: owner script, nParams, nLocals, entry pc), `+0x50` bytecode | `0x1001a550.c`, `0x10005f00.c`, `0x1000e7d0.c` |
| **gsThread** (0x28 B) | `+0x14` frame, `+0x18` state (1 running, 2 finished, 3 sleeping, 4 blocked on watchers), `+0x1C` group index, `+0x20` wake time (double) | `0x10028dd0.c`, `0x1002a800.c` |
| **Frame** (0x24 B, one per thread) | `[0]` executing script, `[1]` pc, `[2]` operand stack (a `gsArrayVariant`, initial capacity 256), `[4]` → stack depth, `[5]` watcher list, `[6]` locals base, `[7]` locals count, `[8]` context top (`with`/`foreach`) | `0x10028c60.c` |
| **gsThreadFrame** (saved caller state, lives on the operand stack) | `+8` locals base, `+0xC` locals count, `+0x10` script, `+0x14` pc | `0x10028830.c` |
| **Thread group** (0x20 B) | `+0` time (double), `+8` speed (float, default 1.0), `+0xC` pause count, `+0x10` sleeping threads ordered by wake time | `0x1002abb0.c`, `0x1002c5d0.c` |

Singletons: `DAT_10193aa8` = valueless (`gsNoneVariant`), `DAT_10193aac` = `null` (`gsNullVariant`)
(`0x1002e940.c`, `0x1002e2c0.c`, `0x1002e330.c`; **observed**).

Locals live **on the operand stack** of the frame: local `i` is stack slot `frame[6] + i`. `PUSH_LOCAL`/`PUSH_GLOBAL`
push a `gsIndexVariant` (container + index) that reads and writes through that slot (`0x10016a00.c`,
`0x1002d7b0.c`; **observed**).

---

## 1. Names

### 1.1 `$name` — compile-time decision (single pass, source order)

The compiler decides at each `$name` occurrence whether it is a **script variable** (`PUSH_GLOBAL id`, storage in the
executing script's code object) or a **local** (`PUSH_LOCAL idx`). The rule depends on the compile mode
(`0x10006440.c:46-109`, **observed**):

| Where the `$name` occurs | Resolution |
|---|---|
| Top-level code, a top-level `thread` body, a **macro body** | Always a script variable. If the name is new, it is added to the script's variable map now (index = map size). |
| Function body, event-handler body, or a `thread` body inside them | 1. A local of the current function/handler if already declared (parameter, `local`, or earlier implicit local). 2. Otherwise a script variable **if the name is already in the script's variable map at this point of the compile**. 3. Otherwise a **new implicit local**. |

* The variable map is filled only by: top-level mentions (`0x10006440.c:56`), `global $x` (`0x100067c0.c:698-724`),
  `obj.$x` member access at run time (`0x1001a650.c:89`) and `setVariable`/`mergeVariables` (`0x1001a9e0.c:46`). There is
  no pre-pass (`0x10005f00.c` compiles statement by statement). **Observed.**
* Consequence: inside a function, `$x` means "the script's `$x`" only if `$x` appeared **earlier in the file** at top
  level or in a `global`. A function written above the first top-level use of `$x` gets a private local `$x`.
  **Corroborated** by `metro/ui/loading/loading.script`: `$toLoad = [];` at line 4 makes `$toLoad` shared, while
  `$elapsedLoadTime`, `$totalLoadTime`, `$loadPosition` first appear inside `prepare()` and are declared with
  `global` (lines 154-156) so that the event handler at line 166 and `updateLoadingPosition` at line 211 see them.
* Inside a function, `this.$x` (member access) always reaches the script variable, independent of compile order
  (**observed**, `0x1001a650.c`).

### 1.2 `local` and `global`

* `local $x` (keyword 22): forces a local. If `$x` is not yet a local of the current function/handler, a new local slot is
  allocated; then `PUSH_LOCAL` (`0x100067c0.c:782-808`, **observed**). Only one shipped use, inside a function
  (`metro/ui/menus/graphics.script:231`). At top level the top-level frame reserves no locals, so `local` there aliases
  operand-stack slot `idx` — undefined behaviour (**inferred**, see open questions).
* `global $x` (keyword 18): forces the script variable, creating the map entry if needed, then `PUSH_GLOBAL`
  (`0x100067c0.c:698-724`, **observed**). Because it adds `$x` to the map, every **later** plain `$x` in the same
  function (and in later functions) also resolves to the script variable, unless `$x` was already a local there.
* `$0`, `$1`, … (the lexer's "Parameter" token) compile to `PUSH_LOCAL n`: the n-th parameter of a function or event
  handler, or the n-th captured value of a macro. Elsewhere: compiler error *"Parameter "$%ls" not allowed here!"*
  (`0x100058b0.c:16-35`, **observed**).

### 1.3 Initial values

* Script variables created at link time start as **`null`**: the linker allocates the storage with `code+0x28` slots,
  all set to `DAT_10193aac` (`0x1000e7d0.c:37-50`, `0x1002d240.c`). `defined $x` is therefore **true** for a declared
  but never-assigned own variable (`gsNullVariant` `+0x70` returns 1; **observed** vtable `0x10158f4c`).
* Variables created at run time by member access `obj.$name` start **valueless** (`0x1001a650.c:83-98`), so
  `defined obj.$name` is **false** until assigned. **Corroborated**: `metro/scripts/init.script:10`
  (`if (!(defined $settings.$gameName))`) and `loading.script:172` (`if (!defined $script.$loadingPos)`).
* Function locals and missing arguments start valueless (`0x10028960.c:52-58`); new thread frames copy locals or start
  empty (`0x10028c60.c`). **Observed.**

### 1.4 Bare identifiers (link table)

A bare identifier compiles to `PUSH_LINK idx` (`0x100067c0.c:1097-1136`). Each distinct name gets one link record
holding its line of first use. The linker (`0x1000e7d0.c`, **observed**) resolves records in two steps:

1. **Functions defined in this script.** Every function-table entry fills its link record with a new
   `gsScriptFunctionVariant` bound to **this script object** (`0x1000e7d0.c:63-85`). Own functions therefore shadow
   natives of the same name, and functions are **hoisted**: a call above the definition shares the same link record.
2. **The global registry** `DAT_101939ec` (`std::map<string, variant>`; insert `0x1000f5f0.c`). It holds native functions,
   native constants and `MAIN` (`0x100a15c0.c:21-23`). The variant is captured (add-ref'd) **at link time**
   (`0x1000e7d0.c:99-155`).

* **Nothing else is searched**: not the loading script, not `MAIN`, not other scripts' functions, not members of `this`.
  **Corroborated**: both `addons/stationpack01/scripts/editor.script:7` and `game.script:10` define their own
  `selectMultilevelStation`, and cross-script calls are always written `MAIN.$ui.createChoiceButton(…)`.
* **Unresolved** → linker exception at the first unresolved record, in link-table (first-use) order. The message is
  *`Linker error in "<file>" at line <first use>: Undefined identifier "<name>"!`* (`0x1001b5c0.c`). The catch resets
  the code object (`Catch_1001a452` → `FUN_1001a1f0`). The **whole script is discarded and none of its code runs**,
  even if the bad name sits in a branch that is never executed. **Observed.**
* Duplicate function names in one script → compiler error *"Dublicate function definition "%ls"!"*
  (`0x100067c0.c:600-607`). A repeated parameter name → *"Dublicate variable definition"* (`0x100067c0.c:412` for event parameters, `:694` for function parameters).
  Function definitions inside a function → *"Reserved word "function" not allowed here!"* (`0x100067c0.c:559-566`).
  All are compile errors, so the whole script is discarded. **Observed.**

---

## 2. Objects

### 2.1 Script objects (`gsScript`)

* A script object holds one code object: variables (name map + storage), functions, event-handler records,
  link table, bytecode, and the list of event bindings it made on other objects. There is **no parent/child link
  between scripts** (`0x10017360.c`, `0x10019490.c`; **observed**). Parent/children exist only on `gsModule`
  (UI/scene tree, children at `+0x4C`).
* Every `load` creates a **new** script object and compiles the source again: there is no compile cache, and two loads
  of one file give two independent sets of variables (`0x10019490.c:25-41`, `0x1001a8f0.c:18-31`, **observed**).

### 2.2 Member access `obj.name` / `obj.$name` (GET_MEMBER, `+0x58`)

On a **gsScript or any subclass** (all `gsModule`s, i.e. controls, sounds, scenes, `MAIN`) — `0x1001a650.c`, **observed**:

| Name | Behaviour |
|---|---|
| starts with `$` | If the object is destroyed → error 14. Otherwise look the name (without `$`) up in the variable map. **If it is missing, create it now** with a valueless value. Return a reference to the slot, so reading **creates** the variable. |
| no `$` | 1. Native members of the class (fields, methods, events; binary search, case-sensitive; `0x1001eb50.c`, `0x1001bea0.c`). 2. A **script function of that object** (`FUN_10017190`, bound to that object). 3. Error 13 *"Undefined member name"*. Native members win over script functions of the same name. |

On a plain `gsObjectVariant` (threads, materials, textures, colour maps, display modes): only native members exist,
and `$name` → error 13 (`0x1001ef10.c`, **observed**). On `null`/valueless: error 13 (`0x1002cd40.c`, `0x1002e320.c`).

### 2.3 Assignment and creation

* `ref = v` calls `ref+0x44` (assign). For a slot reference this calls `container.setItem(idx, v)`. That dereferences
  `v`, calls the old value's `+0xC` "replace" hook (this is where watchers and `@` references act) and stores the result
  (`0x1002da30.c`, `0x1002d590.c`; **observed**).
* Script variables are created by mention (compile time) or by any member access `obj.$x` (run time). Assignment to an
  unknown own `$x` cannot happen: the compiler always allocates it.
* Fields of native objects bind to typed field variants; their setters and read-only flags are listed in
  `../native-api.md`.

### 2.4 `this`

`PUSH_THIS` pushes `frame[0]`, the script that owns the code currently executing (`0x1002ae90.c:491-496`):

| Context | `this` | Evidence |
|---|---|---|
| top-level code of a loaded script | the new script object | `0x10019490.c:51` |
| `init.script` top level | `MAIN` (the file is compiled **into** the `gsMain` object) | `0x100a1750.c:124` → `0x100a12f0.c:33-46` |
| script function | the script that **defines** the function (bound at link time), whatever object it was reached through | `0x1000e7d0.c:78`, `0x10028960.c:47-49` |
| event handler | the script containing the `event` statement, **not** the object that fired the event; that object is the context value (`current`, `.x`) | `0x1000e7d0.c:55-60`, `0x1001f960.c:47-49` |
| `thread` body | the creating frame's script | `0x1002ae90.c:408` |
| macro body | the script executing at `MAKE_MACRO` time | `0x10028b30.c`, `0x1002c660.c` |

### 2.5 `MAIN`

`MAIN` is a global (registry entry) that points at the root `gsMain` object (a `gsModule`). It is registered by
`gsMain`'s vtable slot 41 at start-up, which also installs the error and output sinks (`0x100a15c0.c:18-25`). The `-i`
start-up file (`metro/scripts/init.script`) is compiled into that object, so in `init.script` `$x` and `MAIN.$x` are the
**same storage**. The scripts' redundant `MAIN.$settings = $settings;` is harmless. On shutdown `MAIN` is reset to
`null` (`0x100a16a0.c`). **Observed.**

---

## 3. Calls

`CALL argc` invokes `f+0x48(argc, &args, thread)`, where `f` is the value below the arguments. After any call the
interpreter re-reads the code pointer. If `frame[0]`'s code object is gone (script deleted), the thread **finishes
silently** (`0x1002ae90.c:277-292`, **observed**).

| Callee | Behaviour | Evidence |
|---|---|---|
| **Script function** (`gsScriptFunctionVariant`: `+8` owner, `+0xC` pc, `+0x10` min, `+0x14` max, `+0x18` nLocals) | `argc` must lie in `[nParams − nDefaults, nParams]`, else error 11 *"Wrong number of arguments (%d)!"*. **Each argument is dereferenced** (`+0x10`), which passes by value; an `@` reference arrives as a plain slot reference, which gives by-reference parameters. Then an in-place frame switch: a `gsThreadFrame` with the caller's base, count, script and pc is pushed, `frame[6] = depth − argc`, `frame[7] = nLocals`, `frame[0] = owner`, `frame[1] = pc`, and `nLocals − argc` valueless slots are pushed. **No C recursion**, so a `wait` inside a callee suspends the whole call chain. | `0x1002e000.c`, `0x10028960.c` |
| **Default arguments** | `DEFAULT_ARG idx` skips the default if the slot is not *the valueless singleton*. Passing an explicitly valueless value (e.g. an unset member) **also triggers the default**. The default expression runs in the callee, at call time, and can use earlier parameters. | `0x1002ae90.c:738-752`, `0x100067c0.c:651-664` |
| **Native global** (`gsTransparentFunction`/`gsOpaqueFunction`) | Range check `[min, max]` → error 11. `result = handler(argc, args)`; the arguments are passed **raw**: plain `$x` arrives as a slot reference and the handler converts it through the reference. The handler's own choice decides whether it snapshots a value (`+0x14`; `@` references stay live, macros stay macros). Then pop `argc + 1`, push the result. **The two classes differ only in RTTI name and destructor**: same vtable slots, same constructor shape, no type test anywhere. | `0x1002e110.c`, `0x10028900.c`, `0x1002e050.c` vs `0x1002e170.c`, `0x100a5e40.c` (setArg uses `+0x14`) |
| **Native method** (`gsMethodVariant`, bound to the object at GET_MEMBER time) | Object destroyed → error 14 *"Trying to use destroyed object!"*; range check; `handler(obj + offset, argc, args)`. | `0x1001d4d0.c` |
| **Macro** | `argc` must be 0 (else error 11); the macro is evaluated and the result pushed. | `0x1002c8c0.c` |
| anything else | error 4 *"Cannot call non-function variant!"* | `0x1002ccb0.c` |

* `RETURN` pops the return value (dereferenced), pops the saved frame, restores base/count/script/pc, pops
  `nLocals + 1` (parameters, locals and the callee value) and pushes the value. If the restored script has no code
  any more, the thread ends (`0x1002ae90.c:448-474`, `0x1002b324.c`). **Observed.**
* `return;` pushes valueless. `return` is legal **only inside functions and event handlers**; at top level, in a top-level
  `thread` body or in a macro it is the compiler error *"Reserved word "return" not allowed here!"*. In an **event
  handler** `return` compiles to `END` and must have **no expression** (`0x100067c0.c:907-945`, **observed**).
  `vm-opcodes.md` lists "top-level `return` → END"; that is really the event-handler case.
* `obj.f()` on a script object calls that object's own function (owner = `obj`, so `this == obj`). A function value stored
  elsewhere (`.$action = f; .$action()`) keeps `this` = its defining script. **Observed** (§2.2, §2.4).
* The context value (`with`/`foreach`, see §8) is **not reset on call**. A function called inside `with (x)` sees `x` as
  `current`/`.member` (`0x10028960.c` never touches `frame[8]`; **observed** code, **inferred** consequence).
* **No recursion limit.** The operand stack doubles without bound (`0x100287a0.c`). Error 21 *"Stack size exceeded!"*
  has a message but **no throw site**: none of the 67 call sites of the exception constructor `0x10029320` or the
  formatter `0x1001b6e0` passes 0x15, and the string `0x10157578` is referenced only by the formatter (byte scan).
  Runaway recursion exhausts memory. **Observed.**

### Call pseudocode

```
CALL argc:
  f = stack[depth-1-argc]
  f.call(argc, &stack[depth-argc], thread)          // vtable +0x48
  if thread.frame and thread.frame.script.code == null: setState(thread, FINISHED)

ScriptFunction.call(argc, args, thread):
  if argc < minArgs or argc > maxArgs: throw E11(argc)
  for a in args: a = a.deref()                        // +0x10
  fr = thread.frame
  saved = gsThreadFrame(fr.localsBase, fr.localsCount, fr.script, fr.pc)
  fr.localsBase = depth - argc;  fr.localsCount = nLocals
  fr.script = owner;  fr.pc = entryPc
  repeat nLocals - argc: push(VALUELESS)
  push(saved)                                         // the interpreter loop just continues

RETURN:
  rv = pop().deref();  saved = pop()
  fr.script, fr.pc, fr.localsBase, fr.localsCount = saved.…
  pop(nLocalsOfCallee + 1);  push(rv)
  if fr.script.code == null: setState(thread, FINISHED)

Native.call(argc, args, thread):
  if argc not in [min,max]: throw E11(argc)
  r = handler(argc, args)        // raw args
  pop(argc + 1); push(r)
```

---

## 4. Events

### 4.1 Binding (`BIND_EVENT idx` / `BIND_EVENT_F idx`)

* Syntax `event <obj>.<eventMember>::[<filter>](<params>) <body>`. The target expression must yield a
  `gsEventVariant` (a member of kind *event*). Binding to anything else → error 12 *"Cannot set event to variant of
  this type!"* (`0x1002cd10.c`). The handler body is skipped with `JMP` and ends with `END`
  (`0x100067c0.c:319-414`). **Observed.**
* **Binding happens when the statement executes** (it is not hoisted), with handler record `code+0x38 + idx*16`
  (`0x1002ae90.c:482-508`).
* Target destroyed → error 14 (`0x10022250.c:56`).
* **Unfiltered event** (descriptor flag 0): the per-object, per-event container is a `std::set` keyed by **handler
  record pointer**. Re-executing the **same** `event` statement on the same object is a no-op. Different statements
  **accumulate**, so binding never replaces an earlier handler (`0x10021db0.c:28-60`). **Observed.**
* **Filtered event** (descriptor flag 1): a `std::multimap<std::string, record>`. The key is `filter.toString()`
  narrowed to bytes (low byte of each UTF-16 unit, `0x1002eeb0.c`). The same record under the same key → no-op;
  otherwise inserted (`0x10021fb0.c:62-80`). **Observed.**
* A mismatch (filter on an unfiltered event, or none on a filtered one) is **silently ignored**: the insert function
  returns false and nothing is bound (`0x10021db0.c:28`, `0x10021fb0.c:59-61`). **Observed.**
* Each successful bind is also recorded in the binding script's code object (`FUN_1001a4a0`, `0x10022250.c:49`).
  Deleting or resetting that script unbinds all its handlers (`0x1001a1f0.c:13-19` → `FUN_10022170`). Destroying the
  target object drops all handlers bound on it (`0x10022350.c`). **Observed.**
* Handler scope: the compiler **clears the local map** for the handler body and restores the enclosing function's map
  afterwards, so a handler defined inside a function **cannot see that function's locals** (`0x100067c0.c:343-345`,
  `370-372`). Parameters become locals 0…n−1; `$x` follows §1.1 function-mode rules. Event parameters cannot have
  defaults (the parser only accepts `$name` separated by `,`). **Observed.**

### 4.2 Firing (native side)

`FUN_1001f960(obj, eventId, argc, args)` for unfiltered events, `FUN_100201c0(obj, eventId, key, argc, args)` for
filtered ones (exact `std::string` match of the key, `equal_range`). Wrappers: `FUN_1001faf0`/`FUN_10020360` (one vararg).
**Observed:**

1. Add-ref the object. Iterate the matching handlers: unfiltered in **ascending record-address order** (source order
   within one script, heap order across scripts); filtered in key order and then insertion order.
2. For each handler:
   * create a new `gsThread` in **group 0** (the default group), whatever the binding script's group;
   * `n = min(argc, nParams)`: the first `n` arguments are copied (raw, add-ref'd) into locals 0…n−1 and the rest of
     the locals are valueless, so **extra arguments are dropped and missing ones are valueless**;
   * **push the event object as the context value**, giving `current` and `.member` in the body;
   * **run it immediately and synchronously** (nested interpreter) until it ends or yields. On `wait`/`skip_frame` it
     stays in group 0 and continues later. (`0x1001f960.c:38-51`, `0x100201c0.c:42-62`)
3. The loop stops early if the object's handler table disappears (object destroyed by a handler). The filtered loop
   advances its iterator before running, so self-unbinding is safe there.
4. Release the arguments (the caller hands over ownership) and the object. If this was the last reference, run the
   destroy sequence (§8.4).
5. **Return value**: true if at least one handler existed. Handler results are discarded; an event handler cannot
   return a value. Callers use the flag, e.g. the error sink shows a MessageBox only when no `onErrorMsg` handler
   exists (`0x100a1400.c`).

Built-in filtered example: key events fire with a key-name string from the table at `DAT_101903b8` and no arguments
(`0x10082900.c`, `0x100a6d20.c`), which is how `keyDown::["Enter"]` matches. State machines fire `enter`/`exit` with
the state name (`main.states`, `metro/scripts/main.script:831`). Event id 0 of every object is `onDestroy`.

---

## 5. Threads

### 5.1 Creation

| Construct | Group | Starts | Locals | Evidence |
|---|---|---|---|---|
| `thread :: stmt` (0x17) | **always group 0**, not the creator's group | **deferred**: state 3 with wake 0, runs at the next scheduler pass of group 0 | copy of the creating frame's locals `[frame[6], frame[6]+frame[7])`: pointers add-ref'd into **separate slots**, so later assignments in either thread are not shared, but arrays and maps are | `0x1002ae90.c:396-416`, `0x1002ac10.c`, `0x10028c60.c` |
| `thread <g> :: stmt` (0x2D) | group named `g.toString()`, created if missing | deferred | same | `0x1002ae90.c:675-703` |
| `load` top level | group 0, or the named group for `load g :: path` | **immediate** (nested interpreter) | none | `0x10019490.c:46-53` |
| event handler | group 0 | **immediate** | parameters | §4.2 |
| macro evaluation | group 0 | **immediate** | captured values | `0x1002c660.c` |

A new frame's context value is valueless (`0x10028c60.c`), so `current`/`.x` at the start of a `thread` body does
**not** see an enclosing `with`/`foreach` (**observed** code; consequence **inferred**). The thread body compiles in
the enclosing mode, so inside a function it can use the function's locals by name (the copies). The value pushed by
`thread` is the `gsThread` object.

### 5.2 Thread groups

* Group 0 is created at VM init and is **unnamed** (`0x1002c9c0.c`). Named groups are created on first mention by
  `thread g ::`, `load g ::`, `pause`, `resume` or `setRunSpeed` (`FUN_1002ada0(name, create=1)`). They are appended in
  creation order.
* **A newly created named group starts with pause count 1, i.e. paused** (`0x1002ada0.c:47`). **Corroborated**: `game`
  and `gameUI` are first resumed when entering the Game state (`metro/scripts/main.script:822-823`), and `loading.script`
  runs `load "loading" :: x; resume "loading"; …; pause "loading"` (lines 169-179).
* `pause g` increments and `resume g` decrements the count, with **no clamping**. The scheduler runs a group only when
  the count is **exactly 0**, so an extra `resume` (count −1) keeps it stopped (`0x1002ae90.c:704-737`,
  `0x1002c5d0.c:31`). **Observed.** `pause` or `resume` on a new name first creates it with count 1.
* `setRunSpeed(g, s)` sets the group's speed (`0x1000c320.c` → `0x1002c5a0.c`). Group lookup is by the name string.

### 5.3 Scheduler (`runThreads(dt)` = `FUN_1002c5d0`)

It is called once per frame from `gsMain.tick` before the module tree ticks (`0x100a0700.c:17`). The main loop clamps
`dt` to `[0.0001, 0.05]` s (`../simulation/README.md`).

```
runThreads(dt):
  g_dt = dt                                   // the value of `skip_frame`
  if interpreterDepth != 0: return true       // re-entrant call: do nothing
  pending = false
  for gi = 0 .. groups.count-1:               // group 0 first, then creation order
    g = groups[gi]
    if g.pauseCount != 0: continue
    g.time += dt * g.speed                    // x87: fld dt; fmul speed(float); fadd time(double); fstp
    while g.sleepers not empty:
      t = g.sleepers.first                    // smallest wake; FIFO among equal wakes
      if t.wake > g.time: pending = true; break
      interpret(t)                            // removes t from the set while it runs
  return pending

interpret(t, macroResult = null):            // FUN_1002ae90
  if t.frame == null or t.frame.script.code == null: setState(t, FINISHED); return
  addref t; interpreterDepth++; setState(t, RUNNING)
  try   { while t.state == RUNNING: dispatch(code[pc++]) }   // 0xFF → FINISHED
  catch (ScriptError e) { report(e, t); setState(t, FINISHED) }
  catch (...)           { report(E19 "Internal error!"); rethrow }
  interpreterDepth--; release t
```

Evidence: `0x1002c5d0.c:18-45`, bytes@0x1002c5d0 (`d9 44 24 10 … d8 4e 08 dc 06 dd 1e`, so time is a **double** and
the Ghidra float casts are misleading), wake comparison `dd 41 20 dc 1e df e0 f6 c4 41 7a` (runs when
`wake <= time`), `0x1002ae90.c:146-179`, `Catch_1002c3ab`, `Catch_1002c43e`. **Observed.**

* Ordering: the sleeping set is a multiset keyed by wake time with **upper-bound insertion**, so ties are FIFO
  (`0x10028ff0.c`). New and woken threads have wake 0 and run before every timed sleeper of their group. A thread
  created or woken in group *k* while group *k* is being processed runs **in the same pass**. If group *k* was
  already processed this frame, it runs next frame (**inferred** from the loop structure).
* Group time advances whenever the group is unpaused, even with no threads in it.

### 5.4 Time and yielding

* `wait s`: `wake = groups[t.group].time + s` (`s` via `+0x24` toFloat), state 3 (`0x1002ae90.c:383-395`). The time
  base is the thread's **own group clock**, so it is scaled by group speed and frozen while paused. `wait 0` (or a
  negative value) makes the thread due again **in the same pass**; a loop of `wait 0` hangs the frame (**inferred**).
* `skip_frame`: `wake = group.time + 1e-5`, state 3, then `PUSH_DT` pushes `g_dt` **when the thread resumes**. The value is
  the raw frame `dt`, **not** scaled by group speed (`0x1002ae90.c:658-674`, **observed**).
* `wait_until c` / `wait_while c`: the condition is compiled once, and the compiler collects the **script variables and
  locals that appear literally** in it (`0x100067c0.c:1010-1060`). Evaluation: if the condition is already satisfied,
  continue. Otherwise `WAIT_WATCH` registers the thread on each collected slot and blocks (state 4); waking jumps back to
  re-evaluate the condition (`0x1002ae90.c:417-437`).
  * The watcher is a `gsTriggerVariant` **spliced into the variable's slot**, wrapping the old value
    (`0x1002db40.c`, `0x1002e620.c`). Any **store into that slot** (`setItem`) calls the trigger's `+0xC`, which
    forwards the store, unwraps itself and wakes the thread (`0x1002dc90.c` → `0x1002a940.c`: wake = 0, state 3). This
    includes any assignment to `$x`, the same `$x` written from another script via `obj.$x = …`, `setVariable` and
    `mergeVariables`, **even if the value is unchanged**. **Observed.**
  * In-place mutation does **not** wake: `$a[] = v`, `$a[i] = v`, map edits, native field changes, members used in the
    condition (`.x`, `obj.y`). A condition without any `$var` blocks forever (**inferred**).
  * When the thread leaves state 4, **all** its watchers are removed (`0x1002a800.c:58-59`, `0x10028d80.c`). It is
    re-armed if the condition is still false.
  * **Corroborated**: `metro/ui/ui.script:713` `wait_until $dialogResult != 0;`, with `$dialogResult` declared at
    top level (line 11) and set by event handlers and macros (lines 642-708).

### 5.5 Thread object, finishing, deletion

* States change only through `setState` (`0x1002a800.c`). FINISHED is terminal. Entering FINISHED frees the frame
  (stack, watchers) and runs the object's destroy sequence. **A finished thread fires its `onDestroy` event, and its
  `destroyed` field becomes true** (`0x1002a800.c:22-29` → `0x1001c2e0.c`, **observed**). Entering SLEEPING inserts
  into the group set and add-refs; leaving it removes and releases.
* The thread object supports only `gsObjectVariant` members: `destroyed`, `hasMember`, `listMembers`, `className`,
  `memberInfo`, `getMember`, `free()`, `onDestroy`. `delete $t` or `$t.free()` → class destroy hook
  `0x1002aca0.c`: state FINISHED, which stops it at once, removes it from its group and drops its watchers. Deleting the
  running thread from inside itself is safe: after `DELETE` the frame is null, so the loop exits
  (`0x1002ae90.c:580-592`). **Observed.** **Corroborated** usage: `$skipThread = thread "gameUI" :: loop {…}` …
  `delete $skipThread;` (`metro/scripts/game.script:493-551`).
* `exit` emits `CTX_POP` × (open `with`/`foreach`), then `END`. **`END` finishes the whole thread**, even inside a
  called function: the caller frames are discarded (`0x100067c0.c:416-425`, `0x1002ae90.c:178-179`). In an event
  handler it ends that handler's thread.
* **Threads of a deleted script** are not killed eagerly. A sleeping thread dies the next time the scheduler picks it,
  because `frame[0].code == null` at interpreter entry. A thread that is inside another script's function dies when it
  `RETURN`s into the deleted script, or after its next `CALL` while `frame[0]` is the deleted script
  (`0x1002ae90.c:148-152`, `283-291`, `0x1002b324.c`; **observed**). A thread blocked on the deleted script's
  variables is never woken; releasing the storage releases the triggers that held it (**inferred**).

---

## 6. `load` and `load g :: path`

`LOAD`: `path = pop().toString()`, then `FUN_10019490(path, frame[0], group 0)`. `LOAD_IN` does the same with the named
group. **Observed** (`0x1002ae90.c:612-621`, `879-896`; `0x10019490.c`):

1. Allocate a new `gsScript`. Set the "kept by global script list" flag, give it a serial number, insert it into the
   global list `DAT_101939fc` (which holds a reference) and set `scriptFilename = path`.
2. Run the compile callback with **the calling script as base** (`PTR_FUN_1018ef14`, the engine version
   `0x10095340.c`):
   * Path resolution `FUN_1007de40(path, dir(callerFile))`. `dir()` keeps everything up to and including the last
     `/` or `\` (`0x1007ce30.c`). Then:
     * empty → empty;
     * **leading `/` → strip it**, i.e. root of the virtual file system;
     * **leading `~/` → strip it**, also root;
     * otherwise `base + path`: a `/` is added if `base` lacks a trailing separator, and a leading `./` is stripped;
       `..` is not normalised here.
     * `scriptFilename` is replaced by the resolved path. The base is the directory of the script **whose code is
       executing** (`frame[0]`): for a `load` inside a function, that is the function's owner script.
     * **Corroborated**: `metro/scripts/init.script:35` `load "window.script"` → `metro/scripts/window.script`.
   * Read through the VFS with `mustExist = 1` (`6A 01` = `push 1` at bytes@0x1009548b). A missing file reports
     *`File "<resolved>" not found!`* through the error sink (`0x100809e0.c:143-148`, `191-197` → `0x100a0320.c`).
   * Compile (`+0x88` = `0x1001a8f0.c`): a fresh code object, then parse → compile → link. Each stage's error is
     logged and **resets the code object**, so the script ends up empty (§9.2).
3. If compilation succeeded: create a thread in the given group with pc 0 and **run its top-level code immediately and
   synchronously** until it finishes or first yields; it then continues under the scheduler in that group, even if the
   group is paused at the time of the load. Release the thread.
4. **Return the script object in every case.** After a missing file or a parse/compile/link error it is an empty script
   with no functions or variables. The caller is not told; scripts detect failure with `defined $s.$knownVar`
   (`init.script:10`). **Observed**; **corroborated**.

* `load` is an expression. The whole following expression is the path (`load "/a/" + x + ".strings"`).
* The loaded script stays alive through the global list until it is explicitly `delete`d, whatever its reference count
  (`0x10019490.c:36-40`, `0x1001ac50.c:15-18`). **Observed.** Scripts keep handles to delete them later
  (`MAIN.$gameScripts[] = $script = load …`, `loading.script:169`).
* Start-up: the `-i <file>` argument compiles the file **into `MAIN`** and runs its top level in a group-0 thread
  (`0x100a1750.c:124`, `0x100a12f0.c`). `-s <source>` compiles a source string into `MAIN` (`0x1009fb50.c`).

---

## 7. Macros

* Syntax `macro :: expr`, `macro (a1, …, an) :: expr`, or `macro a1 :: expr` (one capture). There are at most 255
  captures (`0x100067c0.c:829-880`). The captures are evaluated **at creation** and stored with `+0x14`: plain
  values and plain `$var` references become **snapshots of the current value**, `@` references stay **live**, and
  `current` becomes the current element value (`0x10028b30.c`, `0x1002d930.c`, `0x1002df30.c`). **Observed.**
* The body is compiled with function mode **off** and macro mode on (`0x100067c0.c:869-874`). So `$name` in a macro
  always means the script variable, even when the macro is written inside a function. Function locals must be passed in
  as captures and read as `$0…$n`. **Corroborated**: `macro($i) :: selectStop($0)` (`metro/ui/linepanel/linepanel.script:936`);
  `macro(@$slider.$value, $data) :: … $0 … $1[0]`.
* The macro records `frame[0]` (its `this`), its entry pc and the captures. `MAKE_MACRO` pushes a `gsMacro`
  (`0x1002ae90.c:631-657`).
* **Evaluation** (`FUN_1002c660`): a new group-0 thread whose locals are the captures, run synchronously; `HALT_VALUE`
  stores `pop().deref()` and finishes the thread (`0x1002ae90.c:622-630`). The result is valueless if the body did not
  reach `HALT_VALUE` synchronously, e.g. because it yielded inside a called function; that thread continues later and
  its result is discarded.
* **Evaluation is triggered by:**
  * a call with **zero** arguments, `$m()`; other counts give error 11 (`0x1002c8c0.c`). **Corroborated**:
    `.$action();` in `metro/ui/ui.script` `createChoiceButton`;
  * **every conversion**: `+0x1C` toBool, `+0x20` toInt, `+0x24` toFloat, `+0x28` toString, `+0x2C` toObject each
    re-evaluate the body (`0x1002c700.c`, `0x1002c790.c`, `0x1002c890.c`). A native that keeps a macro (e.g.
    `setArg(i, macro :: …)` stores it with `+0x14`, which is the macro itself) therefore gets a "live" value each
    time it converts it. **Observed.**
* Dereferencing (`+0x10`) a macro returns the macro itself. Passing it to a script function passes the macro.

---

## 8. `with`, `foreach`, `current`, `delete`, `echo`

### 8.1 Context stack

`with (x)` pushes `x+0x14`, i.e. the **value**: `with ($b = new_control(…))` targets the object, not the variable
(`0x1002ae90.c:509-515`). The previous context is saved on the operand stack; `CTX_POP` restores it (`0x10028a90.c`,
`0x10028ae0.c`). A leading `.member` and `current` both read the top (`PUSH_CTX`, `0x1002ae90.c:519-528`). `return`
and `exit` emit the pops for the open blocks of the current function. **Observed.**

### 8.2 `foreach (coll)` / `foreach_reverse (coll)`

* `coll = pop().deref()`. The **iterator is a `gsIndexVariant(coll, i)`** pushed as the context value; it starts at
  `i = 0`, or at `i = −1` for reverse, where negative indices count from the end. The loop body is skipped when
  `count == 0`. `FOREACH_NEXT` does `i++` and loops while `i < coll.count()`; `count` is **re-read on every step**, so
  appending inside the loop extends it. Reverse does `i--` and loops while `i >= −count` (the test is
  `-i == count || -count < i`, `0x1002ae90.c:788`; corrected by the coordinator, an earlier version said `i > −count`)
  (`0x1002ae90.c:529-555`, `760-796`). **Observed.**
* **`current` is the iterator itself**, a reference to `coll[i]`: `current = v` writes the element. Storing it in a
  variable or passing it to a script function dereferences it to the element value. A native that keeps the raw
  argument keeps the moving iterator (**inferred**).
* Iterable means anything implementing `+0x38` count and `+0x3C` getItem: arrays, and **`gsModule` objects, which
  iterate their children** (`0x100a0160.c`, `0x100a05b0.c`). Maps report their size but `getItem` raises error 8
  *"Variant not indexable!"* on the first `current` read, so scripts write `foreach (keys $m)` / `foreach (values $m)`
  (29 and 10 shipped uses). Out-of-range `current` → error 6. **Observed** (vtables); **corroborated**.

### 8.3 `delete x`

* `obj = x.toObject()` (`+0x2C`). If not null → destroy sequence (`FUN_1001c2e0`). Then, if the current frame is gone or
  its script has no code (e.g. `delete this`), the thread ends at once (`0x1002ae90.c:580-592`). **Observed.**
* `delete null` is a no-op (`gsNullVariant` `+0x2C` returns 0). `delete` on a number, string, valueless, function or
  macro → error 5 *"Cannot read valueless variant!"*, the generic "conversion not supported" error (`0x1002cc20.c`).
  **Observed.**
* `delete $x` / `delete current` / `delete $a[i]` destroy the **object** the expression refers to. The variable or array
  element still holds the (now destroyed) object; scripts null it themselves (`delete $popupMenu; $popupMenu = null;`,
  `metro/ui/ui.script`). Removing array or map entries uses the natives `array_erase` and `map_erase`.
* `obj.free()` is the same as `delete obj` (`0x1001d630.c`).

### 8.4 Destroy sequence (`FUN_1001c2e0`, also the last-release path in `0x1001f960.c:64-77`)

If not yet destroyed: add-ref; flags = 2; **fire `onDestroy` (event 0) with no arguments**, so handlers run
synchronously as in §4.2; call the class hook `+0x74`; set the destroyed bit; release. The memory is freed only when
the reference count reaches 0. Afterwards member access raises error 14 for `$`-members, methods and event binding.
Class hooks (**observed**):

* `gsScript` (`0x1001ac50.c`): remove from the global script list; drop handlers bound **on** it; free the code
  object, which unbinds every handler the script bound elsewhere and releases its variables, functions and links.
* `gsModule` (`0x1009fff0.c`): additionally destroys its **children in reverse order** (`0x100a0a30.c`) and detaches
  from its parent.
* `gsThread` (`0x1002aca0.c`): FINISHED.

### 8.5 `echo x`

`x.toString()` goes to the output sink `PTR_FUN_1018efac` (`0x1002ae90.c:556-569`). Before `MAIN` exists the sink is a
MessageBox titled "Script output" (`0x1001b050.c`). After start-up it is `0x100a14e0.c`: `wprintf("PORT: infoMsg %ls\n")`,
then fire `MAIN.onInfoMsg($message)` (event 6). If no handler is bound, the MessageBox is shown. `scripts/debug/log.script`
binds both sinks to append to `userdata/log_metro.txt`. **Observed.**

---

## 9. Errors

### 9.1 Runtime errors

* Every runtime error is a C++ exception (`FUN_10029320(code, a, b, c)`, type `DAT_10178994`). It is caught by the
  interpreter running **that thread** (`Catch_1002c3ab`), which formats it with `FUN_1001b6e0` and then **finishes
  only that thread**. Other threads, the script object and its variables, functions and handlers are unaffected.
  Errors in a nested interpreter (event handler, macro, `load`ed top level) kill only the nested thread; the outer
  native call and the outer thread continue. A macro that dies yields valueless. **Observed.**
* Message: `Runtime error in "<scriptFilename of frame[0]>" at line <line of pc−1>: <text>`, or without
  `at line` if no line is known. Then one line per saved caller frame on the operand stack, innermost first:
  `Called from "<file>" at line <n>` (`0x1001b6e0.c:36-47`, `151-180`). **Each line is a separate call to the error sink.**
* Other C++ exceptions: *"Internal error!"* (code 19) is logged and the exception **rethrown** out of the interpreter
  (`Catch_1002c43e`). Where it is finally caught was not traced.
* `assert c` with false `c` logs *"Assert failed!"* with the traceback and **continues** (`0x1002ae90.c:839-857`).
  **Observed.**

| Code | Text | Typical source |
|---|---|---|
| 0 | `Invalid command (%d)!` | opcode > 0x3B |
| 1 | `Division by zero!` | operators |
| 2 | `Left side cannot be assigned!` | assigning to a non-reference (`0x1002cc80.c`) |
| 3 | `Operator %d not defined for operands!` | operator table miss |
| 4 | `Cannot call non-function variant!` | CALL on a non-callable |
| 5 | `Cannot read valueless variant!` | **any unsupported conversion** (base `+0x1C…+0x30`) |
| 6 | `Array index out of bounds (%d)!` | arrays, module children |
| 7 | `Map index out of bounds (%ls)!` | maps |
| 8 | `Variant not indexable!` | `[]`/`getItem` on a non-container |
| 9 | `Unmatched array assignment (%d != %d)!` | array-to-array assign |
| 10 | `Variant cannot be referenced!` | `@` on a non-reference |
| 11 | `Wrong number of arguments (%d)!` | all call paths |
| 12 | `Cannot set event to variant of this type!` | `event` on a non-event |
| 13 | `Undefined member name "%ls"!` | GET_MEMBER miss |
| 14 | `Trying to use destroyed object!` | members, methods, events of destroyed objects |
| 15 | `Calling a function with invalid argument(s)!` | natives |
| 16 | `Initializing map with invalid number of arguments (%d)!` | `map [...]` |
| 17 | `Variant is not a map!` | map natives |
| 18 | *(custom text)* | natives (`FUN_10029760`, e.g. `Cannot open file "…"`) |
| 19 | `Internal error!` | foreign C++ exception |
| 20 | `Assert failed!` | `assert` (logged directly, not thrown) |
| 21 | `Stack size exceeded!` | **never raised** (§3) |

### 9.2 Load-time errors

Each is logged once and leaves the script empty (code object reset). **Observed.**

* `Parser error in "<file>" at line <n>: <Invalid character "%c"! | Unterminated string! | Unterminated comment! | String literal too long (%d > 255)! | Bad character literal!>` (`0x1001b0d0.c`, `Catch_1001a3ab`).
* `Compiler error in "<file>" at line <n>: <message>`, with the offending token (up to 31 chars) in the message. 14
  messages, including the misspelled *"Dublicate function definition"* (`0x1001b2c0.c`, `Catch_1001afda`).
* `Linker error in "<file>" at line <n>: Undefined identifier "<name>"!` — first failure only
  (`0x1001b5c0.c`, `Catch_1001a452`).
* `File "<path>" not found!` (§6).

### 9.3 The sink

`PTR_FUN_1018efa8` (`setErrorProc`, `0x1001b070.c`). Default: MessageBox "Error". Once `MAIN` exists it is
`0x100a1400.c`: `wprintf("PORT: errorMsg %ls\n")`, then fire `MAIN.onErrorMsg($message)` (event 7); if no handler is
bound, the MessageBox is shown. `log.script` prefixes `Error: ` and appends to the log; the debug console mirrors it
(`scripts/console/console.script:53`). **Observed.**

---

## 10. Save-related methods (runtime effects only)

The file formats are in [`../handlers/batch-C.md`](../handlers/batch-C.md).

* `getVariable(name)` returns the current value of an own variable, not a reference, or valueless. It does **not**
  create the variable, unlike `obj.$name` (`0x10018260.c`).
* `setVariable(name, v)` and `mergeVariables(other)` store via `setItem`, creating names when needed. This **wakes
  watchers** on existing variables. `mergeVariables` shares the variant objects, so arrays and maps become aliased
  between the two scripts (`0x1001a9e0.c`, `0x1001aba0.c`).
* `exportVariables` / `exportVariablesTo` only read values (through triggers transparently) and write text files or
  `memory/` files. Objects export as `null /* cannot export object */`. No runtime state changes.
* `getFunction(name)` returns the function variant bound to that script (`this` = that script when called).

---

## 11. Rules a reimplementer is most likely to get wrong

1. `$x` inside functions and handlers depends on **source order** (§1.1): `0x10006440.c:73-109`; `loading.script:4,154-168`.
2. Bare names see **only own functions and native globals**; one unresolved name discards the whole script at link time (§1.4): `0x1000e7d0.c:99-153`.
3. Event handlers: `this` = defining script, the target is `current`; they run **immediately** in **group 0**;
   handlers **accumulate**, and re-binding the same statement is a no-op (§4): `0x1001f960.c:38-51`, `0x10021db0.c:57-60`.
4. New named groups start **paused** (count 1); `thread ::` without a name always goes to group 0; pause counts are not
   clamped (§5.2): `0x1002ada0.c:47`, `0x1002ae90.c:397-403`, `0x1002c5d0.c:31`.
5. Own variables start `null` (defined), unknown `obj.$x` start valueless (undefined) and reading them creates them
   (§1.3): `0x1000e7d0.c:50`, `0x1001a650.c:83-98`.
6. Errors kill only the current thread; `assert` continues; there is no stack limit; a failed `load` still returns an
   (empty) script (§9, §6): `Catch_1002c3ab`, `0x1002ae90.c:839-857`, `0x10019490.c:43-55`.

Also easy to miss: `exit` ends the whole thread even inside a function; `return expr` is illegal in event handlers;
`skip_frame` yields the raw `dt`; calling a function of a deleted script silently ends the caller's thread; `wait_until`
only watches `$vars` written literally in its condition, and any store to them wakes it.

---

## 12. Open questions

1. **Top-level `local`**: the top-level frame reserves no locals, so `PUSH_LOCAL n` addresses operand-stack slot `n`.
   Behaviour is undefined; no shipped script does it. Treat it as a compile error, or reserve slots?
2. **`return` inside a `thread` body inside a function** compiles to `RETURN` with no saved frame on the stack. It
   probably corrupts the thread or crashes; the code path was not traced. No shipped script found.
3. **`wait_until` with no watchable `$var`**: the thread blocks with no trigger holding it. It is probably freed when the
   last reference drops, or leaks if stored. The thread destructor `0x10028e60` was not read.
4. Order of **unfiltered handlers across scripts** is heap-address order of the handler records. A port needs a
   deterministic substitute; script load order is the natural choice.
5. Whether the VFS normalises `..` in resolved `load` paths (`FUN_1007de40` does not).
6. Whether compound assignment and `++`/`--` store through the slot (`+0x44`/`setItem`, which wakes watchers) or replace
   it otherwise: owned by the operator analysis (`operators.json`).
7. Purpose of the global script list's serial number (`+0x38`) and whether any native iterates `DAT_101939fc`.
8. Where rethrown non-script C++ exceptions (code 19) are finally caught (`gsMainLoop` installs `0x10082ad0` via
   `FUN_10122114`).
9. The `PORT: errorMsg/infoMsg` `wprintf` strings are present in the shipped DLL; whether stdout goes anywhere in the
   retail build was not checked.
