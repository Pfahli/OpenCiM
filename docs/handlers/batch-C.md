# Batch C: global script functions and gsScript save-system methods

**Overview.**
* **Dates:** `mkdate(d, m, y)` returns a day number counted from 1 January of year 0 (proleptic Gregorian, year 0 is a leap year; 1 Jan 1970 = 719528). `mktime(h, m, s)` returns milliseconds since midnight. A `gsClock` stores `date` (day number) and `time` (ms) and rolls over at 86 400 000 ms. The shipped `mkdate` has an encoder bug: years 101–103, 201–203 and 301–303 of each 400-year cycle lose their year offset (1901–1903 all encode as 1 Jan 1900). This is confirmed in the raw bytes.
* **File system:** there is one case-sensitive VFS map, keyed by root-relative paths with no leading `/`. `userdata/…` maps to the folder set by `init_userdata`. Writes to `memory/…` go to RAM files that survive `updateFileSystem`. Absolute `X:\…` paths and paths starting with `/` skip the VFS and hit the disk directly. Loose files override archive entries. Every write registers its path in the VFS at once, but no write creates directories.
* **Saves:** `exportVariables` and `exportVariablesTo` rewrite a script-source text file line by line, writing `$name = <export>;` lines ended by CRLF. They use the script's own source file as the template. `writePackage` then zlib-packs the listed files, keeping only their bare names, into a GS10 archive and re-registers that archive's entries in the VFS straight away.
* **Errors:** argument validation errors in this batch use script error code `0xf`, "Calling a function with invalid argument(s)!" (`0x1001b6e0.c:124-125`; code 5 is "Cannot read valueless variant!", `0x1001b6e0.c:66-67`). A missing file raises a different C++ exception type (`DAT_101810e8`), not the script-error type `DAT_10178994`.

Tags: **[obs]** = seen in the pseudocode or raw bytes, **[inf]** = inferred.

## global.random
- **Signature:** `random([a: int|float], [b: int|float]) -> int|float`
- **Behavior:** With no arguments it returns a float in [0, 1]. With one argument, an int `n` gives an int in [0, n−1] and a float `x` gives a float in [0, x]. With two arguments, two non-floats give an int in [a, b] (both ends included); if either is a float it gives a float in [a, b].
- **Details:**
  - Float results are `rand() * (hi − lo) / 32767.0 + lo`, using CRT `rand()` (RAND_MAX 32767). Both endpoints can be returned, and there are only 32768 distinct steps [obs].
  - Int results use the composite `(rand() << 16) | (rand() & 0xFFFF)`, then `% n` (plus `lo` in the two-argument form). Because `rand()` is 15-bit, bits 15 and 31 are always 0 (values 0..0x7FFF7FFF), which gives a small modulo bias [obs].
  - "Float" means exactly type category 3. Everything else (bool, string, null) goes through `toInt` [obs].
  - Error 0xf is raised when one int argument is below 1, when one float argument is below 0.0, or when `b < a` [obs]. `random(0.0)` is allowed and returns 0.0 [obs].
  - The handler uses the global CRT `rand()` state [obs]. Where `srand` is called was not checked.
- **Evidence:** `0x1000afd0.c:19-35` (2 ints), `:37-47` (2 args with a float), `:49-62` (1 int), `:63-70` (1 float), `:72-76` (0 args). Error text from `0x1001b6e0.c:124-125`.
- **Confidence:** high. Seeding not investigated.

## global.vector_length
- **Signature:** `vector_length(v: vector) -> float`
- **Behavior:** Returns the Euclidean length √(x²+y²+z²) of a vector. Any non-vector argument returns 0.0 without an error.
- **Details:**
  - Checks the type against the runtime-registered vector type id `DAT_1019198c`, registered with size 0xc at `0x10093a10.c:30` [obs].
  - Gets the components through vtable `+0x30` (`0x10092940` copies +8/+0xc/+0x10) [obs]. The square root uses CRT thunk `0x1011fd74` [inf: x87 sqrt].
  - Quaternions and other types return 0.0 [obs].
- **Evidence:** `0x10093740.c:15-23`. Callees `0x10084fa0.c:10-13`, `0x10092940.c`.
- **Confidence:** high.

## global.vector
- **Signature:** `vector([x: float, y: float, z: float]) -> vector`. Also `vector(v: vector) -> vector`.
- **Behavior:** With three arguments it builds a new `gsVectorVariant(x, y, z)` using `toFloat`. With none it returns (0, 0, 0). With one vector argument it returns that same object (retained, not copied); any other single argument gives (0, 0, 0). Two arguments raise error 0xb.
- **Details:**
  - The default vector `DAT_10196a3c` is set to (0, 0, 0) by a static initializer [obs `0x1014e6a0.c:11`].
  - Object layout: 0x14 bytes, vtable `0x1015f44c` (RTTI gsVectorVariant), floats at +8/+0xc/+0x10 [obs].
  - A non-vector single argument is silently ignored [obs].
  - The single-vector form aliases the argument: vtable `+0x14` is `0x1001c560`, which only increments the refcount [obs]. Whether the aliasing is visible depends on vectors being mutable [inf].
  - Two arguments give "Wrong number of arguments (2)!" (code 0xb) [obs].
- **Evidence:** `0x100929f0.c:28-36` (1 arg), `:38-43` (2 args), `:45-61` (3 args), `:66-73` (default). Constructor `0x100928d0.c:16-19`.
- **Confidence:** high.

