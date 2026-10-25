# From here to Factorio built by kovc

Written 2026-10-24, answering "what is needed to compile the game with our compiler". The counts
are from the Factorio corpus (`src`, `tests`, `libraries/CommonUtil`, `libraries/Agui`, 1.33 M
lines) with plain `grep` over use sites, so they say how much code touches each thing, not how
hard each site is.

## Where kovc is

- **Front end.** Parses 99.54% of Factorio's files and types 77% of the expressions and 78% of
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

Still open from that round: calls to function template specialisations, brace lists passed to
`InitializerList` parameters, `thread_local`, destructors of globals at exit, wide string
literals (`L"..."`), and SDL as a linked library for the few CommonUtil functions that use it.

## Towards Agui (2026-10-25, later)

`libraries/Agui` (235 files, 132 Kov files after conversion) parses 100% clean and types 92.6%
of its expressions; code generation started at 1,208 complaints and stands at 249 after one
pass over the biggest categories (`compiler\build\tests\agui` is the scratch directory; the
scouting command is the check step of convert.cmd pointed at the Agui output):

- **Non-type template parameters** (`template<typename T, int N> struct Array`, `bool` defaults
  as in CommonUtil's `UniquePointer`): a value argument is a type of a new kind (`Value`, the
  constant and its type), so template argument lists, instantiation keys and substitution did
  not change shape; `N` in the body is a constant of the instantiation; defaults for
  parameters not written; `Array<char, bufferSize>` with a constant as the argument (the parser
  reads a bare name as a type, the resolver evaluates it). Test: `valueparams.kov`.
- **Alias templates** (`template<class T> using Ptr = UniquePointer<T>`), member typedefs
  through instantiations (`Vector<int>::iterator`, also through an alias of the class),
  out-of-line member definitions seeing their class scope for parameter types, reference
  members (initialised with the address, never destroyed), friend declarations dropped by the
  converter.
- **Pointers to data members**: `&Style::field` is the field's offset (an `i64`), `.*`/`->*`
  address it, `T C::*member` parameters deduce both the member and the class type: Agui's
  Style properties (`getProperty(&Style::defaultGraphicalSet)`, 290 sites) compile.
  Test: `memberptr.kov`. Pointers to member functions are not supported.
- **Library**: `UniquePtr<T>` with `makeUnique` (what `std::unique_ptr` converts to; CommonUtil's
  `UniquePointer` with a deleter exists beside it), `Queue<T>`, `Optional::emplace`, `at()`,
  `insert` at a position (also of a range), `replace`, `advance`, `makeOptional`, iterator
  typedefs; `!object` through the class's bool conversion, class-typed `?:`, `(void)x`,
  delegating constructors.

The second pass (2026-10-25, later) took Agui from 249 to 103 complaints:

- **`Function<R(Args...)>`** (stdlib/core/Function.kov): the compiler reads the signature and
  makes `Function<void(int)>` mean `FunctionImpl<void, int>`, a library class with the callable
  on the heap behind `invoke`/`destroy`/`clone` function pointers to member template
  specialisations; lambdas with captures, function pointers and functors; copied, moved,
  reset, tested for emptiness. Test: `function.kov`.
- **Value categories.** Overload resolution now knows an lvalue from an rvalue: an lvalue
  argument presents itself as `T&`, binds to no `T&&`, and a forwarding reference `T&&` given
  an lvalue deduces `T = U&` (reference collapsing), so `emplace_back(name)` copies and
  `emplace_back(move(name))` moves, as in C++. Code generation copies lvalues through the copy
  constructor only (it used to take the last same-class constructor, the move constructor, and
  emptied the source), moves from `move(x)` and temporaries, and gives classes without copy or
  move constructors memberwise moves. Deleted functions are diagnosed when chosen. Library:
  `Vector`, `String`, `WString` have move constructors and move assignment, `push_back(T&&)`.
  Test: `moves.kov`.
- Range-for over iterator objects (`rbegin()`, `ReverseIterator<T>`), init-captures
  (`[x = move(x)]`), pack expansions as the only constructor argument (`T(forward<Args>(args)...)`
  for copies, moves and aggregates), default arguments carried by the converter from a dropped
  header declaration to the definition, diagnostics naming the instantiation and who requested it.

