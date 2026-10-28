/**
* Copyright (C)2026 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "common/redSystem/include/redSystemPublic.h"

void InitModuleDependencies()
{
    RED_INITIALIZE_MODULE( libTree );
    RED_INITIALIZE_MODULE( telemetry );
    RED_INITIALIZE_MODULE( gameDebugVisualizer );
    RED_INITIALIZE_MODULE( gameUI );
    RED_INITIALIZE_MODULE( gameSceneCore );
    RED_INITIALIZE_MODULE( physics );
    RED_INITIALIZE_MODULE( gameAI );
    RED_INITIALIZE_MODULE( gameInteractions );
    RED_INITIALIZE_MODULE( functionalTestsEngine );
    RED_INITIALIZE_MODULE( redReflection );
    RED_INITIALIZE_MODULE( networkServices );
    RED_INITIALIZE_MODULE( interopServices );
    RED_INITIALIZE_MODULE( worldGame );
    RED_INITIALIZE_MODULE( inkCore );
    RED_INITIALIZE_MODULE( sessionRecordingTool );
    RED_INITIALIZE_MODULE( worldPrecomputedLight );
    RED_INITIALIZE_MODULE( network );
    RED_INITIALIZE_MODULE( gameJournalManager );
    RED_INITIALIZE_MODULE( renderData );
    RED_INITIALIZE_MODULE( inkRendering );
    RED_INITIALIZE_MODULE( ai );
    RED_INITIALIZE_MODULE( resource );
    RED_INITIALIZE_MODULE( gameSceneSystem );
    RED_INITIALIZE_MODULE( resourceInk );
    RED_INITIALIZE_MODULE( gamePopulation );
    RED_INITIALIZE_MODULE( gameVehicles );
    RED_INITIALIZE_MODULE( sound );
    RED_INITIALIZE_MODULE( commChannel );
    RED_INITIALIZE_MODULE( localization );
    RED_INITIALIZE_MODULE( game );
    RED_INITIALIZE_MODULE( visibility );
    RED_INITIALIZE_MODULE( gameStateMachine );
    RED_INITIALIZE_MODULE( gamePhotoMode );
    RED_INITIALIZE_MODULE( imgui );
    RED_INITIALIZE_MODULE( gameGraph );
    RED_INITIALIZE_MODULE( world );
    RED_INITIALIZE_MODULE( commProtocol );
    RED_INITIALIZE_MODULE( gameInput );
    RED_INITIALIZE_MODULE( cp );
    RED_INITIALIZE_MODULE( gameDebug );
    RED_INITIALIZE_MODULE( render );
    RED_INITIALIZE_MODULE( gameServices );
    RED_INITIALIZE_MODULE( engine );
    RED_INITIALIZE_MODULE( gameServicesDurango );
    RED_INITIALIZE_MODULE( resourceMesh );
    RED_INITIALIZE_MODULE( soundEngine );
    RED_INITIALIZE_MODULE( gameAudio );
    RED_INITIALIZE_MODULE( functionalTestsService );
    RED_INITIALIZE_MODULE( worldPhysics );
    RED_INITIALIZE_MODULE( worldBlackboardSystem );
    RED_INITIALIZE_MODULE( gameServicesWin );
    RED_INITIALIZE_MODULE( gpuApi );
    RED_INITIALIZE_MODULE( gameQuest );
    RED_INITIALIZE_MODULE( gameWatchdog );
    RED_INITIALIZE_MODULE( networkCore );
    RED_INITIALIZE_MODULE( interop );
    RED_INITIALIZE_MODULE( debugPages );
    RED_INITIALIZE_MODULE( gameTweakDB );
    RED_INITIALIZE_MODULE( archive );
    RED_INITIALIZE_MODULE( spline );
    RED_INITIALIZE_MODULE( galaxy );
    RED_INITIALIZE_MODULE( gameSpawn );
    RED_INITIALIZE_MODULE( gameSpatialQueries );
    RED_INITIALIZE_MODULE( worldStatic );
    RED_INITIALIZE_MODULE( worldStreaming );
    RED_INITIALIZE_MODULE( navmeshGeneration );
    RED_INITIALIZE_MODULE( worldEntities );
    RED_INITIALIZE_MODULE( worldNavigation );
    RED_INITIALIZE_MODULE( particles );
    RED_INITIALIZE_MODULE( triggers );
    RED_INITIALIZE_MODULE( textSystem );
    RED_INITIALIZE_MODULE( gamePersistencySystem );
    RED_INITIALIZE_MODULE( gameCore );
    RED_INITIALIZE_MODULE( animation );
    RED_INITIALIZE_MODULE( spacePartitioning );
    RED_INITIALIZE_MODULE( renderDraw );
    RED_INITIALIZE_MODULE( core );
    RED_INITIALIZE_MODULE( shaderCache );
    RED_INITIALIZE_MODULE( resourceSVG );
    RED_INITIALIZE_MODULE( functionalTestsData );
    RED_INITIALIZE_MODULE( multiplayerGame );
    RED_INITIALIZE_MODULE( gameWorkspots );
    RED_INITIALIZE_MODULE( resourceMaterial );
    RED_INITIALIZE_MODULE( app );
    RED_INITIALIZE_MODULE( localizationPersistence );
    RED_INITIALIZE_MODULE( gameTelemetry );
    RED_INITIALIZE_MODULE( gameServicesOrbis );
    RED_INITIALIZE_MODULE( worldEnvironment );
    RED_INITIALIZE_MODULE( gameAsserts );
    RED_INITIALIZE_MODULE( gameSave );
    RED_INITIALIZE_MODULE( gameSmartObjects );
    RED_INITIALIZE_MODULE( gameEffectSystem );
    RED_INITIALIZE_MODULE( gameVisionModeSystem );
    RED_INITIALIZE_MODULE( multiplayerFramework );
    RED_INITIALIZE_MODULE( worldVisibility );
    RED_INITIALIZE_MODULE( worldAnimation );
    RED_INITIALIZE_MODULE( soundMetadata );
    RED_INITIALIZE_MODULE( gamePSM );
    RED_INITIALIZE_MODULE( gameRulesSystem );
    RED_INITIALIZE_MODULE( gameCpo );
    RED_INITIALIZE_MODULE( worldTerrain );
    RED_INITIALIZE_MODULE( worldUserInterface );
    RED_INITIALIZE_MODULE( replication );
    RED_INITIALIZE_MODULE( input );
    RED_INITIALIZE_MODULE( videoPlayer );
    RED_INITIALIZE_MODULE( gameMappins );
    RED_INITIALIZE_MODULE( gameFramework );
}
