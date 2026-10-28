$ErrorActionPreference = 'Stop'
$sourceRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) { throw 'Engine source staging failed' }
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
$objectDir = Join-Path $PSScriptRoot 'build\objects'
New-Item -ItemType Directory -Path $objectDir -Force | Out-Null
$argsCommon = @('-std=c++17','-fshort-wchar',"-I$PSScriptRoot\src","-I$sourceRoot",'-O0','-g','-ferror-limit=5','-ffast-math','-fno-finite-math-only','-D__FAST_MATH__=1','-msimd128','-msse4.1','-pthread','-DRED_COMPILER_CLANG','-DRED_CONFIGURATION_FINAL','-DNDEBUG','-DRELEASE','-DNO_EDITOR','-DUNICODE','-D_UNICODE','-sMEMORY64=1','-ivfsoverlay',(Join-Path $PSScriptRoot 'clang-vfs.json'))
$units = @(
    @{ Name='engineTime'; Module='redReflection'; Relative='common\redReflection\src\engineTime.cpp' },
    @{ Name='systemFile'; Module='redIO'; Relative='common\redIO\src\redIOSystemFileLinuxAPI.cpp' },
    @{ Name='string'; Module='redContainers'; Relative='common\redContainers\src\string\string.cpp' },
    @{ Name='memoryBudgets'; Module='core'; Relative='common\core\src\memoryBudgets.cpp' }
)
$results = @()
foreach ($unit in $units) {
    $moduleRoot = Join-Path $sourceRoot "common\$($unit.Module)"
    $includes = @("-I$moduleRoot\src", "-I$moduleRoot\include", "-I$PSScriptRoot\generated\commProtocol")
    $compileLog = Join-Path $PSScriptRoot "build\$($unit.Name).log"
    & $env:EMSDK_PYTHON $compiler @argsCommon @includes "-DRED_MODULE_$($unit.Module)" -c (Join-Path $sourceRoot $unit.Relative) -o (Join-Path $objectDir "$($unit.Name).o") 2>&1 | Out-File -LiteralPath $compileLog -Encoding utf8
    $unitExitCode = $LASTEXITCODE
    $results += [ordered]@{ Name=$unit.Name; Source=$unit.Relative; Compiled=($unitExitCode -eq 0); Log=$compileLog }
    if ($unitExitCode -ne 0) {
        Select-String -LiteralPath $compileLog -Pattern ': error:|fatal error' | ForEach-Object {$_.Line}
    } else { Write-Output "Compiled original $($unit.Name) to WebAssembly object" }
}
$results | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'build\core-result.json') -Encoding utf8
if (@($results | Where-Object {-not $_.Compiled}).Count) {throw 'Original core source compilation failed; stale objects must not be linked'}