The third pass (2026-10-25, later) took Agui from 103 to 6 complaints, with several fixes that
matter beyond Agui:

- **Overload resolution** now rejects a template whose deduction left a parameter dependent
  (`operator<<(UniquePtr<T>&)` given a `Label&` used to rank as "unknown" and win), checks
  viability even for a single candidate (`owner == raw` went through the explicit
  `UniquePtr(T*)` and deleted the object twice), deduces `T&` from a const lvalue as `const T`
  (so `forward<Args>` works for `const Event&` elements), treats a converting constructor from
  another instantiation of the same template as a conversion, and takes the callable's
  parameters into account when a `Function<R(Args...)>` is built from a lambda
  (`onSliderMove(Function<void()>)` next to `onSliderMove(Function<void(double)>)`).
- **Code generation**: calls through function pointers use the Kov calling convention
  (classes and references by pointer, sret); casts to references are lvalues (a `forward`
  returning a scalar reference used to return the address of a dead temporary); `?:` with two
  objects yields one of them; brace lists as arguments (`push_back({a, b, c})`,
  `max({a, b, c})` through `InitializerList<T>`), as ranges, and as member initializers;
  arrays of objects with constructors; assignment through converting constructors
  (`invertColors = InvertColors::True`, `view = string`); enumerators bound to references;
  `constexpr static` members as constants; `using Base::f` members; nested enums of a base
  through a derived class; local type aliases as variable types; out-of-line constructors of
  class templates (`template<typename Policy> ScrollBar<Policy>::ScrollBar(...)`).
- **Class template argument deduction** from a constructor (`ScopedSetter guard(x, true)`).
- **Converter**: function-like macros defined in a .cpp are expanded at their uses
  (`HANDLE_CASE`, `E(i, j)`), `unique_ptr<T[]>` becomes `UniqueArray<T>`, std algorithms and
  `move`/`forward`/`swap` are spelled `::name` (a class's own `move` or a namespace's `anyOf`
  no longer captures them), `a & b` and `a && b` in conditions are no longer read as reference
  declarations.
- **Library**: `HashSet<T>`, `UniqueArray<T>`, `remove`, `max`/`min` of a brace list,
  `Vector::insert` of rvalues and `erase` returning the next position, `cbegin`/`crbegin`,
  `String(first, last)`, `Optional == value`, `UniquePtr == pointer`/`nullptr`, `hashOf`
  for pointers. Tests: `conversions.kov`, `library2.kov`, `callables.kov`, `refparams.kov`,
  `fnpointer.kov`, `outofline.kov`.

Two more fixes closed the pass: a template body re-typed for an instantiation now looks its
qualifier up from the definition's namespace (`ScrollBar<Policy>::ScrollBar` in `agui` could
not see `TextButton`), and `typeid(x)` yields the library's `TypeInfo`: every vtable starts
with the class's name, so the dynamic type of a polymorphic object is one load away;
`typeid(T)` and non-polymorphic objects get their static name. Test: `typeid.kov`.

The generic lambda (an `auto` parameter) was the one construct left out at this point:
`Table::findFirstRowWhere` was a hand-written template in `programs/factorio/overrides/Agui/Widget/Table.kov`
until generic lambdas arrived the same evening (below); the override is gone and `Table.cpp`
converts as written.

## Agui links and runs (2026-10-25, later)

The converted Agui (132 files) plus CommonUtil link with `programs/factorio/tests/agui_smoke.kov`
into one program, and the program computes a layout: a `Gui` of 640 x 480 with its Void backend
(the backend Factorio uses for headless runs: fonts of 10 pixels per character and 20 per line),
a `Label` added to the top container, `logic()` once, and the label measures 100 x 20 for
"Hello Agui". convert.cmd builds it and compares the output with `agui_smoke.expected`, next to
the CommonUtil smoke test. Agui's style properties are inherited along a parent chain whose root
must define every property (the game fills the roots from its Lua prototypes); the smoke test
fills the three root styles by hand. `SystemClipboard` (SDL) is stubbed in `tests/stubs`.

Linking found four gaps: a function declared in several modules was declared twice in one
(declarations are now deduplicated per module), `= default` members referenced through vtables
or function pointers had no body (they are synthesised through the worklist), out-of-line
members of class templates were never paired with their declarations (the pairing used the
first part of the qualified name), and `static` written on the declaration only was lost on the
definition. Running found seven miscompiles that compile-time statistics, linking and the test
suite had all passed over, each now a test:

