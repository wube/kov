@echo off
rem Converts Factorio's CommonUtil and Agui libraries and the game's Util module to Kov (into kov\),
rem checks that kovc compiles them, and runs the smoke tests against the result. Hand-written parts
rem are in overrides\; stand-ins for modules the smoke tests do not need are in tests\stubs\.
setlocal
cd /d "%~dp0"
set KOVC=..\..\compiler\build\kovc.exe
set SRC=C:\Factorio\src
set CINC=--cinclude C:\Factorio\libraries\SDL3\include --cinclude C:\Factorio\libraries\Lua --cinclude C:\Factorio\libraries --cinclude C:\Factorio\libraries\curl\include
%KOVC% --no-std --quiet --convert kov --overrides overrides C:\Factorio\libraries\CommonUtil C:\Factorio\libraries\Agui %SRC%\Util %SRC%\Typedefs.hpp %SRC%\Graphics\BitmapFormat.hpp %SRC%\Noise\NoiseRegisterIndex.hpp %SRC%\FloatCast.hpp || exit /b 1
type kov\CONVERSION.md | findstr /B "Files -"
echo == kovc on the converted sources
%KOVC% --std ..\..\stdlib %CINC% --errors 10 --unresolved 20 --emit-llvm kov\out.ll kov
echo == smoke test: tests\smoke.kov against the converted CommonUtil (SystemClipboard stubbed: the real one needs SDL)
setlocal EnableDelayedExpansion
set FILES=
for %%f in (kov\CommonUtil\*.kov) do if not %%~nf==SystemClipboard set FILES=!FILES! %%f
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\smoke.exe !FILES! tests\stubs\SystemClipboard.kov tests\smoke.kov || exit /b 1
kov\smoke.exe > kov\smoke.out.txt
fc /w kov\smoke.out.txt tests\smoke.expected > nul && echo smoke test: output as expected || (echo smoke test: OUTPUT DIFFERS & fc /w kov\smoke.out.txt tests\smoke.expected & exit /b 1)
echo == Agui smoke test: tests\agui_smoke.kov against the converted Agui (Void backend)
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\agui_smoke.exe !FILES! tests\stubs\SystemClipboard.kov kov\Agui tests\agui_smoke.kov || exit /b 1
kov\agui_smoke.exe > kov\agui_smoke.out.txt
fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected > nul && echo agui smoke test: output as expected || (echo agui smoke test: OUTPUT DIFFERS & fc /w kov\agui_smoke.out.txt tests\agui_smoke.expected & exit /b 1)
echo == Util smoke test: tests\util_smoke.kov against the converted Util subset (containers, sorting, the math expression evaluator; Logging, Util helpers, Math and FloatCast stubbed)
set UFILES=
for %%f in (kov\Util\Container\*.kov) do if not %%~nf==ExtendedBitBuffer set UFILES=!UFILES! %%f
%KOVC% --std ..\..\stdlib %CINC% --quiet --exe kov\util_smoke.exe !FILES! !UFILES! tests\stubs\SystemClipboard.kov tests\stubs\Log.kov tests\stubs\UtilHelpers.kov tests\stubs\MathStubs.kov tests\stubs\FloatCast.kov kov\Util\Sort kov\Util\MathExpressions kov\Util\BitBuffer.kov kov\Util\ToStr.kov kov\Util\LogLevel.kov kov\Typedefs.kov kov\BitmapFormat.kov kov\NoiseRegisterIndex.kov kov\FloatCast.kov tests\util_smoke.kov || exit /b 1
kov\util_smoke.exe > kov\util_smoke.out.txt
fc /w kov\util_smoke.out.txt tests\util_smoke.expected > nul && echo util smoke test: output as expected || (echo util smoke test: OUTPUT DIFFERS & fc /w kov\util_smoke.out.txt tests\util_smoke.expected & exit /b 1)