## global.mkdate
- **Signature:** `mkdate(day: int, month: int, year: int) -> int`
- **Behavior:** Converts a calendar date to a day number: days since 1 January of year 0 (proleptic Gregorian, year 0 is a leap year). This is the unit of `gsClock.date`.
- **Details:**
  - Year algorithm [obs]:
    - `q = floor(y/400)`, `r = y − 400q` (floor rounding for negative years). `base = q*146097`.
    - If `r < 100`: `base += (r/4)*1461` and `rem = r%4`.
    - Otherwise: `base += 36525 + ((r−100)/100)*36524` and `yc = (r−100)%100`. If `yc ≥ 4`: `base += 1460 + ((yc−4)/4)*1461` and `rem = (yc−4)%4`.
    - If `rem == 0` the year is a leap year; otherwise `base += 1 + rem*365`.
  - Day-of-year is `cum[month−1] + day − 1`, with `cum = [0,31,59,90,120,151,181,212,243,273,304,334,365]`, plus 1 if it is a leap year and `month > 2` [obs].
  - The day is only clamped from above. A day past the end of the month becomes the last day (29 for February in a leap year). Day ≤ 0 is not clamped and rolls back into earlier days [obs].
  - The month is not range-checked. Values outside 1..12 read outside the 13-entry stack table, so the result is undefined [obs].
  - **BUG:** when `yc` is 1..3 the jump skips the year-offset addition. Verified in the raw bytes: `cmp ecx,4 / jl 0x10074180` at 0x10074128–0x10074133.
    - Affected years are those ≡ 101–103, 201–203 or 301–303 mod 400 (for example 1901–1903 and 2101–2103). 1 Jan of 1900, 1901, 1902 and 1903 all return 693961. These years are still correctly treated as non-leap [obs].
    - The inverse in `gsClock` (`0x10073c40`) is correct, so such dates decode as 1900 [obs].
    - Shipped scripts use 1948–2003 dates, which are unaffected [obs: grep]. Keep the bug in a reimplementation if mkdate values must match the original game bit for bit [inf].
  - Reference values: 1 Jan 1920 = 701265, 1 Jan 1970 = 719528, 1 Jan 2000 = 730485 [inf: computed from the algorithm and cross-checked against the known 1970 count].
- **Evidence:** `0x100740b0.c:19-74`. Raw bytes 0x100740b0–0x1007422c, decoded by hand. Decoder `0x10073c40.c:12-45`.
- **Confidence:** high.

## global.mktime
- **Signature:** `mktime(hours: int, minutes: int, seconds: int) -> int`
- **Behavior:** Returns `((h*60 + m)*60 + s)*1000`, i.e. milliseconds since midnight, the unit of `gsClock.time`. For example `mktime(8,30,0)` = 30 600 000.
- **Details:** No range checks and no wrapping [obs]. A signed 32-bit value overflows beyond about 596 hours [inf].
- **Evidence:** `0x10074230.c:17-22`.
- **Confidence:** high.

## global.loadMaterial
- **Signature:** `loadMaterial(video: gsVideo, path: string) -> gsMaterialVariant`
- **Behavior:** Reads a `.material` file through the VFS and has the video driver create a native material. It parses the file into that material and returns it wrapped in a new `gsMaterialVariant`.
- **Details:**
  - `args[0]` is dynamic-cast to `gsVideo`. If the cast fails or the argument is null, the pointer stays NULL and the next virtual call (`+0x114`) dereferences it. That is a crash, not a script error [obs].
  - The native material comes from `video->vtable[+0x114]()` [obs]. Reading that slot as "create material" is an inference: `0x1008b420` uses the same slot to create a missing material.
  - The file is loaded with mustExist = 1 (`push 1` at 0x1008fb63). A missing file logs `File "<path>" not found!` to the root module and throws [obs: bytes and `0x100809e0.c`].
  - Format [obs tags; meaning inf]:
    - The root chunk tag must be 0x02013100; the first 0x48 body bytes are a header.
    - `0x1008ec70` then parses sub-chunks with case labels 0x02013110, 0x02013120, 0x02013130, 0x02013140 and 0x02013150, each with a 0x28-byte header (Ghidra shows these labels as floats). Texture names are resolved against `extractFilePath(path)`.
    - In the shipped `colognedom/01 - Default.material`, the header holds the material name, sub-chunk 0x02013110 names `colognedom.dds`, and a nested 0x02013111 holds the artist's original path.
    - So `.material` **is** chunked, which corrects REPORT §4. If the root tag differs, the material keeps its defaults with no error [obs].
  - The wrapper is 0xc4 bytes with vtable `0x1015e91c` (RTTI gsMaterialVariant), +0x14 = video, +0x18 = native material. Its init `0x1008b420` registers members and copies native properties into script fields (getters +0x3c..+0x54, 8 texture slots) [obs].
  - The handler does no caching: every call creates a new material [obs].
- **Evidence:** `0x1008faf0.c:38-50`, `:51`, `:52-57`, `:59-72`, `:73-84`. Byte at 0x1008fb63. Callees `0x100809e0`, `0x1007bd90`, `0x1007be00`, `0x1007ce30` (extractFilePath), `0x1008ec70` (case labels decoded to tags), `0x10087bf0`, `0x1008b420`.
- **Confidence:** medium. Material fields not decoded; name of the `+0x114` slot inferred.

