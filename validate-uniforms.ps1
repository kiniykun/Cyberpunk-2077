$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'generate-uniform-layouts.py')
if ($LASTEXITCODE -ne 0) { throw 'Uniform layout generation failed' }
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'generate-graphics-uniform-layouts.py')
if ($LASTEXITCODE -ne 0) { throw 'Original graphics uniform layout generation failed' }
$output = Join-Path $PSScriptRoot 'build\webgpu\uniform-validation.js'
& $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\em++.py') '-std=c++17' '-sMEMORY64=1' '-sENVIRONMENT=node' (Join-Path $PSScriptRoot 'src\gpu-uniform-web.cpp') (Join-Path $PSScriptRoot 'src\gpu-uniform-validation.cpp') '-o' $output
if ($LASTEXITCODE -ne 0) { throw 'Uniform validation compilation failed' }
& (Join-Path $sdkRoot 'node\24.19.0_64bit\node.exe') $output
if ($LASTEXITCODE -ne 0) { throw 'Uniform validation failed' }
