# Macros in Factorio, and what replaces them in Kov

Survey date: 2026-10-23. Corpus: `C:\Factorio\src`, `tests`, `libraries\CommonUtil`,
`libraries\Agui`: 8,056 files, 1.33 M lines, the same corpus as [prototype-results.md](prototype-results.md).
Every number below comes from `compiler\bench\macro_survey.py` and `macro_categories.py`
(see "How to reproduce"); the categories are assigned by hand-written rules and checked by eye.

## Headline

- Factorio's own code defines **611 macros** (467 distinct names: 422 function-like, 189
  object-like) and expands them at **22,647 places**, one per 59 lines. The macros are
  concentrated: 6 names (`assert`, `ADD_VALUE`, `max`, `MAKE_FOR_TABLE`, `release_assert`,
  `min`) make 62% of the use sites, 20 names make 85%.
- Library macros on top of that: UnitTest++ about **55,800** uses (`CHECK_EQUAL` 21,971,
  `CHECK` 21,946, `TEST` 7,206, `TEST_FIXTURE` 1,145, `CHECK_CLOSE` 834, `SUITE` 556, ...),
  SDL 1,925 (nearly all constants), Metal `MTL` 689, OpenGL `GL_*` 467, the Lua API about 850
  (`lua_pop`, `lua_newtable`, `lua_isnil`, `luaL_*`, `LUA_T*`), `M_PI` 177, `INT32_MAX` and
  friends 141, Tracy `ZoneScoped` 126, Windows `FAILED`/`SUCCEEDED`/`HIBYTE`/`LOBYTE` 226.
- Directives: 58,189 `#include`, 4,152 `#pragma` (once), **2,098 conditional blocks**
  (`#ifdef` 1,457, `#ifndef` 414, `#if` 227, `#elif` 70) keyed by about 40 symbols, 261 `#undef`.
- kovc's resolver sees the same picture from the other side: of the names it cannot resolve
  in Factorio, 14,773 are macro-like (`assert` 5,255, `release_assert` 1,728, `lua_pop`,
  `ZoneScoped`, ...) and a further group are enumerators and typedefs that only exist inside
  macro expansions (`normalQuality` 577, `Nothing` 268, `PrototypeType` 267, `Success` 235).
- **Every use falls into one of eleven cases, and none of them needs textual substitution.**
  Eight language or library features cover all eleven; the table in "What Kov needs" ranks
  them by how much code they remove. Kov can have no preprocessor at all.

## The eleven cases

| case | own macros | use sites | Kov answer |
|---|---:|---:|---|
| A. assert, abort, log with source location | 39 | 9,487 | built-in `assert`, implicit caller location, conditionally compiled functions |
| B. test suite | 37 (+UnitTest++) | 1,880 (+55,800) | tests and `check` in the compiler |
| C. member names as strings | 28 | 4,733 | compile-time reflection over members, `nameof` |
| D. lists of types/values, code generated per entry | 83 | 770 | `for constexpr` over type lists at every scope, typed unions, no explicit instantiation |
| E. enum and bit-field definition machinery | 63 | 254 | real enums with `str()`, iteration, count; `Flags<E>` in the library |
| F. scoped objects that need a unique name | 12 | 819 | unnamed locals (`_`) living to the end of the scope |
| G. compiler modifiers, pragmas, warnings | 16 | 361 | attributes; explicit-by-default removes the warning pragmas |
| H. configuration flags, platform conditionals | 36 (+2,098 `#if` blocks) | 206 | typed `config` constants and `if constexpr` at every scope |
| I. constants and string tables | 78 | 222 | `const` globals (order-independent, so no header needed) |
| J. aliases and library workarounds | 32 | 3,691 | disappear with C import that does not import function-like macros |
| K. inline code snippets | 44 | 224 | inline functions, lambdas, constexpr evaluation |
| | **468** | **22,647** | |

### A. Assertions, aborts and logging that need the source location (9,487 uses)

`assert` 4,665 (Factorio redefines it in `Common.hpp` so that a debugger breaks in place and
tests throw), `release_assert` 1,564 (`ReleaseAssertFailed(__FILE__, __LINE__, #value)`),
`LOG_AND_ABORT` 978, `LOG_ENUM_AND_ABORT` 629, the `LOG_*` family 1,365 (`Logging::log(__FILE__,
__LINE__, LogLevel::X, ...)`, with `LOG_DEBUG`/`LOG_VERBOSE` compiled to `((void)0)` outside debug
builds), `LOG_D3D_ERROR`/`LOG_METAL_ERROR`/`OPENGL_INTERFACE_CHECK_ERRORS` (`__func__`, `__LINE__`).