## global.colormap
- **Signature:** `colormap(path: string) -> gsColorMap`, `colormap(width: int, height: int) -> gsColorMap`, `colormap(path: string, width: int, height: int) -> gsColorMap`
- **Behavior:** Creates a `gsColorMap` holding a 24-bit RGB pixel buffer. One argument decodes a BMP. Two arguments allocate a blank width × height map. Three arguments read a headerless raw RGB file as width × height.
- **Details:**
  - Object: 0x20 bytes, +0x14 RGB buffer (`w*h*3`), +0x18 width, +0x1c height [obs].
  - BMP decoding [obs]:
    - Skips the 14-byte file header. Accepts a 12-byte OS/2 core header or a Windows info header.
    - Reads a palette for ≤ 8 bpp and decodes 8, 24 and 32 bpp.
    - Flips rows to top-down and converts BGR(A) to RGB.
    - Does not seek to `bfOffBits`. Other depths (1/4/16 bpp) leave the buffer uninitialised.
  - Raw form copies `min(fileSize, w*h*3)` bytes [obs].
  - The blank map buffer is not zeroed [obs].
  - The raw form loads with mustExist = 1 (`push 1` at 0x10075e10), so a missing file throws [obs]. The BMP form probably does the same [inf].
- **Evidence:** `0x100762f0.c:26-38` (3 args), `:40-47` (2 args), `:49-56` (1 arg). Callees `0x10075710.c`, `0x10075790.c:9-22`, `0x10075dc0.c:18-39`, `0x10075eb0.c` (headers, then 8/24/32-bit loops).
- **Confidence:** high for the structure, medium for BMP edge cases.

## global.max
- **Signature:** `max(a: number, b: number) -> number`
- **Behavior:** Returns the larger argument as the original variant, without conversion. It compares with `toInt` unless either argument is a float (category 3), in which case it uses `toFloat`.
- **Details:**
  - On a tie it returns `a` [obs].
  - The type is preserved: `max(3, 2.5)` → int 3, `max(1, 2.5)` → float 2.5 [obs: the argument is returned through vtable `+0x14`, retained].
  - Other types (string, bool, null) are compared through `toInt` [obs]. A string goes through the "true"/"false"/stream parser [obs, see `int`]. Types without `toInt` raise code 5 [inf: base `gsVariant` slot +0x20 = `0x1002cc20`].
- **Evidence:** `0x1000ad30.c:17-47`.
- **Confidence:** high.

## global.min
- **Signature:** `min(a: number, b: number) -> number`
- **Behavior:** Returns the smaller argument as the original variant. Comparison rules are the same as `max`.
- **Details:** On a tie it returns `a` (the test is `a <= b`) [obs]. Mixed int/float compares as floats but returns the original type [obs].
- **Evidence:** `0x1000acb0.c:17-47`.
- **Confidence:** high.

## global.clamp
- **Signature:** `clamp(x: number, lo: number, hi: number) -> number`
- **Behavior:** If `x < lo` it returns `lo`; else if `hi < x` it returns `hi`; else it returns `x`. The result is always one of the original variants. It compares as ints unless any of the three is a float.
- **Details:** When `lo > hi`, the `lo` test wins for small `x` [obs]. No error when `lo > hi` [obs].
- **Evidence:** `0x1000adb0.c:16-61`.
- **Confidence:** high.

## global.int
- **Signature:** `int(v: any) -> int`
- **Behavior:** Returns `newInt(v.toInt())`.
- **Details:**
  - A float truncates toward zero (CRT ftol: `0x1002cf00` → `FUN_101210d8`) [obs call; inf rounding mode].
  - A string returns 1 for `"true"` and 0 for `"false"`; otherwise it is parsed with a wide istringstream (`0x10024db0`) [obs]. A failed parse probably gives 0 [inf].
  - Zero returns the shared int-0 singleton [obs: per conventions].
- **Evidence:** `0x1000af90.c:13-14`. Callees `0x1002cf00.c`, `0x1002d130.c` → `0x10024db0.c:16-30`.
- **Confidence:** high.

## global.expandPattern
- **Signature:** `expandPattern(pattern: string, v0: any, v1: any, …) -> string`
- **Behavior:** Builds a string from `pattern`, replacing `%` directives with the extra arguments (`%0` = the first value after the pattern).
- **Details:**
  - The handler passes `argc−1` and `args+1` to the core [obs bytes 0x100aa698–0x100aa6a6: `dec eax; push eax; lea ecx,[eax+4]`].
  - Directives [obs]:
    - `%D` (single digit 0–9): `toString` of value D. Floats format as `"%#.3f"` (for example `"2.500"`).
    - `%!NN` (two digits): `toString` of value NN.
    - `%f[d<sep><prec>]D`: float of value D, printed with `swprintf("%#" sep prec "f")`. The default is `%#.3f`; `%fd.2D` gives 2 decimals [inf: the sep slot only makes sense as `.`].
    - `%i[t<c>][p<n>][d<c><g>]D`: integer of value D. Options may repeat in any order:
      - `t<c>`: insert a thousands separator every 3 digits.
      - `p<n>`: zero-pad to at least n digits.
      - `d<c><g>`: insert decimal char `c` before the last g digits, so 5 with `d.2` gives `0.05`.
      - Negative numbers get `-`. Padding is added **in front of** the sign, so −5 with `p3` gives `0-5` [obs].
    - `%` followed by any other character emits that character, so `%%` gives `%`.
  - Example from scripts: `"start_sandbox_task%ip20"` = zero-pad to 2 digits, value 0.
  - Edge cases [obs]:
    - An index ≥ value count, or a null value, inserts nothing silently.
    - A directive cut off by the end of the pattern ends the output (the rest is dropped). A lone trailing `%` is dropped.
    - Only single-digit indices work outside `%!NN`.
