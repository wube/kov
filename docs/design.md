# Kov design

Kov is a C++-like language with a new compiler built around one idea: **no includes at the
deepest level of how the compiler works**, so that builds are orders of magnitude faster.
The original proposal lives in this repository: [readme.md](../readme.md) (language changes)
and [compilation/index.md](../compilation/index.md) (compilation model).

This file records decisions that have been made. When a decision changes, update it here.

## Decided

### 1. A native compiler, not a layer over Clang
- Our own front end (lexer, parser, name/type resolution, template instantiation, constexpr
  interpreter). LLVM is used only as the code generation backend.
- Rejected: Clang fork, or a build driver that generates C++20 modules / includes behind
  the scenes. Clang's declare-before-use, one-file-one-thread model cannot deliver the goal.

### 2. Compilation model
- **No `#include`, no forward declarations.** Every project symbol is visible everywhere.
  The project is a set of files (a "scope expansion path"), not an include graph.
- **Order-independent.** Declaration order does not matter (except runtime static
  initialization order within a file). Circular dependencies that cannot be resolved
  (e.g. type A contains B by value and B contains A) are reported as errors.
- **One multithreaded compiler process** sharing one symbol table, instead of independent
  per-file processes:
  1. Index: parse all files in parallel, build the global symbol tree, record unresolved
     references, keep function bodies.
  2. Resolve: once everything is indexed, resolve references and types.
  3. Codegen: with sizes and the full tree known, generate code. No separate link step.
- **Each template instantiation is generated exactly once** (reuse / wait for the thread
  producing it / produce it).
- **Persistent compiler.** It stays running with the program model in memory, recompiles
  precisely what changed, and serves IDE queries.
- No macros and no preprocessor (decided 2026-10-23 after the Factorio survey in
  [macro-survey.md](macro-survey.md)): build configuration is a set of typed `config` constants,
  and every use of macros is covered by the features in section 7.

Expected limit: once headers and duplicate instantiations are gone, LLVM optimization
dominates release builds. The big wins are debug builds and incremental rebuilds; release
builds win less unless machine code is cached per function.

### 3. Simple templates
- Templates are for generic types and functions (`Vector<T>`, `max<T>`), not a
  metaprogramming language.
- Kept: type and value parameters, variadic parameters, full specialization,
  `if constexpr`, constraints written as plain conditions (`requires`-like).
- Dropped: SFINAE / `enable_if` tricks, template metaprogramming as computation,
  `typename` / `template` disambiguators.
- Compile-time computation is done with ordinary constexpr code run by an interpreter.
- Factorio survey: almost no SFINAE (2 `enable_if`, 1 `void_t`), but 593 full
  specializations, plus `if constexpr`, `requires` and variadics, so those stay.

### 4. Our own standard library
- No C++ std. A smaller, saner library written in Kov: one UTF-8 `String` with small-string
  optimization (no traits/char-type/allocator template parameters; allocators passed at
  runtime), Vector, SmallVector, HashMap, Optional, Span, UniquePtr, Function, formatting,
  sorting, file/OS wrappers. No iostreams.
- Factorio's in-house containers (FlatMap, SmallVector, TinyVector, IntrusiveList...) are
  the starting point.

