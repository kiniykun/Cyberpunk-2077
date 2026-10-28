/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "runtimeSystemNodeStreaming.h"
#include "worldNodeInstance.h"
#include "worldNode.h"
#include "worldContentSupplier.h"
#include "worldGlobalNodeRuntimeResolver.h"
#include "worldContentObserver.h"
#include "renderDebugContext.h"
#include "worldStreamingListener.h"

#include "../../world/include/cameraSystemInterface.h"
#include "../../world/include/gameInstance.h"


#include "../../renderData/include/renderFrameInfo.h"
#include "../../imgui/include/redImGui.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "worldNodeStreamingProxy.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "cameraSystemInterface.h"
#include "prefabProxyMeshNodeInstance.h"
#include "prefabProxyMeshNode.h"

#include "../../resourceMesh/include/mesh.h"
#include "../../redReflection/include/mathUtils.h"
#include "../../redSystem/include/stopWatch.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif
#include <cstdio>
#include <atomic>

// CP2077 web [terrain]: prefab proxy mesh diagnostics. ?proxyprobe=1 logs, every ~10 s, how many prefab proxies
// (terrain prefab_lodN tiles and building proxies) are saturated (all children attached -> near auto-hide applied)
// vs locked (some child not attached -> the coarse proxy keeps drawing up close), and the locked ones nearest to the
// camera. ?proxyNear=force applies the near auto-hide even when the proxy is not saturated (A/B only).
//
// FIX (default ON, ?proxyNear=engine = original behaviour): the Minimal-mode cook of 03_night_city lost one terrain
// LOD tile node (terrain_aaayaai_prefab_lod4_16352_16336, x -4096..-2048 / y -6144..-4096), so its parent, the 4 km
// lod5 tile over the whole Nomad start area, waits forever for 4/4 children (3/4 exist), keeps its near auto-hide
// cleared and draws its very coarse geometry right over the detailed terrain cells and roads ("terrain clipping
// through the roads"). Terrain tile proxies that stay partially attached (>= 1 child, not all) for 3 s now get their
// near auto-hide anyway; tiles with no attached child keep the engine behaviour (no holes when nothing is loaded).
extern "C" { int cp2077_terrain_proxyprobe = 0; } // [terrain] read by the renderer overlay (renderProxyMesh.cpp)
namespace world
{
	static int s_cpProxyProbe = -1;
	// near auto-hide policy for proxies whose children are not all attached: 0 = engine (keep drawing the coarse
	// proxy up close), 1 = default: terrain tile proxies partially attached for >= 3 s get the near auto-hide,
	// 2 = force for every proxy (A/B only). ?proxyNear=engine|force
	static int s_cpProxyNearForce = 1;
	// per proxy state index (heap, allocated on first use: static tables count against the link's initial memory)
	static double* s_cpPartialSince = nullptr;   // when it became partially attached
	static Bool* s_cpPartialReady = nullptr;     // partially attached long enough -> near auto-hide applies
	static void CpProxyProbeInit()
	{
		if ( s_cpProxyProbe >= 0 )
		{
			return;
		}
#ifdef __EMSCRIPTEN__
		s_cpProxyProbe = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'proxyprobe' ); return ( v && v !== '0' ) ? 1 : 0; } catch ( e ) { return 0; } } );
		s_cpProxyNearForce = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'proxyNear' ); return v === 'force' ? 2 : ( v === 'engine' || v === '0' ) ? 0 : 1; } catch ( e ) { return 1; } } );
#else
		s_cpProxyProbe = 0;
#endif
		cp2077_terrain_proxyprobe = s_cpProxyProbe;
		std::fprintf( stderr, "CP2077 terrain proxy switches: proxyprobe=%d proxyNear=%s\n", s_cpProxyProbe, s_cpProxyNearForce == 2 ? "force" : s_cpProxyNearForce == 1 ? "terrainpartial" : "engine" );
	}
	// child attach counters (NotifyProxyMeshChildNodeAttached): calls, no setup info, with proxy id, road/building/terrain flags
	static std::atomic< Uint32 > s_cpChild[ 10 ];
	static double CpProxyNowMs()
	{
#ifdef __EMSCRIPTEN__
		return emscripten_get_now();
#else
		return 0.0;
#endif
	}
}

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeSystemNodeStreaming, world );
	RTTI_PARENT_TYPE( world::IRuntimeSystem );
RTTI_END_TYPE(); 

RTTI_BEGIN_ABSTRACT_TYPE_IN_NAMESPACE( EditorDebugColoringSettings, world );
	RTTI_PARENT_TYPE( ISerializable );
RTTI_END_TYPE();

//#pragma optimize("",off)

namespace Config
{
	TConfigVar< Float > cvNodeAttachingTimeLimit( "Streaming", "TimeLimitAttachingPerFrame", 0.5f, 0.0f, FLT_MAX, eConsoleVarFlag_Save );
	TConfigVar< Float > cvEditorNodeAttachingTimeLimit( "Streaming/Editor", "TimeLimitAttachingPerFrame", 10.0f, 0.0f, FLT_MAX, eConsoleVarFlag_Save );
	TConfigVar< Float > cvEditorNodeDetachingTimeLimit( "Streaming/Editor", "TimeLimitDetachingPerFrame", 10.0f, 0.0f, FLT_MAX, eConsoleVarFlag_Save );
}

namespace world	
{
	RuntimeSystemNodeStreaming::RuntimeSystemNodeStreaming()	
		: m_scene( nullptr )
		, m_allInstances( red::PoolEngine() )
		, m_instancesToUpdate( red::PoolEngine() )
		, m_wasStreamingLastFrame( true )
		, m_instancesDestructionCounter( job::Priority::Latent )
		, m_nodeAttached( 0 )
		, m_isLoadingMode( false )
		, m_generation( 0 )
	{
	}

	RuntimeSystemNodeStreaming::~RuntimeSystemNodeStreaming()
	{
		RED_FATAL_ASSERT( !m_scene, "Runtime system was not shut down" );
		RED_FATAL_ASSERT( m_observers.Empty(), "There are still some world content observers attached" );
		RED_FATAL_ASSERT( m_allInstances.Empty(), "There are still some node instances attached to the scene" );
		RED_FATAL_ASSERT( m_nodeInstanceToAttachSerial.Empty(), "There are still some node instances pending to attach" );
		RED_FATAL_ASSERT( m_nodeInstanceToDetachSerial.Empty(), "There are still some node instances pending to detach" );
		RED_FATAL_ASSERT( m_streamingListeners.Empty(), "There are still some streaming listeners registered" );
	}

	Bool RuntimeSystemNodeStreaming::IsStreaming() const
	{
		return m_wasStreamingLastFrame.GetValue();
	}

	void RuntimeSystemNodeStreaming::ForceUpdate()
	{
		// #tbd: was missing PoolFrame and PoolDoubleBufferedFrame
		// So for now just be safe and don't use a frame allocator for this!
		job::FlushCounter( m_processContentCounter );
		// Finish processing last frame content, so we don't run a risk of a
		// race condition in ScheduleParallelNodesOperation if same nodeInstance ends up in both 
		// m_nodeInstanceToAttachParallel and m_nodeInstanceToDetachParallel at the same time
		// Blind fix for NodeInstances sometimes destroying on shutdown without being detached first (CYB-629721 & CYB-604390)
		job::Builder lastframeUpdateBuilder;
		Update( lastframeUpdateBuilder, true );
		DestroyPendingNodeInstances( lastframeUpdateBuilder );
		auto lastFrameCounter = lastframeUpdateBuilder.ExtractWaitCounter();
		job::FlushCounter( lastFrameCounter );
		job::Builder builder;
		ProcessContent( builder );
		job::FlushCounter( builder.ExtractWaitCounter() );
		job::Builder updateBuilder;
		Update( updateBuilder, true );
		DestroyPendingNodeInstances( updateBuilder );
		auto counter = updateBuilder.ExtractWaitCounter();
		job::FlushCounter( counter );
		job::FlushCounter( m_instancesDestructionCounter );


		RED_FATAL_ASSERT( m_nodeInstanceToAttachSerial.Empty(), "There are still some node instances pending to attach" );
		RED_FATAL_ASSERT( m_nodeInstanceToDetachSerial.Empty(), "There are still some node instances pending to detach" );
	}

	void RuntimeSystemNodeStreaming::RegisterUpdateFunctions( UpdatableSystemRegistrar& registrar )
	{
		registrar.Register( UpdateTickGroup::PlayerAimUpdate, *this, "RuntimeSystemNodeStreaming/ProcessContent", [this]( const UpdateContext& updateContext, job::Builder& ) 
		{
			job::Builder builder{job::Priority::RenderPath}; // ctremblay: RenderPath as I do not want to block game jobs of running. 

			ProcessPrefabProxyMeshes( builder );
			ProcessContent( builder );
			m_processContentCounter += builder.ExtractWaitCounter();
		} );

		registrar.Register( UpdateTickGroup::FrameBegin, *this, "RuntimeSystemNodeStreaming/ProcessNodes",
			[this] ( const UpdateContext& updateContext, job::Builder& builder )
		{
			const Float engineTime = updateContext.m_timeDelta;
			const tick::Info& tickInfo = updateContext.m_tickInfo;
		
			if( m_sharedContentContext )
			{
				builder.DispatchWait( m_processContentCounter );
				Update( builder );
				UpdateNodes( builder, engineTime, tickInfo );

				DestroyPendingNodeInstances( builder );
			}
		} );
	}