Three things are needed, and they are the same three everywhere: the text of an expression, the
caller's location, and "compile to nothing in this configuration, do not evaluate the arguments".

Kov answers with three parts that work together; they are not alternatives, each covers a
different part of the list above:
- For `assert` and `release_assert` (6,229 uses): `assert(expr)` and `release_assert(expr)` are
  statements of the language. The compiler has the
  expression text and the location; a failed assert reports both and the values of the operands
  (it knows the AST, so `assert(a == b)` prints `a` and `b`). `assert` compiles to nothing when
  `config.debug` is false.
- For the `LOG_*` family, `LOG_AND_ABORT`, `LOG_ENUM_AND_ABORT` and the graphics error helpers
  (about 3,100 uses): any function can take an implicit caller location. A parameter declared
  `CallerLocation here = caller` is filled at each call site. `logError("...", args...)` is then a plain variadic
  function using the type-safe `format` (no `printf` attribute, see G).
- For what must vanish from a build (`LOG_DEBUG`, `LOG_VERBOSE`, `ZoneScoped`, dev-only tools):
  a function can be declared conditional on a compile-time constant,
  `conditional(config.debugLog) void logDebug(...)`. When the condition is false, calls to it
  compile to nothing and their arguments are not evaluated. This covers the debug log levels,
  profiling zones (`ZoneScoped`) and dev-only tools without a macro per level.

### B. The test suite (1,880 own uses plus about 55,800 from UnitTest++)

UnitTest++'s `TEST(name) { ... }` registers a test through a static object, `SUITE` is a
namespace, `TEST_FIXTURE` derives the test body from a class, and the `CHECK*` family captures
the expression text, the location and the operand values. Factorio adds 37 helpers on top
(`CHECK_BUILDABILITY` 712, `CHECK_LUA_NUMBER_EQUALS` 707, `SETUP_MUSIC_*` 110,
`TEST_GENERATOR(Name)` with `class TestGenerator_##Name`), all of them wrapping `CHECK_EQUAL`.

Kov: the proposal's "test tools" item. `test "name" { ... }` is a declaration the compiler
collects and the standard runner selects by name, file or suite; fixtures are ordinary classes;
`check(a == b)` is built in and reports like `assert` does (text, location, operand values), with
`check` failures recorded instead of aborting. With that, the 55,800 library uses and all 37 helper
macros become functions and lambdas.

### C. Member names as strings (4,733 uses)

The prototype explorer lists every member of every prototype: `ADD_VALUE(sprite)` expands to
`Values::addValue(sprite, demangleMemberName("sprite"), "sprite", result)`; 2,653 of these in 257
files, plus `MAKE_FOR_TABLE(NAME)` 1,657, `SKIP_VALUE` 114, `ADD_ENUM`, `MAKE_ENUM_FOR_TABLE`.
Smaller versions of the same idea: `SAVE_UTILITY_CONSTANT(stream, KEY)` writes `"KEY:" value`,
`EQUALS_COMPARE(VALUE)` compares `this->VALUE != other->VALUE`, `FOR_EACH_FIELD(MACRO)` with
`MAKE_GETTER`/`MAKE_SETTER` and `VARIABLE_NAME(field) _##field` builds accessors for 13 fields.
`MAKE_VERIFIER(Type)` (106) is an explicit specialisation per type.

