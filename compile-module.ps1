param([ValidatePattern('^[A-Za-z][A-Za-z0-9]*$')][string]$Module = 'redContainers', [switch]$RetryFailed, [string[]]$RecompileSources = @(), [ValidateRange(1,8)][int]$CompilerWorkers = 4)
$ErrorActionPreference = 'Stop'
$resolvedGraph = Get-Content (Join-Path $PSScriptRoot 'build/module-graph.json') -Raw | ConvertFrom-Json
$resolvedProject = $resolvedGraph.resolvedUses.psobject.Properties[$Module]
if (-not $resolvedProject -or $resolvedProject.Value -ne 'project') {
    throw "Module is not an original resolved launcher project: $Module"
}

$sourceRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
if ($Module -eq 'gameTweakDB') {
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'build-tweakdb-lexer.py')
    if ($LASTEXITCODE -ne 0) {throw 'Original TweakDB scanner generation failed'}
}
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-port.py')
if ($LASTEXITCODE -ne 0) { throw 'Engine source staging failed' }
$sourceDirectory = if ($Module -eq 'render') {'renderer'} else {$Module}
$moduleRoot = Join-Path $sourceRoot "common\$sourceDirectory"
if ($Module -eq 'cp') { $moduleRoot = Join-Path $sourceRoot 'games/cp' }
$premakeText = Get-Content (Join-Path $moduleRoot 'premake5.lua') -Raw
$sources = @([regex]::Matches($premakeText, '"(src/[^"\r\n]+\.(?:cpp|cxx|cc|c))"') | ForEach-Object {$_.Groups[1].Value} | Sort-Object -Unique)
if ($Module -eq 'redHTTP') {
    $baseDefinition = [regex]::Match($premakeText, '(?s)redHTTP_files\s*=\s*\{(.*?)\r?\n\}')
    if (-not $baseDefinition.Success) {throw 'Original shared HTTP source table missing'}
    $sources = @([regex]::Matches($baseDefinition.Value, '"(src/[^"\r\n]+\.cpp)"') | ForEach-Object {$_.Groups[1].Value} | Sort-Object -Unique)
}
if ($Module -eq 'input') {
    $baseDefinition = [regex]::Match($premakeText, '(?s)input_files\s*=\s*\{(.*?)\r?\n\}')
    if (-not $baseDefinition.Success) {throw 'Original portable input source table missing'}
    $sources = @([regex]::Matches($baseDefinition.Value, '"(src/[^"\r\n]+\.cpp)"') | ForEach-Object {$_.Groups[1].Value} | Sort-Object -Unique)
}
# Select portable/POSIX sources; excluded platform implementations are recorded.
$excluded = @($sources | Where-Object {($_ -cmatch '(Win(?:32|64|dows)?(?=[A-Z_./]|$)|Orbis|Durango|PS4|XboxOne|sce_callstack)' -and -not ($Module -eq 'redFileSystem' -and $_ -eq 'src/filePathsWindows.cpp')) -or ($Module -eq 'redSystem' -and $_ -eq 'src/timerLinux.cpp') -or ($Module -eq 'redMemory' -and $_ -eq 'src/flexibleSystemAllocator.cpp')})
if ($Module -eq 'redMemory') {
    $excluded += @($sources | Where-Object {$_ -match 'Linux\.cpp$'})
}
if ($Module -eq 'redNetwork') {
    # Match the original POSIX build's named-pipe exclusions. Emscripten's
    # socket implementation requires WebSocket peers; native listeners are
    # unavailable in browsers and are not claimed as functioning services.
    $excluded += @($sources | Where-Object {$_ -eq 'src/rawPipeManager.cpp'})
}
if ($Module -eq 'core') {
    # Preserve the original Linux build exclusions for named-pipe editor tools.
    $excluded += @($sources | Where-Object {$_ -in @('src/interactiveCommandlet.cpp','src/namedPipeMessageHandler.cpp')})
}
if ($Module -eq 'render') {
    # Match original Linux/Final renderer platform exclusions.
    $excluded += @($sources | Where-Object {$_ -in @('src/aviutils.cpp','src/renderNodes.cpp','src/ngx.cpp')})
}
$sources = @($sources | Where-Object {$_ -notin $excluded})
if ($Module -eq 'redMemory') {$sources += @('@web/memory-web.cpp','@web/metrics-web.cpp')}
if ($Module -eq 'redSystem') {$sources += '@web/diagnostics-web.cpp'}
if ($Module -eq 'input') {$sources += '@web/input-web.cpp'}
if ($Module -eq 'redHTTP') {$sources += '@web/http-web.cpp'}
if ($Module -eq 'gameServices') {
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-save-worker.py')
    if ($LASTEXITCODE -ne 0) {throw 'Original file-based save worker staging failed'}
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-game-services-web.py')
    if ($LASTEXITCODE -ne 0) {throw 'Browser platform service staging failed'}
    $sources += @('@web/game-services-worker-web.cpp','@web/persistence-web.cpp','@web/game-services-web.cpp')
}
if ($sources.Count -eq 0) { throw 'No original source units found in the module build definition' }
$moduleBuild = Join-Path $PSScriptRoot "build\modules\$Module"
New-Item -ItemType Directory -Path $moduleBuild -Force | Out-Null
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
$commonArgs = @('-std=c++17','-fshort-wchar',"-I$PSScriptRoot\src",'-O0','-g','-ferror-limit=5','-ffast-math','-fno-finite-math-only','-D__FAST_MATH__=1','-msimd128','-msse4.1','-pthread','-DRED_COMPILER_CLANG','-DRED_CONFIGURATION_FINAL','-DNDEBUG','-DRELEASE','-DNO_EDITOR','-DUNICODE','-D_UNICODE',"-DRED_MODULE_$Module",'-sMEMORY64=1','-ivfsoverlay',(Join-Path $PSScriptRoot 'clang-vfs.json'),"-I$moduleRoot\src","-I$moduleRoot\include","-I$PSScriptRoot\generated\commProtocol")
$results = @()
$commonArgs += "-I$sourceRoot\common\redMemory\include"
$commonArgs += "-I$sourceRoot"
$commonArgs += "-I$moduleRoot"
if ($Module -eq 'redReflection') {$commonArgs += "-I$PSScriptRoot\generated\redReflection"}
if ($Module -in @('soundMetadata','gameFramework','world','animation','particles','gameServices','galaxy','telemetry','networkCore','replication','gameSave','redHTTP','physics','worldPhysics','gameUI','worldEntities', 'worldAnimation', 'worldStreaming', 'visibility', 'worldVisibility', 'worldBlackboardSystem', 'worldStatic', 'worldUserInterface', 'gameCore', 'gameInput', 'gameTweakDB', 'multiplayerFramework', 'game', 'worldGame', 'gameGraph', 'gameStateMachine', 'gameWatchdog', 'gamePersistencySystem', 'gameSpawn', 'worldEnvironment', 'render', 'ai', 'worldNavigation', 'worldTerrain', 'worldPrecomputedLight')) {$commonArgs += "-I$sourceRoot\..\external\wwise\include"}
if ($Module -in @('inkCore','textSystem','gameFramework','world','gameServices','gameSave','gameUI','worldEntities', 'worldAnimation', 'worldStreaming', 'visibility', 'worldVisibility', 'worldBlackboardSystem', 'worldStatic', 'worldUserInterface', 'gameCore', 'gameInput', 'gameTweakDB', 'multiplayerFramework', 'game', 'worldGame', 'gameGraph', 'gameStateMachine', 'gameWatchdog', 'gamePersistencySystem', 'gameSpawn', 'worldEnvironment', 'render', 'ai', 'worldNavigation', 'worldTerrain', 'worldPrecomputedLight')) {$commonArgs += @("-I$sourceRoot\..\external\icu\source\common", "-I$sourceRoot\..\external\icu\source\i18n", "-I$sourceRoot\..\external\freetype\include", "-I$sourceRoot\..\external\harfbuzz\include")}
if ($Module -in @('renderData','resourceMesh')) {$commonArgs += @("-I$sourceRoot\..\external\freetype\include","-I$sourceRoot\..\external\speedtree_v8\include","-I$sourceRoot\..\external\speedtree_v8\include\wind","-I$sourceRoot\..\external\speedtree_v8\cpp_source")}
if ($Module -in @('physics','worldPhysics','worldEntities', 'worldAnimation', 'worldStreaming', 'visibility', 'worldVisibility', 'worldBlackboardSystem', 'worldStatic', 'worldUserInterface', 'gameCore', 'gameInput', 'gameTweakDB', 'multiplayerFramework', 'game', 'worldGame', 'gameGraph', 'gameStateMachine', 'gameWatchdog', 'gamePersistencySystem', 'gameSpawn', 'worldEnvironment', 'render', 'ai', 'worldNavigation', 'worldTerrain', 'worldPrecomputedLight')) {
    $commonArgs += @(
        "-I$sourceRoot\..\external\PhysX342\PxShared\include",
        "-I$sourceRoot\..\external\PhysX342\PhysX_3.4\Include",
        "-I$sourceRoot\..\external\PhysX342\PhysX_3.4\Source\Common\src",
        "-I$sourceRoot\..\external\PhysX342\PxShared\include\foundation",
        "-I$sourceRoot\..\external\PhysX342\PxShared\src\foundation\include",
        "-I$sourceRoot\..\external\NvCloth\include",
        "-I$sourceRoot\..\external\NvCloth\src\foundation\include")
}
if ($Module -eq 'gameStateMachine') {
    # Original module Premake include directories for its short header names.
    foreach ($dependency in @('gameInput','gameGraph','gameTweakDB','worldEntities')) {
        $commonArgs += "-I$sourceRoot\common\$dependency\include"
    }
}
if ($Module -eq 'render') {
    $commonArgs += @(
        "-I$sourceRoot\..\external\bink2\include",
        "-I$sourceRoot\..\external\tinyexr",
        "-I$sourceRoot\..\external\speedtree_v8\cpp_source",
        "-I$sourceRoot\..\external\speedtree_v8\include",
        "-I$sourceRoot\..\external\speedtree_v8\include\wind",
        "-I$sourceRoot\..\external\d3d12on7\include")
}
# Shared dependency roots for additional original launcher modules.
$commonArgs += @(
    "-I$sourceRoot\..\external\wwise\include",
    "-I$sourceRoot\..\external\icu\source\common",
    "-I$sourceRoot\..\external\icu\source\i18n",
    "-I$sourceRoot\..\external\freetype\include",
    "-I$sourceRoot\..\external\harfbuzz\include",
    "-I$sourceRoot\..\external\PhysX342\PxShared\include",
    "-I$sourceRoot\..\external\PhysX342\PhysX_3.4\Include",
    "-I$sourceRoot\..\external\PhysX342\PhysX_3.4\Source\Common\src",
    "-I$sourceRoot\..\external\PhysX342\PxShared\include\foundation",
    "-I$sourceRoot\..\external\PhysX342\PxShared\src\foundation\include",
    "-I$sourceRoot\..\external\NvCloth\include",
    "-I$sourceRoot\..\external\NvCloth\src\foundation\include")