	SystemID RuntimeSystemNodeStreaming::GetSystemID() const
	{
		return SYSTEM_ID;
	}

	void RuntimeSystemNodeStreaming::OnInitialize( RuntimeScene& scene, const RuntimeSystemInitInfo& initInfo, job::Counter& counter )
	{
		m_scene = &scene;
	}

	void RuntimeSystemNodeStreaming::OnUninitialize( RuntimeScene& scene )
	{
		RED_FATAL_ASSERT( m_allInstances.Empty(), "Improper world and scene shutdown! Was the world unmounted first? Remaining node instances: %u", m_allInstances.Size() );

		// TODO flush everything (detach nodes, destroy them etc)

		m_scene = nullptr;
	}

	red::UniquePtr< ContentObserver > RuntimeSystemNodeStreaming::CreateObserver(const String& debugName)
	{
		red::UniquePtr< ContentObserver > observer = red::CreateUniquePtr< ContentObserver >( *this, debugName );
		RED_SCOPE_LOCK( m_observersLock );
		m_observers.PushBack( observer.Get() );
		return observer;
	}

	void RuntimeSystemNodeStreaming::DetachObserver( ContentObserver* observer )
	{
		RED_SCOPE_LOCK( m_observersLock );
		RED_VERIFY( m_observers.RemoveReorder( observer ).IsSuccessful(), "Scene observer does not exist" );
	}

    struct NodeMemoryStat
    {
        THandle<Node> node = 0;
        Uint32 nodeSize = 0;
        Uint32 instanceSize = 0;
        Uint32 nbInstances = 0;

        Float NodeSize_kB() const { return (Float)nodeSize / 1024.f; }
        Float InstanceSize_kB() const { return (Float)instanceSize / 1024.f; }
        Float TotalInstanceSize_kB() const { return (Float)(instanceSize * nbInstances) / 1024.f; }

        Bool operator <  ( const NodeMemoryStat& other ) const { return node < other.node; }
        Bool operator == ( const NodeMemoryStat& other ) const { return node == other.node; }

    };

    static rend::DebugFilter DF_NodeStreaming_ShowClassMemoryStats( "RuntimeSystem/Node/MemoryStats" );
	static rend::DebugFilter DF_NodeStreaming_ShowStreamingObservers("RuntimeSystem/Node/StreamingObservers");
	static rend::DebugFilter DF_NodeStreaming_ShowProxyMeshesInfo( "RuntimeSystem/Node/ProxyMeshInfo" );
	void RuntimeSystemNodeStreaming::OnRenderDebug( const RenderDebugContext& context )
	{
		DrawNodeStreamingStats( context.debugDrawer );
		RenderStreamingObservers( context );
		RenderProxyMeshesInfo( context );
		RenderImGuiNodesInfo( context );

		if ( m_editorDebugColoringSettings )
		{
			// update the frame info if debug coloring is on
			context.frameInfo.m_enableDebugMeshColoring = true;
		}
	}

	void RuntimeSystemNodeStreaming::RenderStreamingObservers(const RenderDebugContext& context)
	{
		if (!context.debugFilterMask.Test(DF_NodeStreaming_ShowStreamingObservers))
		{
			return;
		}

		rend::FragmentBuilder builder(context.debugDrawer);
		builder.EnableOverlay(true);
		builder.SetColor(Color::GREEN());

		const Vector3 activeCameraPosition = m_cameraSystem ? m_cameraSystem->GetActiveCameraPosition() : Vector3::ZEROS();

		RED_SCOPE_SHARED_LOCK(m_observersLock);
		for (const auto* obs : m_observers)
		{
			if (obs->HasValidReferencePosition())
			{
				const auto pos = obs->CalcForwardVelocityPosition();
				builder.AddWireSphere(pos, 0.5f);
				builder.AddWorldTextf(pos, 0, 0, "%hs: %.2f m/s", obs->GetDebugName().AsChar(), obs->DEBUG_CalcForwardVelocitySpeed());
				builder.AddWorldTextf(pos, 0, 15, "cameraDist: %.2f", activeCameraPosition.DistanceTo(pos));
			}
		}
	}

	void RuntimeSystemNodeStreaming::RenderProxyMeshesInfo( const RenderDebugContext& context )
	{
		if( !context.debugFilterMask.Test( DF_NodeStreaming_ShowProxyMeshesInfo ) )
			return;

		rend::FragmentBuilder fb( context.debugDrawer );
		fb.EnableOverlay( false );
		fb.EnableTransparency( true );
		fb.EnableTextBackground( Color::GRAY() );
		RED_SCOPE_SHARED_LOCK( m_proxyChildLock );
		for( auto it = m_proxyLookup.Begin(); it != m_proxyLookup.End(); ++it )
		{
			const Uint32 index = it.Value().index;
			const PrefabProxyMeshState& state = m_proxyState[index];
			const THandle<PrefabProxyMeshNodeInstance> instance = state.weakProxyMesh.ToHandle();
			if( instance )
			{
				const THandle<PrefabProxyMeshNode>& source = instance->GetSourceNodeAs<PrefabProxyMeshNode>();
				const THandle<CMesh> mesh = instance->GetLoadedMesh();
				const Box localBounds = mesh->GetBoundingBox() * 1.05f;
				const Box worldBounds = instance->Slow_GetInitialTransformAsMatrix().TransformBox( localBounds );
				if( context.frustum.TestBox( worldBounds ) == FR_Outside )
					continue;

				fb.SetColor( state.antistreamingLocked ? Color::RED() : Color::GREEN() );
				fb.AddWireBox( worldBounds );

                if( state.hideButKeepStreamed )
                {
                    const Box scaledBounds = worldBounds * Vector3( 0.99f );
                    fb.SetColor( Color::ORANGE() );
                    fb.AddWireBox( scaledBounds );
                }


				const Float fillAmount = (Float)state.attachedDeps / (Float)state.nbDeps;
				fb.SetColor( Color::Lerp( fillAmount, Color::RED( 32 ), Color::DARK_GREEN( 32 ) ) );
				fb.AddSolidBox( worldBounds );

				Vector4 closestPoint;
				MathUtils::GeometryUtils::ClosestPointToBox3D( worldBounds, context.m_position, closestPoint );
				if( closestPoint.DistanceSquaredTo( context.m_position ) < MSqr( 20.f ) )
				{
					fb.SetColor( Color::YELLOW() );
					fb.AddWorldTextf( closestPoint, 0, 0, "%u/%u\n%5.5f\n%s", 
						state.attachedDeps, state.nbDeps, 
						source->GetNearAutoHideDistance(),
						res::prv::Debug_GetResourcePathDebugText( source->GetMesh().GetPath() ).AsChar() );
				}
			}
		}
	}

