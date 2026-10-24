@echo off
rem Converts the parts of Factorio that are being brought over to Kov (CommonUtil for now) into
rem programs\factorio\kov, applying the hand-written overrides, and checks the result with kovc.
setlocal
cd /d "%~dp0"
set KOVC=..\..\compiler\build\kovc.exe
%KOVC% --no-std --quiet --convert kov --overrides overrides C:\Factorio\libraries\CommonUtil || exit /b 1
type kov\CONVERSION.md | findstr /B "Files -"
echo == kovc on the converted sources
%KOVC% --std ..\..\stdlib --cinclude C:\Factorio\libraries\SDL3\include --errors 10 --unresolved 20 --emit-llvm kov\out.ll kov
