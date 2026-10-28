$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'generate-web-shader-package.py')
if ($LASTEXITCODE -ne 0) { throw 'Shader package generation failed' }
$output = Join-Path $PSScriptRoot 'build\webgpu\shader-identity-validation.js'
& $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\em++.py') '--use-port=emdawnwebgpu' '-std=c++17' '-pthread' '-sMEMORY64=1' '-sENVIRONMENT=node' (Join-Path $PSScriptRoot 'src\gpu-shader-web.cpp') (Join-Path $PSScriptRoot 'src\gpu-shader-identity-validation.cpp') '-o' $output
if ($LASTEXITCODE -ne 0) { throw 'Shader identity validation compilation failed' }
& (Join-Path $sdkRoot 'node\24.19.0_64bit\node.exe') $output
if ($LASTEXITCODE -ne 0) { throw 'Shader identity validation failed' }