- Function-local statics were allocas (`static Style result(nullptr, nullptr)` in
  `Widget::getStyleStatic`): now a guarded global of the program. `staticlocal.kov`.
- Reference globals were objects (`static const Style& defaultWidgetStyle = getStyleStatic()`):
  now a pointer bound by the global constructor. Out-of-line definitions of static data members
  (`double Gui::scale = 1.0`, `const Style& Widget::defaultWidgetStyle = ...`) are paired with
  their in-class declarations, so the initializer is no longer lost, and a constant defined that
  way serves as an array bound. `staticmembers.kov`.
- `this` in a const member function was `C*`, so `this->getStyle()` picked the non-const
  virtual overload (null for the top container) over `getStyle() const`. `constthis.kov`.
- A global of class type without user constructors was zero-initialised even when the class has
  default member initializers, a vtable pointer, or members that need constructing.
  `constthis.kov`.
- `Derived*` to `Base*` skipped the adjustment to a base that is not at offset 0 (a polymorphic
  `Widget` over the plain `GenericTargetable`: the base sits after the vtable pointer), because
  both are `ptr` in LLVM; and a null pointer must stay null across that adjustment, in both
  directions. `nonprimarybase.kov`.
- An out-of-line static member emitted before any call to it kept a hidden `this`
  (`ResizableText::splitNewlinesOnly` read its arguments shifted by one and produced empty
  lines): the main pass now pairs every out-of-line definition with its declaration first.
  `staticoutofline.kov`.
- Default arguments of constructors called by default construction and of members of class
  templates (re-typed for the instantiation); `using namespace` inside a body. `defaultargs.kov`,
  `templatemembers.kov`, `outoflinedefault.kov`, `defaulted.kov`.

Crashes were located without a debugger: `--lib "-Wl,/MAP:file"` writes the linker map, the
smoke program installs a vectored exception handler that prints the faulting address relative
to the module and every stack slot pointing into the image, and a small script resolves them
against the map. The fastest route to each cause was to add `printf` lines to the converted
source (convert.cmd regenerates it), then shrink the finding to a probe against the standard
library alone (seconds to build, against 1.5 minutes with CommonUtil and Agui), then to a test.

What was left in Agui proper at that point: generic lambdas and `dynamic_cast` (both done since,
see below), pointers to member functions, thread-safe statics, destructors of globals at exit. Next: the SDL backend through C import and a
window, or the game's `Util` library, whichever the game's dependency order wants first.

## The specification suite (2026-10-25, later)