	void RuntimeSystemNodeStreaming::RenderImGuiNodesInfo( const RenderDebugContext& context )
	{
#ifdef RED_USE_IMGUI
		const Bool canUseImGui = context.frameInfo.m_systemOnRenderDebug;
		static Bool s_sortBySource = false;
		if( context.debugFilterMask.Test( DF_NodeStreaming_ShowClassMemoryStats ) && canUseImGui )
		{
			if( ImGui::Begin( "Nodes stats" ) )
			{
				ImGui::SliderFloat( "Global streaming distance scale", &m_globalStreamingDistanceScale, 0.1f, 1.f );
				{
					RED_SCOPE_SHARED_LOCK( m_runtimeDistanceScaleLock );
					if( m_runtimeDistanceScaleData.Size() )
					{
						ImGui::Separator();
						ImGui::Text( "Streaming distance scale overrides" );
						for( const auto& data : m_runtimeDistanceScaleData )
						{
							ImGui::Text( "   %ull : %2.5f", data.source, data.value );
						}
						ImGui::Separator();
					}
				}

				if( ImGui::CollapsingHeader( "ProxyChildCounters", ImGuiTreeNodeFlags_DefaultOpen ) )
				{
					RED_SCOPE_SHARED_LOCK( m_proxyChildLock );
					ImGui::Text( "Number registered: %u", m_proxyIdAllocator.GetNumAllocated() );
					ImGui::Text( "Update time: %5.5f[ms]", m_dbgUpdateProxyChildCountersTimeMS );
					if( ImGui::CollapsingHeader( "Proxies" ) )
					{
                        ImGui::Checkbox( "Show not ready only", &m_dbgShowOnlyNotReadyStreamedProxies );
						ImGui::Columns( 2 );
						{
							for( auto it = m_proxyLookup.Begin(); it != m_proxyLookup.End(); ++it )
							{
								const Uint32 index = it.Value().index;
								const PrefabProxyMeshState& state = m_proxyState[index];
								const THandle<PrefabProxyMeshNodeInstance> instance = state.weakProxyMesh.ToHandle();

                                if( m_dbgShowOnlyNotReadyStreamedProxies )
                                {
                                    if( (instance && (state.attachedDeps != state.nbDeps)) )
                                    {
                                        const THandle<PrefabProxyMeshNode>& source = instance->GetSourceNodeAs<PrefabProxyMeshNode>();
                                        ImGui::Text( "%s (%u)", source->DEBUG_GetPrefabResourcePathString().AsChar(), it.Key() );
                                    }
                                }
                                else
                                {
                                    if( instance )
                                    {
                                        const THandle<PrefabProxyMeshNode>& source = instance->GetSourceNodeAs<PrefabProxyMeshNode>();
                                        ImGui::Text( "%s (%u)", source->DEBUG_GetPrefabResourcePathString().AsChar(), it.Key() );
                                    }
                                    else
                                    {
                                        ImGui::Text( "%u", it.Key() );
                                    }
                                }
								

								ImGui::NextColumn();
								ImGui::Text( "%u/%u", state.attachedDeps, state.nbDeps );
								ImGui::NextColumn();
							}
						}

						ImGui::Columns();
					}
				}

				if( ImGui::CollapsingHeader( "Streamed nodes" ) )
				{
					ImGui::Checkbox( "Sort by Source", &s_sortBySource );
					red::HashMap<const rtti::ClassType*, NodeMemoryStat> tmpStats{ red::PoolFrame() };
					tmpStats.Reserve( 10 * 1024 );

					for( const THandle<INodeInstance>& instance : m_allInstances.Elements() )
					{
						THandle<Node> sourceNode = instance->GetSourceNode();
						if( NodeMemoryStat* found = tmpStats.FindPtr( sourceNode->GetClass() ) )
						{
							found->nbInstances += 1;
						}
						else
						{
							NodeMemoryStat stat;
							stat.node = sourceNode;
							stat.nodeSize = sourceNode->GetClass()->GetSize();
							stat.instanceSize = sourceNode->GetInstanceClass()->GetSize();
							stat.nbInstances = 1;
							tmpStats.Insert( sourceNode->GetClass(), stat );
						}
					}

					red::DynArray<NodeMemoryStat> flat = tmpStats.GetValues();
					std::sort( flat.Begin(), flat.End(), [bySource = s_sortBySource]( const NodeMemoryStat& a, const NodeMemoryStat& b )
					{
						if( bySource )
						{
							return a.nodeSize > b.nodeSize;
						}
						else
						{
							return a.instanceSize*a.nbInstances > b.instanceSize * b.nbInstances;
						}
					} );

					ImGui::Columns( 5 );

					for( Uint32 i = 0; i < flat.Size(); ++i )
					{
						if( i > 100 )
							break;

						const NodeMemoryStat& stat = flat[i];
						const rtti::ClassType* sourceClass = stat.node->GetClass();
						const CName sourceClassName = sourceClass->GetName();

						ImGui::Text( sourceClassName.AsChar() ); ImGui::NextColumn();
						ImGui::Text( "%5.2f", stat.NodeSize_kB() ); ImGui::NextColumn();
						ImGui::Text( "%5.2f", stat.InstanceSize_kB() ); ImGui::NextColumn();
						ImGui::Text( "%u", stat.nbInstances ); ImGui::NextColumn();
						ImGui::Text( "%5.2f", stat.TotalInstanceSize_kB() ); ImGui::NextColumn();
					}
				}
			}
			ImGui::End();
		}
#endif
	}

	void RuntimeSystemNodeStreaming::AttachContentSupplier( IContentSupplier& supplier )
	{
		m_contentSuppliers.PushBack( &supplier );
	}

	void RuntimeSystemNodeStreaming::DetachContentSupplier( IContentSupplier& supplier )
	{
		supplier.CleanupContent(); // allow the content supplier to detach all nodes supplied by it

		m_contentSuppliers.Remove( &supplier );
	}

	void RuntimeSystemNodeStreaming::CleanupNodeInstances()
	{
		PC_SCOPE( RuntimeSystemNodeStreaming/CleanupNodeInstances );

		ForceUpdate();

		// #todo: should detach content suppliers and cleanup
		// but for now the editor requires this
		{
			INodeInstanceRegistry* nodeInstanceRegistry = m_scene->GetSystem< INodeInstanceRegistry >();
			const TNodeInstancesSet nodesToRemove = std::move( m_allInstances );
			for ( const THandle< INodeInstance >& node : nodesToRemove )
			{
				if ( node->IsAttached() )
				{
					node->PerformDetach( *m_scene );
					const GlobalNodeID& nodeId = node->GetGlobalNodeID();
					if( nodeId.IsDefined() )
					{
						if( nodeInstanceRegistry )
						{
							nodeInstanceRegistry->UnregisterNodeInstance( nodeId, node );
						}
					}
				}
			}
		}

		m_instancesToUpdate.Clear();

		// Cleanup node visibility states
		{
			RED_SCOPE_LOCK( m_nodeVisiblityStateLock );
			m_pendingNodeVisibilityStates.Clear();
			m_nodeVisibilityStates.Clear();
		}
	}

    Bool RuntimeSystemNodeStreaming::CheckSaturation( const PrefabProxyMeshState& state )
    {
        return state.attachedDeps == state.nbDeps;
    }

