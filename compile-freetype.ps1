$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
$original = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\freetype'
$cmake = Get-Content "$original\CMakeLists.txt" -Raw
$list = [regex]::Match($cmake, 'set\(BASE_SRCS\s+([\s\S]*?)\)').Groups[1].Value
$sources = @([regex]::Matches($list, 'src/[^\s)]+\.c') | ForEach-Object {$_.Value})
# The original CMake definition appends the non-Windows diagnostics unit.
# It provides FT_Trace_Enable/Disable even when tracing is configured off.
if ($cmake -notmatch 'list\(APPEND BASE_SRCS src/base/ftdebug\.c\)') {throw 'Original FreeType portable platform source changed'}
$sources += 'src/base/ftdebug.c'
if ($sources.Count -lt 30) {throw 'Original FreeType build source list could not be read'}
$output = Join-Path $PSScriptRoot 'build\modules\freetype'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$flags = @('-x','c','-std=c11','-fshort-wchar','-pthread','-msimd128','-sMEMORY64=1','-O0','-g','-DFT2_BUILD_LIBRARY',"-I$original\include")
$units = @()
foreach ($source in $sources) {
    $name = $source.Replace('/','_').Replace('.c','')
    $object = Join-Path $output "$name.o"
    $log = Join-Path $output "$name.log"
    & $env:EMSDK_PYTHON "$sdkRoot\upstream\emscripten\em++.py" @flags -c "$original\$source" -o $object 2>&1 | Out-File -LiteralPath $log -Encoding utf8
    $compiled = $LASTEXITCODE -eq 0
    $units += @{Source=$source;Compiled=$compiled;Object=$object;Log=$log}
    if (!$compiled) {Select-String -LiteralPath $log -Pattern 'error:' | Select-Object -First 3 | ForEach-Object {$_.Line}}
}
$compiledCount = @($units | Where-Object {$_.Compiled}).Count
$complete = $compiledCount -eq $sources.Count
$library = Join-Path $PSScriptRoot 'build\lib\libfreetype.a'
if ($complete) {
    & $env:EMSDK_PYTHON "$sdkRoot\upstream\emscripten\emar.py" rcs $library @($units | ForEach-Object {$_.Object})
    if ($LASTEXITCODE -ne 0) {throw 'FreeType archiving failed'}
}
@{Module='freetype';Sources=$sources.Count;Compiled=$compiledCount;Complete=$complete;Library=$(if ($complete) {$library} else {$null});OriginalBuildDefinition="$original\CMakeLists.txt";OriginalBuildSHA256=(Get-FileHash -LiteralPath "$original\CMakeLists.txt" -Algorithm SHA256).Hash;License="$original\license.txt";Units=$units} | ConvertTo-Json -Depth 5 | Set-Content "$output\result.json" -Encoding utf8
Write-Output "freetype: $compiledCount/$($sources.Count) sources compiled; complete=$complete"
