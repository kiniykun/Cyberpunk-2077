param([ValidateRange(1,8)][int]$CompilerWorkers = 8)
$ErrorActionPreference = 'Stop'
$sdkRoot = 'D:\cp2077SourceCode\tools\emsdk'
$env:EMSDK_PYTHON = Join-Path $sdkRoot 'python\3.13.3_64bit\python.exe'
$compiler = Join-Path $sdkRoot 'upstream\emscripten\em++.py'
$outputRoot = Join-Path $PSScriptRoot 'build\webgpu'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$source = Join-Path $PSScriptRoot 'src\gpu-device-web.cpp'
$result = [ordered]@{
    Source = $source
    SourceSHA256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    CompilerWorkers = $CompilerWorkers
    DeviceBridgeCompiled = $false
    DeviceBridgeLinked = $false
    BufferBridgeCompiled = $false
    CommandBridgeCompiled = $false
    EngineGpuApiCompiled = $false
    BrowserDeviceRequested = $false
    EngineGpuApiIntegrated = $false
    GameRunning = $false
}
try {
    foreach ($generator in @('generate-uniform-layouts.py','generate-web-shader-package.py',
        'prepare-native-ui-bindings.py','generate-ui-graphics-package.py',
        'generate-graphics-uniform-layouts.py','generate-sampler-presets.py','generate-portable-texture-layout.py','generate-internal-textures.py')) {
        & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot $generator)
        if ($LASTEXITCODE -ne 0) { throw "GPU source generation failed: $generator" }
    }
    $common = @('--use-port=emdawnwebgpu','-std=c++17','-pthread','-sMEMORY64=1','-fshort-wchar')
    $engineRoot = 'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src'
    $adapterArgs = $common + @('-ffast-math','-fno-finite-math-only','-D__FAST_MATH__=1','-msimd128','-msse4.1','-DRED_COMPILER_CLANG',
        '-DRED_CONFIGURATION_FINAL','-DNDEBUG','-DRELEASE','-DNO_EDITOR','-DUNICODE','-D_UNICODE',
        '-DRED_MODULE_gpuApi','-ivfsoverlay',(Join-Path $PSScriptRoot 'clang-vfs.json'),
        "-I$PSScriptRoot\src","-I$PSScriptRoot\generated\commProtocol","-I$engineRoot",
        "-I$engineRoot\common\redMemory\include","-I$engineRoot\common\gpuApi\include",
        "-I$engineRoot\common\gpuApi\src")
    $bridges = @('gpu-device-web','gpu-buffer-web','gpu-command-web','gpu-texture-web',
        'gpu-sampler-web','gpu-uniform-web','gpu-shader-web','gpu-compute-web','tinyexr-runtime-web')
    $adapters = @('gpu-api-device-web','gpu-api-buffer-web','gpu-api-command-web','gpu-api-texture-web',
        'gpu-api-sampler-web','gpu-api-shader-web','gpu-api-vertex-web','gpu-api-om-state-web','gpu-api-native-common-web','gpu-api-image-exr-web')
    $units = @($bridges + $adapters | ForEach-Object {
        [ordered]@{Source=$_;Input=(Join-Path $PSScriptRoot "src\$_.cpp");
            Object=(Join-Path $outputRoot "$_.o");Log=(Join-Path $outputRoot "$_.compile.log");
            Args=$(if ($_ -in $adapters) {$adapterArgs} else {$common})}
    })
    $jobsPath = Join-Path $outputRoot 'gpu-compile-jobs.json'
    $resultsPath = Join-Path $outputRoot 'gpu-compile-results.json'
    $statusPath = Join-Path $outputRoot 'gpu-compilation-status.json'
    [ordered]@{Module='gpuBridgeAndAdapters';Compiler=$compiler;CommonArgs=$common;
        Workers=$CompilerWorkers;Units=$units;Retained=@();StatusPath=$statusPath;ResultsPath=$resultsPath} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jobsPath -Encoding utf8
    & $env:EMSDK_PYTHON (Join-Path $PSScriptRoot 'compile-units.py') $jobsPath
    if ($LASTEXITCODE -ne 0) { throw 'GPU compiler batch infrastructure failed' }
    $compiled = @(Get-Content -LiteralPath $resultsPath -Raw | ConvertFrom-Json)
    $failures = @($compiled | Where-Object {-not $_.Compiled})
    [ordered]@{Module='gpuBridgeAndAdapters';Sources=$units.Count;
        Compiled=@($compiled | Where-Object {$_.Compiled}).Count;Complete=($failures.Count -eq 0);
        BuildInProgress=$false;CompilerWorkers=$CompilerWorkers;Units=$compiled} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statusPath -Encoding utf8
    if ($compiled.Count -ne $units.Count -or $failures.Count) {
        throw "GPU compilation failed: $($failures.Source -join ', '); see per-unit logs"
    }
    $result.DeviceBridgeCompiled = $true
    $result.BufferBridgeCompiled = $true
    $result.CommandBridgeCompiled = $true
    $result.TextureBridgeCompiled = $true
    $result.SamplerBridgeCompiled = $true
    $result.UniformRepackingCompiled = $true
    $result.ShaderPipelineBridgeCompiled = $true
    $result.ComputeBindingDispatchBridgeCompiled = $true
    $result.EngineGpuApiCompiled = $true
    $bridgeObjects = @($bridges | ForEach-Object {Join-Path $outputRoot "$_.o"})
    # Standalone dependency check; this is not a game entry point.
    & $env:EMSDK_PYTHON $compiler '--use-port=emdawnwebgpu' '-pthread' '-sMEMORY64=1' '-sLINKABLE=1' '-sMODULARIZE=1' '-sENVIRONMENT=web,worker' '--no-entry' @bridgeObjects '-o' (Join-Path $outputRoot 'device-bridge.js')
    if ($LASTEXITCODE -ne 0) { throw 'WebGPU device link validation failed' }
    $result.DeviceBridgeLinked = $true
    Write-Output "GPU batch: $($units.Count)/$($units.Count) files compiled with $CompilerWorkers workers"
} finally {
    $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'device-build-result.json') -Encoding utf8
}
