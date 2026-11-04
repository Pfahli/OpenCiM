# Script values, conversions and operators

What every script value class in `GSystem.dll` does, and what every operator does for every pair of value
categories. Written for the C++ port: if the port follows these rules, scripts produce the same values,
strings and runtime errors as the original.

* Companion file: [`operators.json`](operators.json) lists every reachable (operator, left category, right
  category) cell with its handler, semantics and result category.
* Evidence is given as `0xADDR.c:line` (Ghidra pseudocode in `decompiled/GSystem-v2/functions/`) or
  `bytes@VA` (raw bytes of `GSystem.dll`, file offset = VA − 0x10000000). Each rule is tagged
  **[obs]** (read directly from code or bytes) or **[inf]** (inferred: library behaviour, likely intent, or
  a consequence of several observed facts).
* `L` = left operand, `R` = right operand. "Unset" = the valueless singleton (`gsNoneVariant`).
* Operator numbers are the indices from `docs/vm-opcodes.md` (01 `+` … 2B `#`).

---

## 1. The value model

Every value is a heap object derived from `gsVariant` with a reference count at `+4` and a 29-slot vtable
(`gsMapVariant` has 32, object classes more). Five values are shared singletons created at startup and never
freed (their addRef/release do nothing): **[obs]** `0x1002e940.c:26-62`

| Global | Class | Script spelling | Category |
|---|---|---|---|
| `DAT_10193aa8` | `gsNoneVariant` | unset (no literal; `PUSH_VOID`) | 0 |
| `DAT_10193aac` | `gsNullVariant` | `null` | 5 |
| `DAT_10193ab0` | `gsZeroVariant` | integer `0` (every int result equal to 0 is this object) | 2 |
| `DAT_10193ab4` | `gsFalseVariant` | `false` | 1 |
| `DAT_10193ab8` | `gsTrueVariant` | `true` | 1 |

Int results go through `FUN_1000a560`, which returns the zero singleton for 0 **[obs]** `0x1000a560.c:21`.
Bool results are always one of the two bool singletons **[obs]** (every comparison handler returns
`DAT_10193ab4`/`DAT_10193ab8`).

### 1.1 References are values too

A variable is an element of an array (the locals array or the globals array). `PUSH_LOCAL` and `PUSH_GLOBAL`
push a **`gsIndexVariant`** (container, index) **[obs]** `0x1002ae90.c:479`, `0x1002ae90.c:306`,
`0x10016a00.c:24`. `a[i]` pushes a `gsIndexVariant` too (`0x1002ae90.c:351`, `0x1002e6a0.c:24`), and
`m[k]` pushes a `gsMapIndexVariant` (`0x1000fb60.c:27`). These reference objects forward every read
(category, conversions, count, member access, call) to the current target **[obs]** `0x1002d950.c:16`
and siblings, `0x1000f860.c`.

Consequences a port must keep:

* The operator table is indexed by the category of the *target*, read at the moment the operator runs.
* Reading through a reference to a missing target raises the target's error at that moment, even in the
  category lookup (see §5.9 and §6.2).

### 1.2 Vtable slots

| Slot | Meaning (name used in this document) | Default in `gsVariant` |
|---|---|---|
| +0x00 | scalar deleting destructor | |
| +0x04 | `addRef()`, returns `this` (`bytes@0x1001c560 8b c1 ff 40 04 c3`) | |
| +0x08 | `release()` | delete at 0 `0x1002cbe0.c:14` |
| +0x0C | `storeInSlot(v)`: called on the value currently held by a container slot; returns what the slot holds afterwards. Plain values: release self, return `v`. References: write `v` through to the target, return self | `0x1002cc00.c:13` |
| +0x10 | `resolve()`: consumes the caller's reference, returns an owned plain value (reference → target value) | return `this` `0x1002e4c0.c:11` |
| +0x14 | `getValue()`: new reference to the plain value | addRef, return `this` |
| +0x18 | operator category | 0 |
| +0x1C | `toBool()` | error 5 `0x1002cc20.c:13` |
| +0x20 | `toInt()` | error 5 |
| +0x24 | `toFloat()` (always rounded to float32, see §2.4) | error 5 |
| +0x28 | `toString(out)` (UTF-16 `std::wstring`) | error 5 `0x1002f0b0.c:13` |
| +0x2C | `toObject()` (native object pointer) | error 5 |
| +0x30 | `getRaw(out float[])` (vector/quaternion payload) | error 5 |
| +0x34 | `export(out)`: script source text | `"0 /* cannot export variant */"` `0x1002f0e0.c:23` |
| +0x38 | `count()` (`#`) | error 8 `0x1002cd70.c:13` |
| +0x3C | `getElement(i)` (borrowed pointer) | error 8 |
| +0x40 | `setElement(i, v)` | release `v`, error 8 `0x1002cc50.c:14` |
| +0x44 | `assign(v)` (`=`), consumes `v` | release `v`, error 2 `0x1002cc80.c:14` |
| +0x48 | `call(argc, args, thread)` | error 4 `0x1002ccb0.c:13` |
| +0x4C | `makeReference()` (`@`) | error 10 `0x1002cce0.c:13` |
| +0x50 | `watch(watcher, subIndex)` (installs `wait_until` triggers) | no-op (`ret 8`) |
| +0x54 | `bindEvent(record, filter)` | error 12 `0x1002cd10.c:13` |
| +0x58 | `getMember(name)` | error 13 with the name `0x1002cd40.c:13` |
| +0x5C | unknown flag (see open questions) | `true` (`bytes@0x100d47b0 b0 01 c3`) |
| +0x60 | `index(key)` → element reference | error 8 |
| +0x64 / +0x68 | `keys()` / `values()` | error 17 `0x1002cda0.c:13` |
| +0x6C | `isElementDefined(i)` | `false` `0x1002cdd0.c:11` |
| +0x70 | `defined()` | `true` |
| +0x74/+0x78/+0x7C | map only: `mapGet(key)`, `mapSet(key, v)`, `mapHas(key)` | |

Runtime error texts (`0x1001b6e0.c:51-146`): 1 "Division by zero!", 2 "Left side cannot be assigned!",
3 "Operator %d not defined for operands!", 4 "Cannot call non-function variant!", 5 "Cannot read valueless
variant!", 6 "Array index out of bounds (%d)!", 7 "Map index out of bounds (%ls)!" (the key's *export* text),
8 "Variant not indexable!", 9 "Unmatched array assignment (%d != %d)!", 10 "Variant cannot be referenced!",
11 "Wrong number of arguments (%d)!", 12 "Cannot set event to variant of this type!", 13 "Undefined member
name \"%ls\"!", 14 "Trying to use destroyed object!", 16 "Initializing map with invalid number of arguments
(%d)!", 17 "Variant is not a map!". **[obs]**

