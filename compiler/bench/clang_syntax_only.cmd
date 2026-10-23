@echo off
rem Runs clang_syntax_only.ps1 inside the Visual Studio developer environment.
setlocal
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clang_syntax_only.ps1" %*
