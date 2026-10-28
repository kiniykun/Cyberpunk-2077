$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'build/modules/wwise-sound-core/result.json')) {
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-browser-audio.py')
    if ($LASTEXITCODE -ne 0) {throw 'Browser audio platform compilation failed'}
}
& (Join-Path $PSScriptRoot 'compile-gpu-device.ps1')
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-static-cache.py')
if ($LASTEXITCODE -ne 0) {throw 'Original static cache staging failed'}
$objectRoot = Join-Path $PSScriptRoot 'build\objects'
$objects = @('mainBrowser','mainRed','gameApp','moduleDependencies','engineTime','memoryBudgets','utf16-runtime') | ForEach-Object {Join-Path $objectRoot "$_.o"}
$audioStatusPath = Join-Path $PSScriptRoot 'build/browser-audio-result.json'
$binkEngineStatusPath = Join-Path $PSScriptRoot 'build/bink-engine-result.json'
$binkAllocatorStatusPath = Join-Path $PSScriptRoot 'build/bink-allocator-result.json'
if (Test-Path -LiteralPath $binkAllocatorStatusPath) {
    $binkAllocatorStatus = Get-Content -LiteralPath $binkAllocatorStatusPath -Raw | ConvertFrom-Json
    if ($binkAllocatorStatus.Compiled) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-bink-allocator.py')
        if ($LASTEXITCODE -ne 0) {throw 'Bink allocator service compilation failed'}
        $objects += $binkAllocatorStatus.Object
    }
}
if (Test-Path -LiteralPath $binkEngineStatusPath) {
    $binkEngineStatus = Get-Content -LiteralPath $binkEngineStatusPath -Raw | ConvertFrom-Json
    if ($binkEngineStatus.Compiled) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-bink-engine.py')
        if ($LASTEXITCODE -ne 0) {throw 'Bink engine boundary compilation failed'}
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-bink-runtime.py')
        if ($LASTEXITCODE -ne 0) {throw 'Bink CPU worker staging failed'}
        $objects += $binkEngineStatus.Object
    }
}
$binkControlStatusPath = Join-Path $PSScriptRoot 'build/bink-control-result.json'
if (Test-Path -LiteralPath $binkControlStatusPath) {
    $binkControlStatus = Get-Content -LiteralPath $binkControlStatusPath -Raw | ConvertFrom-Json
    if ($binkControlStatus.Compiled) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-bink-control.py')
        if ($LASTEXITCODE -ne 0) {throw 'Bink browser control compilation failed'}
        $objects += $binkControlStatus.Object
    }
}
$galaxyStatusPath = Join-Path $PSScriptRoot 'build/galaxy-boundary-result.json'
if (Test-Path -LiteralPath $galaxyStatusPath) {
    $galaxyStatus = Get-Content -LiteralPath $galaxyStatusPath -Raw | ConvertFrom-Json
    if ($galaxyStatus.Compiled) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-galaxy-boundary.py')
        if ($LASTEXITCODE -ne 0) {throw 'Galaxy browser boundary compilation failed'}
        $objects += if ($galaxyStatus.Objects) {$galaxyStatus.Objects} else {$galaxyStatus.Object}
    }
}
$voiceStatusPath = Join-Path $PSScriptRoot 'build/modules/voice-broadcast/result.json'
if (Test-Path -LiteralPath $voiceStatusPath) {
    $voiceStatus = Get-Content -LiteralPath $voiceStatusPath -Raw | ConvertFrom-Json
    if ($voiceStatus.Complete) {
        # Include original factory-containing objects explicitly so archive
        # extraction cannot omit their real static registration constructors.
        $objects += @($voiceStatus.Units | ForEach-Object {$_.Object})
    }
}
$oodleStatusPath = Join-Path $PSScriptRoot 'build/oodle-boundary-result.json'
if (Test-Path -LiteralPath $oodleStatusPath) {
    $oodleStatus = Get-Content -LiteralPath $oodleStatusPath -Raw | ConvertFrom-Json
    if ($oodleStatus.Compiled) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-isolated-oodle.py')
        if ($LASTEXITCODE -ne 0) {throw 'Isolated Oodle worker compilation failed'}
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-oodle-boundary.py')
        if ($LASTEXITCODE -ne 0) {throw 'Oodle engine boundary compilation failed'}
        $oodleStatus = Get-Content -LiteralPath $oodleStatusPath -Raw | ConvertFrom-Json
        $objects += $oodleStatus.Objects
    }
}
if (Test-Path -LiteralPath $audioStatusPath) {
    $audioStatus = Get-Content -LiteralPath $audioStatusPath -Raw | ConvertFrom-Json
    if ($audioStatus.Compiled) {$objects += $audioStatus.Object}
}
  $objects += @('gpu-device-web','gpu-api-device-web','gpu-buffer-web','gpu-api-buffer-web','gpu-command-web','gpu-api-command-web','gpu-texture-web','gpu-api-texture-web','gpu-sampler-web','gpu-api-sampler-web','gpu-uniform-web','gpu-shader-web','gpu-api-shader-web','gpu-api-vertex-web','gpu-api-om-state-web','gpu-api-native-common-web','gpu-compute-web','tinyexr-runtime-web','gpu-api-image-exr-web') | ForEach-Object {Join-Path $PSScriptRoot "build\webgpu\$_.o"}