---

## 2. Categories and conversions

### 2.1 Category of every class

| Cat | Classes | Evidence |
|---|---|---|
| 0 | `gsNoneVariant` (unset), `gsVariant`, `gsFieldVariant` base, **`gsMacro`**, `gsTimerVariant`, `gsThreadFrame` | `bytes@0x100369e0 33 c0 c3` shared by all six vtables |
| 1 | `gsTrueVariant`, `gsFalseVariant`, `gsBoolField(Reference)Variant`, `gsBitField(Reference)Variant` | `bytes@0x1001c4f0` = 1 |
| 2 | `gsIntVariant`, `gsZeroVariant`, `gsIntField(Reference)Variant` | `bytes@0x1001c630` = 2 |
| 3 | `gsFloatVariant`, `gsFloatField(Reference)Variant` | `bytes@0x1001c760` = 3 |
| 4 | `gsStringVariant`, `gsStrField(Reference)Variant`, `gsWStrFieldVariant` | `bytes@0x1001c890` = 4 |
| 5 | `gsNullVariant`, `gsObjectVariant` and all 80+ engine/script object classes, `gsThread` | `bytes@0x1001c360` = 5 |
| 6 | `gsArrayVariant` | `bytes@0x1002d4e0` = 6 |
| 7 | `gsFunctionVariant`, `gsMethodVariant`, `gsScriptFunctionVariant`, `gsTransparentFunction`, `gsOpaqueFunction` | `bytes@0x1002df90` = 7 |
| 8 | `gsEventVariant` | `bytes@0x1001d620` = 8 |
| 9 | `gsMapVariant` | `bytes@0x1000f6c0` = 9 |
| 10 | `gsVectorVariant` (returns `DAT_1019198c`) | `0x10093440.c`, `0x10093a10.c:30` |
| 11 | `gsQuaternionVariant` (returns `DAT_10191990`) | `0x10093480.c`, `0x10093a10.c:31` |
| fwd | `gsIndex(Reference)Variant`, `gsMapIndex(Reference)Variant`, `gsTriggerVariant`: target's category | `0x1002d950.c:16`, `0x1002dd00.c` |
| dyn | `gsCustomField…`: the field descriptor's type (vector fields → 10); `gsVarField…`: held value's category, 5 if none | `0x1001ca20.c:11`, `0x1001e8b0.c:27` |

Vector and quaternion are runtime-registered types: `FUN_1002e1d0` returns `count + 10`
(`0x1002e1d0.c:36`) and is called exactly twice, vector first (`0x10093a10.c:30-31`) **[obs]**. Categories
12–15 never occur.

### 2.2 Conversion matrix

| Class | `toBool` | `toInt` | `toFloat` | `toString` | `export` |
|---|---|---|---|---|---|
| unset | error 5 | error 5 | error 5 | error 5 | `0 /* cannot export variant */` |
| null | false | 0 | 0.0 | `null` | `null` |
| true / false | true / false | 1 / 0 | 1.0 / 0.0 | `true` / `false` | same |
| int | `!= 0` | value | value rounded to float32 | `%d` | `%d` |
| float | `!= 0.0` (NaN → true) | truncate toward 0 (§2.4) | value | `%#.3f` | `%#.3f` |
| string | §2.5 | §2.5 | §2.5 | itself | quoted + escaped (§7) |
| array | `count != 0` | count | count | concatenation of element `toString`s, **no separator** | `[e1, e2]` |
| map | `size != 0` | size | size (unsigned) | for each entry in key order: key **export** + value `toString`, no separator | `map [k1, v1, k2, v2]` |
| vector | not all three components == 0.0 | `(int)length` | length | `(x, y, z)` each `%#.3f` | `vector(x, y, z)` |
| quaternion | not exactly (0,0,0,1) | `(int)w` | w | `(x, y, z, w)` | `quat(x, y, z, w)` |
| function (cat 7) | false | 0 | 0.0 | `""` | `null /* cannot export function */` |
| event (cat 8) | false | 0 | 0.0 | `""` | `null /* cannot export event */` |
| object (cat 5) | true | the object's address | `(float)address` | address as `%d` | `null /* cannot export object */` |
| macro (cat 0) | evaluates the macro, then converts the result (an unset result raises error 5) | same | same (rounded) | same | `0 /* cannot export variant */` |
| timer (cat 0) | error 5 | handle value | error 5 | error 5 | `0 /* cannot export variant */` |

Evidence **[obs]**: null `0x1002f520.c:23`, `bytes@0x100369e0`, `bytes@0x10036a00 32 c0 c3`,
`bytes@0x1002e3f0` (loads 0.0 from 0x10156620); true/false `0x1002f600.c:23`, `0x1002f670.c:23`,
`bytes@0x1002e4d0` (1.0 at 0x10156630); zero singleton `0x1002f590.c:23` (`"0"`); int `0x1002ce50.c:11`,
`0x1004dba0.c:11`, `bytes@0x1002ce60` (`fild; fstp dword; fld dword`), `0x10025050.c:21`; float
`0x1002cee0.c:11`, `bytes@0x1002cf00 d9 41 08 e9…` (→ `_ftol2`), `0x100250e0.c:21`; array `0x1002d4f0.c:11`,
`bytes@0x1002d500`, `0x1002f1c0.c:39`, `0x1002f2e0.c:33-40`; map `0x1000fb20.c:11`, `0x1000fb50.c:11`,
`0x1000fb30.c:15`, `0x100104e0.c:53-55`, `0x100106c0.c:51`; vector `0x10093450.c:13`, `0x10093460.c:13`,
`0x10093470.c:11`, `0x10094240.c:57`, `0x10094500.c:59`; quaternion `0x10093490.c:11` (+`0x10084210.c`,
identity set at `0x1014e7f0.c:11`), `bytes@0x100934b0 d9 41 14 e9`, `0x100934c0.c:11`,
`0x100947c0.c:74`, `0x10094b80.c:74`; functions `0x1002f440.c:23`, `0x1002f4b0.c:23`; events
`0x100212d0.c:23`, `0x10021340.c:23`; objects `bytes@0x1002e4c0 8b c1 c3` (toInt = `this`),
`bytes@0x1001c370`, `0x1001fb10.c:19`, `0x10020cd0.c:23`; macro `0x1002c700.c:16`, `0x1002c760.c:18`,
`0x1002c790.c:46`, evaluation runs a nested interpreter `0x1002c660.c:34`; timer `0x1015da7c` vtable,
`0x10073930.c:22`.