	void RuntimeSystemNodeStreaming::ProcessPrefabProxyMeshes( job::Builder& builder )
	{
		builder.DispatchJob( "ProcessPrefabProxyMeshes", [this]( const job::RunContext& )
		{
#ifndef RED_CONFIGURATION_FINAL
			red::StopWatch watch;
#endif
			RED_SCOPE_LOCK( m_proxyChildLock );

			for( red::BitSetIterator<red::BitSet64<c_maxProxy>> it( m_proxyDeathMask ); it.IsValid(); it.FindNext() )
			{
				const Uint32 index = it.GetIndex();
				PrefabProxyMeshState& state = m_proxyState[index];
				if( state.attachedDeps == 0 && state.weakProxyMesh.Expired() )
				{
					m_proxyIdAllocator.Release( index );
					m_proxyLookup.Remove( state.proxyHash );
					m_proxyStateChangeMask.Clear( index );

					state = {};
				}
			}
			m_proxyDeathMask.ClearAll();

			if ( s_cpProxyNearForce == 1 ) // [terrain] periodic scan of partially attached terrain tiles
			{
				static_assert( c_maxProxy == RED_KILO_BYTE( 20 ), "[terrain] partial arrays sized for c_maxProxy" );
				if ( !s_cpPartialSince )
				{
					s_cpPartialSince = new double[ c_maxProxy ]();
					s_cpPartialReady = new Bool[ c_maxProxy ]();
				}
				static double s_nextScan = 0.0;
				const double now = CpProxyNowMs();
				if ( now >= s_nextScan )
				{
					s_nextScan = now + 500.0;
					static Uint32 s_reported = 0;
					for ( Uint32 index = 0; index < c_maxProxy; ++index )
					{
						PrefabProxyMeshState& state = m_proxyState[ index ];
						const Bool partial = state.proxyHash != 0 && state.hideButKeepStreamed && state.attachedDeps > 0 && !CheckSaturation( state ) && !state.weakProxyMesh.Expired();
						if ( !partial )
						{
							s_cpPartialSince[ index ] = 0.0;
							if ( s_cpPartialReady[ index ] )
							{
								s_cpPartialReady[ index ] = false;
								state.refreshLockState = 1;
								m_proxyStateChangeMask.Set( index );
							}
							continue;
						}
						if ( s_cpPartialSince[ index ] == 0.0 )
						{
							s_cpPartialSince[ index ] = now;
						}
						if ( !s_cpPartialReady[ index ] && now - s_cpPartialSince[ index ] >= 3000.0 )
						{
							s_cpPartialReady[ index ] = true;
							state.refreshLockState = 1;
							m_proxyStateChangeMask.Set( index );
							if ( s_reported++ < 32 )
							{
								std::fprintf( stderr, "CP2077 terrain proxy fix: terrain tile %016llx partially attached %u/%u for 3 s -> near auto-hide applied\n",
									( unsigned long long )state.proxyHash, Uint32( state.attachedDeps ), Uint32( state.nbDeps ) );
							}
						}
					}
				}
			}

			for( red::BitSetIterator<red::BitSet64<c_maxProxy>> it( m_proxyStateChangeMask ); it.IsValid(); it.FindNext() )
			{
				const Uint32 index = it.GetIndex();
				PrefabProxyMeshState& state = m_proxyState[index];
				RED_FATAL_ASSERT( state.refreshLockState );

				if( THandle< PrefabProxyMeshNodeInstance> strong = state.weakProxyMesh.ToHandle() )
				{
                    const Bool lockAntistreaming = !CheckSaturation( state );
					const Bool cpApplyNearAnyway = s_cpProxyNearForce == 2 || ( s_cpProxyNearForce == 1 && s_cpPartialReady && s_cpPartialReady[ index ] ); // [terrain]
					if( lockAntistreaming && !cpApplyNearAnyway )
					{
						strong->ClearAutoHideDistanceNear();
						state.antistreamingLocked = 1;
					}
					else
					{
						strong->ApplyAutoHideDistanceNear();
						state.antistreamingLocked = 0;
					}

                    if( state.hideButKeepStreamed )
                    {
                        Internal_LockAntistreaming( (NodeStreamingProxy*)strong->GetStreamingProxy(), true );
                    }
                    else
                    {
                        Internal_LockAntistreaming( (NodeStreamingProxy*)strong->GetStreamingProxy(), lockAntistreaming );
                    }

					state.refreshLockState = 0;
				}
			}

			m_proxyStateChangeMask.ClearAll();

			if ( s_cpProxyProbe > 0 )
			{
				static double s_next = 0.0;
				const double now = CpProxyNowMs();
				if ( now >= s_next )
				{
					s_next = now + 10000.0;
					struct Near { float d; Uint32 index; };
					Near nearest[ 24 ];
					Uint32 numNear = 0;
					Uint32 total = 0, alive = 0, saturated = 0, locked = 0, terrainAlive = 0, terrainLocked = 0, lockedWithin300 = 0, terrainLockedWithin300 = 0;
					const Vector3 cam = m_cameraSystem ? m_cameraSystem->GetActiveCameraPosition() : Vector3::ZEROS();
					for ( Uint32 index = 0; index < c_maxProxy; ++index )
					{
						const PrefabProxyMeshState& state = m_proxyState[ index ];
						if ( state.proxyHash == 0 )
						{
							continue;
						}
						++total;
						const THandle< PrefabProxyMeshNodeInstance > strong = state.weakProxyMesh.ToHandle();
						if ( !strong )
						{
							continue;
						}
						++alive;
						const Bool isTerrain = state.hideButKeepStreamed;
						terrainAlive += isTerrain ? 1 : 0;
						if ( CheckSaturation( state ) )
						{
							++saturated;
							continue;
						}
						++locked;
						terrainLocked += isTerrain ? 1 : 0;
						{
							// locked proxies whose bounds reach the camera (pivot distance minus bounding radius)
							const THandle< PrefabProxyMeshNode > bsource = strong->GetSourceNodeAs< PrefabProxyMeshNode >();
							if ( bsource )
							{
								const Box lb = bsource->GetLocalBoundingBox();
								const float radius = Max( lb.Min.AsVector3().Mag(), lb.Max.AsVector3().Mag() );
								const Vector3 bpos = strong->GetInitialTransform().GetPosition().AsVector3();
								const float bd = ( bpos - cam ).Mag();
								static Uint32 s_boundsPrinted = 0;
								if ( bd - radius < 10.0f && s_boundsPrinted < 300 )
								{
									++s_boundsPrinted;
									std::fprintf( stderr, "CP2077 terrain proxy covering: d=%.0f radius=%.0f pos=(%.0f,%.0f,%.0f) attached=%u/%u near=%.0f class=%s hash=%016llx\n",
										bd, radius, bpos.X, bpos.Y, bpos.Z, Uint32( state.attachedDeps ), Uint32( state.nbDeps ), bsource->GetNearAutoHideDistance(),
										strong->GetClass()->GetName().AsChar(), ( unsigned long long )state.proxyHash );
								}
							}
						}
						if ( state.attachedDeps > 0 )
						{
							const Vector3 ppos = strong->GetInitialTransform().GetPosition().AsVector3();
							const float pd = ( ppos - cam ).Mag();
							static Uint32 s_partialPrinted = 0;
							if ( pd < 150.0f && s_partialPrinted < 400 )
							{
								++s_partialPrinted;
								const THandle< PrefabProxyMeshNode > psource = strong->GetSourceNodeAs< PrefabProxyMeshNode >();
								std::fprintf( stderr, "CP2077 terrain proxy partial: d=%.0f pos=(%.0f,%.0f,%.0f) attached=%u/%u near=%.0f class=%s hash=%016llx\n",
									pd, ppos.X, ppos.Y, ppos.Z, Uint32( state.attachedDeps ), Uint32( state.nbDeps ), psource ? psource->GetNearAutoHideDistance() : -1.0f,
									strong->GetClass()->GetName().AsChar(), ( unsigned long long )state.proxyHash );
							}
						}
						const Vector3 pos = strong->GetInitialTransform().GetPosition().AsVector3();
						const float d = ( pos - cam ).Mag();
						if ( d < 300.0f )
						{
							++lockedWithin300;
							terrainLockedWithin300 += isTerrain ? 1 : 0;
						}
						Uint32 slot = numNear < 24 ? numNear++ : 24;
						if ( slot == 24 )
						{
							Uint32 worst = 0;
							for ( Uint32 i = 1; i < 24; ++i ) if ( nearest[ i ].d > nearest[ worst ].d ) worst = i;
							if ( nearest[ worst ].d <= d ) continue;
							slot = worst;
						}
						nearest[ slot ] = { d, index };
					}
					std::fprintf( stderr, "CP2077 terrain proxy probe: cam=(%.1f,%.1f,%.1f) states=%u alive=%u saturated=%u locked=%u terrainAlive=%u terrainLocked=%u lockedWithin300=%u terrainLockedWithin300=%u force=%d\n",
						cam.X, cam.Y, cam.Z, total, alive, saturated, locked, terrainAlive, terrainLocked, lockedWithin300, terrainLockedWithin300, s_cpProxyNearForce );
					std::fprintf( stderr, "CP2077 terrain proxy children: attachCalls=%u noSetupInfo=%u withProxyId=%u road=%u roadWithProxy=%u building=%u buildingWithProxy=%u terrain=%u terrainWithProxy=%u decoration=%u\n",
						s_cpChild[ 0 ].load(), s_cpChild[ 1 ].load(), s_cpChild[ 2 ].load(), s_cpChild[ 3 ].load() + s_cpChild[ 4 ].load(), s_cpChild[ 4 ].load(),
						s_cpChild[ 5 ].load() + s_cpChild[ 6 ].load(), s_cpChild[ 6 ].load(), s_cpChild[ 7 ].load() + s_cpChild[ 8 ].load(), s_cpChild[ 8 ].load(), s_cpChild[ 9 ].load() );
					{
						// orphan states: created by a child notify whose proxy hash never registered (NotifyProxyMeshChildAttached
						// does not set state.proxyHash, so they are only reachable through the lookup map)
						Uint32 orphans = 0, orphanDeps = 0, printed = 0;
						Uint64 sumRegisteredAttached = 0;
						for ( auto it = m_proxyLookup.Begin(); it != m_proxyLookup.End(); ++it )
						{
							const PrefabProxyMeshState& state = m_proxyState[ it.Value().index ];
							if ( state.weakProxyMesh.Expired() )
							{
								++orphans;
								orphanDeps += Uint32( state.attachedDeps );
								if ( printed < 8 )
								{
									++printed;
									std::fprintf( stderr, "CP2077 terrain proxy orphan: hash=%016llx attached=%u\n", ( unsigned long long )it.Key(), Uint32( state.attachedDeps ) );
								}
							}
							else
							{
								sumRegisteredAttached += state.attachedDeps;
							}
						}
						std::fprintf( stderr, "CP2077 terrain proxy orphans: lookup=%u orphans=%u orphanDeps=%u registeredAttachedSum=%llu\n",
							Uint32( m_proxyLookup.Size() ), orphans, orphanDeps, ( unsigned long long )sumRegisteredAttached );
					}
					{
						// every terrain tile proxy (prefab_lodN): which are unsaturated, and how many cells/tiles they wait for
						static Uint32 s_terrainDumps = 0;
						if ( s_terrainDumps++ < 6 )
						{
							for ( Uint32 index = 0; index < c_maxProxy; ++index )
							{
								const PrefabProxyMeshState& state = m_proxyState[ index ];
								if ( state.proxyHash == 0 || !state.hideButKeepStreamed ) continue;
								const THandle< PrefabProxyMeshNodeInstance > strong = state.weakProxyMesh.ToHandle();
								if ( !strong ) continue;
								const THandle< PrefabProxyMeshNode > tsource = strong->GetSourceNodeAs< PrefabProxyMeshNode >();
								const Vector3 tpos = strong->GetInitialTransform().GetPosition().AsVector3();
								const Box lb = tsource ? tsource->GetLocalBoundingBox() : Box();
								std::fprintf( stderr, "CP2077 terrain tile: sat=%d attached=%u/%u near=%.0f pos=(%.0f,%.0f,%.0f) d=%.0f box=(%.0f,%.0f)-(%.0f,%.0f) hash=%016llx\n",
									int( CheckSaturation( state ) ), Uint32( state.attachedDeps ), Uint32( state.nbDeps ), tsource ? tsource->GetNearAutoHideDistance() : -1.0f,
									tpos.X, tpos.Y, tpos.Z, ( tpos - cam ).Mag(), lb.Min.X, lb.Min.Y, lb.Max.X, lb.Max.Y, ( unsigned long long )state.proxyHash );
							}
						}
					}
					for ( Uint32 i = 0; i < numNear; ++i )
					{
						const PrefabProxyMeshState& state = m_proxyState[ nearest[ i ].index ];
						const THandle< PrefabProxyMeshNodeInstance > strong = state.weakProxyMesh.ToHandle();
						if ( !strong )
						{
							continue;
						}
						const Vector3 pos = strong->GetInitialTransform().GetPosition().AsVector3();
						const THandle< PrefabProxyMeshNode > source = strong->GetSourceNodeAs< PrefabProxyMeshNode >();
						std::fprintf( stderr, "CP2077 terrain proxy locked: d=%.0f pos=(%.0f,%.0f,%.0f) terrain=%u attached=%u/%u near=%.0f hash=%016llx\n",
							nearest[ i ].d, pos.X, pos.Y, pos.Z, Uint32( state.hideButKeepStreamed ), Uint32( state.attachedDeps ), Uint32( state.nbDeps ),
							source ? source->GetNearAutoHideDistance() : -1.0f, ( unsigned long long )state.proxyHash );
					}
				}
			}

#ifndef RED_CONFIGURATION_FINAL
			m_dbgUpdateProxyChildCountersTimeMS = watch.GetDeltaMS();
#endif
		} );
	}

