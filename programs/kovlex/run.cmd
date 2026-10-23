@echo off
rem Builds kovlex from Kov and from the C++ transliteration, then times both on the Factorio
rem corpus, followed by kovc's own (thread-safe) lexer on one thread. See docs/prototype-results.md.
setlocal
cd /d "%~dp0"
..\..\compiler\build\kovc.exe --release --quiet --exe kovlex.exe kovlex.kov || exit /b 1
call build_cpp.cmd || exit /b 1
set CORPUS=C:\Factorio\src C:\Factorio\tests C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui
echo == kovlex, Kov
kovlex.exe %CORPUS%
echo == kovlex, C++ transliteration, same clang
kovlex_cpp.exe %CORPUS%
echo == kovc --lex-only -j 1 (the thread-safe lexer, best of 3)
..\..\compiler\build\kovc.exe --lex-only -j 1 --repeat 3 %CORPUS%
