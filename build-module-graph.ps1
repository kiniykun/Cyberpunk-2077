$ErrorActionPreference = 'Stop'
$python = 'D:\cp2077SourceCode\tools\emsdk\python\3.13.3_64bit\python.exe'
$premake = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\tools\buildsystem\premake5.exe'
& $python (Join-Path $PSScriptRoot 'stage-build-graph.py')
if ($LASTEXITCODE -ne 0) {throw 'Original build-definition staging failed'}
Push-Location (Join-Path $PSScriptRoot 'build-tools\premake\Mainline\dev\src')
try {
    if (Test-Path -LiteralPath 'browser-module-graph.txt') {Remove-Item -LiteralPath 'browser-module-graph.txt'}
    & $premake --file=browser-graph.lua --platform=x64_linux --configuration=final --gs=null cpwebgraph *> (Join-Path $PSScriptRoot 'build-tools\premake\graph.log')
    if ($LASTEXITCODE -ne 0) {throw 'Original project graph generation failed; see graph.log'}
} finally {Pop-Location}
& $python (Join-Path $PSScriptRoot 'finish-build-graph.py')
if ($LASTEXITCODE -ne 0) {throw 'Original module graph coverage validation failed'}