	void RuntimeSystemNodeStreaming::ProcessContent( job::Builder& builder )
	{
		PC_SCOPE( RuntimeSystemNodeStreaming_ProcessContent  );
	
		m_statsCollector.Reset();

		const Uint8 generation = m_generation;
		m_generation += 1;

		// Collects the nodes to attach/detach
		m_sharedContentContext = red::CreateSharedPtr< ContentUpdateContext >(
			m_scene,
			m_nodeInstanceToAttachParallel,
			m_nodeInstanceToAttachSerial,
			m_nodeInstanceToDetachParallel,
			m_nodeInstanceToDetachSerial,
			m_isLoadingMode,
			generation );

		{
			RED_SCOPE_SHARED_LOCK( m_runtimeDistanceScaleLock );

			m_sharedContentContext->m_globalStreamingDistanceScale = 1.f;
			if( m_runtimeDistanceScaleData.Size() )
			{
				m_sharedContentContext->m_globalStreamingDistanceScale = m_runtimeDistanceScaleData.Back().value;
			}
			m_sharedContentContext->m_globalStreamingDistanceScale *= m_globalStreamingDistanceScale;
		}

			// collect the observer positions
		{
			RED_SCOPE_SHARED_LOCK( m_observersLock );

			for( ContentObserver* ptr : m_observers )
			{
				if( ptr->HasValidReferencePosition() )
				{
					const Vector3 position = ptr->GetReferencePosition();
					const Vector3 clampedPosition( truncf( position.X ), truncf( position.Y ), truncf( position.Z ) );
					const Vector3 velocity = ptr->CalcForwardVelocityPosition();
					const Vector3 clampedVelocity( truncf( velocity.X ), truncf( velocity.Y ), truncf( velocity.Z ) );

					// #tbd: could consolidate into one virtual ptr->Get*()
					m_sharedContentContext->m_streamingContexts.EmplaceBack(
						clampedPosition,
						( m_isLoadingMode ? clampedPosition : clampedVelocity ),
						ptr->GetUncappedVelocity(),
						ptr->IsStreamingOccluded() );
				}
			}
		}

		// update content suppliers
		for( IContentSupplier* contentSupplier : m_contentSuppliers )
		{
			builder.DispatchJob( "ProcessContent", [contentSupplier, this]( const job::RunContext& runContext ) {
				job::Builder depBuilder{runContext};
				contentSupplier->ProcessContent( depBuilder, m_sharedContentContext );
			} );
		}
	}

	void RuntimeSystemNodeStreaming::Update( job::Builder& builder, Bool flush )
	{
		PC_SCOPE( RuntimeSystemNodeStreaming/Update );

		builder.DispatchJob( "UpdateStreamingState", [this, flush]( const job::RunContext& runContext )
		{
			if( !m_sharedContentContext )
				return;

			for( IContentSupplier* contentSupplier : m_contentSuppliers )
			{
				contentSupplier->PostProcessContent( m_sharedContentContext );
			}

			m_wasStreamingLastFrame.SetValue( m_sharedContentContext->m_isStreaming || !m_nodeInstanceToAttachSerial.Empty() );

			job::Builder builder( runContext );
			ScheduleParallelNodesOperation( builder, flush );
			ScheduleSerialNodesOperation( builder, flush );
			builder.DispatchFenceExplicitly();
			ScheduleNodesEpilogues( builder, flush );

			builder.DispatchJob( "Streaming_UpdateNodesVisibilityState", [this]( const job::RunContext& context )
			{
				UpdateNodesVisibilityState();

				StreamingStats stats;
				stats.nodeAttached = m_nodeAttached;
				stats.nodePendingAttach = m_sharedContentContext->m_nodeStreamingCount;
				stats.nodeCulled = m_sharedContentContext->m_nodeCulledCount;
				stats.nodeCulledFallback = m_sharedContentContext->m_nodeCulledFallbackCount;
				stats.nodeStreamingCancelled = m_sharedContentContext->m_nodeStreamingCancelledCount;
				stats.nodePendingDetails = m_sharedContentContext->m_nodeStreamingDetail;
				const Uint32 ctxSize = math::Min( m_sharedContentContext->m_streamingContexts.Size(), stats.observers.Capacity() );
				stats.observers.Resize( ctxSize );
				for( Uint32 ctxIdx = 0; ctxIdx < ctxSize; ++ctxIdx )
				{
					stats.observers[ ctxIdx ].baseReferencePosition = m_sharedContentContext->m_streamingContexts[ ctxIdx ].m_baseReferencePosition;
					stats.observers[ ctxIdx ].forwardVelocityPosition = m_sharedContentContext->m_streamingContexts[ ctxIdx ].m_forwardVelocityPosition;
					stats.observers[ ctxIdx ].uncappedVelocity = m_sharedContentContext->m_streamingContexts[ ctxIdx ].m_uncappedVelocity.velocity;
					stats.observers[ ctxIdx ].uncappedVelocitySource = static_cast< Uint8 >( m_sharedContentContext->m_streamingContexts[ ctxIdx ].m_uncappedVelocity.velocitySource );
					stats.observers[ ctxIdx ].useStreamingOcclusion = m_sharedContentContext->m_streamingContexts[ ctxIdx ].m_useStreamingOcclusion;
				}
				stats.numSectorsLoading = m_sharedContentContext->m_numSectorsLoading;
				stats.numResourcesPreloading = m_sharedContentContext->m_numResourcesPreloading;

				GEngine->SetStreamingStats( stats );
			} );
		} );
	}

	void RuntimeSystemNodeStreaming::UpdateNodes( job::Builder& builder, const Float timeDelta, const tick::Info& tickInfo )
	{
		// #tbd: parallelfor
		// update attached nodes
		builder.DispatchJob( "RuntimeSystemNodeStreaming/UpdateNodes", [this, timeDelta, &tickInfo] ( const job::RunContext& runContext )
		{
			PC_SCOPE( RuntimeSystemNodeStreaming/UpdateNodes );

			Editor_ExecuteTransformUpdates();

			for ( const auto& node : m_instancesToUpdate )
			{
				node->PerformUpdate( *m_scene, timeDelta, tickInfo );
			}
		} );
	}

	RED_INLINE static Float GetTimeBudgetMultiplier()
	{
#if defined(RED_CONFIGURATION_DEBUG) || defined(RED_CONFIGURATION_NOPTS)
		// increase limit in debug build so the streaming is not slowed down
		// TODO ideally this value should be based on actual framerate
		return 3.0f;
#else
		return 1.0f;
#endif
	}

	void RuntimeSystemNodeStreaming::ScheduleParallelNodesOperation( job::Builder& builder, Bool flush )
	{
		const Uint32 c_detachStep = 32;

		for( Uint32 index = 0, end = m_nodeInstanceToDetachParallel.Size(); index < end; index += c_detachStep )
		{
			const Uint32 lastIndex = std::min( index + c_detachStep, end );
			builder.DispatchJob< job::Fence::None >( "Streaming_DetachNodesParallel", [this, index, lastIndex]( const job::RunContext& context ) 
			{
				for( Uint32 i = index; i != lastIndex; ++i )
				{
					const THandle< INodeInstance >& node = m_nodeInstanceToDetachParallel[ i ];
					node->PerformDetach( *m_scene );
				}
			} );
		}

		const Uint32 c_attachStep = 32;

		for( Uint32 index = 0, end = m_nodeInstanceToAttachParallel.Size(); index < end; index += c_attachStep )
		{
			const Uint32 lastIndex = std::min( index + c_attachStep, end );
			builder.DispatchJob< job::Fence::None >( "Streaming_AttachNodesParallel", [this, index, lastIndex]( const job::RunContext& context ) 
			{
				for( Uint32 i = index; i != lastIndex; ++i )
				{
					const THandle< INodeInstance >& node = m_nodeInstanceToAttachParallel[ i ];

					if( !node->IsPendingDestruction() )
					{
						node->PerformAttach( *m_scene );
					}
				}
			} );
		}
	}

