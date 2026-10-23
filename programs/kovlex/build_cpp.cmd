@echo off
rem Builds the C++ transliteration with the same clang that kovc uses, inside the VS 2022
rem environment (so that clang finds the matching MSVC headers and libraries).
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
clang -O2 -w kovlex.cpp -o kovlex_cpp.exe