- **Evidence:** `0x100aa680.c:27-31` plus the bytes above. Core `0x100a9cb0.c:98-128` (`%!`), `:133-155` (`%D`), `:233-266` (`%f`), `:160-232` and `:279-348` (`%i`), `:268-272` (literal).
- **Confidence:** high for the syntax, medium for `%f` with a non-`.` separator.

## global.escapePattern
- **Signature:** `escapePattern(text: string) -> string`
- **Behavior:** Doubles every `%` so the text passes through `expandPattern` unchanged.
- **Details:** The loop appends one extra character when it sees `%`, then always appends the character itself [obs]. The appended value is held in a register Ghidra did not show [inf: it is `%` / the current char]. No other characters are escaped; `{` and `}` used by `formatText` are left alone [obs].
- **Evidence:** `0x100a9a50.c:27-31` → `0x100a9910.c:31-44`.
- **Confidence:** high.

## global.quat
- **Signature:** `quat() -> quaternion`, `quat(q: quaternion) -> quaternion`, `quat(angle: float, axis: vector) -> quaternion`, `quat(x: float, y: float, z: float, w: float) -> quaternion`
- **Behavior:** Builds a `gsQuaternionVariant`, stored x, y, z, w. With no arguments, or one non-quaternion, it returns the identity (0, 0, 0, 1). One quaternion argument is returned as the same object (retained). The angle/axis form builds a rotation from an axis and an angle. The 4-float form stores the raw components. Three arguments raise error 0xb.
- **Details:**
  - Angle/axis form: the axis comes from `args[1]` if it is a vector, else (0, 0, 0). The result is `xyz = normalize(axis) * sin(angle/2)`, `w = cos(angle/2)` [obs structure; inf which CRT thunk is sin vs cos]. A zero axis is left unnormalised [obs `0x10085050`]. The angle is in radians [inf: scripts pass `3.1415926536 * 0.5`].
  - 4-float form: Ghidra mislabels the constructor as `CRect::CRect` with `(int)` casts, but the bytes use `fstp dword [esp]` (`d9 1c 24` at 0x100935fc, 0x10093607, 0x10093612, 0x1009361d), so the values are stored as floats with no int conversion [obs bytes].
  - The identity default `DAT_10196980` is set to (0, 0, 0, 1) by `0x1014e7f0.c:11` [obs]. Quaternion type id `DAT_10191990` [obs].
  - Shipped scripts often pass non-normalised 4-tuples such as `quat(0.0, -0.3, 0.0, 0.0)`. No normalisation happens [obs].
- **Evidence:** `0x10093590.c:35-42`, `:43-50`, `:52-66`, `:68-71`, `:72-92`. Callees `0x100932e0.c`, `0x100855d0.c`, `0x10092960.c`, `0x100840c0.c`.
- **Confidence:** high, apart from the sin/cos thunk assignment (medium).

## global.fappend
- **Signature:** `fappend(path: string, text: any) -> valueless`
- **Behavior:** Appends `text.toString()` to a real disk file, opened in C text append mode.
- **Details:**
  - The path is mapped through the engine path map `0x1007dae0`: `userdata/…` becomes `<userdata root>/…` and anything else stays relative to the working directory. It is then opened with `_wfsopen(path, L"a", _SH_DENYNO)` [obs]. The VFS is not consulted and not updated [obs].
  - If the open fails it throws a script error with the custom text `Cannot open file "<path>"` [obs].
  - The text is narrowed by taking the low byte of each UTF-16 unit (`0x1002eeb0`), so non-Latin-1 characters get mangled. In text mode `\n` becomes CRLF on disk [obs narrowing; inf CRT text mode].
- **Evidence:** `0x1000d120.c:39-49`, `:64-80`, `:81-89`. Callees `0x100178e0`, `0x1007dae0.c`, `0x10120698.c` (`_wfsopen`), `0x1002eeb0.c`.
- **Confidence:** high.

## global.changeFileExt
- **Signature:** `changeFileExt(path: string, ext: string) -> string`
- **Behavior:** Replaces the extension: everything from the last `.` that comes after the last `/` or `\` is replaced by `ext`. If there is no such dot, `ext` is appended.
- **Details:** `ext` must include its own dot (`changeFileExt(x, ".scenario")`) [obs]. An empty `ext` strips the extension; `writePackage` uses this [obs]. A dot inside a directory name is ignored [obs].
- **Evidence:** `0x1007d3c0.c:31-37` → `0x1007cfc0.c:16-48`.
- **Confidence:** high.

## global.extractFileName
- **Signature:** `extractFileName(path: string) -> string`
- **Behavior:** Returns the substring after the last `/` or `\`, or the whole string if there is none.
- **Details:** A trailing separator gives an empty string [obs]. The extension is kept [obs].
- **Evidence:** `0x1007d0c0.c:26-30` → `0x1007cce0.c:15-36`.
- **Confidence:** high.

## global.file_exists
- **Signature:** `file_exists(path: string) -> bool`
- **Behavior:** For a virtual path, returns true when the path is a key in the VFS map. For an absolute `X:\…` path, or any path starting with `/`, it tries `_wfsopen(path, "rb")` on the raw string.
- **Details:**
  - The first call triggers the full filesystem scan if it has not run yet [obs].
  - The lookup is case-sensitive and exact, with no normalisation of `\`, `..` or `//` [obs: compare `0x100118e0`].
  - `.gs` archive files are not keys after a scan, because they are mounted as folders. So `file_exists("userdata/state.gs")` is false after `updateFileSystem` but true right after `copy_file`/`writePackage` creates it, since the writer registers the path [obs `0x1007f5e0`, `0x10080f00`]. Scripts test the contents instead (`userdata/saves/<n>/map.grid`).
  - A leading-`/` path goes to the disk as written, i.e. the root of the current drive [obs].
