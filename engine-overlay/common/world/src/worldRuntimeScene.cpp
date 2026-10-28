/**
* Copyright (c) 2015 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include "worldRuntimeScene.h"
#include "worldRuntimeSystemsSetup.h"
#include "worldNode.h"
#include "worldNodeEditorData.h"
#include "worldNodeInstance.h"
#include "runtimeSystemDebugRendering.h"
#include "runtimeSystemNodeStreaming.h"
#include "worldRuntimeSystemPrefabInstancing.h"
#include "renderDebugContext.h"
#include "gameQuality.h"

#include "worldStreamingDebugMinimapInterface.h"

//#david.block: need to reexamine generic shutdown code
#include "../../worldEntities/include/worldEntitiesPublic.h"
#include "../../worldEntities/include/runtimeSystemEntity.h"
#include "../../worldEntities/include/runtimeSystemEntityAppearanceChanger.h"
#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"

#include "../../engine/include/systemsUpdater.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../renderData/include/renderFrameInfo.h"
#include "../../renderData/include/renderer.h"
#include "../../redJobs2/include/jobBuilder.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeScene, world )
RTTI_END_TYPE()

namespace world
{

	RuntimeScene::RuntimeScene()
	{
		red::Memzero( &m_systems, sizeof( m_systems ) );
	}

	RuntimeScene::~RuntimeScene()
	{
		// ctremblay: Should be called from Shutdown explicitly. But, of course, editor and scene preview are completely hacked together. 
		// should not crash if called more than once. It might crash if incorrect shutdown sequence are in there.
		ShutdownSystems( true ); 
	}

	void RuntimeScene::Initialize( const RuntimeSystemInitInfo& initInfo, const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
	{
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		job::Counter counter = CreateSystems( initInfo );
		job::Builder builder{ context };
		builder.DispatchWait( counter );
	}

	void RuntimeScene::Setup( const RuntimeSceneSetupInfo& setupInfo, job::Counter& counter )
	{
		SetRuntimeInfo( setupInfo.m_runtimeInfo );
		counter += SetupSystems( setupInfo );
	}

	void RuntimeScene::SetRuntimeInfo( const world::RuntimeInfo& runtimeInfo )
	{
		m_runtimeInfo = runtimeInfo;
	}

	void RuntimeScene::Shutdown()
	{
		ShutdownSystems();
	}

	void RuntimeScene::AttachGame( game::IGameInstance& gameInstance )
	{
		RED_FATAL_ASSERT(m_gameInstance == nullptr, "Scene already has a game instance attached");
		m_gameInstance = &gameInstance;

		for( const auto & system : m_systems )
		{
			if( system )
			{
				system->Attach(*m_gameInstance);
			}
		}

		RED_LOG("World: Game attached to world");
	}

	void RuntimeScene::DetachGame( game::IGameInstance& gameInstance )
	{
		RED_FATAL_ASSERT(m_gameInstance == &gameInstance, "Scene has no game instance attached");

		
		if(GRender)
		{
			// Help avoid deadlock from full render command queue. Flush render commands to help ensure that render command queue isn't "suspended" during rendering.
			// We're about to shove in tons of render commands in. E.g., removing render proxies.

			// This first flush is probably enough and the rest are overkill. If this fixes the queue getting full in CYB-130869
			// then try with only this flush.
			GRender->Flush();
		}

		for( const auto& system : m_systems.Reverse() )
		{
			if( system )
			{
				system->Detach(*m_gameInstance);
			}
		}
		
		m_gameInstance = nullptr;

		RED_LOG("World: Game detached from world");
	}

    void RuntimeScene::OnQualitySettingsChanged( const game::GameQuality& newQuality )
    {
        for( const auto & system : m_systems )
        {
            if( system )
            {
                system->OnQualitySettingsChanged( newQuality );
            }
        }
    }

    void RuntimeScene::PushNodeInstanceForDebug( THandle<INodeInstance> node ) const
	{
		if (m_nodeInstanceDebugRenderData)
		{
			m_nodeInstanceDebugRenderData->m_nodes.PushBack( node );
		}
	}

	void RuntimeScene::PushPrefabNodeInstanceForDebug( THandle<INodeInstance> node ) const
	{
		if (m_nodeInstanceDebugRenderData)
		{
			m_nodeInstanceDebugRenderData->m_prefabs.PushBack( node );
		}
	}

	const red::DynArray<THandle<world::INodeInstance>> & RuntimeScene::GetDebugPrefabNodes() const
	{
		RED_FATAL_ASSERT( m_nodeInstanceDebugRenderData, "Trying to get debug data before it's been created" );
		return m_nodeInstanceDebugRenderData->m_prefabs;
	}

	void RuntimeScene::OnGainFocus()
	{
		for ( auto &system : m_systems )
		{
			if ( system )
			{
				system->OnGainFocus();
			}
		}
	}

	void RuntimeScene::OnLoseFocus()
	{
		for ( auto &system : m_systems )
		{
			if ( system )
			{
				system->OnLoseFocus();
			}
		}
	}

	Bool RuntimeScene::ShouldInstantiateSystem( const rtti::ClassType* classPtr, const IRuntimeSystem::Setup& setup, const RuntimeSystemInitInfo& initInfo )
	{
		if ( initInfo.mode == RuntimeMode::Singleplayer && !setup.isRequiredInSingleplayer )
		{
			RED_LOG( "Core: System '%hs' not allowed in singleplayer", classPtr->GetName().AsChar() );
			return false;
		}
		else if ( initInfo.mode == RuntimeMode::Multiplayer_Client || initInfo.mode == RuntimeMode::Multiplayer_Server )
		{
			//check for server settings
			if ( initInfo.mode == RuntimeMode::Multiplayer_Server )
			{
				if ( !IsHeadlessMode() && !setup.isRequiredOnServer )
				{
					RED_LOG( "Core: System '%hs' not allowed on non-headless server", classPtr->GetName().AsChar() );
					return false;
				}
				else if ( IsHeadlessMode() && !setup.isRequiredOnHeadlessServer )
				{
					RED_LOG( "Core: System '%hs' not allowed on headless server", classPtr->GetName().AsChar() );
					return false;
				}
			}
			//check for client settings
			else
			{
				if ( !IsHeadlessMode() && !setup.isRequiredOnClient )
				{
					RED_LOG( "Core: System '%hs' not allowed on client", classPtr->GetName().AsChar() );
					return false;
				}
				else if ( IsHeadlessMode() && !setup.isRequiredOnHeadlessClient )
				{
					RED_LOG( "Core: System '%hs' not allowed on headless client", classPtr->GetName().AsChar() );
					return false;
				}
			}
		}
		else if ( initInfo.mode == RuntimeMode::Preview || initInfo.mode == RuntimeMode::GamePreview || initInfo.mode == RuntimeMode::Thumbnail )
		{
			if ( !setup.isRequiredInPreview )
			{
				RED_LOG( "Core: System '%hs' not allowed in preview", classPtr->GetName().AsChar() );
				return false;
			}
		}

		return true;
	}

	job::Counter RuntimeScene::CreateSystems( const RuntimeSystemInitInfo& initInfo )
	{
		job::Counter counter{ job::Priority::Latent };

		// enumerate possible classes
		red::DynArray< const rtti::ClassType* > systemClasses{ red::PoolEngine() };
		GetRttiSystem().EnumClasses( ClassID< IRuntimeSystem >(), systemClasses );
		RED_LOG( "Core: Found %d scene runtime systems", systemClasses.Size() );

		// #tbd: or just don't hard code MAX_SYSTEMS?
		RED_FATAL_ASSERT( systemClasses.Size() <= MAX_SYSTEMS, "Need to increase MAX_SYSTEMS from %u to %u", MAX_SYSTEMS, systemClasses.Size() );

		m_systems.Resize( MAX_SYSTEMS );

		// prepare systems setup info
		RuntimeSystemsSetup systemsSetup;

		// create the systems
		for ( const auto* classPtr : systemClasses )
		{
			const auto setup = systemsSetup.GetSetupForSystem( classPtr->GetName() );

			// do the filtering
			if ( ShouldInstantiateSystem( classPtr, setup, initInfo ) )
			{
				auto system = classPtr->CreateHandle< IRuntimeSystem >();

				// register in the system list
				const auto systemID = system->GetSystemID();
				RED_FATAL_ASSERT( m_systems[systemID] == nullptr, "System ID %d is already in use", systemID );
				m_systems[systemID] = system;
			}
		}

		// initialize the systems in the order indicated by their IDs
		for ( const auto & system : m_systems )
		{
			if ( system )
			{
				const auto setup = systemsSetup.GetSetupForSystem( system->GetLocalClass()->GetName() );
				counter += system->Initialize( *this, initInfo, setup );
			}
		}

		INodeInstance::InitPlaceholderClassNameMap();
		// runtime scene initialized
		return counter;
	}

	job::Counter RuntimeScene::SetupSystems( const RuntimeSceneSetupInfo& setupInfo )
	{
		job::Counter counter{ job::Priority::Latent };

		// initialize the systems in the order indicated by their IDs
		for ( const auto & system : m_systems )
		{
			if ( system )
			{
				RED_FATAL_ASSERT( system->IsInitialized(), "Runtime system is not initialized" );
				counter += system->SetupSystem( *this, setupInfo );
				InitializeSystemUpdateFunctions( system.Get(), setupInfo.m_processor );
			}
		}

		return counter;
	}

	void RuntimeScene::InitializeSystemUpdateFunctions( IRuntimeSystem* system, SystemsUpdater* updater )
	{
		UpdatableSystemRegistrar registrar( *updater );
		system->RegisterUpdateFunctions( registrar );
	}

	void RuntimeScene::OnStreamingWorldLoaded( job::Counter& counter )
	{
		unsigned cpSystemIndex=0;
		for ( const auto& system : m_systems )
		{
			if ( system )
			{
				RED_FATAL_ASSERT( system->IsInitialized(), "Runtime system is not initialized" );
				
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=system-begin index=%u\n",cpSystemIndex);
#endif
system->OnStreamingWorldLoaded( *this, counter );
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=system-end index=%u\n",cpSystemIndex);
#endif

			}
            ++cpSystemIndex;
		}
	}

	void RuntimeScene::ShutdownSingleSystem( SystemID systemId, Bool uninitialize )
	{
		if ( m_systems[ systemId ] != nullptr )
		{
			if ( uninitialize && m_systems[ systemId ]->IsInitialized() )
			{
				m_systems[ systemId ]->Uninitialize( *this );
			}
			else if ( !uninitialize && m_systems[ systemId ]->NeedsPostWorldDetached() )
			{
				m_systems[ systemId ]->PostWorldDetached( *this );
			}
		}
	}

	template< typename T >
	void RuntimeScene::ShutdownSingleSystem( Bool uninitialize )
	{
		ShutdownSingleSystem( T::SYSTEM_ID, uninitialize );
	}

	void RuntimeScene::ShutdownSystems( Bool uninitialize )
	{
		if ( m_systems.Empty() )
		{
			return;
		}

		// hacky, but we need to make sure that all nodes are detached before proceeding
		{
			RuntimeSystemNodeStreaming* sysWorldNodes = GetSystem< RuntimeSystemNodeStreaming >();
			if ( sysWorldNodes )
			{
				sysWorldNodes->CleanupNodeInstances();
			}
		}

		// TODO as below - there are system interdependencies - systems containing streaming grid must be released first
		ShutdownSingleSystem< RuntimeSystemPrefabInstancing >( uninitialize );

		// work::WorkspotGlobalItemInstance is spawning items using world::RuntimeSystemEntity
		// We have to finish all node streaming jobs (@see world.NodeStreamingGrid.FlushDanglingLoadingJobs() called  from world::RuntimeSystemWorldStreaming::OnPostWorldDetached())
		// before world::RuntimeSystemEntity unitialization/post detach.
		ShutdownSingleSystem< RuntimeSystemWorldStreaming >( uninitialize );

		// Appearance changer needs to be shut down before entity scene is deleted, since it may still be processing appearance change operations.
		ShutdownSingleSystem< RuntimeSystemEntityAppearanceChanger >( uninitialize );

		// #david.block: #tbd: Need to shutdown the entity scene before any other systems
		// because entities and their components could be depending on them
		// #fixme: should make sure RuntimeSystemEntities is the last initialized, so the first to be destroyed!
		ShutdownSingleSystem< RuntimeSystemEntity >( uninitialize );

		// #david.block: Two phase shutdown. Ideally we would shutdown and delete, but something may be caching a pointer
		// and then it will be dangling. Now the risk is more some imbalanced shutdown
		for ( auto id : m_systems.ReverseIndices() )
		{
			ShutdownSingleSystem( id, uninitialize );
		}
	}

	void RuntimeScene::ShutdownSystemsAfterFailedInitialization()
	{
		for ( auto& failedSystem : m_systems )
		{
			if ( failedSystem )
			{
				const auto systemID = failedSystem->GetSystemID();

				failedSystem->Uninitialize( *this );
				failedSystem.Reset();
				m_systems[ systemID ] = nullptr;
			}
		}
	}

	rend::DebugFilter DF_SelectedNodeName( "Nodes/SelectedName" );
	rend::DebugFilter DF_SelectedNodeName2d( "Nodes/SelectedName2d" );

	void RuntimeScene::FinalizeNodeDebugRender( rend::IDebugDrawer& dd )
	{
		if ( dd.GetMask().Test( DF_SelectedNodeName2d ) || dd.GetMask().Test( DF_SelectedNodeName ) )
		{
			String typeName;
			Float textScaling = 1.1f;
			static const Int32 yStartOffset = 5;
			Int32 yPos = yStartOffset;

			rend::FragmentBuilder builder( dd );
			Color textBackground( Color::BLACK() );
			textBackground.A = 100; //Make the background translucent...but doesn't seem to work?
			builder.EnableTextBackground( textBackground );
			red::DynArray<CName> namesToPrint{ red::PoolDebug() };

			auto debugDrawNodeInstanceNames = [&builder]( THandle<INodeInstance> nodeInstance )
			{
				auto node = nodeInstance->GetSourceNode();

				CName typeName = node->GetClass()->GetName();
				CName nodeName = node->GetName();

				Vector4 center = nodeInstance->GetInitialTransform().TransformPoint( node->GetPivotTransform().GetPosition() );
				Float verticalOffset = node->GetEditorData().GetBoundingBox().CalcSize().Z / 2.f + 0.2f;
				center.Z = ( center.Z + verticalOffset );

				static const Int32 yOffset = 10, xOffset = 10;
				builder.AddWorldTextf( center, xOffset, yOffset, "%s (%s) ", nodeName.AsChar(), INodeInstance::GetUserClassName( typeName ).AsChar() );
			};

			auto addToNamesToPrintList = [ &builder, &namesToPrint, textScaling]
			( THandle<INodeInstance> nodeInstance )
			{
				auto node = nodeInstance->GetSourceNode();

				CName nodeName = node->GetName();
				Int32 localNodeNameWidth = ( Int32 )( builder.MeasureText( nodeName.AsChar(), textScaling ).X );
				namesToPrint.PushBack( nodeName );
			};

			for ( const auto& prefab : m_nodeInstanceDebugRenderData->m_prefabs )
			{
				if ( prefab->GetParentNode() == nullptr || prefab->GetParentNode()->ShouldRenderDebugName( dd ) ) //Must call this on the parent or the prefab will always filter itself
				{
					if ( dd.GetMask().Test( DF_SelectedNodeName2d ) )
					{
						addToNamesToPrintList( prefab );
						typeName = INodeInstance::GetUserClassName( prefab->GetSourceNode()->GetClass()->GetName() );
					}
					if ( dd.GetMask().Test( DF_SelectedNodeName ) )
					{
						debugDrawNodeInstanceNames( prefab );
					}
				}
			}

			for ( const auto& node : m_nodeInstanceDebugRenderData->m_nodes )
			{
				if ( node->ShouldRenderDebugName( dd ) )
				{
					if ( dd.GetMask().Test(DF_SelectedNodeName2d) )
					{
						addToNamesToPrintList( node );
						typeName = INodeInstance::GetUserClassName( node->GetSourceNode()->GetClass()->GetName() );
					}
					if ( dd.GetMask().Test( DF_SelectedNodeName ) )
					{
						debugDrawNodeInstanceNames( node );
					}
				}
			}

			if ( namesToPrint.Size() > 1 )
			{
				typeName = "Multiple Selected";
			}

			typeName = String::Printf( "( %s )", typeName.AsChar() );

			Uint32 rightMargin = builder.GetScreenWidth() - 5;

			for ( const auto& nodeName : namesToPrint )
			{
				Uint32 width = 0, height = 0;
				width = (Int32)( builder.MeasureText( nodeName.AsChar() ).X );
				height = builder.GetFontLineHeight();
				yPos += height;

				builder.AddFlatText( rightMargin - width, yPos, nodeName.AsChar(), textScaling );

			}

			if ( namesToPrint.Size() > 0 )
			{
				Uint32 width = 0, height = 0;
				width = (Int32)( builder.MeasureText( typeName.AsChar() ).X );
				height = builder.GetFontLineHeight();
				yPos += height;

				builder.AddFlatText( rightMargin - width, yPos, typeName.AsChar(), textScaling );
			}
		}

		m_nodeInstanceDebugRenderData->m_nodes.Clear();
		m_nodeInstanceDebugRenderData->m_prefabs.Clear();
	}

	void RuntimeScene::RenderDebug( CRenderFrameInfo& info, CRenderCameraFrameInfo& cameraInfo )
	{
		PC_SCOPE( RuntimeScene_RenderDebug );

		const CRenderCamera& camera = cameraInfo.GetRenderCamera();
		RenderDebugContext context
		{
			info,
			cameraInfo.GetRenderingMask(),
			cameraInfo.GetDebugDrawer(),
			cameraInfo.m_frustum,
			camera.GetPosition().AsVector3(),
			camera.GetFOV(),
			camera.GetCameraForward().Normalized3(),
			camera.GetCameraUp().Normalized3(),
			camera.GetCameraRight().Normalized3()
		};

#if !defined(RED_CONFIGURATION_FINAL) || defined(USE_PROFILER)
		if( m_streamingDebugMinimap )
		{
			m_streamingDebugMinimap->Render( context );
		}

		// render systems
		for( Uint32 i = 0; i < MAX_SYSTEMS; ++i )
		{
			THandle< IRuntimeSystem > system = m_systems[ i ];
			if( system != nullptr
#ifdef RED_CONFIGURATION_FINAL
				&& system->NeedsRenderDebugInProfiling()
#endif
				&& ( info.m_systemOnRenderDebug || system->CanRenderDebugOnSelection() ) )
			{
				m_systems[ i ]->OnRenderDebug( context );
			}
		}
#endif

#ifdef RED_ENABLE_RENDER_DEBUG

		if (!m_nodeInstanceDebugRenderData)
		{
			m_nodeInstanceDebugRenderData = red::CreateUniquePtr<NodeInstanceSceneDebugRenderData>();
		}

		RuntimeSystemDebugRendering* debugRenderSystem = GetSystem< RuntimeSystemDebugRendering >();
		if( debugRenderSystem )
		{
			// Not available on dedicated server
			debugRenderSystem->ExecuteRenderDebug( cameraInfo );
		}

		FinalizeNodeDebugRender( context.debugDrawer );

#endif

	}

	red::UniquePtr< RuntimeScene > CreateBackedScene( const RuntimeSceneSetupInfo& setupInfo, Bool delayLink )
	{
		red::UniquePtr< RuntimeScene > ret = red::CreateUniquePtr< RuntimeScene >();

		red::Atomic<Uint32> keepRunning{ true };
		job::Builder builder;
		builder.DispatchJob( "CreateBackedScene", [&ret, &keepRunning, mode = setupInfo.m_runtimeInfo.GetRuntimeMode()]( const job::RunContext& context )
		{
			world::RuntimeSystemInitInfo initInfo{ mode };
			ret->Initialize( initInfo, context, keepRunning );
		} );

		auto counter = builder.ExtractWaitCounter();
		job::FlushCounter( counter );
	
		if ( !keepRunning.GetValue() )
		{
			RED_LOG_ERROR( "CreateBackedScene: Failed to initialize scene" );
			ret->Shutdown();
			return nullptr;
		}

		job::Counter setupCounter;
		ret->Setup( setupInfo, setupCounter );
		job::FlushCounter( setupCounter );

		if ( !delayLink )
		{
			setupInfo.m_processor->Link();
		}

		return ret;
	}

} // world
