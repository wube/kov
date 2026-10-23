@echo off
rem Configures (once) and builds kovc with clang-cl + Ninja into build\ next to this script.
setlocal
cd /d "%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build build %*