kovarex's direction for the next step: instead of learning what the compiler lacks by converting
and recompiling the whole game at every step, build a suite of small snippets, one per construct
the Factorio sources use, and let the suite be the specification of what is needed.
`compiler/tests/spec/<area>/<name>.kov` holds one program per construct with its expected output;
`compiler\tests\spec.cmd` compiles and runs them all (129 snippets in two seconds) and writes
[spec-status.md](spec-status.md), where every construct is pass or pending (a known gap, with the
compiler's first complaint and the construct's count in the game's sources). A pending snippet that
starts passing fails the run until its pending line is removed, so the document never goes stale.

The construct counts come from a regex survey over the 7,371 files of `src`, `CommonUtil` and
`Agui` (scratch script, numbers in the snippet headers): default arguments 32k, `override` 16k,
`std::string` 11k, range-for 8.6k, lambdas 5.3k, `std::vector` 5.2k, `std::unique_ptr` 3.8k,
`throw` 3.1k, operator overloads 2.9k, `std::string_view` 1.7k, `std::optional` 1.6k,
`std::function` 1.5k, `requires` 1.3k, `std::map` 827, `dynamic_cast` 443, generic lambdas 330,
`std::array` 372, `std::span` 147, variadic templates 175, fold expressions 110.

The first run found 49 gaps (80 constructs pass), among them several that the Agui smoke test had not reached: aggregate
initialisation drops default member initializers of omitted members, two-dimensional array
initialisation, assigning through a returned reference, binary literals, designated initializers,
`goto`, bit-field layout, `alignas`, structured bindings (one form crashes the compiler), partial
specialisations silently ignored, per-instantiation static members of class templates shared, the
recursive variadic pattern, `emplace_back(7)` into `Vector<int>`, `::sort` without a comparator,
and the library's missing `Map`, `Tuple`, `Array`/`Span`, `SharedPtr`, `Variant`, threads.
Small library additions came out of it too: `memset`, `abs(int)`, the C math functions,
`OutOfRange`/`InvalidArgument`, `UniquePtr = nullptr`.

## Working down the suite (2026-10-25, evening)

kovarex's rule for the suite: fix errors first. In that order, one commit per few fixes, each
checked against the suite, the 40 end-to-end tests and the CommonUtil and Agui smoke tests:

1. **Crashes.** `auto& [w, h] = s;` crashed the compiler: the parser only recognised bindings
   under a plain `auto`, and the resolver and code generator had nothing for them. Bindings now
   type and address the matching member (or array element) of the hidden object, by value and
   by reference.
2. **Silent miscompiles.** Aggregate initialisation dropped default member initializers of the
   members left out; `int a[2][3]` was built as `int[3][2]`; `cond ? a : b` was no lvalue, so
   `pick(x, y) = 20` wrote into a temporary; `0b1010` was 0; `return local;` copied instead of
   moving; two conversion operators shared one symbol; a `Derived*` to a base after the vtable
   pointer was not adjusted and null did not survive the adjustment (found the same day by the
   Agui smoke test).
3. **Silently ignored.** Partial specialisations (the primary was used), static members of class
   templates (one object for all instantiations), designated initializers (taken positionally),
   constexpr functions as array bounds (bound 0), requires clauses (not consulted), alignas,
   bit-field widths, dynamic_cast (static), `goto`.
4. **Compile-time gaps.** Enum values as template arguments; a template calling itself deducing
   its own parameters afresh; `Args&&...` packs under references and `T(forward<Args>(args)...)`
   for scalar T; alias templates (their parameters were invisible while the target was converted);
   `typename T::Unit` and `Num<I>::Type` as dependent member types resolved on substitution (a
   new type kind, `DependentMember`), with value arguments matching specialisations; nested
   classes of class templates carrying the enclosing arguments; out-of-line members of templates
   with value parameters; default template arguments of function templates after deduction
   (`EnableIf` removes a candidate); `Conditional`; `sizeof(T[N])`; wide string literals; calls
   leaving a required parameter without an argument are now an error instead of a call with
   garbage (which is what `agui::label()` in the smoke test had been).
5. **Library.** `Map`, `Array<T, N>`, `Span<T>` (Vector and Array convert to it), `Tuple` with
   `get<I>`, `SharedPtr`/`WeakPtr`, `Thread`, `Mutex`, `LockGuard`, `UniqueLock`,
   `ConditionVariable`, `Atomic` (SRW locks, CreateThread, and `kov_atomic_*` builtins the
   compiler turns into atomic instructions), `Vector` from a brace list, `memset`, `abs(int)`,
   the C math functions, `OutOfRange`/`InvalidArgument`.

6. **Generic lambdas** (330 Factorio uses) and deduced returns of function templates. `[](auto x)
   {...}` is a template: the parser invents a type parameter per `auto` parameter (as C++ does),
   the closure carries the parameter list, and the call operator is typed per call. The type
   checker keeps, for every lambda with template parameters, the environment it was written in
   (the enclosing locals, scope, `this`, instantiation); a call deduces the lambda's parameters
   from the arguments like a function template's, types the body in that environment once per
   set of arguments, and takes the result type from there. The code generator emits one call
   operator per instantiation, at the first call and after the call's arguments: a lambda
   argument's capture list is read where it is written, so its closure has to exist before the
   operator's body calls it (found by Agui's `Table`, where a predicate lambda captures another
   lambda and `this`; the closure of a `const auto f = [this](int) {...}` is the expression's,
   found by `ElementImageSet`). A lambda type depends only on parameters it does not bind itself,
   so a generic lambda can be a template argument. `auto&`, `const auto*` and plain `auto`
   returns of function templates take the written shape of the deduced type and are recorded on
   the call like any deduced arguments. `Table.cpp` converts as written; its override is gone.
