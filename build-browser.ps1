$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build-module-graph.ps1')
& (Join-Path $PSScriptRoot 'build-protocol.ps1')
& (Join-Path $PSScriptRoot 'build-rtti.ps1')
& (Join-Path $PSScriptRoot 'build-rtti.ps1') redReflection
& (Join-Path $PSScriptRoot 'build-rtti.ps1') resource
& (Join-Path $PSScriptRoot 'build-rtti.ps1') renderData
& (Join-Path $PSScriptRoot 'build-rtti.ps1') interop
& (Join-Path $PSScriptRoot 'build-rtti.ps1') localization
& (Join-Path $PSScriptRoot 'build-rtti.ps1') shaderCache
& (Join-Path $PSScriptRoot 'build-rtti.ps1') core
& (Join-Path $PSScriptRoot 'build-rtti.ps1') libTree
& (Join-Path $PSScriptRoot 'build-rtti.ps1') input
& (Join-Path $PSScriptRoot 'build-rtti.ps1') resourceMaterial
& (Join-Path $PSScriptRoot 'build-rtti.ps1') resourceMesh
& (Join-Path $PSScriptRoot 'build-rtti.ps1') localizationPersistence
& (Join-Path $PSScriptRoot 'build-rtti.ps1') soundMetadata
& (Join-Path $PSScriptRoot 'build-rtti.ps1') resourceInk
& (Join-Path $PSScriptRoot 'build-rtti.ps1') inkCore
& (Join-Path $PSScriptRoot 'build-rtti.ps1') renderDraw
& (Join-Path $PSScriptRoot 'build-rtti.ps1') resourceSVG
& (Join-Path $PSScriptRoot 'build-rtti.ps1') inkRendering
& (Join-Path $PSScriptRoot 'build-rtti.ps1') textSystem
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameFramework
& (Join-Path $PSScriptRoot 'build-rtti.ps1') world
& (Join-Path $PSScriptRoot 'build-rtti.ps1') spline
& (Join-Path $PSScriptRoot 'build-rtti.ps1') spacePartitioning
& (Join-Path $PSScriptRoot 'build-rtti.ps1') triggers
& (Join-Path $PSScriptRoot 'build-rtti.ps1') animation
& (Join-Path $PSScriptRoot 'build-rtti.ps1') particles
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameServices
& (Join-Path $PSScriptRoot 'build-rtti.ps1') galaxy
& (Join-Path $PSScriptRoot 'build-rtti.ps1') networkCore
& (Join-Path $PSScriptRoot 'build-rtti.ps1') replication
& (Join-Path $PSScriptRoot 'build-rtti.ps1') physics
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldPhysics
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameUI
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldEntities
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldAnimation
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldStreaming
& (Join-Path $PSScriptRoot 'build-rtti.ps1') visibility
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldVisibility
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldBlackboardSystem
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldStatic
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldUserInterface
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameCore
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameInput
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameTweakDB
& (Join-Path $PSScriptRoot 'build-rtti.ps1') multiplayerFramework
& (Join-Path $PSScriptRoot 'build-rtti.ps1') game
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldGame
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameGraph
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameStateMachine
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameWatchdog
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gamePersistencySystem
& (Join-Path $PSScriptRoot 'build-rtti.ps1') gameSpawn
& (Join-Path $PSScriptRoot 'build-rtti.ps1') worldEnvironment
& (Join-Path $PSScriptRoot 'build-rtti.ps1') render
& (Join-Path $PSScriptRoot 'build-parsers.ps1')
& (Join-Path $PSScriptRoot 'compile-lexer.ps1')
& (Join-Path $PSScriptRoot 'compile-freetype.ps1')
& (Join-Path $PSScriptRoot 'compile-harfbuzz.ps1')
& 'D:\cp2077SourceCode\tools\emsdk\python\3.13.3_64bit\python.exe' (Join-Path $PSScriptRoot 'compile-icu.py')
if ($LASTEXITCODE -ne 0) {throw 'Original ICU compilation failed'}
& 'D:\cp2077SourceCode\tools\emsdk\python\3.13.3_64bit\python.exe' (Join-Path $PSScriptRoot 'compile-icu-data.py')
if ($LASTEXITCODE -ne 0) {throw 'Original ICU data embedding failed'}
& 'D:\cp2077SourceCode\tools\emsdk\python\3.13.3_64bit\python.exe' (Join-Path $PSScriptRoot 'compile-physx.py')
if ($LASTEXITCODE -ne 0) {throw 'Original physics middleware build incomplete'}
& 'D:\cp2077SourceCode\tools\emsdk\python\3.13.3_64bit\python.exe' (Join-Path $PSScriptRoot 'compile-nvcloth.py')
if ($LASTEXITCODE -ne 0) {throw 'CPU cloth middleware build incomplete'}
& (Join-Path $PSScriptRoot 'compile-runtime.ps1')
& (Join-Path $PSScriptRoot 'compile-bootstrap.ps1')
& (Join-Path $PSScriptRoot 'compile-core.ps1')
foreach ($module in @('redContainers','redSystem','redCore','redMemory','redConfig','redReflection','redMath','redFileSystem','redCompression','redIO','app','redJobs2','redImage','resource','engine','archive','renderData','interop','localization','shaderCache','core','libTree','input','resourceMaterial','resourceMesh','localizationPersistence','soundMetadata','resourceInk','inkCore','renderDraw','resourceSVG','inkRendering','textSystem','gameFramework','world','spline','spacePartitioning','commProtocol','commChannel','redNetwork','triggers','animation','particles','gameServices','galaxy','telemetry','networkCore','replication','gameSave','redHTTP','physics','worldPhysics','gameUI','worldEntities', 'worldAnimation', 'worldStreaming', 'visibility', 'worldVisibility', 'worldBlackboardSystem', 'worldStatic', 'worldUserInterface', 'gameCore', 'gameInput', 'gameTweakDB', 'multiplayerFramework', 'game', 'worldGame', 'gameGraph', 'gameStateMachine', 'gameWatchdog', 'gamePersistencySystem', 'gameSpawn', 'worldEnvironment', 'render')) {
    & (Join-Path $PSScriptRoot 'compile-module.ps1') $module
}
& (Join-Path $PSScriptRoot 'compile-engine-startup.ps1')
# Undefined engine symbols and incompatible wide CRT calls remain fatal.
& (Join-Path $PSScriptRoot 'link-bootstrap.ps1')