No engine object class overrides `toString` or `export` (all 81 object vtables share `0x1001fb10` and
`0x10020cd0`) **[obs]** (vtable survey of `docs/rtti-classes.json`). So `"x" + $obj` produces `"x"` followed
by a decimal pointer value: non-deterministic across runs.

### 2.3 Number → string

* **int**: `swprintf(L"%d")` **[obs]** `0x10025050.c:21`.
* **float**: `swprintf(L"%#.3f", (double)f)` **[obs]** `0x100250e0.c:21`, into a 32-wchar stack buffer.
  Always exactly three decimals, no exponent form:

  | value | text |
  |---|---|
  | 1.0 | `1.000` |
  | 0.1 (float 0.100000001…) | `0.100` |
  | 1e10 (exactly 10000000000 as float) | `10000000000.000` |
  | 2.5 | `2.500` |
  | 1/3 | `0.333` |
  | 0.0004 | `0.000` |
  | -0.0001 | `-0.000` **[inf]** (C runtime keeps the sign) |

  The DLL statically links the VS2008-era CRT (no msvcr import; its `1#INF`/`1#QNAN` strings are at
  0x10171524..). For a port: use `%.3f` on the `double` value, plus the legacy-CRT differences listed in the
  open questions (exact ties, huge values, inf/NaN) **[inf]**.
* This same text is used for string concatenation, `echo`, array/map/vector `toString` **and `export`**, so
  `exportVariables` keeps only three decimals of every float **[obs]** (`0x10094500.c:59` uses
  `FUN_100250e0`; float export slot = `0x1002ed00`).

### 2.4 Float ↔ int

* **toFloat always returns a float32.** Even the int and object versions store through a `float` before
  returning (`bytes@0x1002ce60 51 db 41 08 d9 1c 24 d9 04 24 59 c3`, `bytes@0x1002d500`, `bytes@0x1001c370`)
  **[obs]**. So `16777217` used as a float is `16777216.0`, rounded to nearest-even.
* **float → int** truncates toward zero through `_ftol2` into a 64-bit integer, then keeps the low 32 bits
  **[obs]** `bytes@0x1002cf00` → `bytes@0x101210d8` (`fistp qword` + truncation fix-up). So 3e9 → −1294967296,
  values ≥ 2^63 or NaN → 0 **[inf]** (from the `_ftol2` algorithm).
* Arithmetic on two floats: the original computes in x87 with the CRT default 53-bit precision and rounds
  the result to float32; with float32 inputs this equals IEEE-754 binary32 arithmetic (round to nearest
  even) **[inf]**. Port: use `float` arithmetic.

### 2.5 String → bool / int / float

| Input | `toBool` (`0x10022450.c:13-25`) | `toInt` (`0x10024db0.c`) | `toFloat` (`0x10024ef0.c`) |
|---|---|---|---|
| `"true"` | true | 1 | 1.0 |
| `"false"` | false | 0 | 0.0 |
| `"1"` / `"0"` | true / false | 1 / 0 | 1.0 / 0.0 |
| `""` | false | **undefined** | **undefined** |
| any other text | true | parsed | parsed |

All **[obs]**. Parsing other text: a `std::wistringstream` (VS2008 `num_get`) reads `>> int` / `>> float`
(`0x10024db0.c:53`, `0x10024ef0.c:53`) **[obs]**. That means **[inf]** (standard library behaviour):
leading white space is skipped; an optional sign; decimal digits only (`"0x10"` → 0, `"010"` → 10); parsing
stops at the first character that does not fit and **the rest is ignored** (`"12abc"` → 12, `"2.7"` as int →
2); floats accept `1.5`, `1e3`, `.5`.

When nothing can be parsed (`"abc"`, `""`, `"-"`) or an int overflows 32 bits, the stream leaves the target
unchanged, and the target local is **never initialised**: the original returns stack garbage **[obs]**
(`bytes@0x10024e85 8d 54 24 08 52`: address of an unwritten local is passed, and the same local is returned
at `bytes@0x10024edc 8b 44 24 08`; the float version likewise, `bytes@0x10024fcc`). Port: return 0 / 0.0
and log it.

`toBool("0.0")` is **true** and `toBool(" ")` is **true** (only the four exact spellings are special).

---

## 3. Operator dispatch

* Lookup index `(op << 8) + (leftCat << 4) + rightCat` in `DAT_101939f8` **[obs]** `0x1002af4b.c:112`.
  Prefix operators use `leftCat = 0` (`0x1002af4b.c:84`), postfix operators `rightCat = 0`
  (`0x1002af4b.c:61`) **[obs]**.
* Handlers are called `binary(L, R)`, `prefix(NULL, R)`, `postfix(L, NULL)` **[obs]** `bytes@0x1002b0dd`,
  `bytes@0x1002b02b`, `bytes@0x1002afa2`. They return a new reference; the interpreter then releases the
  operands.
* No handler → runtime error 3, `"Operator %d not defined for operands!"`, with the operator index printed in
  **decimal** (e.g. `+=` → "Operator 29") **[obs]** `0x1001b6e0.c:61`.
* `=` (0x1C) never reaches the table: it compiles to opcode 0C, `ref.assign(value)`, and the reference stays
  on the stack as the expression's value **[obs]** `0x10005340.c:11`, `0x1002ae90.c:308-312`.
* **Every other binary operator, including `&&`, `||`, `?` and `:`, is compiled as "evaluate L, evaluate R,
  BINOP"**: the only operator emitter is `FUN_10005340` (`0x10005340.c:17`) and the expression compiler
  emits no jumps (jump slots exist only in the statement compiler `FUN_100067c0`) **[obs]**. There is **no
  short-circuit evaluation** anywhere.
* `?` and `:` are two separate binary operators of equal precedence 15 (`docs/vm-opcodes.md`); binary
  operators pop equal-precedence operators first, i.e. associate left (`0x10008a80.c:139`), so
  `c ? a : b` is `(c ? a) : b` **[obs]**.