$startupStatus = Join-Path $PSScriptRoot 'build\engine-startup\result.json'
if (Test-Path -LiteralPath $startupStatus) {
    $startupUnits = @(Get-Content -LiteralPath $startupStatus -Raw | ConvertFrom-Json)
    if (@($startupUnits | Where-Object {-not $_.Compiled}).Count) {throw 'Recompile all startup units before linking'}
    $objects += @($startupUnits | ForEach-Object {$_.Object})
}
$coreUnits = @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build\core-result.json') -Raw | ConvertFrom-Json)
if (@($coreUnits | Where-Object {-not $_.Compiled}).Count) {throw 'Recompile the original core units before linking'}
$libraries = @(@('redContainers','redSystem','redMemory','redCore','redReflection','redConfig','redMath','redFileSystem','redLexer','redCompression','redIO','app','redJobs2','redImage','resource','engine','archive','renderData','freetype','interop','localization','shaderCache','core','libTree','input','resourceMaterial','resourceMesh','localizationPersistence','soundMetadata','resourceInk','inkCore','renderDraw','resourceSVG','inkRendering','textSystem','harfbuzz','gameFramework','world','spline','spacePartitioning','commProtocol','commChannel','redNetwork','triggers','animation','particles','gameServices','galaxy','telemetry','networkCore','replication','gameSave','redHTTP','physics','worldPhysics','gameUI','worldEntities', 'worldAnimation', 'worldStreaming', 'visibility', 'worldVisibility', 'worldBlackboardSystem', 'worldStatic', 'worldUserInterface', 'gameCore', 'gameInput', 'gameTweakDB', 'multiplayerFramework', 'game', 'worldGame', 'gameGraph', 'gameStateMachine', 'gameWatchdog', 'gamePersistencySystem', 'gameSpawn', 'worldEnvironment', 'render') | ForEach-Object {
    $statusPath = Join-Path $PSScriptRoot "build\modules\$_\result.json"
    if (Test-Path -LiteralPath $statusPath) {
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if ($status.Complete) { $status.Library }
    }
})
# Original middleware archives are included only after every selected unit compiles.
# Include newly completed original engine modules from the resolved launcher graph.
# This avoids silently omitting a module when its compilation has been added.
$resolvedGraph = Get-Content (Join-Path $PSScriptRoot 'build/module-graph.json') -Raw | ConvertFrom-Json
foreach ($moduleDirectory in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'build/modules') -Directory) {
    $project = $resolvedGraph.resolvedUses.psobject.Properties[$moduleDirectory.Name]
    $statusFile = Join-Path $moduleDirectory.FullName 'result.json'
    if ($project -and $project.Value -eq 'project' -and (Test-Path -LiteralPath $statusFile)) {
        $moduleStatus = Get-Content -LiteralPath $statusFile -Raw | ConvertFrom-Json
        if ($moduleStatus.Complete -and $moduleStatus.Library -and $moduleStatus.Library -notin $libraries) {
            $libraries += $moduleStatus.Library
        }
    }
}
$libraries += @(Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'build/modules') -Directory -Filter 'physx-*' | ForEach-Object {
    $statusPath = Join-Path $_.FullName 'result.json'
    if (Test-Path -LiteralPath $statusPath) {
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if ($status.Complete) {$status.Library}
    }
})
$clothStatusPath = Join-Path $PSScriptRoot 'build/modules/nvcloth/result.json'
if (Test-Path -LiteralPath $clothStatusPath) {
    $clothStatus = Get-Content -LiteralPath $clothStatusPath -Raw | ConvertFrom-Json
    if ($clothStatus.Complete) {$libraries += $clothStatus.Library}
}
foreach ($wwiseComponent in @('opus-decoder', 'wwise-memory', 'wwise-stream', 'wwise-music', 'wwise-sound-core', 'enet-browser', 'recording-core-browser')) {
    $wwiseStatusPath = Join-Path $PSScriptRoot "build/modules/$wwiseComponent/result.json"
    if (Test-Path -LiteralPath $wwiseStatusPath) {
        $wwiseStatus = Get-Content -LiteralPath $wwiseStatusPath -Raw | ConvertFrom-Json
        if ($wwiseStatus.Complete) {$libraries += $wwiseStatus.Library}
    }
}
$networkDataStatusPath = Join-Path $PSScriptRoot 'build/modules/network-data/result.json'
$platformInitStatusPath = Join-Path $PSScriptRoot 'build/modules/platform-init/result.json'
if (Test-Path -LiteralPath $platformInitStatusPath) {
    $platformInitStatus = Get-Content -LiteralPath $platformInitStatusPath -Raw | ConvertFrom-Json
    if ($platformInitStatus.Complete) {$libraries += $platformInitStatus.Library}
}
$networkStatusPath = Join-Path $PSScriptRoot 'build/modules/network/result.json'
$fullNetworkCompiled = $false
if (Test-Path -LiteralPath $networkStatusPath) {
    $fullNetworkCompiled = (Get-Content -LiteralPath $networkStatusPath -Raw | ConvertFrom-Json).Complete
}
if (-not $fullNetworkCompiled -and (Test-Path -LiteralPath $networkDataStatusPath)) {
    $networkDataStatus = Get-Content -LiteralPath $networkDataStatusPath -Raw | ConvertFrom-Json
    if ($networkDataStatus.Complete) {$libraries += $networkDataStatus.Library}
}
foreach ($object in $objects) {
    if (!(Test-Path -LiteralPath $object)) { throw "Compile bootstrap first: $object missing" }
}
$vendorLibraries = @(@('icucommon','icui18n','icudata') | ForEach-Object {
    $statusPath = Join-Path $PSScriptRoot "build\modules\$_\result.json"
    if (Test-Path -LiteralPath $statusPath) {
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
        if ($status.Complete) {
            if ($status.WcharBytes -ne 4) {throw 'ICU vendor wchar ABI metadata differs from the browser libc'}
            $status.Library
        }
    }
})
& $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'audit-icu-boundary.py') @objects @libraries
if ($LASTEXITCODE -ne 0) {throw 'Engine/ICU wchar boundary inspection failed'}
# Audit short-wchar game inputs separately. The unmodified ICU vendor builds
# use the four-byte libc ABI and must not be classified as short-wchar code.
$boundaryAudit = Get-Content (Join-Path $PSScriptRoot 'build/icu-boundary-audit.json') -Raw | ConvertFrom-Json
$unsafeWide = @($boundaryAudit.UnsafeWideCRTImports)
if (-not $boundaryAudit.Passed -or $unsafeWide.Count) {throw 'Engine/ICU boundary inspection failed'}
$libraries += $vendorLibraries
$outputRoot = Join-Path $PSScriptRoot 'build\game'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$linkLog = Join-Path $PSScriptRoot 'build\link.log'
# Keep missing-symbol errors enabled. A launcher with fake engine stubs is not
# a runnable game. Remaining engine libraries must satisfy these dependencies.
& $env:EMSDK_PYTHON $compiler @objects @libraries '--use-port=emdawnwebgpu' --pre-js (Join-Path $PSScriptRoot 'src\oodle-decoder-broker-web.js') --pre-js (Join-Path $PSScriptRoot 'src\bink-decoder-broker-web.js') --pre-js (Join-Path $PSScriptRoot 'src\filesystem-web.js') --pre-js (Join-Path $PSScriptRoot 'src\resource-filesystem-web.js') '--preload-file' "$PSScriptRoot/runtime/engine@/game/engine" '--preload-file' 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\r6\config@/game/r6/config' '--preload-file' 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\engine\config@/game/engine/config' '-lidbfs.js' '-sFORCE_FILESYSTEM=1' '-sFETCH=1' -fshort-wchar -pthread -msimd128 '-sMEMORY64=1' '-sPROXY_TO_PTHREAD=1' '-sPTHREAD_POOL_SIZE=4' '-sSTACK_SIZE=8388608' '-sDEFAULT_PTHREAD_STACK_SIZE=8388608' '-g2' '-sENVIRONMENT=web' '-sEXIT_RUNTIME=0' '-sALLOW_MEMORY_GROWTH=1' '-sINITIAL_MEMORY=67108864' '-sMAXIMUM_MEMORY=17179869184' '-sMODULARIZE=1' '-sEXPORT_NAME=createCP2077' '-Wl,--error-limit=0' -o (Join-Path $outputRoot 'cp2077.js') 2>&1 | Out-File -LiteralPath $linkLog -Encoding utf8
$linkExitCode = $LASTEXITCODE
if ($linkExitCode -eq 0) {
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'optimize-browser-module.py')
    if ($LASTEXITCODE -ne 0) {throw 'Engine linked, but browser function-count reduction failed'}
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-worker-fault-trace.py')
    if ($LASTEXITCODE -ne 0) {throw 'Native worker fault trace staging failed'}
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'stage-webgpu-memory64.py')
    if ($LASTEXITCODE -ne 0) {throw 'WebGPU Wasm64 string-view staging failed'}
}
$result = [ordered]@{
    BootstrapCompiled = $true
    GameLinked = ($linkExitCode -eq 0)
    GameRunning = $false
    RuntimeParityEstablished = $false
    MaximumLinearMemoryBytes = 17179869184
    LinkExitCode = $linkExitCode
    ObjectInputs = $objects
    LibraryInputs = $libraries
    UnsafeWideCRTImports = $unsafeWide.Count
    Log = $linkLog
}
$result | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $PSScriptRoot 'build\build-result.json') -Encoding utf8
$missing = @(Get-Content -LiteralPath $linkLog | Where-Object {$_ -match 'undefined symbol: '} | ForEach-Object {($_ -split 'undefined symbol: ',2)[1]} | Sort-Object -Unique)
$missing | ConvertTo-Json | Set-Content (Join-Path $PSScriptRoot 'build\unresolved-symbols.json') -Encoding utf8
if ($linkExitCode -ne 0) {
    Select-String -LiteralPath $linkLog -Pattern 'error:|undefined symbol' | Select-Object -First 12 | ForEach-Object {$_.Line}
    throw 'Strict engine link failed. See build/link.log for the actual remaining dependencies.'
}














