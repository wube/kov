@echo off
rem Compiles every tests\run\*.kov with kovc, runs it, and compares stdout with the .expected file.
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
set KOVC=build\kovc.exe
set FAIL=0
set PASS=0
for %%f in (tests\run\*.kov) do (
  set NAME=%%~nf
  %KOVC% --quiet --exe build\tests\!NAME!.exe %%f > build\tests\!NAME!.compile.txt 2>&1
  if errorlevel 1 (
    echo FAIL !NAME!: compile
    type build\tests\!NAME!.compile.txt
    set /a FAIL+=1
  ) else (
    build\tests\!NAME!.exe > build\tests\!NAME!.out.txt
    fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected > nul
    if errorlevel 1 (
      echo FAIL !NAME!: output differs
      fc /w build\tests\!NAME!.out.txt tests\run\!NAME!.expected
      set /a FAIL+=1
    ) else (
      echo ok   !NAME!
      set /a PASS+=1
    )
  )
)
echo %PASS% passed, %FAIL% failed
if %FAIL% neq 0 exit /b 1