7. **`for constexpr`** (design.md 8.2), the first of the section 8 features: `for constexpr
   (typename T : types<A, B>)` and `for constexpr (int i : {1, 2, 4})` inside functions. The loop
   variable is a template parameter of the body; the body is re-typed and generated once per
   element exactly as a pack expansion is (the resolver's per-instantiation re-typing with the
   element bound, then the statement emitted under those arguments), so `T::name()`,
   `squared<i>()` and `int buffer[i + 1]` work in it. Spec: `kov/for_constexpr_values`,
   `kov/for_constexpr_types`, `kov/for_constexpr_constant`; `kov/for_constexpr_switch` (one
   `case` per type) is the next step.
8. **Reflection** (design.md 8.3) on the same machinery: `for constexpr (auto m : members(T))`
   binds `m` to a member descriptor (a new compile-time type kind holding the member, its type
   in the class's instantiation and the class), `m.name` is a string constant, `m.offset` the
   field's offset, and `object.[m]` the member itself (an lvalue, so save and load loops can
   read and write it). Spec: `kov/reflection_members`, `kov/reflection_bases_offsets`.
9. **`E::fromStr(text)`** (design.md 8.4): generated on every Kov enum as `static Optional<E>
   fromStr(const char*)` and lowered to the library's `enumFromStr<E>`, written in Kov over
   `E::values` and `str()`. Found on the way: a qualified name through a template parameter
   bound to an enum (`E::values`, `E::count` with E = Direction) stayed dependent; the lookup now
   follows enum substitutions as it did class ones. Spec: `kov/enum_from_str`,
   `templates/enum_through_parameter`.
10. **Typed unions** (design.md 8.5), the shape of `InputAction` and `std::variant` uses. The
    parser desugars `union enum Property : u8 { { {int, Integer}, {String, Text} } ... }` to a
    class with a nested Kov enum `Tag` (unscoped, so `Property::Integer` is the tag value), a `tag`
    member and an anonymous union of one storage member per alternative; the type checker gives
    `p[Property::Integer]` the alternative's type and `p == Property::Integer` bool; the code
    generator makes `p[E] = v` destroy the alternative held, set the tag and construct the value,
    checks the tag on every other `p[E]` (release_assert), switches on the tag for `switch (p)`
    (exhaustive like a Kov enum), and copies, assigns and destroys by the tag. Spec:
    `kov/typed_unions`, `kov/typed_unions_switch_copy`.
11. **`case` labels from `for constexpr`** (the third example of design.md 8.2): a switch body
    walks its `for constexpr` statements once per element to collect the labels (the Kov enum
    exhaustiveness check sees them too) and once more to emit them. **Pointers to member
    functions** (3 Factorio uses): `int (Calc::*op)(int) const = &Calc::mul;` parses as a
    declaration whose type keeps the class and the function type, `&Calc::mul` is the function's
    address (virtual ones are refused for now), and `(c.*op)(3)` calls through it with the
    object first, sharing the indirect-call emission with function pointers. Spec:
    `kov/for_constexpr_switch`, `classes/member_function_pointers`.
12. **Template template parameters** (one Factorio use): `Container<T>` with `Container` a
    template template parameter is the parameter applied to arguments (a parameter type carrying
    them); deduction against `Vector<int>` binds `Container` to the template itself and `T` to
    `int`, and substitution applies the bound template to the substituted arguments. **The SDL
    header import** passes in the suite now that the runner hands kovc the game's SDL3 include
    directory when the Factorio checkout is present: `SDL_Rect` and `SDL_INIT_VIDEO` come straight
    from `SDL3/SDL.h`. Spec: `templates/template_template_parameters`, `interop/sdl_header`.

The suite went from 80 pass / 49 pending to 137 pass / 1 pending; the Factorio front end stayed
at 99.27% parsed and 77.6-77.8% typed (members of dependent qualifiers now count as dependent
instead of being guessed from the primary template). Left pending, in order of Factorio use:
only `Variant`, which stays pending by decision (a `std::variant` becomes a typed union; the
converter will need a mapping for the 24 uses). Not in the suite yet, and still open in design.md
section 8: `m.type`, `enumerators(E)`, `alternatives(U)`, and the `--tests` runner.

## src/Util through the converter: the first run (2026-10-25, night)

