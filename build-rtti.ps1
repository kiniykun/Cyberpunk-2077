param([ValidatePattern('^[A-Za-z][A-Za-z0-9]*$')][string]$Module='engine')
$ErrorActionPreference = 'Stop'
$resolvedGraph = Get-Content (Join-Path $PSScriptRoot 'build/module-graph.json') -Raw | ConvertFrom-Json
$resolvedProject = $resolvedGraph.resolvedUses.psobject.Properties[$Module]
if (-not $resolvedProject -or $resolvedProject.Value -ne 'project') {
    throw "Module is not an original resolved launcher project: $Module"
}

$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-rtti.py') $Module
if ($LASTEXITCODE -ne 0) {throw 'RTTI generator staging failed'}
$toolRoot = Join-Path $PSScriptRoot 'build-tools\rtti'
$sources = @(Get-ChildItem -LiteralPath (Join-Path $toolRoot 'src') -Filter *.cpp | Where-Object {$_.Name -ne 'cu.cpp'} | ForEach-Object {$_.FullName})
& $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\em++.py') @sources -std=c++17 -O1 -fexceptions "-I$toolRoot\include" '-sENVIRONMENT=node' '-sNODERAWFS=1' '-sEXIT_RUNTIME=1' -o (Join-Path $toolRoot 'rtti.cjs') 2>&1 | Out-File (Join-Path $toolRoot 'compile.log') -Encoding utf8
if ($LASTEXITCODE -ne 0) {Get-Content (Join-Path $toolRoot 'compile.log') | Select-String 'error:'; throw 'Original RTTI generator compilation failed'}
Write-Output 'Compiled original RTTI generator'
$destination = ((Join-Path $PSScriptRoot "generated\$Module") -replace '\\','/') + '/'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
& node (Join-Path $toolRoot 'rtti.cjs') -v -p ((Join-Path $toolRoot "$Module.vcxproj") -replace '\\','/') -d $destination 2>&1 | Out-File (Join-Path $toolRoot "$Module-generation.log") -Encoding utf8
if ($LASTEXITCODE -ne 0) {throw 'Original RTTI generator failed; see generation.log'}
$registration = Join-Path $PSScriptRoot "generated\rtti\$Module\typeRegistryFuncImpl.h"
if (!(Test-Path -LiteralPath $registration)) {throw 'RTTI registration header was not generated'}
$generationLog = Get-Content (Join-Path $toolRoot "$Module-generation.log") -Raw
$typeCountMatch = [regex]::Match($generationLog, 'Project has (\d+) types in RTTI system')
if (-not $typeCountMatch.Success -and $generationLog -notmatch 'No RTTI types found') {throw 'Original generator did not report its type count'}
$typeCount = if ($typeCountMatch.Success) {[int]$typeCountMatch.Groups[1].Value} else {0}
if (-not (Select-String -LiteralPath $registration -Pattern 'void Register\w+RTTI\(\)')) {throw 'Original registration function missing'}
if ($typeCount -gt 0 -and -not (Select-String -LiteralPath $registration -Pattern 'RTTI_REGISTER_')) {throw 'Original generator reported types but registration calls are missing'}
Write-Output "Generated engine type registration: $registration"







