@echo off
rem The feature specification suite: compiler\tests\spec\<area>\*.kov, each compiled, run and compared.
rem Writes docs\spec-status.md. Options: --filter TEXT, --jobs N, --no-doc.
python -X utf8 -I "%~dp0spec\run_spec.py" %*