The table is built by `FUN_10012f40` before the vector module registers (`0x10082810.c:11` →
`0x1001bb60.c:27`, then `0x10082810.c:29`) **[obs]**. Adding categories 10 and 11 copies column/row 0 into
the new column/row and **clears the diagonal cell** `[op][N][N]` for every operator (`0x10011550.c:65-77`)
**[obs]**. Vector/quaternion handlers are then registered for specific pairs (`0x10093a10.c:276-291`).

---

## 4. Operator semantics

### 4.1 Arithmetic `+ - * / %` (and the result type)

The **left** category picks the rule; the right operand is converted to fit **[obs]** (table cells, see
`operators.json`):

| L \ R | int | float | anything else (bool, null, string, array, map, vector, …) |
|---|---|---|---|
| **int** | int op | float op | int op with `R.toInt()` |
| **float** | float op | float op | float op with `R.toFloat()` |
| **string** | `+` only: concatenation | `+`: concatenation | `+`: concatenation with `R.toString()` |
| **vector** | error | error | `vector op vector` only (`+ - * /`) |
| **quaternion** | error | error | `q*q`, `q*v`, `q/q` only |
| unset, bool, null/object, array, function, event, map | error for all five operators | | |

* `-`, `*`, `/`, `%` with a string on the left are errors; there is no string repetition or subtraction
  **[obs]**.
* Examples: `1 + "2"` → 3, `"1" + 2` → `"12"`, `1 + 2.5` → 3.5, `"v=" + 1.5` → `"v=1.500"`,
  `1 + true` → 2, `1 + null` → 1, `1 + [7, 8]` → 3 (count), `2 * $vec` → `2 * (int)length`,
  `true + 1` → error, `[1] + [2]` → error, `$unset + 1` → error **[obs]** (follows from the cells and §2.2).

**int rules** **[obs]**: 32-bit two's-complement wrap for `+ - *` and unary `-` (`0x10011970.c`,
`0x10011b30.c`, `0x10011f80.c`). `/` truncates toward zero, `%` has the sign of the dividend (C semantics).
For `/` and `%` the right operand is converted **first** and a zero divisor raises "Division by zero!"
(`0x10011c10.c:15-21`, `0x10011da0.c`). `INT_MIN / -1` and `INT_MIN % -1` trap in the original (x86 `idiv`)
**[inf]**; the port should pick a result (suggest wrap: INT_MIN and 0).

**float rules** **[obs]**: binary32 arithmetic (§2.4). **A float divisor equal to 0.0 or −0.0 also raises
"Division by zero!"** (`0x10011c60.c:15-16`); there is no `inf` from division. `%` is
`(float)fmod((double)L, (double)R)` with the same zero check (`0x10011df0.c`).

### 4.2 String concatenation

`L.toString() + R.toString()`, the right operand converted first **[obs]** `0x100127a0.c` (calls `+0x28` on
`param_2` before `param_1`). Any right type works except unset, which raises "Cannot read valueless
variant!" (unset `toString` is an error, §2.2). The new string's length is the sum of both lengths
(`0x1002eac0.c:33`).

### 4.3 Equality `==` / `!=`

Defined cells **[obs]** (`operators.json`, op 06/07):

| Pair | Rule |
|---|---|
| bool, bool | `L.toBool() == R.toBool()` |
| int, int | `L.toInt() == R.toInt()` |
| int/float mixed or float, float | float32 equality; NaN ≠ everything |
| string, string | exact UTF-16 equality, case-sensitive |
| **either side category 5** (null/object), any other side | **identity**: same object pointer after `getValue()` |
| array/array, function/function, event/event, map/map | identity |
| vector/vector, quaternion/quaternion | exact component equality |
| **everything else** | **runtime error 3** |

So `null == null` → true, `$obj == null` → false, `$unset == null` → false (no error), `0 == null` → false,
`[1] == [1]` → false. But `1 == "1"`, `true == 1`, `$unset == 0`, `$unset == $unset`, `"a" == 1`,
`1.0 == true` all **raise an error** **[obs]**.

### 4.4 Ordering `< <= > >=`

Defined only for int/int, int/float mixes, float/float (float32, NaN → false), string/string, and
category 5 / category 5 **[obs]**:

* strings: code-unit-wise unsigned comparison, then shorter first, case-sensitive (`0x100118e0.c:36`)
  **[obs]**.
* category 5: `toObject()` pointers compared unsigned (null = 0); i.e. memory-address order **[obs]**
  `0x100122e0.c`.

Everything else (bool, array, map, mixed string/number, unset…) raises error 3.

### 4.5 Logical `&& || !`

* Result is always the `true`/`false` singleton, never an operand **[obs]** `0x10011480.c`,
  `0x100114b0.c`, `0x100114e0.c`.
* Both operand **expressions** are always evaluated (§3); only the second `toBool()` call is skipped
  (`bytes@0x10011480`) **[obs]**. A side effect or a runtime error in the right expression always happens.
* Registered for every category pair **except** vector&&vector, vector||vector, quaternion&&quaternion,
  quaternion||quaternion (cleared diagonal, §3) **[obs]**.
* Conversion uses `toBool()`, so an unset operand raises "Cannot read valueless variant!".

### 4.6 `?` and `:`

**[obs]** `bytes@0x10011500`, `bytes@0x10011520`:

```
L ? R  =  L.toBool() ? R.getValue() : unset
L : R  =  (v = L.getValue()) is the unset singleton ? R.getValue() : v
```

* `:` tests **pointer identity with the unset singleton only**. `null : 5` → `null`, `0 : 5` → 0,
  `false : 5` → false, `"" : 5` → `""`. A macro value is not unset either.
* `c ? a : b` parses as `(c ? a) : b`. Differences from C:
  * both `a` and `b` are evaluated, always;
  * if `c` is true but `a` is unset, the result is `b`;
  * `c1 ? a : c2 ? b : d` is `(((c1 ? a) : c2) ? b) : d`: with `c1` true and `a` = 1 the result is `b`,
    not `a` **[inf]** (follows from precedence/associativity).
* `x : default` does **not** protect against missing map keys or out-of-range array indices: the
  operator lookup itself reads the category through the reference and raises "Map index out of bounds" /
  "Array index out of bounds" first (§5.9). Use `defined()` for that **[obs]** (`0x1000f860.c` → `+0x74` →
  `0x100100c0.c:30`).
