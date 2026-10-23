# Measures clang-cl front-end time (/Zs = syntax + semantic analysis only, no code generation)
# on a random sample of Factorio .cpp files, with the include paths and defines from
# C:\Factorio\fastbuild\fbuild.bff (Windows, FastDebug configuration), without the PCH.
# Usage: powershell -File clang_syntax_only.ps1 [-Sample 40] [-Seed 1] [-Parallel 32]
param([int]$Sample = 40, [int]$Seed = 1, [int]$Parallel = 32)

$root = 'C:\Factorio'
$clang = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe'
$includes = @('src', 'libraries', 'libraries\StackWalker', 'libraries\curl\include', 'libraries\curl\lib', 'libraries\re2',
  'libraries\trio', 'libraries\imgui', 'libraries\glad', 'libraries\stb_image', 'libraries\nvapi', 'libraries\SDL3\include',
  'libraries\SDL_Mixer\include', 'libraries\glad\include', 'libraries\fontstash', 'libraries\steam', 'libraries\Win10SDK',
  'libraries\libbacktrace', 'libraries\WinSDKhotfix') | ForEach-Object { "/I$root\$_" }
$defines = @('WIN32_LEAN_AND_MEAN', 'CRT_NONSTDC_NO_DEPRECATE', '_CRT_SECURE_NO_WARNINGS', '_SCL_SECURE_NO_WARNINGS', '_WINDOWS',
  '_USE_MATH_DEFINES', 'USE_THREADS', 'WIN32', '_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS', 'LUA_USE_LONGLONG', 'FACTORIO_USE_D3D',
  'SDL_MAIN_HANDLED', 'SDL_SENSOR_DISABLED', 'WIN64', 'SPACE_AGE', 'DEBUG') | ForEach-Object { "/D$_" }
$flags = @('/Zs', '/std:c++latest', '/EHs', '/fp:strict', '/FIwin_fp_flags.hpp', '/w', '-Wno-everything') + $defines + $includes

$files = Get-ChildItem "$root\src" -Recurse -Filter *.cpp | Where-Object { $_.Name -ne 'Common.cpp' }
$rng = New-Object System.Random($Seed)
$picked = $files | Sort-Object { $rng.Next() } | Select-Object -First $Sample

# Warm the file cache with one compile, then time each file sequentially.
& $clang @flags $picked[0].FullName 2>$null | Out-Null
$results = @()
foreach ($f in $picked) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $out = & $clang @flags $f.FullName 2>&1
  $sw.Stop()
  $ok = $LASTEXITCODE -eq 0
  $results += [pscustomobject]@{ file = $f.FullName.Substring($root.Length + 1); seconds = $sw.Elapsed.TotalSeconds; ok = $ok }
}
$results | Format-Table -AutoSize | Out-String -Width 200
$okResults = $results | Where-Object ok
$mean = ($okResults | Measure-Object seconds -Average).Average
$total = $files.Count
"files sampled: $($results.Count), compiled OK: $($okResults.Count)"
"mean clang-cl /Zs time per .cpp: {0:N2} s (single process, no PCH)" -f $mean
"all $total .cpp files sequentially (extrapolated): {0:N0} s" -f ($mean * $total)
"all $total .cpp files on $Parallel cores (extrapolated, ideal scaling): {0:N0} s" -f ($mean * $total / $Parallel)

# Measured parallel wall time on the sample.
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$jobs = $picked | ForEach-Object { Start-Process -FilePath $clang -ArgumentList (@($flags) + @("`"$($_.FullName)`"")) -NoNewWindow -PassThru -RedirectStandardError 'NUL' -RedirectStandardOutput 'NUL' }
$jobs | Wait-Process
$sw.Stop()
"sample of $($picked.Count) files with $($picked.Count) parallel clang processes: {0:N2} s wall" -f $sw.Elapsed.TotalSeconds
