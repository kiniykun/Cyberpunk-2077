# Builds the deterministic race test (two translation units, MEMORY64, pthreads) with the engine's emsdk
# and runs it under the emsdk's node. Writes race_trace.json for the analyzer check.
param([string]$Emsdk = 'D:\Cyber punk\tools\emsdk')
$ErrorActionPreference = 'Continue'  # native stderr warnings are not failures; exit codes are checked
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null

$env:EMSDK = $Emsdk
$env:EMSDK_PYTHON = Join-Path $Emsdk 'python\3.13.3_64bit\python.exe'
$env:EM_CONFIG = Join-Path $Emsdk '.emscripten'
$node = Join-Path $Emsdk 'node\24.19.0_64bit\node.exe'
$empp = Join-Path $Emsdk 'upstream\emscripten\em++.exe'

$flags = @('-std=c++17', '-O1', '-pthread', '-sMEMORY64=1', '-sPTHREAD_POOL_SIZE=8', '-sPROXY_TO_PTHREAD=1',
           '-sEXIT_RUNTIME=1', '-sNODERAWFS=1', '-sENVIRONMENT=node', '-Wall', '-Wextra')
& $empp @flags (Join-Path $here 'race_test.cpp') (Join-Path $here 'race_tu2.cpp') -o (Join-Path $out 'race_test.cjs')
if ($LASTEXITCODE -ne 0) { throw "em++ failed ($LASTEXITCODE)" }

$trace = Join-Path $out 'race_trace.json'
& $node (Join-Path $out 'race_test.cjs') $trace
if ($LASTEXITCODE -ne 0) { throw "race test failed ($LASTEXITCODE)" }
