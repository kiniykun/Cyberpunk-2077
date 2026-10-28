$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$vendorRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\harfbuzz_new'
$python = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
& $python (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) {throw 'Source staging failed'}
$definition = Join-Path $vendorRoot 'CMakeLists.txt'
$cmake = Get-Content -LiteralPath $definition -Raw
if ($cmake -notmatch 'set \(project_sources \$\{PROJECT_SOURCE_DIR\}/src/harfbuzz\.cc\)') {throw 'Original HarfBuzz amalgam build definition changed'}
if ($cmake -notmatch 'add_definitions\(-DHAVE_FREETYPE=1\)') {throw 'Original FreeType integration definition missing'}
$output = Join-Path $PSScriptRoot 'build\modules\harfbuzz'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$object = Join-Path $output 'harfbuzz.o'
$log = Join-Path $output 'compile.log'
$library = Join-Path $PSScriptRoot 'build\lib\libharfbuzz.a'
$flags = @('-std=c++17','-fshort-wchar','-pthread','-msimd128','-sMEMORY64=1','-O0','-g','-DHAVE_FREETYPE=1','-DHAVE_PTHREAD=1','-ivfsoverlay',(Join-Path $PSScriptRoot 'clang-vfs.json'),'-DHAVE_UNISTD_H=1',"-I$vendorRoot\src", "-I$vendorRoot\..\freetype\include")
[ordered]@{Module='harfbuzz';Sources=1;Compiled=0;Complete=$false;BuildInProgress=$true;StartedUTC=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $output 'result.json') -Encoding utf8
& $python (Join-Path $sdkRoot 'upstream\emscripten\em++.py') @flags -c (Join-Path $vendorRoot 'src\harfbuzz.cc') -o $object 2>&1 | Out-File -LiteralPath $log -Encoding utf8
$complete = $LASTEXITCODE -eq 0
if ($complete) {
    $newLibrary = Join-Path $PSScriptRoot 'build\lib\libharfbuzz.building.a'
    if (Test-Path -LiteralPath $newLibrary) {Remove-Item -LiteralPath $newLibrary}
    & $python (Join-Path $sdkRoot 'upstream\emscripten\emar.py') rcs $newLibrary $object
    if ($LASTEXITCODE -ne 0) {throw 'HarfBuzz archiving failed'}
    Move-Item -LiteralPath $newLibrary -Destination $library -Force
} else {Select-String -LiteralPath $log -Pattern 'error:' | Select-Object -First 5 | ForEach-Object {$_.Line}}
[ordered]@{Module='harfbuzz';Sources=1;Compiled=[int]$complete;Complete=$complete;Library=$(if($complete){$library}else{$null});OriginalBuildDefinition=$definition;OriginalBuildSHA256=(Get-FileHash -LiteralPath $definition -Algorithm SHA256).Hash;License=(Join-Path $vendorRoot 'COPYING');Features=@('Original OpenType/AAT/fallback shapers','Original FreeType bridge','Built-in Unicode database');Units=@(@{Source='src/harfbuzz.cc';Compiled=$complete;Object=$object;Log=$log})} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'result.json') -Encoding utf8
Write-Output "harfbuzz: complete library=$complete"