- **Evidence:** `0x10095130.c:25-31` → `0x100805f0.c:11-60`.
- **Confidence:** high.

## global.substr
- **Signature:** `substr(s: string, start: int, [count: int]) -> string`
- **Behavior:** Returns `s.toString().substr(start, count)` as a new string. Without `count` it runs to the end (npos).
- **Details:** Uses the `std::wstring` assign(str, pos, count) semantics of `0x10002040` [obs]. A count past the end, or a negative count (it becomes a huge unsigned value), is clipped to the end [inf]. A `start` greater than the length, including any negative start, makes the MSVC STL throw `std::out_of_range`, a C++ exception rather than a script error [inf].
- **Evidence:** `0x1000cd90.c:31-53` (2 args), `:54-75` (3 args). Callee `0x1000cc20.c:16-19`.
- **Confidence:** medium (out-of-range behaviour inferred from the STL).

## global.formatText
- **Signature:** `formatText(text: string, colors: array<int>) -> string`
- **Behavior:** Turns `{d:` … `}` colour markup into engine control codes. `{d:` (d = one digit) becomes U+0004 followed by 8 uppercase hex digits of `colors[d].toInt()` (AARRGGBB). Each `}` becomes U+0005. Every other character is copied.
- **Details:**
  - The hex digits come from the table `"0123456789ABCDEF"` (`DAT_1015de50`), most significant nibble first [obs].
  - A `{` not followed by a digit, or a digit not followed by `:`, raises error 0xf [obs]. A `{` at the very end reads the NUL and also errors [obs].
  - `colors` is indexed through vtable `+0x3c`, so an out-of-range index goes through the array's own bounds error [inf]. The element reference is never released (a leak) [obs].
  - Only colour indices 0–9 work [obs].
  - The output buffer is `len*9` wide chars [obs].
- **Evidence:** `0x10075ad0.c:37-115` (scan loop), `:56-62` and `:78-83` (errors), `:85-97` (hex), `:117-124` (`0x1002cff0` string constructor).
- **Confidence:** high.

## global.list_files
- **Signature:** `list_files([dir: string], [ext: string]) -> array<string>`
- **Behavior:** Returns the names (not full paths) of VFS files directly inside `dir`, optionally only those whose `extractFileExt(name)` equals `ext`.
- **Details:**
  - The first call triggers the filesystem scan if needed [obs].
  - A `/` is appended to `dir` when it is not empty and lacks one. With no arguments, `dir` is the VFS root [obs].
  - Files in sub-folders are excluded, because a further `/` after the prefix disqualifies the key [obs].
  - Matching is a case-sensitive prefix match. The `ext` comparison is exact and includes the dot (`".grid"`) [obs].
  - Sources include archives, loose files, userdata and `memory/` files [obs: one map]. `.gs` archive files themselves are not listed after a scan; savegames are enumerated with `list_dirs("userdata/saves")` [obs scripts].
  - Order is the VFS `std::map` order, sorted by UTF-16 code units [inf].
- **Evidence:** `0x10080130.c:59-62`, `:71-94`, `:95-103`, `:111-176`, `:178-204`. Callees `0x100118e0`, `0x1007cee0`, `0x1002d150` (array constructor).
- **Confidence:** high.

## global.string
- **Signature:** `string(v: any) -> string`
- **Behavior:** Returns `newString(v.toString())`.
- **Details:** Floats format as `swprintf("%#.3f")`, so `string(2.5)` = `"2.500"` [obs `0x100250e0`]. Ints print in decimal [inf]. Objects and other types use their own `toString` [obs: virtual].
- **Evidence:** `0x1000b830.c:24-26`. Callees `0x1000ae70.c`, `0x1002ed00.c` → `0x100250e0.c:7`.
- **Confidence:** high.

## global.loadStateMachine
- **Signature:** `loadStateMachine(path: string, owner: gsModule) -> gsStateMachine`
- **Behavior:** Creates a new `gsStateMachine` (0x118 bytes) as a child of `owner`, resolves `path` against the owner's path and loads the `.states` file. Loading also loads the companion script `<path minus extension>.script`.
- **Details:**
  - `owner` is dynamic-cast to `gsModule`; a non-module becomes NULL, meaning no parent. Parenting goes through vtable `+0xa4` (`0x10073a50`: `setParent` plus registering the tick interface) [obs].
  - Path resolution `0x1007de40(path, owner+0xb0)` [obs]:
    - A leading `/` or `~/` is stripped, which makes the path VFS-root relative (`"/metro/scripts/system.states"` → `metro/scripts/system.states`).
    - A leading `./` is stripped.
    - Otherwise the result is `ownerPath (+ "/" if missing) + path`.
    - With no owner, the path is used verbatim, and a leading `/` then means the raw disk [inf: `0x100809e0` rules].
  - Load `+0xf4` = `0x1009e200` [obs]:
    - Reads the file through the VFS loader.
    - Calls `+0xbc` with `changeFileExt(path, ".script")`, inferred to load the script; `metro/scripts/system.states` has a `system.script` sibling.
    - Then calls `0x1009d780` when field +0x100 is 0 (probably entering the initial state) [inf].
  - The owner reference is released afterwards [obs].
