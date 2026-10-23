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
6. Real enums with `str()`, `fromStr()`, `count` and iteration (section 6).
7. `_` as a name that may be declared any number of times in a scope, each an unnamed local that
   lives to the end of the scope. Done 2026-10-24.
8. Attributes for the real compiler modifiers: `[[noinline]]`, `[[forceinline]]`,
   `[[vectorcall]]`, `[[optimize]]`, `[[no_sanitize]]`. Done 2026-10-24 for `noinline` and
   `forceinline`; the others are accepted and ignored for now.
The spellings above are the working proposal; each is settled and recorded here when the feature
is implemented.

## Benchmark target

Factorio: ~1.3M lines of own C++ in ~8,000 files (~1.6M with tests). Today's full rebuild
including tests is about 2-4 minutes with unity builds (100 files/unit), PCH and FASTBuild.
A 10x win means ~12-24 s for a full build. The key metric is also the rebuild time after a
one-line change in a widely used header.

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
      `new`, `auto` deduction, range-for element types, lambdas (return type from the body).
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
   prototype-results.md). Next inside this item: a first real program beyond tests.
4. Macro replacements (section 7), the small ones first because real programs and the converter
   need them: `assert`/`release_assert`, caller location, `conditional` functions, `config`
   constants with `if constexpr` at every scope, `_` locals, attributes; then the ones that need
   design work: reflection, `for constexpr`, typed unions, real enums, tests.
5. Backend at Factorio scale: measure clang on whole-program IR for a large program, then
   in-process LLVM, parallel per-function codegen, per-function caching, debug-build speed.
6. Persistent compiler + IDE integration: the one-line-change rebuild (16 s in Factorio today).
7. Converter from Factorio-style C++ to Kov (needs 4).
