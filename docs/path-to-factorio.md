# From here to Factorio built by kovc

Written 2026-10-24, answering "what is needed to compile the game with our compiler". The counts
are from the Factorio corpus (`src`, `tests`, `libraries/CommonUtil`, `libraries/Agui`, 1.33 M
lines) with plain `grep` over use sites, so they say how much code touches each thing, not how
hard each site is.

## Where kovc is

- **Front end.** Parses 99.27% of Factorio's files and types 77% of the expressions and 78% of
  the member accesses in 0.32 s on 32 threads. What it cannot type is the standard library
  (54 k `std::` references), macro-generated code and a tail of resolver gaps (templated
  aliases, dependent members through templated bases).
- **Code generation.** The Kov v0 subset runs end to end: classes with constructors, destructors,
  copies and operators, single inheritance with virtual functions, templates with full
  specialisation and parameter packs, lambdas, `if constexpr`, the own library (`String`,
  `Vector`, `HashMap`, `Optional`, `format`), C headers imported straight from `windows.h` and
  the UCRT. 15 end-to-end tests; the lexer written in Kov runs as fast as its C++ version.
- **Backend.** One IR module per group of files, clang in parallel, unchanged modules cached: a
  program with Factorio's number of function bodies builds in 1.7 s (debug), 1.25 s after a
  one-line change, 3.8 s of LLVM for release.
- **Language decisions made.** No preprocessor; `assert` statements, caller locations,
  `conditional` functions, typed `config` constants, `_` locals, attributes, real enums with
  `nameof` are in; `for constexpr`, reflection, typed unions and compiler tests are specified in
  design.md section 8 and wait for review.

## What the game needs that kovc does not have

| area | in Factorio | what it takes | size |
|---|---|---|---|
| **Standard library parity** | `std::string` 13,877 sites, `vector` 6,243, `unique_ptr` 4,068, algorithms (`sort`, `find`, `min`...) 4,619, `optional` 1,675, `function` 1,576, `map` 925, `pair` 873, `set` 745, `array` 500, `chrono`/`filesystem` 445, `deque` 131, `shared_ptr` 103, `unordered_map` 48, `variant` 32, streams 29; threads and locks 629, `atomic` 153 | `String`, `Vector`, `HashMap`, `Optional` exist. Missing: `UniquePtr`, `SharedPtr`, `Function`, an ordered `Map`/`Set`, `Pair`/`Tuple`, `Array`, `Deque`, `Span`, the algorithms, `Thread`/`Mutex`/`Atomic` over Win32, clocks, files and paths, `ssprintf`-style formatting | XL: the long pole, and every one of those 35 k sites goes through the converter |
| **Exceptions** | 3,198 `throw`, 746 `try`, 542 `catch`; used for loading, scripting and network errors | A decision first: implement them (LLVM landing pads with the Windows personality; unwinding through Lua's C frames needs care) or have the converter turn them into error values, which rewrites control flow. Implementing is the faithful route | L |
| **RTTI** | 647 `dynamic_cast`, 93 `typeid` | Type descriptors behind the vtable, `dynamic_cast` through them (single inheritance only, which is the common case) | M |
| **SIMD intrinsics** | 1,122 `_mm_*` calls (bitmaps, atlas, DXT compression) | Intrinsics as compiler builtins lowered to LLVM vector operations; imported `immintrin.h` declares nothing usable | M |
| **Statics and global objects** | about 5,000 `static` declarations; codegen rejects globals with constructors and arrays of objects | Static locals with one-time initialisation, global constructors in a defined order (per file, files in dependency order), arrays of objects | M |
| **`constexpr`** | 4,517 uses, mostly constants, some functions | The constexpr interpreter from the design (ints, bools, enums, simple functions); only literals and `const` initialisers fold today | M |
| **Smaller gaps** | multiple inheritance beyond the first polymorphic base, init-captures, member pointers, bit fields, `goto` (15), stream operators (136), partial specialisations (about 6) | Each small; the converter's error list tells which to do first | S each |
| **Macro replacements, the big four** | the X-macro lists, `ADD_VALUE` reflection, `InputAction`-style unions, UnitTest++ | design.md section 8: `for constexpr`, `members(T)`, `union enum`, `test` blocks (after review) | L in total |
| **Resolver tail** | 22% of expressions untyped today | Most of it is `std::`; the rest is templated `using` aliases, dependent lookups and ADL, fixed as the converter exposes them | M, continuous |
| **C libraries** | Lua (1,665 includes), SDL3 (69), curl, zlib, png, jpeg, opus, vorbis, zstd, openssl, minizip, stb_image, fontstash, nvapi, Logitech, CL | `extern "C" import` works; function-like macros such as `lua_pop` need the importer's inline-function rule or a small shim | S |
| **C++ libraries** | tracy (62), steam (62), glm (19), re2 (13), imgui (9), cxxopts, open-vcdiff, StackWalker; metal-cpp (193) is macOS only | Steam has a flat C API; tracy a C API; re2 and imgui need thin C wrappers; glm is a port (small); the rest are replaceable | M |
| **DirectX 11** | 17 includes, COM interfaces | COM is C-callable through vtables; the importer must take the `lpVtbl` form of the headers, or a thin C wrapper does it | M |
| **Debug info** | | `!dbg` metadata for CodeView so the game can be debugged in Visual Studio; not needed to run, needed to develop | M |
| **The converter** | 1.33 M lines, 8,056 files | Removes includes and forward declarations, inserts `this->`, `explicit`, `fallthrough`, rewrites enums, macros (per the survey) and `std::` to the library, reports global-scope name clashes; semi-automatic, with the compiler's errors as the worklist | L for the tool, XL for the iteration over the corpus |

Not needed for a Windows build: Metal, the Switch and Linux paths, PCH and unity-build
machinery, FASTBuild.

## The converter, first run (2026-10-24)

`kovc --convert OUT <dirs>` rewrites the parsed and resolved C++ into Kov: preprocessor lines
evaluated for Windows/MSVC/debug (`#include` of C headers becomes `extern "C" import`, the rest
goes), constant macros become `const auto`, other macros stay as `// TODO(kov)` comments,
forward declarations go, converting constructors get `implicit`, members get `this->`, a case
that falls through gets `fallthrough;`, `typename`/`template` disambiguators go, and `std::`
names map to the library's spelling (`std::string` to `String`, `std::vector` to `Vector`, 150
names). A header and its source of the same stem merge into one `.kov` file; `CONVERSION.md` in
the output lists what was done and what is left per file.

