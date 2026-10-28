$ErrorActionPreference = 'Stop'
$sourceRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) {throw 'Engine source staging failed'}
$moduleRoot = Join-Path $sourceRoot 'common\engine'
$outputRoot = Join-Path $PSScriptRoot 'build\engine-startup'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$argsCommon = @('-std=c++17','-fshort-wchar','-O0','-g','-ferror-limit=5','-ffast-math','-fno-finite-math-only','-D__FAST_MATH__=1','-msimd128','-msse4.1','-pthread','-DRED_COMPILER_CLANG','-DRED_CONFIGURATION_FINAL','-DNDEBUG','-DRELEASE','-DNO_EDITOR','-DUNICODE','-D_UNICODE','-DRED_MODULE_engine','-sMEMORY64=1','-ivfsoverlay',(Join-Path $PSScriptRoot 'clang-vfs.json'),"-I$PSScriptRoot\src","-I$sourceRoot","-I$moduleRoot\src","-I$moduleRoot\include","-I$PSScriptRoot\generated\commProtocol")
$units = @('gameAppState','gameAppBaseInitializationState','gameAppInitializationState','gameAppRunningState','gameAppShutdownState','engineInit','applicationUtils','baseEngineInitParams')
$results = @()
foreach ($unit in $units) {
    $source = Join-Path $moduleRoot "src\$unit.cpp"
    $object = Join-Path $outputRoot "$unit.o"
    $compileLog = Join-Path $outputRoot "$unit.log"
    & $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\em++.py') @argsCommon -c $source -o $object 2>&1 | Out-File -LiteralPath $compileLog -Encoding utf8
    $compiled = $LASTEXITCODE -eq 0
    $results += [ordered]@{Unit=$unit; Compiled=$compiled; Source=$source; Object=$object; Log=$compileLog}
    if ($compiled) {Write-Output "Compiled engine startup unit: $unit"} else {Select-String -LiteralPath $compileLog -Pattern 'error:' | Select-Object -First 2 | ForEach-Object {$_.Line}}
}
$results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outputRoot 'result.json') -Encoding utf8
if (@($results | Where-Object {-not $_.Compiled}).Count) {throw 'Original startup source compilation failed; stale objects must not be linked'}