This is reflection written by hand, and it is also what the serialisation (`save`/`load`,
the proposal's `accumulate`) and the Lua binding registration do with ordinary code today.

Kov: compile-time reflection over members. `for constexpr (m : members(T))` iterates the
members of a class with `m.name`, `m.type` and `object.[m]` for the value; the loop body is
instantiated once per member like a template body. `nameof(x)` gives the spelling of a single
expression. The prototype explorer becomes one generic function instead of 4,300 lines of
`ADD_VALUE`, and generated `save`/`load` becomes possible (the proposal already notes that
generating them from the class may beat `accumulate`).

### D. Lists of types or values with code generated per entry (770 uses, 83 macros)

Fifteen X-macro lists, 2 to 521 entries each, expanded 2 to 34 times:

| list | entries | expansions |
|---|---:|---:|
| `CALL_ON_EVERY_STYLE` | 521 | 4 |
| `CALL_ON_EVERY_INPUT_ACTION_TYPE_WITH_DATA` | 284 | 12 |
| `CALL_ON_EVERY_INPUT_ACTION_DATA_TYPE` | 164 | 4 |
| `CALL_ON_EVERY_EDITOR_ACTION_TYPE_WITH_DATA` | 143 | 12 |
| `CALL_ON_EVERY_EDITOR_ACTION_DATA_TYPE` | 69 | 4 |
| `CALL_ON_EVERY_INPUT_ACTION_TYPE_NO_DATA` | 59 | 8 |
| `CALL_ON_EVERY_CORE_PROTOTYPE` | 49 | 4 |
| `CALL_ON_EVERY_ID_TYPE` | 42 | 34 |
| `CALL_ON_EVERY_STYLE_TYPE` | 31 | 4 |
| `CALL_ON_EVERY_ACTION_RESULT` | 21 | 2 |
| `CALL_ON_EVERY_EDITOR_ACTION_TYPE_NO_DATA` | 15 | 8 |
| `FOR_EACH_FIELD` | 13 | 5 |
| `CALL_ON_EVERY_VULKAN_INSTANCE_FUNCTION` | 6 | 4 |
| `CALL_ON_EVERY_CORE_ID` | 4 | 4 |
| `FOR_EACH_SUPPORTED_TYPE` | 2 | 6 |

Each expansion passes a per-entry macro that generates one of: an enumerator (`ENUM_DEFINITION`,
`DEFINE_ENUM_ENTRY`), a `case` (`CLONE(name)`: `case Type::name: return make_unique<name##Message>`,
`HANDLE_CASE` 123, `CALL_EMPLACE`), a member or constructor (`CREATE_UNION_GETTER`,
`MAKE_NAMED_CONSTRUCTOR`, `DEFINE_UNION_ENTRY type* wrapped##type`), a registration call
(`REGISTER_LOADER`, `ADD_ACTION` 127 with a lambda per input), a string-table row
(`"custom-input"`), or an explicit template instantiation (`EXPLICIT_COMBINATION`,
`EXPLICIT_INSTANTIATION`, `EXPLICIT_COUNTS_AS_BUTTONS`; Factorio has 612 `template class` and 111
`template function` instantiation lines in total, most of them behind these macros).

Kov, four pieces:
- A list is a compile-time value: `const auto idTypes = types<CustomInputID, EquipmentGridID, ...>;`
  or a `const` array of structs for lists with data (`{ItemID, "item"}`), and `for constexpr`
  iterates it. The loop is allowed at **every scope**: in a function it generates statements
  (`case` labels included), in a class it generates members, at namespace scope it generates
  declarations. This is the same mechanism as the member iteration in C; it makes `##` pasting
  unnecessary because the list holds the types themselves (`CloneMessage`), not name fragments.
- The proposal's typed union (`union enum`) replaces the `InputAction` family: the enum, the
  union, the `switch` that constructs the right alternative and the typed getters are one
  declaration.
- Explicit instantiations are not a concept: one compiler instantiates on demand, exactly once.
- `ADD_ACTION(actions, control, method)` is a function taking a member-function reference or a
  lambda; it was a macro only for the `this->METHOD()` pasting.

### E. Enum and bit-field definition machinery (254 uses, 63 macros)

`Util/Enums.hpp` is 58 macros generated by a Python snippet (`_ENUM_VALIDATE1..16`,
`_ENUM_TOSTR1..16`, `_ENUM_FROMSTR1..16`, `_ENUM_COUNT`, `_ENUM_EXPAND(X) X // MSVC fun`) so that
`ENUM(Name, Type, A, B, C)` can produce a class with `isValid()`, `str()`, `fromStr()`, `count` and
iteration for up to 16 enumerators; `BITFIELD(Name, ...)` (20 uses) does the same for flag sets.

Kov: the proposal's real `enum class` with methods, `str()`, `fromStr()`, `count`, iteration and
exhaustive switches makes the whole file unnecessary; a library `Flags<E>` built on enum
reflection (`count(E)`, `enumerators(E)`) replaces `BITFIELD`.

### F. Scoped objects that need a unique name (819 uses)

`SAVING_TAG(output, "item", ...)` (646) declares `Serialiser::TagWriter _tagWriter<LINE>(output, ...)`,
a guard object that writes on construction and destruction; `SAVING_STATISTICS` (40) is the same.
Tracy's `ZoneScoped` is the same pattern from a library. `INIT_SPRITE_DRAWING`/`PUSH_TO_*` in
`DrawQueue.cpp` (127) are a local protocol: one macro declares `sortOrder_`, the others use it.

Kov: `_` is a name that may be declared any number of times in a scope; each declaration is a
distinct object that lives to the end of the scope: `TagWriter _(output, "item");` (C++26 adopts the
same placeholder). The `DrawQueue` protocol is a lambda or a small local class.

### G. Compiler modifiers, pragmas and warnings (361 uses)

`VCALL` (`__vectorcall`, 127), `DEV_TOOL_VARIABLE` (`inline` or nothing, 42),
`BEGIN/END_SUPPRESS_CONVERSIONS` (`#pragma warning(disable: 4244)`, 76),
`BEGIN/END_FORCE_OPTIMIZE` (`#pragma optimize`, 46), `FORMAT_STRING`/`FORMAT_ATTR`/`TAG_FORMAT_ATTR`
(`__attribute__((format(printf)))` and MSVC's `__format_string`, 43), `NO_INLINE`, `SUPPRESS_UBSAN`,
`ALWAYS_INLINE`, `IMGUI_API`, `_SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING`.

Kov: attributes (`[[noinline]]`, `[[forceinline]]`, `[[vectorcall]]`, `[[optimize]]`,
`[[no_sanitize]]`) on declarations. The format attributes are replaced by the type-safe
`format`; the conversion-warning pragmas have nothing to suppress in a language that is explicit by
default; the portability shims exist because there are three compilers, and Kov has one.

### H. Configuration flags and platform conditionals (36 flag macros, 2,098 `#if` blocks)

The conditions, by number of blocks: `NINTENDO_SWITCH` 479, `DEBUG` 378, `WIN32` 268,
`FACTORIO_PACKAGE` 148, `MAC_OSX` 100, `LINUX` 90, `FACTORIO_USE_METAL` 74, `STEAM` 72, `DEMO` 71,
`NINTENDO_SWITCH2` 60, `DEV_ONLY_DEBUG_TOOL` 54, `HAS_SSE2` 40, `FACTORIO_USE_D3D` 37,
`DISABLE_UPDATER` 22, `IMGUI_ENABLED` 17, `NOISE_DEBUG_ENABLED` 16, `HEADLESS` 11, and a tail of
feature toggles (`ENABLE_DEBUG_LOG`, `DEBUG_NAVMESH_STATS`, `USE_POSIX_POLL`, ...). Own flag macros
are empty defines read by `#ifdef`, or `0`/`1` constants (`DEBUG_BAD_SHIFTINGS 0`,
`CRC_CHECK_SAVE_LOAD 0`). The blocks wrap whole classes and members (`class InNintendoNetworkPrepare`
exists only on Switch), single statements, and `#include`s.

Kov: the configuration is a set of typed compile-time constants given by the build (the "global
macros" of design.md, made typed): `config.platform == Platform::Switch`, `config.debug`,
`config.steam`, `config.graphics == Graphics::Metal`. `if constexpr` on them is allowed at every
scope, so a class, a member or a function can exist only on one platform without a preprocessor,
and a file can state `requires config.platform == Platform::Switch;` at its top to be skipped
entirely elsewhere. Debug-only members (`DEV_TOOL_VARIABLE`) are `if constexpr (config.devTools)`
around the declaration.

### I. Constants and string tables (222 uses)

`UTF8_BULLET "\xE2\x80\xA2"` and 20 more UTF-8 strings, `M_1_2_PI`, `INSET_COLOR_SHIFT 4`,
`C565_5_MASK 0xF8`, `BUILD_MODE "steamdemo"`, `RZRESULT_*` from a vendor SDK, key names.
Kov: `const` globals. They are macros today because a constant in a header needs the header included
everywhere; in Kov every global is visible everywhere.

### J. Aliases and library workarounds (3,691 uses)

`#define max Math::max` and `min` (3,440 uses, in `GDIPlus.hpp`) exist because `windows.h`
defines `min`/`max` as function-like macros and breaks everything that follows; `fseeko64`/`ftello64`/
`fileno` map POSIX names to MSVC ones; `vk*`/`cl*` names are redirected to dynamically loaded
function pointers (`VulkanImpl::`, `OpenCLImpl::`); `load_m128i` casts for an intrinsic;
`toCStr(x)` is `toStr(x).c_str()`; `STR(x)`/`ARG_STRING_TYPE` paste `L` onto string literals.

Kov: C import brings in declarations and constants, not function-like macros, so `min`/`max` cannot
be hijacked; the rest are functions, `if constexpr` on `config.platform`, and String conversions.

### K. Inline code snippets (224 uses)

`E(x, y)` (matrix multiply term, 51), `R_SHUFFLE_D(x, y, z, w)` (an immediate for SSE shuffles,
33), `WRITE_TO_VERTEX` (16), `CALCULATE_VERTICES` (a 64-line macro with 15 parameters, 8),
`SET_LOW_WORD`, `APPLY_FLIP_FLAGS`, `AVG_COLORS8`. Written as macros for speed in debug builds or
because the result must be a compile-time constant.

Kov: `[[forceinline]]` functions and lambdas; the constexpr interpreter evaluates
`R_SHUFFLE_D(3, 2, 1, 0)` at compile time where an immediate is required.

### Library macros

- UnitTest++ (55,800): case B.
- Tracy (`ZoneScoped` 126): cases A and F; Tracy is C++, so it needs a thin C wrapper or a Kov port.
- Lua (about 850): `lua_pop(L, n)` is `lua_settop(L, -(n)-1)`, `lua_newtable(L)` is
  `lua_createtable(L, 0, 0)`, `lua_isnil(L, n)` is a comparison; `LUA_T*` and `luaL_*` are constants
  and functions. The constants come through C import already. For the function-like ones the
  importer can turn a macro whose body is an expression over its parameters into an inline function
  with `auto` parameters; that handles Lua's, SDL's (`SDL_BUTTON(x)`) and Windows' (`HIBYTE`,
  `FAILED`) alike. Macros that are statements or declare things stay out.
- `M_PI`, `INT32_MAX`, `SDL_*`, `GL_*`, `MTL*` constants: imported as constants today.

## What Kov needs, by payoff

| | feature | removes | status in the proposal |
|---|---|---:|---|
| 1 | tests and `check` in the compiler | 57,700 uses | listed ("test tools") |
| 2 | built-in `assert`, implicit caller location, `conditional` functions | 9,500 uses | new |
| 3 | compile-time reflection: members with names, enumerators, `nameof` | 4,700 uses, plus the generated save/load | new (replaces part of `accumulate`'s motivation) |
| 4 | `if constexpr`/`for constexpr` at every scope, with typed `config` constants | 2,100 `#if` blocks, 770 list expansions, 206 flags | partly ("global macros") |
| 5 | typed unions | the `InputAction`/`EditorAction` families, `UpgradableEntity` | listed |
| 6 | real enums with `str()`, iteration, count | 254 uses and all of `Enums.hpp` | listed |
| 7 | unnamed scoped locals `_` | 819 uses | new |
| 8 | attributes for the few real compiler modifiers | 361 uses | new, small |

Cases I, J and K (4,100 uses) need nothing new: constants, functions and C import as it is.

Kovarex's two groups match this table: "modifiers imported from the compiler" are G plus the
portability aliases in J (about 600 uses, trivially fixed), and "solving language shortcomings"
is everything else, with assertions, tests and hand-written reflection far ahead of the
X-macro lists that are the most visible.

## Consequence for the design

Kov needs no preprocessor. The conditional compilation goes through typed constants and
`if constexpr`; the two code-generation uses (member iteration and type lists) go through
`for constexpr` with reflection; everything else is a function, an attribute or a built-in
statement. The "file-local macros" kept open in design.md have no use in this corpus and can be
dropped; the "global macros" become the `config` constants. The converter from Factorio C++ can
translate every case above mechanically except D, where the list macros have to be rewritten as
typed lists by hand (15 lists).

## How to reproduce

```
python -X utf8 -I compiler\bench\macro_survey.py        (writes macro_survey.txt/.json to the current directory)
python -X utf8 -I compiler\bench\macro_categories.py    (reads macro_survey.json, prints the categories)
compiler\build\kovc.exe --unresolved 70 C:\Factorio\src C:\Factorio\tests C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui
```

The survey script joins backslash-continued lines, matches `#define`, strips comments and string
literals before counting identifier occurrences, and counts a use as any occurrence of the name
outside its own definition line (so a macro used inside another macro's body counts once per body,
not once per expansion). The UnitTest++ numbers are plain identifier counts over the same files.