- **Evidence:** `0x1009f500.c:39-51`, `:52-61`, `:62-75`, `:111-115`. Callees `0x1009f420.c`, `0x1008a6f0.c`, `0x1007de40.c`, `0x1009e200.c:36-70`.
- **Confidence:** medium (the `+0xbc` and `0x1009d780` roles are inferred).

## global.writePackage
- **Signature:** `writePackage(archivePath: string, files: array<string>) -> valueless`
- **Behavior:** Writes a GS10 archive (format in REPORT §2) holding every file in `files`, read through the VFS. It then registers the archive's entries in the VFS so they can be read straight away under `<archivePath minus extension>/<name>`.
- **Details:**
  - Each input is loaded with mustExist = 1 (`push 1` at 0x10081652), so a missing input throws `File "…" not found!` [obs bytes].
  - Every entry is zlib-compressed with `compress()` (default level) and gets tag 0x01120000, even when compression does not shrink it [obs].
  - Entry body (0x90 bytes) [obs]:
    - `magic` = first 4 bytes of the data, or 0 if the file is shorter than 4 bytes.
    - `offset` is relative to the root body. It starts at `count*0x98 + 0x14` and grows by each packed size, **with no alignment padding**.
    - `size`, `packedSize`.
    - `name[128]` = narrow `extractFileName(path)` copied with `strncpy` (not NUL-terminated at 128 chars).
  - Paths are **flattened** to bare file names, so `memory/gamedata.script` is stored as `gamedata.script` [obs]. No 0x01130000 or 0x01140000 prefix chunks are written, which triggers the mount rule "archive path minus .gs + /" [obs].
  - Chunk order: GS10 → 0x01000000 → 0x01100000 {count, entries} → 0x01200000 data. Sizes are back-patched through the writer (file or memory) [obs].
  - VFS update: for each entry the key `changeFileExt(archivePath,"") + "/" + name` gets a record {byte0 = 1 (in archive), wide archive path at +2, +0xe4 = absolute offset (root offset + 0x10), +0xe8 size, +0xec packed size, +0xf0 0x01120000}. Any existing record is replaced, unlike mount time, where the first entry wins [obs].
  - The archive itself goes through the generic writer, so a `memory/` path gives an in-RAM archive and anything else goes to disk. The parent folder must already exist; scripts call `mkdir("userdata/saves")` first [obs `0x10080f00`; scripts].
- **Evidence:** `0x10081490.c:87-114`, `:115-145`, `:146-151`, `:160-170`, `:178-223`, `:224-280`, `:300-352`, `:374`. Callees `0x10080f00`, `0x1007d530`, `0x1007d770`, `0x10081110`, `0x1007cce0`, `0x1007cfc0`.
- **Confidence:** high.

## global.copy_file
- **Signature:** `copy_file(src: string, dst: string) -> valueless`
- **Behavior:** Reads `src` whole through the VFS (archives, loose files and memory files all work) and writes it byte for byte to `dst` with the engine file writer.
- **Details:**
  - The source loads with mustExist = 1 (`push 1` at 0x10095061). A missing source logs `File "<src>" not found!` and throws [obs bytes].
  - Destination rules come from `0x10080f00` [obs]:
    - A `memory/` prefix gives a RAM file, registered in the VFS on close.
    - Anything else first replaces any VFS record for `dst` with a loose record, then maps the path (`userdata/` → userdata root) and opens it `"wb+"`.
    - No directories are created. If the open fails, the data is silently lost [inf: the writer ignores a NULL `FILE*`].
  - The source may be an archive file itself: `copy_file("metro/defaults/state", "userdata/state.gs")` copies a GS10 blob. Its contents only become visible after `updateFileSystem()` [obs scripts and the scanner].
- **Evidence:** `0x10095040.c:30-46`. Callees `0x100809e0.c`, `0x10080f00.c`, `0x1007d770`, `0x10081110.c`.
- **Confidence:** high.

## global.init_userdata
- **Signature:** `init_userdata(root: string) -> valueless`
- **Behavior:** Sets the disk folder that backs the `userdata/` virtual prefix, creates it, and scans it into the VFS.
- **Details:**
  - The root is stored in `DAT_1019037c`, the string used by path map `0x1007dae0` (`userdata/x` → `root + "/x"`) [obs].
  - `CreateDirectoryW(root)` creates one level only; failure is ignored [obs `0x10120335`].
  - `0x1007f5e0(1)` scans only the userdata root [obs]:
    - Each `*.gs` is mounted as an archive under `userdata/<rel minus .gs>/…`.
    - Every other file is registered as a loose record `userdata/<rel>`, replacing any existing record.
    - Stale userdata records are not removed.
    - The "filesystem initialised" flag is set.
  - Called once from `metro/config.script:2` with `getMyDocumentsFolder()+"/Cities In Motion"` [obs].
- **Evidence:** `0x1007fd10.c:25-42`. Callees `0x10120335.c`, `0x1007f5e0.c` (userdata branch), `0x1007dae0.c`.
- **Confidence:** high.