	void RuntimeSystemNodeStreaming::ScheduleSerialNodesOperation( job::Builder& builder, Bool flush )
	{
		builder.DispatchJob< job::Fence::None >( "Streaming_NodesSerialOperation", [this, flush]( const job::RunContext& context ) 
		{
			for( Uint32 index = 0, end = m_nodeInstanceToDetachSerial.Size(); index != end; ++index )
			{
				const THandle< INodeInstance >& node = m_nodeInstanceToDetachSerial[ index ];
				node->PerformDetach( *m_scene );
			}

#ifndef NO_EDITOR
			const Bool isEditor = m_scene->GetRuntimeInfo().IsPreview();
			// determine cutoff value for throttling
			const Float baseBudgetMillisec = isEditor ? Config::cvEditorNodeAttachingTimeLimit.Get() : Config::cvNodeAttachingTimeLimit.Get();
			const Float budgetMillisec = GetTimeBudgetMultiplier() * baseBudgetMillisec;

			const EngineTime timeCutoff = EngineTime::GetNow() + budgetMillisec / 1000.f;
#endif

			m_nodeAttachedSerial = 0;
			for( Uint32 index = 0, end = m_nodeInstanceToAttachSerial.Size(); index != end; ++index )
			{
				++m_nodeAttachedSerial;
				const THandle< INodeInstance >& node = m_nodeInstanceToAttachSerial[ index ];
				if( !node->IsPendingDestruction() )
				{
					node->PerformAttach( *m_scene );
				}

#ifndef NO_EDITOR
				if( !flush && isEditor && index % 10 && EngineTime::GetNow() > timeCutoff )
				{
					break;
				}
#endif
			}
		} );
	}

	void RuntimeSystemNodeStreaming::ScheduleNodesEpilogues( job::Builder& builder, Bool flush )
	{
		builder.DispatchJob( "Streaming_DetachNodesEpilogue", [this]( const job::RunContext& context ) 
		{
			m_instancesToDestroy.PushBack( m_nodeInstanceToDetachParallel );
			m_instancesToDestroy.PushBack( m_nodeInstanceToDetachSerial );
			m_nodeInstanceToDetachParallel.Clear();
			m_nodeInstanceToDetachSerial.Clear();

			RED_SCOPE_SHARED_LOCK( m_streamingCallbacksLock );
			INodeInstanceRegistry* nodeInstanceRegistry = m_scene->GetSystem< INodeInstanceRegistry >();
			for( Uint32 i = 0, end = m_instancesToDestroy.Size(); i < end; ++i )
			{
				const THandle< INodeInstance >& nodeInstance = m_instancesToDestroy[ i ];
#ifndef RED_CONFIGURATION_FINAL
				m_allInstances.Remove( nodeInstance );
#endif
				const GlobalNodeID& nodeId = nodeInstance->GetGlobalNodeID();
				if( nodeId.IsDefined() )
				{
					if( nodeInstanceRegistry )
					{
						nodeInstanceRegistry->UnregisterNodeInstance( nodeId, nodeInstance );
					}

					const auto iter = m_streamingCallbacks.Find( nodeId );
					if( iter != m_streamingCallbacks.End() )
					{
						for ( const auto& clb : iter.Value() )
						{
							if ( clb )
							{
								clb( *nodeInstance, NodeStreamingNotice::streamedOut );
							}
						}
					}
				}

				if( nodeInstance->RequiresUpdating() )
				{
					m_instancesToUpdate.Remove( nodeInstance );
				}
			}

			m_nodeAttached -= m_instancesToDestroy.Size();
		} );

		builder.DispatchJob( "Streaming_AttachNodesEpilogue", [this, flush]( const job::RunContext& context ) 
		{
			INodeInstanceRegistry* nodeInstanceRegistry = m_scene->GetSystem< INodeInstanceRegistry >();

			const Bool isEditor = m_scene->GetRuntimeInfo().IsPreview();

			NodeInstancesContainer allNodes{red::PoolStreaming()};
			allNodes.Reserve( m_nodeInstanceToAttachParallel.Size() + m_nodeAttachedSerial );
			allNodes.PushBack( m_nodeInstanceToAttachParallel );
			allNodes.PushBack( red::MakeArraySpan( m_nodeInstanceToAttachSerial.Begin(), m_nodeInstanceToAttachSerial.Begin() + m_nodeAttachedSerial ) );

			m_nodeInstanceToAttachParallel.Clear();
			m_nodeInstanceToAttachSerial.RemoveAt( 0, m_nodeAttachedSerial );

			Uint32 nodeInstanceAttached = 0;
			for( const THandle< INodeInstance >& nodeInstance : allNodes )
			{
				if( nodeInstance->IsPendingDestruction() )
					continue;

#ifndef RED_CONFIGURATION_FINAL
				m_allInstances.Insert( nodeInstance );
#endif

				++nodeInstanceAttached;

#ifndef RED_CONFIGURATION_FINAL
				// apply debug color
				if( m_editorDebugColoringSettings )
				{
					m_editorDebugColoringSettings->NodeAttached( *nodeInstance );
					nodeInstance->Editor_SetDebugColor( m_editorDebugColoringSettings->ComputeDebugColor( *nodeInstance ) );
				}
#endif // RED_CONFIGURATION_FINAL

				if( nodeInstance->RequiresUpdating() )
				{
					m_instancesToUpdate.Insert( nodeInstance );
				}

				if( nodeInstance->GetGlobalNodeID().IsDefined() )
				{
					nodeInstanceRegistry->RegisterNodeInstance( nodeInstance->GetGlobalNodeID(), nodeInstance );
					const auto iter = m_nodeVisibilityStates.Find( nodeInstance->GetGlobalNodeID() );
					if( iter != m_nodeVisibilityStates.End() )
					{
						nodeInstance->SetVisibility( iter.Value() );
					}

					const auto streamingIter = m_streamingCallbacks.Find( nodeInstance->GetGlobalNodeID() );
					if( streamingIter != m_streamingCallbacks.End() )
					{
						for ( const auto& clb : streamingIter.Value() )
						{
							if ( clb )
							{
								clb( *nodeInstance, NodeStreamingNotice::streamedIn );
							}
						}
					}
				}

#ifndef NO_EDITOR

				if( !m_streamingListeners.Empty() )
				{
					const auto iter = m_streamingListeners.Find( nodeInstance->GetEditorObjectIDPath() );
					if( iter != m_streamingListeners.End() )
					{
						const StreamingListenersList& list = iter.Value();
						for( IStreamingListener* listener : list )
						{
							listener->OnOtherNodeStreamedIn( *nodeInstance );
						}
					}
				}
#endif
			}

			m_nodeAttached += nodeInstanceAttached;
		} );
	}

	void RuntimeSystemNodeStreaming::SetNodeStreamingCallback( const GlobalNodeID& nodeID, NodeStreamingListener listener, WorldNodeStreamingCallback callback )
	{
		const Uint32 index = static_cast<Uint32>( listener );
		RED_ASSERT( index < c_nodeStreamingCallbacksMaxCount, "Invalid owner id" );

		RED_SCOPE_LOCK( m_streamingCallbacksLock );

		auto iter = m_streamingCallbacks.Find( nodeID );

		if ( !callback )
		{
			if ( iter != m_streamingCallbacks.End() )
			{
				WorldNodeStreamingCallbacksArray& clbs = iter.Value();
				clbs[index] = nullptr;

				const Bool validCallbackExists = std::any_of( clbs.Begin(), clbs.End(), []( const WorldNodeStreamingCallback& clb ) { return clb; } );
				if ( !validCallbackExists )
				{
					m_streamingCallbacks.Remove( iter );
				}
			}
		}
		else
		{
			if ( iter == m_streamingCallbacks.End() )
			{
				iter = m_streamingCallbacks.Insert( nodeID, WorldNodeStreamingCallbacksArray {} ).Iterator();
			}

			WorldNodeStreamingCallbacksArray& clbs = iter.Value();
			clbs[ index ] = std::move( callback );
		}
	}

	void RuntimeSystemNodeStreaming::DestroyPendingNodeInstances( job::Builder& builder )
	{
		builder.DispatchJob( "ScheduleDestroyPendingNodeInstances", [this]( const job::RunContext& context )
		{
            if( m_instancesToDestroy.Size() )
            {
                NodeInstancesContainer nodes = std::move( m_instancesToDestroy );
                job::Builder destuctionBuilder( job::Priority::Latent );
                destuctionBuilder.DispatchJob( "DestroyNodeInstances", [nodes = std::move( nodes )]( const job::RunContext& context )
                {
					// DO NOTHING. It is just to call DTORs from latent job.
                } );

                m_instancesDestructionCounter += destuctionBuilder.ExtractWaitCounter();
            }
		} );
	}

	void RuntimeSystemNodeStreaming::UpdateNodesVisibilityState()
	{
		PC_SCOPE( RuntimeSystemNodeStreaming/UpdateNodesVisibilityState );

		TPendingNodeVisibility pendingNodeVisibilityStates{ red::PoolStreaming() };
		{
			RED_SCOPE_LOCK( m_nodeVisiblityStateLock );
			pendingNodeVisibilityStates = std::move( m_pendingNodeVisibilityStates );
		}

		for ( const auto& pair : pendingNodeVisibilityStates )
		{
			const THandle< INodeInstance >& nodeInstance = pair.Key();
			const Bool isVisible = pair.Value();

			if ( nodeInstance )
			{
				nodeInstance->SetVisibility( isVisible );
			}
		}
	}