* Typical idiom `MAIN.$petitionbar : false` works because reading an undefined `$member` of a script
  object creates the member holding unset (`0x1001a650.c:32-98`) **[obs]**.
* Not defined for vector:vector, vector?vector, quaternion:quaternion, quaternion?quaternion (cleared
  diagonal) **[obs]**.

### 4.7 Bitwise and shifts

Only int with int; any other pair (including int with float) is error 3 **[obs]** (cells 0x12–0x14,
0x0C–0x0E, 0x22–0x27 exist only at (2,2); `~` only for an int operand).

* `<<`: `L << (R & 31)` (x86 `shl`, `bytes@0x10012647`). `>>`: arithmetic, sign-extending (`sar`,
  `bytes@0x100126b7`). `>>>`: logical (`shr`, `bytes@0x10012727`), result reinterpreted as int32
  (`-1 >>> 0` = −1, `-1 >>> 1` = 2147483647) **[obs]**. Shift count 32 behaves as 0 **[obs]** (masked).
* L is converted before R in shifts and bit operations **[obs]**.

### 4.8 Unary `+ - ~ !`

* unary `+`: int and float operands only; returns the value unchanged **[obs]** `0x10011470.c`.
* unary `-`: int (wraps), float, vector **[obs]**. Strings, bools etc. → error.
* `~`: int only. `!`: any operand (via `toBool`).

### 4.9 `++` / `--`

Registered **only when the current value is an int** (category 2), prefix and postfix **[obs]**
(cells 0x16–0x19). `$f++` on a float, string or unset value raises error 3 ("Operator 24 …").

* prefix: `x.assign(int(x.toInt() ± 1))`, result = new value **[obs]** `0x10011fc0.c`.
* postfix: `old = x.getValue()`, then assign, result = old value **[obs]** `0x10012020.c`.
* Applied to a non-reference (`5++`) the `assign` raises "Left side cannot be assigned!" **[obs]** (base
  `+0x44`).

### 4.10 Compound assignment `+= -= *= /= %= &= |= ^= <<= >>= >>>=`

Same row rules as the plain operator, keyed on the **current category of the left target**, then
`L.assign(result)`, and the expression value is `L.getValue()` (the new value) **[obs]**
(`0x100119d0.c`). Details:

* `$i += 0.5` (int target, float right) uses the float handler: **the variable becomes a float** **[obs]**
  (cell (2,3) → `0x10011a10`).
* `$s += x` appends `x.toString()` **[obs]** `0x100128c0.c`.
* `$v += $w` works for vectors (also `-= *= /=`); quaternions have no compound operators **[obs]**.
* Unset targets (`$total += 1` before initialisation) raise error 3 **[obs]**.

### 4.11 `@` (reference)

`R.makeReference()` **[obs]** `0x10011540.c`:

| Operand | Result |
|---|---|
| variable or array element (`gsIndexVariant`) | `gsIndexReferenceVariant` (`0x1002e730.c:27`) |
| map entry (`gsMapIndexVariant`) | `gsMapIndexReferenceVariant` (`0x1000fbe0.c:27`) |
| native field (`gs*FieldVariant`) | the matching `…FieldReferenceVariant` (`0x1001d860.c:27`) |
| an existing reference | itself (addRef) |
| any plain value | error 10 "Variant cannot be referenced!" |

How a reference binds is described in §5.1.

### 4.12 `#` (count)

`int(R.count())` **[obs]** `0x10012780.c`: string → stored length (`0x1004dbb0.c:11`), array → element
count, map → entry count, engine modules → child count (`gsModule +0x38`). Everything else (int, float,
null, unset, vector…) → "Variant not indexable!".

### 4.13 Vectors and quaternions

Layout: vector `x,y,z` floats at `+8,+0xC,+0x10`; quaternion `x,y,z,w` at `+8..+0x14` **[obs]**
`0x100928d0.c`, `0x10092960.c`.

| Expression | Result | Evidence |
|---|---|---|
| `v + w`, `v - w`, `v * w` | component-wise (`*` is **not** dot or cross) | `0x100832d0.c`, `0x10083320.c`, `0x100833d0.c` |
| `v / w` | component-wise; a component divided by 0.0 gives 0.0, no error | `0x10083420.c` |
| `-v` | negation | `0x10083530.c` |
| `v == w`, `v != w` | exact float equality of all components | `0x10083710.c`, `0x10083750.c` |
| `q1 * q2` | Hamilton product `q1*q2` | `0x100840e0.c`, operand order `bytes@0x10093165` |
| `q * v` | vector `(conj(q) * (v,0) * q).xyz` (rotation by the inverse of `q` for unit `q`) | `0x10085e40.c` |
| `q1 / q2` | `conj(q2) * q1`, no normalisation | `0x100858a0.c` |
| `q1 == q2`, `!=` | exact component equality | `0x100841c0.c` |
| `v * 2`, `v + 1`, `v * q`, `q + q`, `q - q` | **error 3** (not registered) | `0x10093a10.c:276-291` |
| `2 * v`, `1.5 + v` | **scalar**: the int/float rule with `v.toInt()` / `v.toFloat()` = length | cells (2,10), (3,10) |
| `"p=" + v` | `"p=(1.000, 2.000, 3.000)"` | `0x10094240.c` |

All **[obs]**. Vectors have no members: `$v.x` raises "Undefined member name" (vector `+0x58` is the base
error) **[obs]**; scripts use the natives `vector_x/y/z($v)` and `vector_length($v)`, which return 0.0 for a
non-vector argument (`0x10093740.c:23`, `0x100937a0.c`) **[obs]**. Length is
`(float)sqrt(z*z + y*y + x*x)` (`0x10084fa0.c:14`) **[obs]**.

Constructors (natives, listed here for completeness) **[obs]**: `vector()` → (0,0,0); `vector(v)` → `v`
itself if it is a vector, else (0,0,0); `vector(a,b)` → "Wrong number of arguments (2)"; `vector(x,y,z)` →
`toFloat` of each (`0x100929f0.c:40-45`). `quat()` / `quat(non-quat)` → identity; `quat(q)` → `q`;
`quat(angle, axis)` → axis-angle, angle in radians, axis normalised (`0x100855d0.c:22`); `quat(a,b,c)` →
error 11; `quat(x,y,z,w)` → components (`0x10093590.c:52-72`).

---

## 5. Assignment, references and containers

### 5.1 How `=` works