### 5. Interop: C only
- Import C headers / C ABI (OS APIs, SDL, Lua, zlib, openssl, curl...).
- C++ libraries (glm, imgui, tracy, re2, Steam SDK...) get a thin C wrapper or a port.
- Mechanism (decided 2026-10-23): `extern "C" import "header.h";` anywhere in the program. The
  compiler collects all imports, runs clang's preprocessor over them once (cached by content hash
  under `%TEMP%\kov-imports`), parses the result in C mode as one more file of the program and
  turns object-like macros that look like constants (`EOF`, `MAX_PATH`, `HEAP_ZERO_MEMORY`) into
  `const auto` declarations. Function-like macros are not imported. Inline functions in headers are
  declared, not compiled: the library provides them (the link line adds
  `legacy_stdio_definitions` for the UCRT's inline printf/scanf family). `--cinclude DIR` adds
  include directories for the imports.
- C constructs accepted for this: unions (one block of storage, every member at offset 0),
  anonymous struct/union members, `typedef struct { ... } Name;`, `(void)` parameter lists,
  MSVC's `__intN`, calling conventions and `__declspec`. Imported declarations never appear in
  parse statistics or diagnostics unless `--errors` is given.

### 6. Language changes from the proposal (not yet scheduled)
Real enum class with methods/str()/iteration, `this` as a reference, required `this->`,
required `override`, explicit by default (`implicit` keyword), no implicit switch
fallthrough, exhaustive enum switches (abort on invalid value), typed union, `?->`, `?:`,
`both_const` / `class_const`, named parameters, test tools in the compiler, `accumulate`
methods (the Factorio survey suggests generating save/load from the class may beat it).

### 7. No preprocessor; what replaces macros (decided 2026-10-23)
Based on [macro-survey.md](macro-survey.md): Factorio's 611 macros and 22,647 expansions fall
into eleven cases and none of them needs textual substitution, so Kov has no preprocessor. The
replacements, in the order of how much code they remove:
1. Tests and `check` in the compiler: `test "name" { ... }` declarations the compiler collects
   and the standard runner selects; `check(expr)` reports the text, the location and the operand
   values and records the failure instead of aborting.
2. `assert`/`release_assert` as statements of the language (text, location, operand values;
   `assert` compiles to nothing when `config::debug` is false); an implicit caller location
   parameter (`CallerLocation here = caller`, filled at the call site); `conditional(config::x)`
   functions whose calls compile to nothing, arguments unevaluated, when the condition is false.
   Done 2026-10-24: `assert(expr);` and `release_assert(expr);` are statements; a failure prints
   `file:line:column: assertion failed: <expression text>` through `kov_assert_failed`, which a
   program may define itself as `extern "C"` (debugger break, test exception); `CallerLocation`
   lives in `stdlib/core/Location.kov`; `conditional(expr)` is written where `static` would be.
   Operand values in the message: later.
3. Compile-time reflection: `for constexpr (m : members(T))` with `m.name`, `m.type` and
   `object.[m]`; `enumerators(E)` and `count(E)`; `nameof(expr)`.
4. `if constexpr` and `for constexpr` at every scope (function, class, namespace), over typed
   `config` constants and type lists (`types<A, B, C>`, or `const` arrays of structs for lists
   with data). A file may start with `requires config::platform == Platform::X;`.
   Done 2026-10-24: `--config name=value` (and `--release`) generate a `config` namespace of
   `const` values plus `enum class Platform { Windows, Linux, Mac }`; `if constexpr` folds at
   statement scope on any constant, and at namespace and class scope on conditions of config
   constants and literals (the parser evaluates those, so the other branch is never parsed);
   `requires <condition>;` at the top of a file. `for constexpr` and type lists: not yet.
5. Typed unions (`union enum`, section 6).
6. Real enums with `str()`, `fromStr()`, `count` and iteration (section 6). Done 2026-10-24
   except `fromStr()`, see 8.4.
7. `_` as a name that may be declared any number of times in a scope, each an unnamed local that
   lives to the end of the scope. Done 2026-10-24.
8. Attributes for the real compiler modifiers: `[[noinline]]`, `[[forceinline]]`,
   `[[vectorcall]]`, `[[optimize]]`, `[[no_sanitize]]`. Done 2026-10-24 for `noinline` and
   `forceinline`; the others are accepted and ignored for now.
The spellings above are the working proposal; each is settled and recorded here when the feature
is implemented.

### 8. Compile-time iteration, reflection, enums, typed unions, tests (decided 2026-10-24)
The spellings below were accepted by kovarex on 2026-10-24; they implement items 1 and 3 to 6 of
section 7. Everything here is instantiated at compile time by the same machinery as templates:
a body written once is checked and generated once per element.

**8.1 Compile-time lists.** `types<A, B, C>` is a list of types. A `const` array or a brace list
of constants is a list of values. Data that belongs to a type is a static member of the type
(`static const char* name = "custom-input";` in `CustomInputID`), not a second column of the
list, so the X-macro lists with strings become `types<...>` plus `T::name`.

**8.2 `for constexpr`.** The body is instantiated once per element; the loop variable is a type
or a constant:
```
for constexpr (typename T : types<CustomInputID, EntityID, ItemID>)
  registerLoader<T>(T::name);
for constexpr (int i : {1, 2, 4})
  table[i] = make(i);
switch (type)
{
  for constexpr (typename T : messageTypes)
    case T::kind: return parse<T>();
}
```
At namespace and class scope the body may hold function definitions and explicit
specialisations, which share their spelled name and differ by the element's type (overloads);
it may not declare data members or variables, because there is no way to name them per element
and no token pasting. Per-element data is an array indexed by the element, or a typed union.
Done 2026-10-25: `for constexpr` inside functions over `types<...>` and over a brace list of
constants; the loop variable is a template parameter of the body (a type, or a constant usable as
a template argument and an array bound), and the body is re-typed and generated once per element
the way a pack expansion is. Not yet: `case` labels inside the body, a list named by an alias or a
`const` array, bodies at namespace or class scope.

**8.3 Reflection.** `members(T)` is the list of T's non-static data members, bases first, in
declaration order; each member `m` has `m.name` (a string literal), `m.type` (usable where a
type is written), `m.offset`, and `object.[m]` is that member of `object`. `enumerators(E)`
lists an enum's values with `e.name` and `e.value`. `nameof(x)` is the last name of `x` as a
string literal (`nameof(this->sprite)` is `"sprite"`, `nameof(Direction)` is `"Direction"`).
In `if constexpr`, types compare with `==` (`if constexpr (m.type == bool)`).
```
template<typename T> void addValues(const T& object, Values& result)
{
  for constexpr (auto m : members(T))
    result.add(object.[m], m.name);
}
```
Reflection reads declarations; it never creates names. Save and load written this way replace
the `accumulate` idea of section 6 for the serialisation case.
Done 2026-10-25: `for constexpr (auto m : members(T))` with `m.name`, `m.offset` and
`object.[m]` (bases' members first, then the class's own, in declaration order; a member
descriptor is a compile-time value the loop binds, like a template argument). Not yet: `m.type`,
`enumerators(E)`, `alternatives(U)`, comparing types with `==` in `if constexpr`.

**8.4 Real enums**, the proposal's syntax with the inner brace block for the values:
```
enum class Direction : u8
{
  { North, East, South, West, None }
  bool isVertical() const { return this == North || this == South; }
};
Direction::count                       // 5
for (Direction d : Direction::values)  // iteration, in declaration order
d.str()                                // "North"; a user-written str() replaces the generated one
Direction::fromStr("North")            // Optional<Direction>
switch (d) { case North: ... }         // exhaustive, no default; an invalid value aborts
```
Inside the enum's methods `this` is the value and the enumerators are in scope unqualified.
Comparison, copying and `switch` work on the value; the underlying type is explicit or `i32`.
Done 2026-10-24: the brace block, methods with `this` as the value, `count`, `values`, the
generated or hand-written `str()`, `switch` without `default` that must name every value (a
compile error otherwise) and aborts on a value outside them; `nameof(x)` from 8.3. A plain C++
`enum class` without the inner block keeps the C++ rules. `fromStr()` waits for `Optional<E>`
to be constructible from the compiler.

**8.5 Typed unions**, the proposal's `union enum`: an enum whose every value carries a typed
payload, stored as the tag plus storage for the largest alternative.
```
union enum Property : u8
{
  { {int, Integer}, {double, Double}, {String, Text}, {String, Comment} }
  String str() const;
};
Property p;
p[Integer] = 5;                        // sets the tag, constructs the value (destroying the old one)
if (p == Property::Integer) use(p[Integer]);   // p[Integer] on another tag: release_assert
switch (p) { case Integer: return format("{}", p[Integer]); case Text: return p[Text]; ... }
```
Alternatives are listed by `alternatives(Property)` for reflection. `InputAction`,
`EditorAction`, `NetworkMessage`, `UpgradableEntity` in Factorio are this shape.

**8.6 Tests in the compiler.**
```
test "Direction::isVertical"
{
  check(Direction::North.isVertical());
  check(Direction::East.str() == "East");
}
```
A `test "name" { ... }` declaration may appear at namespace scope or inside a class (where it
sees private members). `check(expr)` records a failure with the expression text, the location
and the operand values and continues; `require(expr)` ends the test. Tests are compiled only
with `kovc --tests`, which generates the runner (`--run-test <name or file>` selects); without
the switch they cost nothing. Fixtures are ordinary classes constructed in the test.

### 9. Exceptions and RTTI (decided 2026-10-24)
- Kov has exceptions, with C++'s `throw`/`try`/`catch` semantics, implemented on LLVM's
  unwinding (the Windows personality); the non-throwing path costs nothing. Factorio's 3,198
  `throw` sites convert as they are.
- `dynamic_cast` and `typeid` exist for single-inheritance hierarchies: a type descriptor behind
  the vtable, with the chain of bases, which is what Factorio's 647 casts need. No
  cross-casts or virtual bases.
- Implemented 2026-10-24 (throw, try/catch, rethrow; `dynamic_cast` not yet) on Windows SEH
  rather than the MSVC C++ ABI: `throw` builds the object on the heap and raises an SEH exception
  (code 0xE0000001) carrying the object, its type descriptor and its destructor; every `try`
  gets a filter function that walks the descriptor's base chain against the handlers in order
  (so `catch (const Base&)` works, with the base-subobject offset applied); destructors between
  the throw and the handler run in cleanup funclets; a handler owns the object and destroys it on
  exit unless it rethrows. Calls that may throw become `invoke`s only where something has to
  happen on the way out, so code without destructible locals and without `try` is unchanged.
  Uncaught exceptions print `unhandled exception of type T: message` and abort. Rules that follow
  from this: destructors do not throw; `extern "C"` functions do not throw (calls to them stay
  plain calls); an exception cannot pass through a C callback frame that was not compiled for
  unwinding; `catch` by pointer matches the pointee type's spelling only (no base conversion);
  `new T(args)` leaks the memory when the constructor throws (to fix with the allocator work).

## Benchmark target

Factorio: ~1.3M lines of own C++ in ~8,000 files (~1.6M with tests). The real build, measured
2026-10-23 on this machine (Ryzen 9 9950X, 16 cores, MSVC 14.44, FASTBuild with unity builds of
about 100 files and a precompiled header; details and the commands in
[factorio-build-baseline.md](factorio-build-baseline.md)), Debug x64 with the game and the test
executable together:

| scenario | wall time |
|---|---:|
| clean rebuild | 62 s (919 objects, 19 min 20 s of CPU) |
| one-line edit in an ordinary header | 16 s (5 objects; about 10 s of it is linking) |
| one-line edit in a header included by the PCH | 55 s (143 objects, nearly a full rebuild) |
| no-op rebuild | 0.1 s |
| unit tests, `factorio-test.exe --fork` | 59 s (9,012 tests) |

(The first estimate here was 2 to 4 minutes; the measurement replaced it.) A 10x win therefore
means a clean debug build in about 6 s and a one-line change, in any header, in under 2 s.
Where Kov stands (see [prototype-results.md](prototype-results.md)): the whole front end over
Factorio's own code runs in 0.32 s on 32 threads, and for a generated program with Factorio's
number of function bodies a debug build is 1.7 s from source to executable and 1.25 s after a
one-line change, release 3.8 s of LLVM; the converter will show what the real code costs.

## Kov v0: the first runnable subset (proposed 2026-10-23, not yet reviewed by kovarex)

The smallest language that is still Kov, as the target of the first code generator:

- **Files and scope.** No `#include`; the project is the set of files given to the compiler.
  Every top-level declaration is visible in every file, in any order. Namespaces as in C++.
- **Grammar parseable without name lookup.** Three rules replace C++'s dependence on knowing
  what a name is:
  1. A statement that starts with a (qualified) name followed by `*`, `&`, `&&` chains and then
     an identifier is a declaration (`Foo* p;`); as an expression it would be useless anyway.
  2. `Type name(args)` is a declaration; `Type(args)` is an expression (no most vexing parse).
  3. `name<...>` is a template-id in type positions always, and in expression positions only
     when the closing `>` is followed by `(`, `::`, `{`, `.` or `->`. `a < b > c` without
     parentheses is a compile error.
  4. `f(*p);` and `f(&x);` are calls. A parenthesised declarator is a declaration only when a
     parameter list or an array bound follows: `R (*f)(int)`, `T (&a)[3]`. Pointer and reference
     variables are written `T* p;`, never `T (*p);` (decided 2026-10-23, when `windows.h` showed
     that the C++ reading turns every `QueryPerformanceFrequency(&f);` into a declaration).
  No `typename` or `template` disambiguators (as in the proposal).
- **Types.** `bool`, `i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `char` (UTF-8 code unit),
  pointers, references, arrays, `const`. `int` is an alias of `i32`, `size_t`/`usize` of `u64`
  (the short names exist in the compiler since 2026-10-24; `f32`/`f64` not yet).
- **Declarations.** Functions, `struct`/`class` with methods, bases, constructors and
  destructors, `enum class` with methods, `using` aliases, global and local variables, `auto`.
  Simple templates (type parameters, full specialisation, `if constexpr`).
- **Statements and expressions.** The C++ set, with the proposal's rules: required `this->`,
  explicit by default, no implicit fallthrough, exhaustive `switch`, `?->` and `?:`.
- **C interop.** `extern "C"` declarations of functions and structs; those are the only
  externally linked names (nothing is mangled across Kov code, since everything is one program).
- **Not in v0:** exceptions, RTTI, virtual inheritance, `both_const`/`class_const`, typed
  unions (plain C unions exist, for interop), `accumulate`, coroutines.

**Backend plan.** The compiler writes textual LLVM IR for the whole program and runs the
bundled clang on it (`clang -O2 program.ll`). This needs no LLVM development libraries on the
machine (only the clang binaries exist here) and keeps every codegen decision in Kov's own
code; moving to the LLVM C++ API in-process is a later optimisation. Names in IR are the
qualified Kov names with parameter types, quoted, so there is no mangling scheme to design.

## Open questions
- **Syntax must be parseable without name lookup.** C++ needs to know whether a name is a type
  or a template to parse `a * b;` or `f < a > (b)`. In the index-then-resolve model the parser
  runs before any name is known, so Kov should make these cases unambiguous by grammar
  (candidates: a different template-argument bracket or a required disambiguation, no
  most-vexing-parse, declaration statements recognisable from their first tokens). The prototype
  parser uses heuristics that work on 99.2% of Factorio files; the resolver can flag misparses
  later, but a real compiler should not have to guess.

- **Library names and program names share one global scope.** The first clash happened on
  2026-10-24: a test's own `struct Pair` against the library's `Pair`. Options: the library in a
  namespace (`kov::`) that programs see through an implicit `using`; or program declarations
  shadowing library ones; or simply distinct names. Not decided; the test was renamed.

## Roadmap
1. **Speed-test prototype** (done, see [prototype-results.md](prototype-results.md)): parallel
   lexer + parser + order-independent name resolution, benchmarked on Factorio. Whole front end
   in 0.32 s for 1.33 M lines on 32 threads. No code generation.
2. **Type checker on top of the resolver** (stages 1-4 done 2026-10-23, see
   [prototype-results.md](prototype-results.md)). Staged so that each stage is measurable on
   Factorio:
   1. Canonical types: interned, hash-consed in one thread-safe table, so that two threads that
      build the same type get the same pointer (the type-level form of "generate each
      instantiation once").
   2. Declared types: AST types of variables, parameters, members and functions converted to
      canonical types, with template parameters substituted from the enclosing instantiation
      (`Vector<int>::operator[]` returns `int&` without copying the template's AST).
   3. Expression typing in all function bodies: literals, names, `this`, calls with overload
      selection by arity, operators (built-in rules and overloaded operators by lookup), casts,
      `new`, `auto` deduction, range-for element types, lambdas (return type from the body; an
      `auto` parameter makes the lambda a template whose call operator is typed per call, as in C++).
   4. Member accesses through the object's type: this binds the accesses the resolver had to
      defer and is the first real use of class template instantiation.
   5. Checking template bodies per instantiation, exactly once, with the produce/wait protocol
      from the proposal (done 2026-10-23: 4,100 instantiations, 17,000 bodies, under 0.1 s of
      CPU on Factorio).
   6. Overload resolution with conversion ranking (done 2026-10-23; the library's `append`
      overloads depend on it). Later: members through templated bases, constexpr evaluation,
      per-instantiation diagnostics and expression types.
3. Compiler that runs real Kov programs via LLVM (started 2026-10-23: the v0 subset compiles
   through textual IR and clang, see prototype-results.md "Stage 4"; constructors, destructors,
   copies, operators, templates and the standard library seed `stdlib/core` with `String` and
   `Vector<T>`, virtual functions, lambdas, `HashMap`, `Optional` and `toString`/`println`
   and variadic templates with a type-safe `format` followed the same day, then the C header
   importer: `extern "C" import` of the UCRT and `windows.h`, see "C header import" in
   prototype-results.md; the first real program, `programs/kovlex`, the Kov lexer in Kov, runs
   as fast as its C++ transliteration on the Factorio corpus, 2026-10-24). Real programs live
   under `programs/<name>/`.
4. Macro replacements (section 7), the small ones first because real programs and the converter
   need them: `assert`/`release_assert`, caller location, `conditional` functions, `config`
   constants with `if constexpr` at every scope, `_` locals, attributes; then the ones that need
   design work: reflection, `for constexpr`, typed unions, real enums, tests.
5. Backend at Factorio scale (first measurement 2026-10-24, see prototype-results.md "Backend at
   scale": for 73 k functions LLVM needs 5 s at -O0 and 109 s at -O2 on one thread, kovc 0.84 s
   for everything before it). Done the same day: one IR module per group of files, compiled by
   clang in parallel, -O0 for debug and -O2 for release: 0.6 s and 3.8 s of LLVM for the 73 k
   functions on 32 threads, a debug build of 615 k lines in 1.7 s; unchanged modules keep their
   object files, so a one-line change rebuilds in 1.25 s (0.93 s of it single-threaded IR
   writing). Next: write the IR in parallel too; cross-module inlining; in-process LLVM instead
   of clang.exe when the textual round trip shows up in the numbers.
6. Persistent compiler + IDE integration: the one-line-change rebuild (16 s in Factorio today).
7. Converter from Factorio-style C++ to Kov (needs 4).