	void RuntimeSystemNodeStreaming::ToggleNodeVisibility( const THandle< INodeInstance >& nodeInstance, const Bool visible )
	{
		RED_FATAL_ASSERT( nodeInstance, "Invalid node instance" );
		{
			RED_SCOPE_LOCK( m_nodeVisiblityStateLock );
			m_pendingNodeVisibilityStates.Insert( nodeInstance, visible );
			m_nodeVisibilityStates[nodeInstance->GetGlobalNodeID()] = visible;
		}
	}

	void RuntimeSystemNodeStreaming::ToggleNodeVisibility( const GlobalNodeRef nodeRef, const Bool visible )
	{
		RED_FATAL_ASSERT( nodeRef.IsDefined(), "Invalid global node ID" );

		INodeInstanceRegistry* nodeInstanceRegistry = m_scene->GetSystem<INodeInstanceRegistry>();
		const THandle< INodeInstance > nodeInstance = nodeRef.GetNodeInstance( nodeInstanceRegistry );

		{
			RED_SCOPE_LOCK( m_nodeVisiblityStateLock );
			m_nodeVisibilityStates[nodeRef] = visible;

			// request node instance update if it's streamed in
			if ( nodeInstance )
			{
				m_pendingNodeVisibilityStates.Insert( nodeInstance, visible );
			}
		}
	}

	void RuntimeSystemNodeStreaming::Editor_ScheduleTransformUpdate( const THandle< INodeInstance >& nodeInstance )
	{
		m_pendingTransformUpdates.PushBack( nodeInstance );
	}

	void RuntimeSystemNodeStreaming::EnableLoadingMode(Bool value)
	{
		EnableStreamingSectorInplaceResourceThrottling( value );
        m_isLoadingMode = value;
	}

	void RuntimeSystemNodeStreaming::SetRuntimeGlobalStreamingDistanceScale(Uint64 source, Float value)
	{
		RED_SCOPE_LOCK(m_runtimeDistanceScaleLock);

		for (auto& data : m_runtimeDistanceScaleData)
		{
			if (data.source == source)
			{
				data.value = Clamp(value, 0.1f, 1.f);
				return;
			}
		}

		if (!m_runtimeDistanceScaleData.Full())
		{
			DistanceScaleData& data = m_runtimeDistanceScaleData.EmplaceBack();
			data.source = source;
			data.value = Clamp( value, 0.1f, 1.f );
		}
		else
		{
			RED_LOG_WARNING( "Streaming system: Too many overlaping distance scale overrides" );
		}
	}

	void RuntimeSystemNodeStreaming::UnsetRuntimeGlobalStreamingDistanceScale(Uint64 source)
	{
		RED_SCOPE_LOCK(m_runtimeDistanceScaleLock);

		for (auto it = m_runtimeDistanceScaleData.Begin(); it != m_runtimeDistanceScaleData.End(); ++it)
		{
			if (it->source == source)
			{
				m_runtimeDistanceScaleData.Remove(it);
				return;
			}
		}
	}

	void RuntimeSystemNodeStreaming::RegisterProxyMeshNodeInstance( Uint64 proxyHash, THandle<PrefabProxyMeshNodeInstance> node, const RegisterProxyMeshFlags& flags )
	{
		RED_SCOPE_LOCK( m_proxyChildLock );

    		if( PrefabProxyMeshLookup* lookup = m_proxyLookup.FindPtr( proxyHash ) )
		{
			PrefabProxyMeshState& state = m_proxyState[lookup->index];
			RED_FATAL_ASSERT( state.weakProxyMesh.Expired() );
			
			state.weakProxyMesh = node;
			state.proxyHash = proxyHash;
			state.nbDeps = node->m_numberChildNodesToAttach;
			state.refreshLockState |= 1;
            state.hideButKeepStreamed = flags.hideButKeepStreamedIn;

			m_proxyStateChangeMask.Set( lookup->index );
		}
		else
		{
			const Uint32 index = m_proxyIdAllocator.Alloc();
			
			PrefabProxyMeshState& state = m_proxyState[index];
			RED_FATAL_ASSERT( state.weakProxyMesh.Expired() );

			state.weakProxyMesh = node;
			state.proxyHash = proxyHash;
			state.nbDeps = node->m_numberChildNodesToAttach;
			state.attachedDeps = 0;
			state.refreshLockState |= 1;
            state.hideButKeepStreamed = flags.hideButKeepStreamedIn;
			m_proxyLookup.Insert( proxyHash, { index } );

			m_proxyStateChangeMask.Set( index );
		}
	}

	void RuntimeSystemNodeStreaming::UnregisterProxyMeshNodeInstance( Uint64 proxyHash )
	{
		RED_SCOPE_LOCK( m_proxyChildLock );

		PrefabProxyMeshLookup* lookup = m_proxyLookup.FindPtr( proxyHash );
		PrefabProxyMeshState& state = m_proxyState[lookup->index];
		state.refreshLockState |= 1;
		state.weakProxyMesh = nullptr;
		m_proxyDeathMask.Set( lookup->index );
	}

	void RuntimeSystemNodeStreaming::NotifyProxyMeshChildAttached( Uint64 proxyHash, Uint32 nbInstances )
	{
		RED_SCOPE_LOCK( m_proxyChildLock );

		PrefabProxyMeshState* state = nullptr;
		Uint32 index = UINT32_MAX;
		if( PrefabProxyMeshLookup* lookup = m_proxyLookup.FindPtr( proxyHash ) )
		{
			state = &m_proxyState[lookup->index];
			index = lookup->index;
		}
		else
		{
			index = m_proxyIdAllocator.Alloc();
			state = &m_proxyState[index];
			m_proxyLookup.Insert( proxyHash, { index } );
		}

        const Bool before = CheckSaturation( *state );
        state->attachedDeps += nbInstances;
        const Bool after = CheckSaturation( *state );
		state->refreshLockState |= before != after;
		if( state->refreshLockState )
		{
			m_proxyStateChangeMask.Set( index );
		}

		RED_FATAL_ASSERT( (state->attachedDeps <= state->nbDeps) || (state->nbDeps == 0));
	}

	void RuntimeSystemNodeStreaming::NotifyProxyMeshChildDetached( Uint64 proxyHash, Uint32 nbInstances )
	{
		RED_SCOPE_LOCK( m_proxyChildLock );
			
		PrefabProxyMeshLookup* lookup = m_proxyLookup.FindPtr( proxyHash );
		PrefabProxyMeshState& state = m_proxyState[lookup->index];
		RED_FATAL_ASSERT( state.attachedDeps >= nbInstances );

        const Bool before = CheckSaturation( state );
        state.attachedDeps -= nbInstances;
        const Bool after = CheckSaturation( state );
        
        state.refreshLockState |= before != after;
		RED_FATAL_ASSERT( (state.attachedDeps <= state.nbDeps) || (state.nbDeps == 0) );

		if( state.refreshLockState )
		{
			m_proxyStateChangeMask.Set( lookup->index );
		}
	}

    void RuntimeSystemNodeStreaming::Internal_LockAntistreaming( const NodeStreamingProxy* proxy, Bool value )
    {
        for( IContentSupplier* contentSupplier : m_contentSuppliers )
        {
            contentSupplier->LockAntistreaming( proxy, value );
        }
    }

    DEBUGProxyMeshNodeAttachedDeps RuntimeSystemNodeStreaming::DEBUG_GetProxyMeshNodeAttachedDeps( Uint64 proxyHash ) const
	{
		RED_SCOPE_SHARED_LOCK(m_proxyChildLock);

		DEBUGProxyMeshNodeAttachedDeps result;
		auto found = m_proxyLookup.Find(proxyHash);
		if (found != m_proxyLookup.End())
		{
			const Uint32 index = found.Value().index;
			const PrefabProxyMeshState& state = m_proxyState[index];

			result.numberChildNodesAttached = state.attachedDeps;
			result.numberChildNodeDependencies = state.nbDeps;
		}

		return result;
	}

	void RuntimeSystemNodeStreaming::Editor_ExecuteTransformUpdates()
	{
		PC_SCOPE( RuntimeSystemNodeStreaming / ExecuteTransformUpdates );

		// TODO instead of hard limit here, we should update as many as we can per frame,
		// but there are some per-node-type limits (in physics and rendering)
		const Uint32 c_maxNodesToUpdate = 8192;
		Uint32 nodesToUpdate = Min( m_pendingTransformUpdates.Size(), c_maxNodesToUpdate );

		for ( Uint32 i = 0; i < nodesToUpdate; ++i )
		{
			const THandle< INodeInstance > nodeInstance = m_pendingTransformUpdates[ i ].ToHandle();
			if ( nodeInstance && nodeInstance->IsAttached() )
			{
				nodeInstance->OnInitialTransformChanged();
			}
		}

		m_pendingTransformUpdates.RemoveAt( 0, nodesToUpdate );
	}