On `libraries/CommonUtil` (29 files, 4,140 lines): 21 Kov files, 1,008 `std::` names renamed,
109 forward declarations removed, 26 macros turned into constants, 4 kept as comments. kovc
parses the result 100% clean and types 88% of its expressions with the current library; the
unresolved rest is the library worklist, in order: `Pair` (350 uses), `StringView` (38),
`forward`/`move`, `min`/`max`, the type traits (`isArray`, `isSame`, `RemoveConst`...),
`WString`, `npos`, `RuntimeError`, `Set`, `Deque`, `Chrono`, and `String` methods (`begin`,
`end`, `push_back`, `erase`, `resize`, `find_first_not_of`, `replace`), then `va_list` and the
`trio_*` printf functions behind `ssprintf`.

Later the same day, with that library worklist done, exceptions implemented and hand-written
overrides possible (`programs/factorio/convert.cmd`, `--overrides`): 100% of files parse, 96% of
type names and 88.6% of expressions resolve, and code generation stops at 53 places (from 82
before the library work). By kind: 13 member accesses the library lacks (`rbegin().base()`,
`wstring_convert`/`codecvt`, `chrono` durations), 8 reference bindings and constructor calls
with no matching library constructor (`String` from a `char`, `Vector` from a brace list,
`StringView` into a `String&`), 7 locals of unknown type (the anonymous unions `Math.kov` uses
for float bit casts, `thread_local`, `auto begin = ...data()`), 6 unresolved calls (`va_start`,
`va_copy`, the `EXTRACT_WORDS` macro, `trio_snprintf`), 3 `String` to integer conversions in
`strtoint`, 3 unknown function types in a dropped `#if` region of `Demangle`, and single cases
of `Optional` in boolean context, `nullopt`, `NAN`, globals with constructors. Nothing in the
list is a design question; the chrono/codecvt/varargs files are the first candidates for
hand-written overrides, the rest is library and compiler work.

## CommonUtil compiles, links and runs (2026-10-25)

`programs/factorio/convert.cmd` now converts `libraries/CommonUtil`, compiles the result with
zero code generation complaints, links it (without `SystemClipboard`, which needs SDL) with
`programs/factorio/tests/smoke.kov`, runs it and compares the output with `smoke.expected`:
trimming, splitting, `ssprintf`, number parsing with its `InputException`, UTF-8/UTF-16
conversion, the fast float approximations and fdlibm's `pow`, `LowercaseString`, `CpuTimer`.

What it took, in the order it was found:

