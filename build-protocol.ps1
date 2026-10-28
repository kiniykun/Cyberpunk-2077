$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-protocol.py')
if ($LASTEXITCODE -ne 0) { throw 'Protocol generator staging failed' }
$toolRoot = Join-Path $PSScriptRoot 'build-tools\comm-protocol'
$toolSources = @(Get-ChildItem (Join-Path $toolRoot 'src') -Filter '*.cpp' | ForEach-Object {$_.FullName})
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
$toolLog = Join-Path $toolRoot 'build.log'
& $env:EMSDK_PYTHON $compiler @toolSources -std=c++17 -O1 '-sENVIRONMENT=node' '-sNODERAWFS=1' '-sEXIT_RUNTIME=1' -o (Join-Path $toolRoot 'proto.cjs') 2>&1 | Out-File -LiteralPath $toolLog -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    Select-String -LiteralPath $toolLog -Pattern 'error:|fatal error' | ForEach-Object {$_.Line}
    throw 'Protocol generator compilation failed'
}
$generatedRoot = Join-Path $PSScriptRoot 'generated\commProtocol'
New-Item -ItemType Directory -Path $generatedRoot -Force | Out-Null
$protocolSources = 'D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/src/common/commProtocol/src/'
$generatedPath = $generatedRoot.Replace('\','/') + '/'
& node (Join-Path $toolRoot 'proto.cjs') cpp $protocolSources $generatedPath
if ($LASTEXITCODE -ne 0) { throw 'Original protocol generation failed' }