	void RuntimeSystemNodeStreaming::CollectNodeInstances( red::DynArray< INodeInstance* > &outInstances )
	{
#ifndef RED_CONFIGURATION_FINAL
		outInstances.Reserve( outInstances.Size() + m_allInstances.Size() );

		for ( auto it = m_allInstances.Begin(); it != m_allInstances.End(); ++it )
		{
			outInstances.PushBack( it->Get() );
		}
#endif
	}

	void RuntimeSystemNodeStreaming::CollectNodeInstances( const rtti::ClassType *classTypeToGather, red::DynArray< INodeInstance* > &outInstances )
	{
#ifndef RED_CONFIGURATION_FINAL
		for ( auto it = m_allInstances.Begin(); it != m_allInstances.End(); ++it )
		{
			if ( ( *it )->IsA( classTypeToGather ) )
			{
				outInstances.PushBack( it->Get() );
			}
		}
#endif
	}

	void RuntimeSystemNodeStreaming::Editor_RegisterStreamingListener( const tools::EditorObjectIDPath& nodePath, IStreamingListener* listener )
	{
		RED_SCOPE_LOCK( m_streamingListenersLock );

		StreamingListenersList& list = m_streamingListeners.GetRef( nodePath, red::PoolEngine() );

		// TODO consider using HashSet if it's too slow
		RED_FATAL_ASSERT( !list.Exist( listener ), "Duplicate streaming listener for a given editor path" );

		list.PushBack( listener );
	}

	void RuntimeSystemNodeStreaming::Editor_UnregisterStreamingListener( const tools::EditorObjectIDPath& nodePath, IStreamingListener* listener )
	{
		RED_SCOPE_LOCK( m_streamingListenersLock );

		StreamingListenersList& list = m_streamingListeners.GetRef( nodePath, red::PoolEngine() );

		// TODO consider using HashSet if it's too slow
		RED_VERIFY( list.RemoveReorder( listener ).IsSuccessful() );

		if ( list.Empty() )
		{
			m_streamingListeners.Remove( nodePath );
		}
	}

	void RuntimeSystemNodeStreaming::Editor_NotifyNodeInstanceHasChanged( const INodeInstance& nodeInstance, const rtti::AccessPath& accessPath )
	{
		RED_SCOPE_SHARED_LOCK( m_streamingListenersLock );

		if ( !m_streamingListeners.Empty() )
		{
			const auto iter = m_streamingListeners.Find( nodeInstance.GetEditorObjectIDPath() );
			if ( iter != m_streamingListeners.End() )
			{
				const StreamingListenersList& list = iter.Value();
				for ( IStreamingListener* listener : list )
				{
					listener->OnOtherNodePropertyChanged( nodeInstance, accessPath );
				}
			}
		}
	}

	void RuntimeSystemNodeStreaming::DrawNodeStreamingStats( rend::IDebugDrawer& dd )
	{
#ifdef RED_USE_IMGUI
		static const CName s_visualizationName = RED_NAME_CONSTEXPR( "Streaming##metricsPage_World" );
		if ( red::ImGui::IsWindowRequested( s_visualizationName ) )
		{
			if ( ImGui::Begin( s_visualizationName.AsChar() ) )
			{
				const String label = String::Printf( "Nodes (last %u seconds)", RuntimeSceneNodesStatsCollector::c_secondsSpan );
				if ( ImGui::CollapsingHeader( label.AsChar() ) )
				{
					const RuntimeSceneNodesStats stats = m_statsCollector.GetStats();

					ImGui::Columns( 2, "nodesStats##streaming" );

					ImGui::Text( "To attach:" ); ImGui::NextColumn();
					ImGui::Text( "%u", stats.maxNodesToAttach ); ImGui::NextColumn();

					ImGui::Text( "To detach:" ); ImGui::NextColumn();
					ImGui::Text( "%u", stats.maxNodesToDetach ); ImGui::NextColumn();

					ImGui::Text( "Attaching:" ); ImGui::NextColumn();
					ImGui::Text( "%.3f ms", stats.maxAttachNodesTimeInMillisec ); ImGui::NextColumn();

					ImGui::Text( "Detaching:" ); ImGui::NextColumn();
					ImGui::Text( "%.3f ms", stats.maxDetachNodesTimeInMillisec ); ImGui::NextColumn();

					ImGui::Text( "Destroying:" ); ImGui::NextColumn();
					ImGui::Text( "%.3f ms", stats.maxDestroyNodesTimeInMillisec ); ImGui::NextColumn();

					ImGui::Columns();
				}
			}
			ImGui::End();
		}
#endif // RED_USE_IMGUI
	}

	void RuntimeSystemNodeStreaming::Editor_SetDebugColoring( const THandle< EditorDebugColoringSettings >& settings, Bool force )
	{
		if ( m_editorDebugColoringSettings != settings || force )
		{
			m_editorDebugColoringSettings = settings;

			if ( m_editorDebugColoringSettings )
			{
				for ( const THandle< INodeInstance >& nodeInstance : m_allInstances )
				{
					m_editorDebugColoringSettings->NodeAttached( *nodeInstance );
				}
			}

			for ( const THandle< INodeInstance >& nodeInstance : m_allInstances )
			{
				const Color debugColor = m_editorDebugColoringSettings ? m_editorDebugColoringSettings->ComputeDebugColor( *nodeInstance ) : Color::CLEAR();
				nodeInstance->Editor_SetDebugColor( debugColor );
			}
		}
	}

	void RuntimeSystemNodeStreaming::OnGameInstanceAttached(game::IGameInstance& gameInstance)
	{
		CpProxyProbeInit(); // [terrain]
		m_cameraSystem = gameInstance.GetSystemPtr<game::ICameraSystem>();
	}

	void RuntimeSystemNodeStreaming::OnGameInstanceDetached( game::IGameInstance& gameInstance ) 
	{
		m_cameraSystem = nullptr;

		// ctremblay: gameinstance will be gone after this. Needs to release node now as they might have dependency on gameInstance.
		CleanupNodeInstances();
	}

	world::GlobalNodeID NotifyProxyMeshChildNodeAttached( RuntimeScene* scene, const INodeInstance* nodeInstance, Uint32 nbInstances )
	{
		s_cpChild[ 0 ].fetch_add( 1, std::memory_order_relaxed );
		if( auto* compiledInfo = nodeInstance->GetSetupInfo() )
		{
			const Bool cpHasProxy = compiledInfo->m_proxyMeshId.IsDefined();
			if( cpHasProxy ) s_cpChild[ 2 ].fetch_add( 1, std::memory_order_relaxed );
			if( compiledInfo->IsRoad() ) s_cpChild[ cpHasProxy ? 4 : 3 ].fetch_add( 1, std::memory_order_relaxed );
			if( compiledInfo->IsBuilding() ) s_cpChild[ cpHasProxy ? 6 : 5 ].fetch_add( 1, std::memory_order_relaxed );
			if( compiledInfo->IsTerrain() ) s_cpChild[ cpHasProxy ? 8 : 7 ].fetch_add( 1, std::memory_order_relaxed );
			if( compiledInfo->IsDecoration() ) s_cpChild[ 9 ].fetch_add( 1, std::memory_order_relaxed );
			if( cpHasProxy && s_cpProxyProbe > 0 )
			{
				static std::atomic< Uint32 > s_samples{ 0 };
				static std::atomic< Uint32 > s_terrainSamples{ 0 };
				if( ( compiledInfo->IsTerrain() ? s_terrainSamples : s_samples ).fetch_add( 1, std::memory_order_relaxed ) < 40 )
				{
					const Vector3 p = compiledInfo->m_transform.GetPosition().AsVector3();
					std::fprintf( stderr, "CP2077 terrain proxy child: class=%s pos=(%.0f,%.0f,%.0f) self=%016llx proxy=%016llx road=%d building=%d\n",
						nodeInstance->GetClass()->GetName().AsChar(), p.X, p.Y, p.Z, ( unsigned long long )compiledInfo->m_globalNodeId.GetHash(),
						( unsigned long long )compiledInfo->m_proxyMeshId.GetHash(), int( compiledInfo->IsRoad() ), int( compiledInfo->IsBuilding() ), int( compiledInfo->IsTerrain() ) );
				}
			}
			if( compiledInfo->m_proxyMeshId.IsDefined() )
			{
				world::RuntimeSystemNodeStreaming* system = scene->GetSystem<RuntimeSystemNodeStreaming>();
				system->NotifyProxyMeshChildAttached( compiledInfo->m_proxyMeshId.GetHash(), nbInstances );
				return compiledInfo->m_proxyMeshId;
			}
		}
		else
		{
			s_cpChild[ 1 ].fetch_add( 1, std::memory_order_relaxed );
		}
		return GlobalNodeID();
	}

	void NotifyProxyMeshChildNodeDetached( RuntimeScene* scene, const GlobalNodeID& proxyMeshNodeInstanceId, Uint32 nbInstances )
	{
		if( proxyMeshNodeInstanceId.IsDefined() )
		{
			world::RuntimeSystemNodeStreaming* system = scene->GetSystem<RuntimeSystemNodeStreaming>();
			system->NotifyProxyMeshChildDetached( proxyMeshNodeInstanceId.GetHash(), nbInstances );
		}
	}

} // world

//#pragma optimize("",on)