## global.updateFileSystem
- **Signature:** `updateFileSystem() -> valueless`
- **Behavior:** Rebuilds the VFS from scratch: thin wrapper for `0x1007f5e0(0)`.
- **Details:**
  - Every record that is not a memory file (+0xf4 == 0) is deleted, so `memory/…` files survive [obs].
  - The install directory (working directory) is enumerated recursively (pattern `.*`), skipping keys that start with `userdata` [obs].
  - Each `*.gs` is mounted (`0x1007f4d0`), and each other file is registered as a loose record that **replaces** any existing entry [obs].
  - Archive mounting never replaces an existing key (REPORT §2), so **loose files always win over archive entries**, whatever the enumeration order [obs here + REPORT]. This settles REPORT §3's open question.
  - The userdata root is then rescanned in the same way (see `init_userdata`) [obs].
- **Evidence:** `0x1007fd00.c:11` → `0x1007f5e0.c:50-66` (purge of non-memory records), `:73-80` (enumerate with `L".*"`, `0x1015e014`), `:105-130` (skip `userdata*`; `.gs` → mount; `L".gs"` = `0x1015dff8`), `:131-157` (loose record replaces existing), `:178-270` (userdata rescan), `:271` (initialised flag).
- **Confidence:** high.

## global.clock
- **Signature:** `clock() -> gsClock`, `clock(time: int) -> gsClock`, `clock(time: int, date: int) -> gsClock`, `clock(h: int, m: int, s: int, day: int, month: int, year: int) -> gsClock`
- **Behavior:** Creates a `gsClock` (0x104 bytes) with `time` (ms since midnight; default 0) and `date` (day number; default 0). The six-argument form uses `mktime(h,m,s)` and `mkdate(day,month,year)`. The clock is parented to the root module so it ticks every frame, and it is ticked once with dt = 0 to fill its derived fields.
- **Details:**
  - Argument order is `(time, date)`: `clock($time, $date)` and `clock(.time, .date + 182)` in scripts [obs].
  - 3, 4 or 5 arguments raise error 0xb [obs].
  - The six-argument form calls the `mktime`/`mkdate` handlers directly, so it inherits the mkdate bug [obs].
  - Constructor defaults: `speed` (+0xd4) = 1.0, accumulator +0xd8 = 0, `paused` (+0x100) = false, derived fields zero [obs].
  - Tick (`0x10073f80`, secondary vtable at +0xcc) [obs structure; inf the ×1000 lost in the pseudocode]:
    - If not paused: `acc += dt*speed`.
    - `ms = (int64)(acc*1000)`, `time += ms`, `acc -= ms*0.001`.
    - Days are carried while `time ≥ 86 400 000` or `time < 0`.
    - Then `0x10073c40` recomputes years/months/days/hours/minutes/seconds [obs]. It computes "changed" flags for year/month/day, presumably to fire `onYear`/`onMonth`/`onDay`/`onHour` [inf: the firing code was not read].
- **Evidence:** `0x10074280.c:28-85`, `:87-91`. Callees `0x10073b30.c:16-32`, `0x1009fdf0.c`, `0x10073a50.c`, `0x10073f80.c`, `0x10073c40.c`.
- **Confidence:** high (event firing in `0x10073c40` only partly read).

## global.new_sound
- **Signature:** `new_sound(owner: gsModule|null) -> gsSound`
- **Behavior:** Creates a `gsSound` (0x188 bytes) and parents it to `owner` if `owner` is a `gsModule`, otherwise leaves it unparented. Loading happens later through `.load(…)`.
- **Details:**
  - Defaults [obs]: `minDistance` 10.0 (+0x148), `maxDistance` 100.0 (+0x14c), `pitch` 1.0 (+0x150), `volume` 1.0 (+0x15c), `relative` false (+0x146), plus unnamed floats +0x154 and +0x160 = 1.0, `currentPos`/`length` 0.
  - A non-module owner is silently ignored (the reference is released) [obs].
  - Scripts mostly pass `MAIN.$desktop` or `this` [obs].
- **Evidence:** `0x100983f0.c:23-48`. Constructor `0x10097950.c:16-38`. Field map from native-api.md (gsSound).
- **Confidence:** high.

## gsScript.getVariable
- **Signature:** `script.getVariable(name: string) -> any`
- **Behavior:** Looks up a script-level variable by name (without `$`) in this script's own symbol table and returns its current value, retained. Returns valueless if it is missing.
- **Details:**
  - The name is narrowed by taking the low byte of each UTF-16 unit (`0x1002eeb0`) [obs].
  - Symbol table layout [obs]:
    - `this+0x14` points to the symbol table.
    - Table +0x20 is a `std::map<std::string, int>` (head at +0x24, size at +0x28).
    - Table +0x2c is the value-storage array, read with vtable `+0x3c` getItem(index).
  - Returns valueless when there is no symbol table, the name is unknown, or the value slot is NULL [obs].
  - Only this script's variables are searched: no globals, no parent scope [obs].
  - Lookup is case-sensitive and byte-wise [obs].
- **Evidence:** `0x10018260.c:24-46`. Callees `0x1002eeb0.c`, `0x10089ea0` (map find).
- **Confidence:** high.

