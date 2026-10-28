$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
$original = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\RedLexer\native'
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) {throw 'Source staging failed'}
$output = Join-Path $PSScriptRoot 'build\modules\redLexer'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$flags = @('-std=c++17','-fshort-wchar','-pthread','-msimd128','-sMEMORY64=1','-O0','-g','-DNDEBUG','-DRELEASE','-DNO_EDITOR',"-I$original\include","-I$original\src","-I$PSScriptRoot\generated\redReflection",'-ivfsoverlay',"$PSScriptRoot\clang-vfs.json")
$units = @()
foreach ($unit in @('lexer','state')) {
    $object = Join-Path $output "$unit.o"
    $log = Join-Path $output "$unit.log"
    & $env:EMSDK_PYTHON "$sdkRoot\upstream\emscripten\em++.py" @flags -c "$original\src\$unit.cpp" -o $object 2>&1 | Out-File -LiteralPath $log -Encoding utf8
    $compiled = $LASTEXITCODE -eq 0
    $units += @{Source="$original\src\$unit.cpp";Compiled=$compiled;Object=$object;Log=$log}
    if (!$compiled) {Select-String -LiteralPath $log -Pattern 'error:' | Select-Object -First 5 | ForEach-Object {$_.Line}}
}
$complete = @($units | Where-Object {$_.Compiled}).Count -eq 2
$library = Join-Path $PSScriptRoot 'build\lib\libredLexer.a'
if ($complete) {
    & $env:EMSDK_PYTHON "$sdkRoot\upstream\emscripten\emar.py" rcs $library @($units | ForEach-Object {$_.Object})
    if ($LASTEXITCODE -ne 0) {throw 'Lexer archiving failed'}
}
@{Module='redLexer';Sources=2;Compiled=@($units | Where-Object {$_.Compiled}).Count;Complete=$complete;Library=$(if ($complete) {$library} else {$null});Units=$units} | ConvertTo-Json -Depth 5 | Set-Content "$output\result.json" -Encoding utf8
Write-Output "redLexer: complete=$complete"
