# Kov

A C++-like language and its compiler, designed for order-of-magnitude faster builds:
no includes, order-independent symbols, one multithreaded persistent compiler,
simple templates, own standard library, C-only interop.

Read [docs/design.md](docs/design.md) before making design choices. It is the record of
decided rules; update it when a decision is made or changed.

## Machine
- Work happens on PC-PRACOVNA (Windows 10, Ryzen 9 9950X 16C/32T, 128 GB RAM).
- Toolchain: Visual Studio 2022 Community (MSVC 14.44, bundled clang 19.1.5, CMake, Ninja).
- Factorio source (benchmark corpus) is at C:\Factorio. Read-only: never modify or commit there.

## Current phase
Speed-test prototype: parallel lexer, parser and order-independent name resolution for a
Kov subset. No code generation yet. Measure, don't guess: every speed claim comes with the
benchmark command and numbers.