## gsScript.getFunction
- **Signature:** `script.getFunction(name: string) -> function`
- **Behavior:** Returns the named function defined in this script, retained, or valueless if there is none. Scripts call the result directly: `this.getFunction(expandPattern("start_sandbox_task%ip20",$r))(0)`.
- **Details:**
  - The name is narrowed like in `getVariable` [obs].
  - Binary search over the sorted function index at table +0x30 (count at +0x34, 0x14-byte entries whose first int indexes 0x24-byte records at table +4) [obs]. The record holds a `std::string` name at +0 and the function variant at +0x1c [obs].
  - Byte-wise `strcmp` ordering [obs].
  - Whether the returned function is bound to `this` was not checked.
- **Evidence:** `0x10017470.c:25-47` → `0x10017190.c:12-55`.
- **Confidence:** high.

## gsScript.exportVariables
- **Signature:** `script.exportVariables(name1: string|array<string>, …) -> valueless`
- **Behavior:** Rewrites this script's **own source file** (`scriptFilename`, field +0x18) in place. For each named variable it replaces, or appends, a line `$name = <value>;`. With zero arguments it does nothing.
- **Details:**
  - Thin wrapper: `0x10018340(this, src = this+0x18, dst = this+0x18, argc, args)` [obs]. Merge semantics are under `exportVariablesTo`.
  - Typical target is `userdata/settings.script`, which the path map sends to the Documents folder: `$settings.exportVariables("music", "volume", …)` or `exportVariables(["gameName"])` [obs scripts].
- **Evidence:** `0x10019360.c:11-14`.
- **Confidence:** high.

## gsScript.exportVariablesTo
- **Signature:** `script.exportVariablesTo(path: string, name1: string|array<string>, …) -> valueless`
- **Behavior:** Same as `exportVariables`, but writes the result to `path`. The template is still this script's own source file. This is how save parts are produced (`MAIN.$score.exportVariablesTo("memory/score.script", …)`).
- **Details:** Core `0x10018340` [obs unless marked]:
  - **Read template:** `scriptFilename` is read through callback `0x10081290`, the VFS loader with mustExist = 0 (`push 0` at 0x100812ac). A missing **or empty** file counts as "no template".
  - The text is split on `\n` and NUL, a `\r` before `\n` is dropped, and one trailing empty line is removed.
  - **Names:** each argument is one name or an array of names (type 6, iterated). A name is skipped silently if this script lacks it or its value slot is NULL. Names are narrowed to bytes.
  - **Match:** the line prefix is `"$" + name + " "`. The first existing template line starting with exactly that prefix is replaced. Otherwise the line is appended. Appended lines are not searched again, so a name passed twice is appended twice. A template line written as `$x=…` without the space never matches and stays as a duplicate.
  - **Line:** `"$name = " + value.export() + ";"`. The export uses vtable `+0x34`, the script-literal exporter (objects become `null /* cannot export object */`), and is narrowed by `0x10025170` (conversion method not checked).
  - **Write:** callback `0x100813b0` creates the engine writer `0x10080f00` for `path` (mode `"w"`). Every line, including the last, is written followed by `\r\n`, then the writer is deleted (vtable slot 0, i.e. `0x10081110` closes it and registers memory files).
  - A `memory/…` destination becomes an in-RAM VFS file that `writePackage` can bundle. Anything else is a disk file (no directory creation) [obs `0x10080f00`].
  - All non-matching template lines (code, comments) are copied unchanged. With only `path` given (argc 1), the template is copied to `path` as is.
- **Evidence:** `0x10019390.c:22-31`. Core `0x10018340.c:125-146` (read), `:147-198` (split), `:228-450` (array names), `:451-655` (single name), `:301-305`/`:513-517` (prefix), `:322-385` (search), `:386-399`/`:598-611` (line), `:659-689` (write). Constants at 0x10156c18: `"r"`, `"\r\n"`, `";"`, `"= "`, `" "`, `"$"`; 0x10156660: `"w"`. Callback install `0x10081e80.c:271-273`.
- **Confidence:** high.

## gsScript.mergeVariables
- **Signature:** `script.mergeVariables(other: gsScript) -> valueless`
- **Behavior:** Copies every variable of `other` into this script. A name that already exists is overwritten; a new name is appended to this script's value storage. Used for addon string tables (`$strings.mergeVariables($strings2)`).
- **Details:**
  - A non-`gsScript` argument does nothing (the reference is released) [obs]. A script without variables does nothing [obs].
  - Creates this script's symbol table (0x5c bytes, `0x1001a550`) if missing [obs].
  - New names get index = current map size, written with storage vtable `+0x40` setItem [obs].
  - Values are shared, not deep-copied: the same variant objects end up referenced by both scripts [obs addRef + store; inf no clone]. A possible extra addRef leak was not checked.
  - Iterates in `other`'s map order, sorted by byte-wise name [obs].
- **Evidence:** `0x1001aba0.c:16-36` → `0x1001a9e0.c:12-30`.
- **Confidence:** high.

## gsScript.listVariables
- **Signature:** `script.listVariables() -> array<string>`
- **Behavior:** Returns an array of this script's variable names (without `$`) in symbol-map order, sorted byte-wise. Returns an empty array if the script has no symbol table.
- **Details:** Names are widened back from narrow `std::string` (`0x10016c70` → `0x1002cf20`) [obs]. Values are not included [obs]. The array is built by `0x1002d150(items, count, capacity = argc(0), copy flag 0)` [obs]. No shipped script calls it [obs: native-api uses = 0].
- **Evidence:** `0x10017250.c:24-61`. Callees `0x10016c70.c`, `0x1002d150.c`.
- **Confidence:** high.