if ($Module -eq 'gameAI') { $commonArgs += "-I$sourceRoot\common\game\include" }
# Preserve literal include-directory entries from each original Premake project.
foreach ($includeBlock in [regex]::Matches($premakeText, '(?s)includedirs\s*\{(.*?)\}')) {
    foreach ($includeEntry in [regex]::Matches($includeBlock.Groups[1].Value, '"([^"\r\n]+)"')) {
        $relativeInclude = $includeEntry.Groups[1].Value
        if ($relativeInclude -match '[%$]') { continue }
        $resolvedInclude = [IO.Path]::GetFullPath((Join-Path $moduleRoot $relativeInclude))
        if (Test-Path -LiteralPath $resolvedInclude -PathType Container) {
            $commonArgs += "-I$resolvedInclude"
        }
    }
}
$objects = @()
$previous = $null
if ($RetryFailed) {
    # Explicit repair pass: use only after changes confined to failed units.
    $previous = Get-Content (Join-Path $moduleBuild 'result.json') -Raw | ConvertFrom-Json
}
[ordered]@{Module=$Module; Sources=$sources.Count; Compiled=0; Complete=$false; BuildInProgress=$true; StartedUTC=[DateTime]::UtcNow.ToString('o'); Units=@()} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $moduleBuild 'result.json') -Encoding utf8
$unitJobs = @()
$retained = @()
foreach ($source in $sources) {
    $unitName = $source.Replace('/','_').Replace('.cpp','').Replace('@','')
    $object = Join-Path $moduleBuild "$unitName.o"
    $compileLog = Join-Path $moduleBuild "$unitName.log"
    $prior = @($previous.Units | Where-Object {$_.Source -eq $source -and $_.Compiled -and (Test-Path -LiteralPath $_.Object)})
    if ($RetryFailed -and $prior.Count -eq 1 -and $source -notin $RecompileSources) {$retained += $prior[0]}
    $inputFile = if ($source.StartsWith('@web/')) {Join-Path $PSScriptRoot ('src/'+$source.Substring(5))} else {Join-Path $moduleRoot $source}
    $unitJobs += [ordered]@{Source=$source;Input=$inputFile;Object=$object;Log=$compileLog}
}
$compileManifest = Join-Path $moduleBuild 'compile-jobs.json'
$compileResults = Join-Path $moduleBuild 'compile-results.json'
[ordered]@{Module=$Module;Compiler=$compiler;CommonArgs=$commonArgs;Workers=$CompilerWorkers;Units=@($unitJobs);Retained=@($retained);StatusPath=(Join-Path $moduleBuild 'result.json');ResultsPath=$compileResults} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $compileManifest -Encoding utf8
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-units.py') $compileManifest
if ($LASTEXITCODE -ne 0) {throw 'Compiler batch infrastructure failed; see current per-unit logs'}
$results = @(Get-Content -LiteralPath $compileResults -Raw | ConvertFrom-Json)
$objects = @($results | Where-Object {$_.Compiled} | ForEach-Object {$_.Object})
$libraryRoot = Join-Path $PSScriptRoot 'build\lib'
New-Item -ItemType Directory -Path $libraryRoot -Force | Out-Null
$library = Join-Path $libraryRoot "lib$Module.a"
$allCompiled = ($objects.Count -eq $sources.Count)
if ($allCompiled) {
    $newLibrary = Join-Path $libraryRoot "lib$Module.building.a"
    if (Test-Path -LiteralPath $newLibrary) {Remove-Item -LiteralPath $newLibrary}
    # Large original game modules exceed Windows' process command-line limit.
    # LLVM ar accepts a response file; one quoted path per line preserves spaces.
    $archiveInputs = Join-Path $moduleBuild 'archive-inputs.rsp'
    $objects | ForEach-Object { '"' + ($_ -replace '\\','/') + '"' } | Set-Content -LiteralPath $archiveInputs -Encoding utf8
    & $env:EMSDK_PYTHON (Join-Path $sdkRoot 'upstream\emscripten\emar.py') rcs $newLibrary "@$archiveInputs"
    if ($LASTEXITCODE -ne 0) { throw 'Engine library archiving failed' }
    Move-Item -LiteralPath $newLibrary -Destination $library -Force
}
[ordered]@{Module=$Module; Sources=$sources.Count; ExcludedPlatformSources=$excluded; Compiled=$objects.Count; Complete=$allCompiled; Library=$(if ($allCompiled) {$library} else {$null}); Units=$results} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $moduleBuild 'result.json') -Encoding utf8
Write-Output "$Module : $($objects.Count)/$($sources.Count) sources compiled; complete library=$allCompiled"







