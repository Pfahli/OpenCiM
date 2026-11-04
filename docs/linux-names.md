# Linux symbol names on GSystem.dll

The Linux build (`Cities In Motion.bin`, REPORT §1.1) has full C++ symbols. It was compiled from the same source as
`GSystem.dll` (game version 1.0.22), so most Windows functions have a named Linux twin.
`tools/match-builds.mjs` pairs them up, and `tools/ghidra/ApplyNames.java` writes the names into the Windows
Ghidra project. With the names applied, the Windows pseudocode in `decompiled/GSystem-v2/functions/` shows
`gsClock::move` and `gsStateMachine::enterState` instead of `FUN_10073f80`.

## Result

* **Matched:** 1 996 functions, written to `decompiled/GSystem-v2/linux-names.csv` as `windows,linux,name,method`.
* **Coverage:**
  * 58 % of the 3 177 engine functions in the Linux build (named `gs…` or `Class::…`, at least 4 instructions,
    standard library excluded);
  * 64 % of those with at least 30 instructions.
* **Validation:** the CSV was checked against 21 Windows functions that were identified by hand earlier in this
  project, for example the state machine methods, `gsClock::move`, `gsGrid::move`, `mkdate`, `routeExists` and
  `updateFiles`:
  * 16 are matched to the right name;
  * none is matched to a wrong name;
  * 5 are unmatched. These are the interpreter and two compiler functions, which Ghidra splits differently on
    Windows; `gsObjectVariant::event`, which has three overloads; and `gsScript::load`.
* **Plausibility check:** across all pairs, no pair from the propagation methods (`calls`, `callers`, `class`,
  `calleeset`) differs more than 4× in size. Two pairs whose functions both reference strings share none of them.
  The large size gaps are in vtable pairs and come from different inlining. For example, GCC keeps
  `gsMesh_OGL::render` as a 14-instruction wrapper around a private `render`, which MSVC inlines into 785
  instructions.

| Method | Pairs | How a pair is found |
|---|---|---|
| `export` | 33 | PE export name equals the Linux function name (`gsInitialize`, `runThreads`, …) |
| `native` | 390 | the native API handler of `Class.member` / global `name` (`docs/native-api.json`) is Linux `Class::method_<member>` / `gsFunction_<name>`; a class registrar is `Class::initializeClassData`. Only handlers that serve a single name on Windows count (MSVC folds identical handlers). |
| `string` | 87 | a string referenced by exactly one function in each build; votes per pair, a clear majority on both sides wins |
| `const` | 18 | the same with large constants (at least 2 votes, single partner on both sides) |
| `vtable` | 995 | same class, same virtual slot (see below) |
| `calls` | 267 | between two consecutive matched callees of a matched pair, exactly one unmatched callee on each side |
| `callers` | 56 | a matched pair whose unmatched callers are one function on each side |
| `calleeset` | 18 | unmatched functions whose matched callees (at least 3) map onto exactly the same set |
| `class` | 132 | MSVC lays out a class's methods together: an unmatched Windows function between two matched methods of class C is compared with C's unmatched Linux methods. Pairs are mutual best matches by size, matched callees and constants, with a margin. |

A Windows function that collects two different Linux partners is dropped, and so is a Linux function with two
Windows partners (231 addresses). Most of these are MSVC's identical-code folding: one Windows function serves
several classes' trivial methods, so there is no single name for it.

## Vtables across the two compilers

The Windows vtables come from MSVC RTTI (`docs/rtti-classes.json`). The Linux vtables are the `_ZTV<class>` symbols
of the ELF. Six differences have to be bridged:

1. **Destructors.** GCC has two destructor slots (complete D1, deleting D0), MSVC one (scalar deleting). They are
   folded into one.
2. **Secondary-base overriders in the primary vtable.** When a class overrides a function of a secondary base,
   GCC adds a slot for it to the primary vtable as well (for example `gsClock::move` from `gsMoveable`). MSVC
   reaches it only through the secondary vtable. Such entries are dropped from the Linux primary list. The
   destructor is dropped only if that makes the counts agree, since it often overrides a primary base's destructor
   too. 70 classes need this realignment.
3. **Object layout.** GCC places secondary bases at other offsets than MSVC (`gsMoveable` at 148 instead of 204
   in `gsModule` subclasses). Secondary vtables are therefore paired by order, not by offset.
4. **Thunks.** GCC's secondary-vtable entries are this-adjusting thunks: `sub dword [esp+4|8], n` followed by
   `jmp`, usually a short jump to the function right after the thunk. They are decoded from the ELF bytes and
   replaced by their target.
5. **Overloads.** MSVC orders a run of overloads in reverse declaration order, GCC in declaration order. For each
   run of equal names, the direction that fits the function sizes better is used.
6. **Inherited slots.** A slot that still points at a base-class function is named only by the class that
   defines it. A destructor in a secondary vtable is not named at all, since MSVC gives each secondary vtable a
   destructor thunk of its own.

## What is still unmatched

The biggest gaps, as unmatched / all engine functions per class:

| Class | Unmatched / all |
|---|---|
| free functions | 201 / 371 |
| `gsSkeleton` | 32 / 69 |
| `gsCompiler` | 21 / 21 |
| `gsControl` | 19 / 93 |
| `gsModule` | 18 / 43 |
| `gsGraph` | 18 / 40 |
| `gsMaterialVariant` | 15 / 19 |
| `gsSound` | 15 / 33 |
| `gsGridDivision` | 15 / 23 |
| `gsFile` | 14 / 17 |
| `gsTransmission` | 14 / 15 |
| `gsEngine` | 12 / 12 |

The script compiler and parser are recursive-descent code with almost no strings, constants or virtual
methods, and GCC and MSVC inline them differently. Matching them needs a structural comparison, for example of
control-flow graphs, which the tool does not do yet. The interpreter (`gsThread::execute`) is also unmatched:
Ghidra splits it at the jump table on Windows.

## Running it

```
# features of every function (once per binary; -ProjectName CiMLinux -Binary <bin> -OutDir decompiled/Linux for Linux)
tools/ghidra/run-ghidra.ps1 -Reuse -Script DumpFeatures.java -ScriptArgs <out>\features.jsonl -OutDir <out>

node tools/match-builds.mjs decompiled/GSystem-v2/features.jsonl decompiled/Linux/features.jsonl \
  "<game>/GSystem.dll" "<steam>/steamapps/content/app_73010/depot_206722/Cities In Motion.bin" \
  docs/native-api.json docs/rtti-classes.json decompiled/GSystem-v2/linux-names.csv

# rename in the Windows project (only functions still named FUN_…), then regenerate the pseudocode
tools/ghidra/run-ghidra.ps1 -Reuse -Script ApplyNames.java -ScriptArgs decompiled\GSystem-v2\linux-names.csv
tools/ghidra/run-ghidra.ps1 -Reuse
```

`ApplyNames.java` also adds a comment such as `Linux build: gsClock::move @ 0x080ec8b0 (matched by vtable)` to every
matched function. That includes the 33 exports, which keep their export name. The matcher ignores Windows names,
so it can be rerun on the old feature dump.

Ghidra types `this` from the class namespace. For a method reached through a secondary vtable, `this` actually
points at that base's subobject. For example, `gsClock::move` receives the `gsMoveable` part at object + 0xcc,
so its field offsets are relative to that.
