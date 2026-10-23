# Factorio build baseline

The reference numbers Kov is measured against: Factorio's real C++ build on the same machine the
Kov prototype runs on. Measured 2026-10-23 on the Factorio `master` checkout at commit
`d0495ebfd8a`.

## Machine and toolchain

- AMD Ryzen 9 9950X, 16 cores / 32 threads, 125.6 GB RAM, Windows 10 Pro.
- Visual Studio 2022 Community 17.14, MSVC 14.44.35207, Windows SDK 10.0.19041.
- FASTBuild v1.17 (bundled in `C:\Factorio\fastbuild`), local only: no cache, no distributed workers.
- No Windows Defender exclusions for the source tree or the compiler.
- Factorio's build uses unity builds (about 100 `.cpp` files per unit) and a precompiled header
  (`src/Common.hpp`).

## Numbers

Debug x64, `vs2022-Debugx64-factorio-run` + `vs2022-Debugx64-factorio-test` together.

| Scenario | Wall time | Notes |
|---|---|---|
| Clean rebuild (`-clean`) | **62 s** | 919 objects, 19 m 20 s CPU (18.7x parallelism) |
| No-op rebuild | 0.1 s | dependency check only |
| One-line edit in an ordinary header (`src/AnimationBank.hpp`) | **16 s** | 5 objects recompiled; about 10 s is linking the two executables and `factorio.lib` |
| One-line edit in a header included by the PCH (`src/ActionResult.hpp`) | **55 s** | 143 objects, every unity chunk: nearly a full rebuild |
| Unit tests, `factorio-test.exe --fork` | **59 s** | 9,012 tests pass, 31 forks (49 s reported test time) |

Each incremental number was taken twice (edit, then revert); both runs agreed within 1 s.

## How to reproduce

One-time setup: set the user environment variable `VS143BASEPATH` to
`C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207`
(Factorio's `fbuild.bff` only auto-detects MSVC 14.40). The `justfile` in the Factorio repo is
for Linux only.

```
cd C:\Factorio\fastbuild
FBuild.exe -clean -summary vs2022-Debugx64-factorio-run vs2022-Debugx64-factorio-test
cd C:\Factorio
bin\Debugx64vs2022\factorio-test.exe --fork
```

For the incremental numbers, append a comment line to the header, run the same `FBuild.exe`
command without `-clean`, and read `Real` in the summary.
