$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
$object = Join-Path $PSScriptRoot 'build\objects\utf16-runtime.o'
$compileLog = Join-Path $PSScriptRoot 'build\utf16-runtime.log'
& $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\em++.py') -std=c++17 -fshort-wchar -pthread '-sMEMORY64=1' -c (Join-Path $PSScriptRoot 'src\utf16-runtime.cpp') -o $object 2>&1 | Out-File -LiteralPath $compileLog -Encoding utf8
if ($LASTEXITCODE -ne 0) {throw "UTF-16 runtime compilation failed: $compileLog"}
Write-Output 'Compiled browser UTF-16 compatibility runtime'
