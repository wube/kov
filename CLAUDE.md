# Kov

A C++-like language and its compiler, designed for order-of-magnitude faster builds:
no includes, order-independent symbols, one multithreaded persistent compiler,
simple templates, own standard library, C-only interop.

## Layout
- `readme.md`, `compilation/`: the original language proposal.
- `docs/design.md`: the record of decided rules. Read it before making design choices and update
  it when a decision is made or changed. `docs/prototype-results.md`: measurements and method.
- `compiler/`: the compiler. `compiler\build.cmd` builds `compiler\build\kovc.exe` (CMake + Ninja +
  clang-cl from VS 2022). Benchmarks live in `compiler/bench/`, test inputs in `compiler/tests/`.

## Machine
- Work happens on PC-PRACOVNA (Windows 10, Ryzen 9 9950X 16C/32T, 128 GB RAM).
- Toolchain: Visual Studio 2022 Community (MSVC 14.44, bundled clang 19.1.5, CMake, Ninja).
- Factorio source (benchmark corpus) is at C:\Factorio. Read-only: never modify or commit there.
- Several threads may work in this same clone (C:\kov): commit small, push promptly, pull before
  committing.

## Current phase
The speed-test prototype is done (see docs/prototype-results.md). Next: type checking and simple
template instantiation on top of the resolver. Measure, don't guess: every speed claim comes with
the benchmark command and numbers.
