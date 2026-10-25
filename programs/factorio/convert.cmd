@echo off
rem Converts Factorio's CommonUtil and Agui libraries to Kov (into kov\), checks that kovc compiles
rem them, and runs the smoke tests against the result. Hand-written parts are in overrides\.
setlocal
cd /d "%~dp0"
set KOVC=..\..\compiler\build\kovc.exe
%KOVC% --no-std --quiet --convert kov --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui || exit /b 1
type kov\CONVERSION.md | findstr /B "Files -"
echo == kovc on the converted sources
%KOVC% --std ..\..\stdlib --cinclude C:\Factorio\libraries\SDL3\include --errors 10 --unresolved 20 --emit-llvm kov\out.ll kov
echo == smoke test: tests\smoke.kov against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
set FILES=
for %%f in (kov\CommonUtil\*.kov) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KOVC% --std ..\..\stdlib --cinclude C:\Factorio\libraries\SDL3\include --quiet --exe kov\smoke.exe !FILES! tests\stubs\SystemClipboard.kov tests\smoke.kov || exit /b 1
kov\smoke.exe > kov\smoke.out.txt
fc /w kov\smoke.out.txt tests\smoke.expected > nul && echo smoke test: output as expected || (echo smoke test: OUTPUT DIFFERS & fc /w kov\smoke.out.txt tests\smoke.expected & exit /b 1)
echo == Agui smoke test: tests\agui_smoke.kov against the converted Agui (Void backend)
%KOVC% --std ..\..\stdlib --cinclude C:\Factorio\libraries\SDL3\include --quiet --exe kov\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kov kov\Agui tests\agui_smoke.kov || exit /b 1
kov\agui_smoke.exe > kov\agui_smoke.out.txt
fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected > nul && echo agui smoke test: output as expected || (echo agui smoke test: OUTPUT DIFFERS & fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected & exit /b 1)