`ASSIGN` calls `target.assign(value)` **[obs]** `0x1002ae90.c:312`. For a variable or array element
(`gsIndexVariant`) that is `container.setElement(index, value)` (`0x1002da30.c:11`), which does
**[obs]** `0x1002d590.c:60`:

```
slot = slot.storeInSlot(value.resolve())
```

* `value.resolve()` turns a reference into its current plain value. So `$b = $a` copies the *pointer* to
  `$a`'s value.
* If the slot holds a plain value, `storeInSlot` releases it and the slot takes the new value.
* If the slot holds a **bound reference** (created by `@`, below), `storeInSlot` writes through to the
  reference's target and the slot keeps the reference (`0x1002d890.c:11`) **[obs]**.

`@` references **[obs]**: `gsIndexReferenceVariant.resolve()` and `.getValue()` produce a plain
`gsIndexVariant` pointing at the original container/index (`0x1002dec0.c:25`, `0x1002df30.c:27`); the map
and field versions behave the same (`0x1000fa50.c`, `0x1000fac0.c:27`, `0x1001c570.c:27`). So:

```
$a = @$b;   // $a's slot now holds a gsIndexVariant(locals_of_b, index_of_b): an alias
$a = 5;     // writes 5 into $b; $a stays an alias
$c = $a;    // $c gets 5 (a plain copy; aliases do not propagate through plain assignment)
```

There is no way to un-alias a slot with `=` **[inf]** (every later `=` writes through).