With the suite's compiler-side list empty, the next module of the roadmap is the game's `Util`
(370 files, 47k lines; 144 .cpp and 226 .hpp merged into 304 Kov files). The first run, with
CommonUtil and Agui as context and nothing hand-written yet:

```
compiler\build\kovc.exe --no-std --quiet --convert OUT --overrides programs\factorio\overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui C:\Factorio\src\Util
compiler\build\kovc.exe --std stdlib --cinclude C:\Factorio\libraries\SDL3\include --errors 60 --unresolved 60 --emit-llvm out.ll OUT\CommonUtil OUT\Agui OUT\Util
```

| | |
|---|---|
| files parsing clean | 96.26% (43 parse errors in 18 files) |
| expressions typed | 85.4% (7.3% dependent, 7.3% unknown) of 340k |
| members bound | 82.9% (4.5% on an object of unknown type, 3.2% no such member) |
| unresolved names | 8,719 (mostly other modules: `GlobalContext`, `Log`, `Map/MapSerialiser`, `RuntimeError`, Lua) |
| std names without a mapping | ~80 kinds: traits (`is_trivially_*`, `is_arithmetic_v`...), `variant_size_v`/`visit`, `priority_queue`, `reverse_iterator`, `future`/`async`, `strong_ordering`, `time_t`/`localtime`, `uninitialized_*` |

The parse errors fall into two piles. Compiler gaps, each now a pending snippet: declaration
specifiers after the type (`float static f()`, `void inline g()`, `const static int x[]`),
array and function types as alias targets (`using Row = T[N]`, `using Unary = double(double)`),
operator functions called by name (`!operator==(l, r)`), `__stdcall` inside function pointer
types, a `requires` clause between a member's qualifiers and its body, and deduction guides with
`using Ts::operator()...` (`make_overload`). Converter bugs, to fix in `convert.cpp`: default
arguments `= {}` written as `= ()`; a removed friend or macro-generated declaration leaving its
`template<...>` header or `[[noreturn]]` attributes behind; the printf-format attribute macro
written as `(4, 5)` after the parameter list; a macro standing for `extern "C"` turned into a
constant; and `__try`/`__except` (SEH in CrashHandler and SystemUtil, which want override files
rather than a translation). The C import also needs curl's include directory (`Util/Http`), and
the 8.7k unresolved names say Util cannot be compiled alone: the game's root headers (`Log`,
`GlobalContext`, `RuntimeError`), `Map` and the Lua binding come with it, so the next conversion
set is Util plus those, with stubs for what the smoke tests do not touch.

The second pass, the same night, closed every parse error. Parser: specifiers after the type
(`float static f()`, `void inline g()`, `const static int x[]`), array and function types as
alias targets, operator functions called by name, calling conventions dropped from the tokens of
every source (as the C importer already did), the `requires` clause before a body no longer read
as a braced construction, and deduction guides parsed (and dropped by the converter: Kov deduces
from constructors). Converter: a carried default argument is the text after its `=` with std
names mapped (`= std::string()` had become `= ()`), a removed friend or declaration takes the
`template<...>` header and `[[attributes]]` written above it, `FORMAT_ATTR(4, 5)` goes with its
arguments, `NTAPI`/`WINAPI`/`EXTERNC` are known macros, and `__try`/`__except` become
`try`/`catch (...)` with a todo naming the dropped filter. Util: 0 parse errors in 304 files,
85.4% of expressions typed. The whole Factorio front end moved with it, from 99.27% to 99.54% of
files parsing clean (93 errors in 37 files, down from 208 in 59). Spec: `language/specifier_order`,
`language/alias_array_function_types`, `language/explicit_operator_call`,
`interop/calling_convention_pointers`, `templates/requires_clause_before_body`;
`templates/deduction_guides` stays pending (the `Overload{lambdas...}` idiom needs `using
Ts::operator()...` and aggregate deduction).

## The order that gets to a running game

1. Decide exceptions and RTTI; implement them (they shape what the converter emits).
2. Library parity, driven by the converter on `libraries/CommonUtil` first (313 includes, its
   own unit tests), then `Agui`, then `Util`: the first real modules of the game compiled and
   tested by kovc, with the resolver and codegen gaps fixed as they appear. (CommonUtil and
   Agui compile, link and run smoke tests as of 2026-10-25.)
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