- **Hand-written overrides at definition granularity.** `--overrides DIR` used to replace whole
  files; now a file under `overrides/` is parsed, and every function, class or global it defines
  replaces the converted definition of the same qualified name (functions by arity; a name
  defined once replaces all its overloads, since the original's parameter list may hide behind
  a macro such as `FORMAT_STRING(...)`; a definition the parser did not recognise is removed by
  text). The rest of the override file is appended. Four files have overrides, 24 definitions:
  `CpuTimer` on the library's `Clock`, `widen`/`narrow` on `MultiByteToWideChar`, the trim
  functions without reverse iterators, `ssprintf`/`vssprintf` on `vsnprintf` and a static
  buffer, the four Math functions that used anonymous unions (now `bitCast<T>()`), fdlibm's
  word-access macros as small functions, and the unicode search map as a sorted static array.
- **Compiler features:** exceptions (section 9); free `operator+(const char*, const String&)`
  and friends found from either operand; conversion operators (`explicit operator bool`
  in conditions); `->` through `operator->`; converting constructors for `const T&` and by-value
  class parameters (`f("text")` with `f(const String&)`); globals with constructors or
  non-constant initializers (built at startup through `llvm.global_ctors`; destructors at exit
  do not run yet); arrays from nested brace lists and unsized arrays sized by their initializer;
  `va_start`/`va_end`/`va_copy` and functions defined with `...`; array parameters as pointers;
  MSVC integer suffixes (`1i64`); `(DWORD)-11` folded as a constant; integer literals typed by
  value as in C++ (`0xffffffff` is unsigned; before, `ui & 0xffffffff` kept all 64 bits);
  template parameter types recognised in casts (`(T)x`); out-of-line qualified definitions
  (`void StringUtil::f() {}`) attached to their namespace; a template declared in one block and
  defined in another; `T = const int` keeps its const when deduced through `T*`; function
  template specialisations get distinct names (resolution of calls to them is still naive);
  pointer arguments are no longer silently converted to integers; the linker drops unused
  functions (`-ffunction-sections`, `/OPT:REF`) and links the usual Windows libraries, `--lib`
  adds more.
- **Parser bug worth remembering:** `index < 0 || index >= this->length` was parsed as the
  template-id `index<0 || index>` followed by `= this->length`, because `>=` was split into
  `>` `=` as a template close. C++ splits only `>>`. Every such condition in the library and in
  converted code compiled to an assignment; the smoke test found it through `String::erase`.
- **Library:** `data()` on String, StringView, WString, Vector, Span (the fields are `chars` and
  `items` now); `back()`/`front()` by reference; `erase` by pointer range; `find_first_of` with
  a start position and a StringView set; `Vector(int count)`; `Optional` with `nullopt`,
  `value()`, `*`, `->`, `operator bool`; `InitializerList<T>` (call sites with brace lists are
  not generated yet); `Time.kov` (Duration, TimePoint, Clock on QueryPerformanceCounter);
  `bitCast`; `lowerBound` with a comparator; `Limits<float/double>::quiet_NaN()`; `NAN`; `abs`
  for floats. Converted `std::sqrt` and the other C library functions become `::sqrt`, so a
  namespace's own `sqrt` wrapper does not call itself.
- **Converter:** `<cstdint>`-style headers map to their C names, `stdio.h` joins the common
  imports, `conio.h` and more Windows headers import, `trio_*` becomes the C runtime's printf
  family, explicit instantiation declarations go, `std::` names in `#else` branches the parser
  never tokenized are renamed by text.

Still open before the next library (Agui, then the game): non-type template parameters
(`Array<T, N>`, `std::array`), calls to function template specialisations, brace lists passed
to `InitializerList` parameters, `thread_local`, destructors of globals at exit, wide string
literals (`L"..."`), and SDL as a linked library for the few CommonUtil functions that use it.

## The order that gets to a running game

1. Decide exceptions and RTTI; implement them (they shape what the converter emits).
2. Library parity, driven by the converter on `libraries/CommonUtil` first (313 includes, its
   own unit tests), then `Util`, then `Agui`: the first real modules of the game compiled and
   tested by kovc, with the resolver and codegen gaps fixed as they appear.
3. Statics, intrinsics, constexpr and the small language gaps in the order the converter reports
   them.
4. The section 8 features, which the macro rewrite needs.
5. Lua, SDL3 and DirectX through C import; a window with the game loop; then the rest of `src`
   module by module, measuring the typed percentage climbing to 100.
6. Debug info, then the persistent compiler for the edit-build-run loop.

The compiler-side items are each days to a couple of weeks at the pace of the work so far; the
long pole is the conversion of 1.3 M lines, mechanical once the converter exists but a long tail
of constructs and library idioms, so it should start on CommonUtil as soon as exceptions are
decided rather than after the compiler is "complete".