Assigning *to an array value* (`[@$x, @$y] = [1, 2]`) is element-wise: the counts must match ("Unmatched
array assignment (%d != %d)!"), then `lhs[i].assign(rhs[i].getValue())` **[obs]** `0x1002d6b0.c:29-37`.
Because array literals store resolved values (§5.2), the left elements only accept assignment if they are
`@` references **[inf]**.

### 5.2 Value vs reference semantics

* Ints, floats, strings, bools, null, vectors and quaternions are immutable; sharing pointers is invisible
  **[obs]** (no class has a mutating method for them).
* **Arrays and maps are shared by reference.** `$b = $a; $b[0] = 9;` changes `$a[0]`. Array/map
  `resolve()` returns the same object (`bytes@0x1002e4c0` in their vtables) **[obs]**. There is no
  implicit copy anywhere in the value system.
* An array literal `[a, b]` stores `resolve()` of each item (plain values, not variable references)
  **[obs]** `0x1002d150.c:41`, called with mode 0 from `0x1002ae90.c:372`.

### 5.3 Arrays (`gsArrayVariant`)

Layout `+8` element pointers, `+0xC` count, `+0x10` capacity, `+0x14` modification counter **[obs]**
`0x1002d150.c`.

| Operation | Rule | Evidence |
|---|---|---|
| index conversion | `key.toInt()`: `$a[1.9]` → element 1, `$a["2"]` → element 2, `$a[unset]` → error 5 | `0x1002e6a0.c:24` [obs] |
| read `i` in `0..n-1` | element | `0x1002d510.c` [obs] |
| read `i < 0` | element `n + i` while `i >= -n` | `0x1002d510.c:14` [obs] |
| read out of range | error 6 "Array index out of bounds (i)!" (original index) | `0x1002d510.c:15` [obs] |
| write `i >= n` | grows: capacity doubles (min 1, or `i+1` if still short); new slots `n..i` filled with **unset**; then store | `0x1002d590.c:34-53` [obs] |
| write `i < 0` | wraps from the end; `i < -n` → error 6 | `0x1002d590.c` [obs] |
| write | increments the modification counter | `0x1002d590.c:62` [obs] |
| `$a[] = x` | reference to index `count` (append) | `0x1002ae90.c:753-756` [obs] |
| `defined($a[i])` | false when out of range (no error), else `element.defined()` | `0x1002d770.c:16-24` [obs] |
| `#$a`, `toInt`, `toFloat`, `toBool` | count / count / count / count != 0 | §2.2 [obs] |
| `foreach` | iterates `0..count-1` by a live element reference; `count` re-read every step | `0x1002ae90.c:539-550` [obs] |
| `foreach_reverse` | indices −1, −2, … −count | `0x1002ae90.c:770` [obs] |
| `keys` / `values` on an array | error 17 "Variant is not a map!" | base slot [obs] |

Indexing a string, number, null or unset is "Variant not indexable!" (base `+0x60`) **[obs]**. Indexing an
**unset** variable does not create an array: initialise with `[]` first **[obs]**.

### 5.4 Maps (`gsMapVariant`)

A `std::map<gsVariant*, gsVariant*>` (red-black tree, `+0xC` head, `+0x10` size) plus cached `keys`
(`+0x14`) and `values` (`+0x18`) arrays **[obs]** `0x10011240.c`, `0x1000fe70.c`.

**Key order** **[obs]** `0x1000f640.c:22-31`: `less(a, b)` uses the `<` cell for `(cat a, cat b)` if one
exists, otherwise `cat(a) < cat(b)`. Consequences **[obs]** (follow from the `<` table):

* int and float keys compare numerically: `1` and `1.0` are the **same key**.
* strings sort by code unit; objects by address (iteration order of object keys changes between runs).
* Categories without a `<` handler collapse: **`true` and `false` are the same key**; every array is the
  same key as every other array; likewise all maps, all functions, all events, all vectors, all
  quaternions, all unset/macro values.
* Mixed categories order by category number: unset/macro < bool < numbers < string < object < array <
  function < event < map < vector < quaternion.

| Operation | Rule | Evidence |
|---|---|---|
| `$m[k]` | reference holding `k.getValue()` as the key | `0x1000f6d0.c:24` [obs] |
| read missing key | **error 7 "Map index out of bounds (<export of key>)!"**, not unset | `0x100100c0.c:30` [obs] |
| write | insert (key object kept, value resolved) or `storeInSlot` into the existing entry; drops both caches | `0x10011120.c:21-50` [obs] |
| `defined($m[k])` | `mapHas(k)`: true for an existing entry even if its value is unset | `0x1000f9e0.c:11` [obs] |
| `keys $m` / `values $m` | array in key order; the **same array object** is returned again while the map and that array are unmodified | `0x1000fe70.c:28` [obs] |
| `#$m`, `toBool`, `toInt`, `toFloat` | size, size != 0, size, size | §2.2 [obs] |
| `$m[] = x`, `foreach ($m)` element access | error 8 (map has no positional `getElement`) | map `+0x3C/+0x40` = base [obs] |
| map literal `map [k1, v1, …]` | odd item count → error 16; duplicate keys: later value wins, first key object kept | `0x10011240.c:60-108` [obs] |

Map literal keys are taken from the stack **without** `resolve()` **[obs]** (`0x10011240.c:60`, mode 0
from `0x1002ae90.c:811`): `map [$k, 1]` stores the *variable reference* `$k` as the key, so the key follows
later changes of `$k` **[inf]** (and can break the tree order). Port: resolve keys, or reproduce if a
script depends on it (none found in `extracted/`).

### 5.5 Strings

Immutable UTF-16; `+8` buffer, `+0xC` length **[obs]** `0x1002eac0.c`. `#` uses the stored length, but
`toString`, comparison and concatenation copy the buffer with `wcslen` (`0x1002f150.c:25`), so an embedded
U+0000 truncates in those paths **[obs]**. Not indexable (no `$s[0]`).

### 5.6 Unset

Produced by: `PUSH_VOID` (functions without a value, empty `return`), new `$members` of script objects,
array growth fill, `false ? x`, and natives that return `DAT_10193aa8` **[obs]**. Reading it as bool, int,
float or string raises "Cannot read valueless variant!" (so `if ($u)` and `"x" + $u` fail); `defined($u)` is
false; `$u : d` gives `d`; `$u == null` is false; `$u == 0`, `$u + 1` are error 3 **[obs]**.

### 5.7 Macros (`gsMacro`, category 0)

`macro :: expr` builds a closure. Every conversion (`toBool/toInt/toFloat/toString/toObject`) evaluates it
synchronously in a fresh interpreter and converts the result (`0x1002c660.c:34`); `call()` with 0 args
evaluates it, with arguments raises error 11 (`0x1002c8c0.c:15`) **[obs]**. Because the category is 0,
`$macro + 1` is error 3, but `1 + $macro`, `"x" + $macro`, `!$macro`, `if ($macro)` evaluate it **[obs]**.

### 5.8 Native fields (`gs*FieldVariant`)

`obj.field` for a native property yields a field variant: `{owner, descriptor, pointer}` **[obs]**.
Reads call the descriptor's refresh callback first (unless the owner is destroyed) and return a fresh value
(`0x1001d8c0.c`). Writes: destroyed owner → error 14; a setter callback if present; otherwise the value is
converted with the field's type: bool fields `toBool`, bit fields set/clear the mask, int `toInt`, float
`toFloat`, string `toString` (`0x1001d9d0.c:28-35`, `0x1001d240.c:44`, `0x1001dbe0.c:35`,
`0x1001ddf0.c:35`, `0x10020eb0.c:36`) **[obs]**. So `$obj.intField = 2.9` stores 2.

### 5.9 Errors raised through references

A reference's category lookup reads the target. Therefore every operator applied to `$a[99]` (out of range)
or `$m["missing"]` raises the container error before the operator runs, including `:` and `==`. Only
`defined(...)` and assignment are safe on missing targets **[obs]** (`0x1002d950.c:16` →
`0x1002d510.c:15`; `0x1000f860.c` → `0x100100c0.c:30`).

---

## 6. Per-class slot reference

Slots not listed behave as the `gsVariant` default (§1.2).

**`gsNoneVariant`** (unset, vtable `0x10158ed4`): cat 0; addRef/release no-ops; all conversions error 5;
export `0 /* cannot export variant */`; `getMember` error 13 (`0x1002e320.c`); `defined` false
(`bytes@0x10036a00`).

**`gsNullVariant`** (`0x10158f4c`): cat 5; false / 0 / 0.0 / `"null"` / `toObject` 0 / export `null`;
`getMember` error 13.

**`gsTrueVariant` / `gsFalseVariant`** (`0x101590b4` / `0x1015903c`): cat 1; 1/0, 1.0/0.0, `"true"`/`"false"`,
export same.

**`gsZeroVariant`** (`0x10158fc4`): cat 2; false, 0, 0.0, `"0"`.

**`gsIntVariant`** (`0x10158a24`, value at `+8`): cat 2; §2.2.

**`gsFloatVariant`** (`0x10158a9c`, float at `+8`): cat 3; §2.2.

**`gsStringVariant`** (`0x10158b14`): cat 4; conversions §2.5; `count` = length; export §7.

**`gsArrayVariant`** (`0x10158b8c`): cat 6; `+0x3C/+0x40` get/set element (§5.3); `+0x44` element-wise
assign (`0x1002d6b0.c`); `+0x50 watch` wraps element `i` in a `gsTriggerVariant` that it installs in the
slot (`0x1002e620.c:25`, `0x1002db40.c`);
`+0x60` index; `+0x6C` isElementDefined.

**`gsMapVariant`** (`0x101568ac`): cat 9; `+0x60` index → `gsMapIndexVariant`; `+0x64/+0x68` keys/values;
`+0x74` get (error 7), `+0x78` set, `+0x7C` has.

**`gsIndexVariant`** (`0x10158c04`, `{container, index}`): every read slot forwards to
`container.getElement(index)`; `+0x0C storeInSlot` and `+0x44 assign` → `container.setElement`; `+0x10`
resolves to the element's value and releases itself (`0x1002d8b0.c:21`); `+0x4C` → `gsIndexReferenceVariant`;
`+0x50` forwards `watch` to the container (the arguments are not visible in the pseudocode, `0x1002da70.c`;
presumably its own index **[inf]**); `+0x70` → `container.isElementDefined(index)` (`0x1002db30.c:11`).

**`gsIndexReferenceVariant`** (`0x10158cf4`): as above, but `+0x10`/`+0x14` return a new plain
`gsIndexVariant` (the alias) and `+0x4C` returns itself.

**`gsMapIndexVariant` / `gsMapIndexReferenceVariant`** (`0x1015679c` / `0x10156814`, `{map, key}`): same
pattern via `mapGet/mapSet/mapHas` (`0x1000f7a0.c`, `0x1000f7c0.c:21`, `0x1000f9e0.c:11`).

**`gsVectorVariant` / `gsQuaternionVariant`** (`0x1015f44c` / `0x1015f4c4`): cat 10 / 11; conversions §2.2;
`+0x30 getRaw` copies 3 / 4 floats (`0x10092940.c`, `0x100929d0.c`); no members, not indexable.

**`gsMacro`** (`0x101588a4`): §5.7.

**`gsFunctionVariant` family** (cat 7): false/0/0.0/`""`; `+0x48 call` per subclass (`gsMethodVariant`
`0x1001d4d0`, `gsScriptFunctionVariant` `0x1002e000`, `gsTransparentFunction`/`gsOpaqueFunction`
`0x1002e110`); `+0x5C` is false for `gsMethodVariant` and `gsOpaqueFunction`.

**`gsEventVariant`** (`0x10158464`, cat 8): false/0/0.0/`""`; `+0x54` binds a handler (`0x10022250`).

**`gs*FieldVariant` family**: §5.8; categories in §2.1; `+0x5C` false; `+0x0C storeInSlot` assigns through
and returns the new value (`0x1001c450.c:11`); `+0x10` resolves (`0x1001c470.c:13`); `+0x4C` → the
`…ReferenceVariant`, whose `+0x14` creates a bound field variant.

**`gsTimerVariant`** (`0x1015da7c`, cat 0): wraps a timer handle; `toInt` = handle, everything else default.

**`gsTriggerVariant`** (`0x10158c7c`): temporary wrapper placed in a watched slot by `wait_until`; every read
forwards to the wrapped value; `storeInSlot` forwards the store, puts unset in its place, wakes the waiting
thread and drops itself (`0x1002dc90.c:17`) **[obs]**.

**`gsObjectVariant`** and all engine/script objects, `gsThread` (cat 5): true / address / `(float)address` /
address `%d` / `toObject = this` / export `null /* cannot export object */`; destroy-aware release
(`0x1001d7e0.c`); `getMember` resolves native events, methods and fields (`0x1001ef10.c`); script objects
create `$members` on first read (`0x1001a650.c:32-98`), unknown non-`$` names raise error 13
(`0x1001a650.c:119`).

**`gsThreadFrame`** (cat 0): defaults only.

---

## 7. Export format (`export`, `exportVariables`)

| Type | Text |
|---|---|
| int | `%d` |
| float | `%#.3f` (three decimals; precision is lost) |
| bool | `true` / `false` |
| null | `null` |
| string | `"` + escaped + `"`: TAB → `\t`, LF → `\n`, CR → `\r`, `"` → `\"`, `\` → `\\`, code units ≥ 0x80 → `\uXXXX` (4 upper-case hex digits), all other code units < 0x80 copied as-is (other control characters are not escaped; U+0000 ends the text) |
| array | `[` + item exports joined by `, ` + `]` |
| map | `map [` + `key, value` pairs in key order, joined by `, ` + `]` |
| vector | `vector(x, y, z)` |
| quaternion | `quat(x, y, z, w)` |
| reference | export of its target |
| unset, macro, timer | `0 /* cannot export variant */` |
| function | `null /* cannot export function */` |
| event | `null /* cannot export event */` |
| object | `null /* cannot export object */` |

Evidence **[obs]**: `0x1002efd0.c:24-25`, `0x100226c0.c:29-57` (escapes), `0x10024bd0.c:48` (bytes widened
1:1 after `strlen`), `0x1002f2e0.c:33-40`, `0x100106c0.c:51`, `0x10094500.c:59`, `0x10094b80.c:74`.
`exportVariables` writes lines of the form `… = <export>;` (`0x10018340.c`, strings `"= "` and `";"`)
**[obs]**; the line prefix and file rewriting are outside this document.

---

## 8. Rules most likely to be implemented wrong

1. No short-circuit: `&&`, `||`, `?`, `:` always evaluate both sides; `c ? a : b` is `(c ? a) : b`.
2. `:` replaces only the unset singleton, and cannot rescue a missing map key or array index.
3. Floats print as `%#.3f` everywhere, including `export`.
4. Cross-category `==` is an error unless one side is null/object or both are numbers.
5. The left operand's category decides arithmetic: int + anything-but-float is int via `toInt`, string +
   anything is concatenation, bool/null/array/map/unset on the left is an error.
6. `++`/`--` exist only for ints; float division by zero is an error; `+=` with a float turns an int
   variable into a float.
7. Map keys: `true`/`false` collide, `1`/`1.0` collide, missing keys are errors.

---

## 9. Open questions

1. **Legacy CRT `%#.3f` details** [inf]: VS2008's `printf` rounds the decimal digit string half-up, so exact
   binary ties (0.0625, 0.1875, 2.0625, …) may print `0.063` where a modern C runtime prints `0.062`; values
   ≥ 1e17 may print only 17 significant digits followed by zeros; ±inf / NaN print as `1.#IO`-style strings
   (`1.#INF` / `1.#QNAN` / `-1.#IND` rounded to 3 decimals); `-0.0` sign. Needs a test on the original
   (`echo 0.0625; echo 1e20; echo 3e38*10;`). Values with more than ~26 integer digits overflow the
   32-wchar buffer in `0x100250e0.c` (undefined behaviour).
2. **Failed string → number parses** return an uninitialised stack value (§2.5). Is any shipped script
   relying on a stable value? Port proposal: 0.
3. **`+0x5C` flag**: true for values, false for field variants, `gsMethodVariant`, `gsOpaqueFunction`. No
   caller found in the pseudocode or by scanning for `call [reg+5Ch]` in `.text` outside four unrelated
   functions. Possibly used by the linker/compiler for constant folding.
4. **`INT_MIN / -1`** and `INT_MIN % -1` trap in the original (hardware divide error inside a handler); was
   this ever caught by an SEH handler? The port needs a defined result.
5. **Map literal keys as live references** (§5.4): confirm with a test whether `map [$k, 1]` really keeps the
   variable reference (the pseudocode shows no `resolve()`), and whether later changes of `$k` reorder the
   map.
6. **Field variant `storeInSlot`** (`0x1001c450.c`) assigns and then returns the value it just released;
   only reachable if a field variant is stored in an array slot (e.g. as a function argument without `@`?).
   Needs checking against the call path before the port relies on it.
7. **Vector `&&`, `||`, `?`, `:` errors** come from the diagonal clearing in `FUN_10011550`; an unintended
   side effect of the category-growth code, but scripts written for the original cannot use them, so the
   port can either reproduce the error or allow them.
8. **Precision of the x87 control word**: the analysis assumes the CRT default (53-bit) and that no library
   changes it. Since all float operands are float32 (§2.4), the results equal IEEE binary32 either way,
   except for double rounding of subnormal results.
