#include "build.h"

#include "questsSystem.h"
#include "../../gameCore/include/playerSystemInterface.h" // [nomadplay]
#include "../../gameCore/include/teleportationFacilityInterface.h" // [nomadplay]
#include "../../gameCore/include/gameObject.h" // [nomadplay]
#include "../../gameCore/include/vehicleSystemInterface.h" // [nomadplay]
#include "../../world/include/runtimeSystemTrigger.h" // [nomadplay]
#ifndef TRIGGERS_API
#define TRIGGERS_API
#endif
#include "../../triggers/include/triggerManager.h" // [nomadplay]
#include "../../gameCore/include/mappinSystemInterface.h" // [nomadplay]
#ifndef MAPPINS_API
#define MAPPINS_API
#endif
#include "../../gameMappins/include/mappin.h" // [nomadplay]
#include "../../gameCore/include/mountingFacilityInterface.h" // [nomadplay]
#include "../../gameVehicles/include/vehicleBaseObject.h" // [nomadplay]
#include "../../game/include/puppetMountableComponent.h" // [nomadplay]
#include "../../game/include/stateMachineEntityEvents.h" // [nomadplay]
#include "../../game/include/stimuliEventData.h" // [nomadplay]
#include <cstdio>
#include <cstring>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include "questsSystemReplicatedState.h"
#include "nodeInstanceDataManager.h"
#include "gameplayTimerWrapper.h"
#include "phaseNode.h"
#include "fdbImpl.h"
#include "questRuntimeHelper.h"
#include "questResourceLoader.h"
#include "questPhaseFreezer.h"
#include "questDebugMonitor.h"
#include "questSceneDataHelper.h"
#include "phaseNodeQuestPrefabsListener.h"
#include "questDebugLogManager.h"
#include "stsRecoveryHelper.h"
#include "playerObjectListenerMapper.h"
#include "externalEventListenerWrapperRegistry.h"
#include "questEntityCache.h"

#include "../../redSystem/include/hex.h"
#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/gameDelaySystemInterface.h"
#include "../../gameCore/include/gameISceneSystemInterface.h"
#include "../../gameCore/include/phantomEntitySystemInterface.h"
#include "../../gameCore/include/communitySystemInterface.h"
#include "../../gameCore/include/populationSystemInterface.h"
#include "../../gameCore/include/gameObjectCarrySystemInterface.h"
#include "../../gameSave/include/saveVersion.h"
#include "../../gameSceneSystem/include/scnsElwSceneListener.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../worldEntities/include/runtimeEntityRegistry.h"
#include "../../redReflection/include/censorshipSystem.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../world/include/gameInstance.h"
#include "../../worldBlackboardSystem/include/blackboardSystemInterface.h"
#include "../../worldStreaming/src/worldSectorWrapper.h"
#include "../../engine/include/renderEngineViewport.h"
#include "../../core/include/memoryBufferFile.h"

#ifdef RED_USE_IMGUI
#include "../imgui/include/imguiObjectEditor.h"
#endif

namespace
{
	static constexpr Int32 UNDEFINED_SOCKET_INDEX = -1;
	static CName PONR_FACT_NAME = RED_NAME_CONSTEXPR("loaded_ponr_save");
	static CName PONR_REWARD_FACT_NAME = RED_NAME_CONSTEXPR( "added_ponr_rewards" );
}



#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif


RTTI_BEGIN_TYPE_IN_NAMESPACE( QuestsSystem, quest )
	RTTI_PARENT_TYPE( quest::IQuestsSystem )
	RTTI_SCRIPT_ALIAS( "QuestsSystem" )
	RTTI_NATIVE_FUNCTION( "GetFact", funcGetFact );
	RTTI_NATIVE_FUNCTION( "GetFactStr", funcGetFactStr );
	RTTI_NATIVE_FUNCTION( "SetFact", funcSetFact );
	RTTI_NATIVE_FUNCTION( "SetFactStr", funcSetFactStr );
	RTTI_NATIVE_FUNCTION( "RegisterListener", funcRegisterListener );
	RTTI_NATIVE_FUNCTION( "UnregisterListener", funcUnregisterListener );
	RTTI_NATIVE_FUNCTION( "RegisterEntity", funcRegisterEntity );
	RTTI_NATIVE_FUNCTION( "UnregisterEntity", funcUnregisterEntity );
	RTTI_NATIVE_FUNCTION( "GetGameplayTimerData", funcGetGameplayTimerData );
RTTI_END_TYPE()


namespace game
{
	class FactChangedEvent : public red::Event
	{
		RTTI_DECLARE_TYPE( FactChangedEvent );

	public:
		CName m_factName;

	private:
		void funcGetFactName( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
		{
			FINISH_PARAMETERS;
			RETURN_NAME( m_factName );
		}
	};
}

RTTI_BEGIN_TYPE_IN_NAMESPACE( FactChangedEvent, game )
	RTTI_IMPORT_ONLY();
	RTTI_PARENT_TYPE( red::Event );
	RTTI_SCRIPT_ALIAS( "FactChangedEvent" );
	RTTI_PROPERTY( m_factName );
	RTTI_NATIVE_FUNCTION( "GetFactName", funcGetFactName, FF_ConstFunction );
RTTI_END_TYPE();

namespace quest
{
	// ---------------------------------------------------------------------------------------------------------
	// CP2077 web [nomadplay]: quest progress log + test command channel. Every 10 s (and on change) logs the
	// QuestsSystem wait flags, prints waiting quest nodes (phaseInstance.cpp NomadplayQuestDump). Commands: the
	// page sets globalThis.cp2077NomadCmd = { seq: N, text: '...' } (main thread; packed into g_npCmd by a
	// 200 ms main-thread interval). text: 'fact <name> <int>' sets a FactsDB user fact, 'getfact <name>',
	// 'dump' prints all waiting nodes.
	// ---------------------------------------------------------------------------------------------------------
	Double NomadplayNowSeconds();
	void NomadplayQuestDump( Bool all );
	alignas( 4 ) static volatile Int32 g_npCmdSeq = 0;
	static char g_npCmdBuf[ 256 ];
	static void NpCmdInit()
	{
		static Bool s_done = false;
		if ( s_done )
		{
			return;
		}
		s_done = true;
#ifdef __EMSCRIPTEN__
		MAIN_THREAD_EM_ASM( {
			const seqAddr = Number( $0 ); const buf = Number( $1 );
			let last = 0;
			setInterval( () => {
				try {
					const c = globalThis.cp2077NomadCmd;
					if ( !c || !c.seq || c.seq === last ) return;
					last = c.seq;
					const b = new TextEncoder().encode( String( c.text || '' ).slice( 0, 250 ) );
					for ( let i = 0; i < b.length; ++i ) HEAPU8[ buf + i ] = b[ i ];
					HEAPU8[ buf + b.length ] = 0;
					Atomics.store( HEAP32, seqAddr >> 2, c.seq | 0 );
				} catch ( e ) {}
			}, 200 );
		}, ( double )( uintptr_t )&g_npCmdSeq, ( double )( uintptr_t )&g_npCmdBuf[ 0 ] );
#endif
		std::fprintf( stderr, "[nomadplay] quest cmd channel ready: globalThis.cp2077NomadCmd={seq,text} ('fact <name> <int>', 'getfact <name>', 'dump')\n" );
	}
}

namespace quest
{
	template< class TGi > static void NomadplayTick( QuestsSystem& self, const Int32 flags, TGi* gi )
	{
		NpCmdInit();
		static Double s_next = 0.0;
		static Double s_nextAll = 0.0;
		static Int32 s_lastFlags = -1;
		static Int32 s_lastSeq = 0;
		const Double now = NomadplayNowSeconds();
		if ( flags != s_lastFlags )
		{
			std::fprintf( stderr, "[nomadplay] quest system wait flags: restore=%d prefabs=%d entities=%d\n", flags & 1, ( flags >> 1 ) & 1, ( flags >> 2 ) & 1 );
			s_lastFlags = flags;
		}
		const Int32 seq = __atomic_load_n( &g_npCmdSeq, __ATOMIC_ACQUIRE );
		if ( seq != s_lastSeq )
		{
			s_lastSeq = seq;
			char text[ 256 ];
			std::memcpy( text, g_npCmdBuf, sizeof( text ) );
			text[ 255 ] = 0;
			std::fprintf( stderr, "[nomadplay] cmd seq %d '%s'\n", seq, text );
			char name[ 200 ] = {};
			int value = 0;
			if ( std::sscanf( text, "fact %199s %d", name, &value ) == 2 )
			{
				const Int32 before = self.GetFactsDB().Get( USER_TABLE_ID(), FactID( name ) );
				self.GetFactsDB().Set( USER_TABLE_ID(), FactID( name ), value );
				std::fprintf( stderr, "[nomadplay] fact %s: %d -> %d\n", name, before, value );
			}
			else if ( std::sscanf( text, "getfact %199s", name ) == 1 )
			{
				std::fprintf( stderr, "[nomadplay] fact %s = %d\n", name, Int32( self.GetFactsDB().Get( USER_TABLE_ID(), FactID( name ) ) ) );
			}
			else if ( std::strncmp( text, "dump", 4 ) == 0 )
			{
				NomadplayQuestDump( true );
			}
			else if ( std::strncmp( text, "veh", 3 ) == 0 || std::strncmp( text, "mount", 5 ) == 0 || std::strncmp( text, "tpveh", 5 ) == 0 )
			{
				// [nomadplay] 'veh' lists vehicles within 40 m; 'mount <i> [1=passenger]' mounts the player directly (like a scene
				// mount); 'tpveh <i>' puts the player 1.8 m from the driver door, facing the vehicle (for the native [F] prompt).
				auto playerSystem = gi ? gi->template GetSystemPtr< game::IPlayerSystem >() : nullptr;
				auto vehicles = gi ? gi->template GetSystemPtr< game::IVehicleSystem >() : nullptr;
				THandle< game::Object > player = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				if ( player && vehicles )
				{
					const Vector3 pp = player->GetWorldPosition().AsVector3();
					const red::DynArray< THandle< vehicle::BaseObject > > list = vehicles->GetVehicles();
					int want = -1, slot = 0;
					std::sscanf( text + ( text[ 0 ] == 'm' ? 5 : 5 ), "%d %d", &want, &slot );
					int idx = 0;
					for ( const auto& v : list )
					{
						if ( !v )
						{
							continue;
						}
						const Vector3 vp = v->GetWorldPosition().AsVector3();
						const Float d = ( vp - pp ).Mag();
						if ( d > 40.0f )
						{
							continue;
						}
						const Vector3 vf = v->GetWorldForward();
						if ( text[ 0 ] == 'v' )
						{
							std::fprintf( stderr, "[nomadplay] veh %d at (%.2f, %.2f, %.2f) yaw %.1f dist %.1f record=%s (0x%016llx)\n", idx, vp.X, vp.Y, vp.Z, EulerAngles::YawFromXY( vf.X, vf.Y ), d, v->GetVehicleRecordID().ToString().AsChar(), ( unsigned long long )v->GetVehicleRecordID().ToNumber() ); // [missionflow] + record
						}
						else if ( idx == want && text[ 0 ] == 'm' )
						{
							auto mounting = gi->template GetSystemPtr< game::mounting::IMountingFacility >();
							if ( mounting )
							{
								const CName slotName = slot == 1 ? RED_NAME_CONSTEXPR( "seat_front_right" ) : RED_NAME_CONSTEXPR( "seat_front_left" );
								game::mounting::MountingInfo info( player->GetEntityID(), v->GetEntityID(), game::mounting::MountingSlotId( slotName ) );
								THandle< game::MountEventData > data = CreateHandle< game::MountEventData >();
								data->m_mountEventOptions = CreateHandle< game::MountEventOptions >();
								const Bool ok = mounting->Mount( CreateHandle< game::mounting::MountingRequest >( info, true, data ) );
								std::fprintf( stderr, "[nomadplay] mount vehicle %d slot %s -> %d\n", idx, slotName.AsChar(), ok ? 1 : 0 );
							}
						}
						else if ( idx == want && text[ 0 ] == 't' )
						{
							Vector3 f = vf;
							f.Z = 0.0f;
							f.Normalize();
							const Vector3 left( -f.Y, f.X, 0.0f );
							const Vector3 at = vp + left * 1.8f + f * 0.3f;
							auto teleport = gi->template GetSystemPtr< game::ITeleportationFacility >();
							if ( teleport )
							{
								teleport->Teleport( player, Vector4( at.X, at.Y, vp.Z + 0.2f, 1.0f ), EulerAngles( 0.0f, 0.0f, EulerAngles::YawFromXY( -left.X, -left.Y ) ) );
								std::fprintf( stderr, "[nomadplay] tpveh %d: player -> (%.2f, %.2f) facing the driver door\n", idx, at.X, at.Y );
							}
						}
						++idx;
					}
					if ( text[ 0 ] == 'v' )
					{
						std::fprintf( stderr, "[nomadplay] veh: %d vehicles within 40 m (of %u)\n", idx, Uint32( list.Size() ) );
					}
				}
			}
			else if ( std::strncmp( text, "mappins", 7 ) == 0 )
			{
				// [nomadplay] 'mappins': world positions of all world-target mappins (quest objective markers)
				auto mappins = gi ? gi->template GetSystemPtr< game::mappins::IMappinSystem >() : nullptr;
				red::DynArray< THandle< game::mappins::IMappin > > list{ red::PoolFrame() };
				if ( mappins && mappins->GetMappins( game::mappins::MappinTargetType::World, list ) )
				{
					Uint32 i = 0;
					for ( const auto& m : list )
					{
						if ( m && i < 40 )
						{
							const Vector3& mp = m->GetWorldPosition();
							std::fprintf( stderr, "[nomadplay] mappin %u %s at (%.1f, %.1f, %.1f)\n", i, m->GetClass()->GetName().AsChar(), mp.X, mp.Y, mp.Z );
						}
						++i;
					}
					std::fprintf( stderr, "[nomadplay] mappins: %u\n", Uint32( list.Size() ) );
				}
				else
				{
					std::fprintf( stderr, "[nomadplay] mappins: none (system=%d)\n", mappins ? 1 : 0 );
				}
			}
			else if ( std::strncmp( text, "triggers", 8 ) == 0 )
			{
				// [nomadplay] 'triggers [radius]': bounding boxes of all trigger areas within radius (default 300 m) of the player
				int radius = 300;
				std::sscanf( text, "triggers %d", &radius );
				auto playerSystem = gi ? gi->template GetSystemPtr< game::IPlayerSystem >() : nullptr;
				THandle< game::Object > player = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				world::RuntimeSystemTriggers* triggerSystem = gi ? gi->GetScene().template GetSystem< world::RuntimeSystemTriggers >() : nullptr;
				ITriggerManager* tm = triggerSystem ? triggerSystem->GetTriggerManager() : nullptr;
				if ( player && tm )
				{
					const Vector3 pp = player->GetWorldPosition().AsVector3();
					red::DynArray< TriggerObjectDebugInfo > infos{ red::PoolFrame() };
					tm->CollectTriggersDebugInfo( infos );
					Uint32 shown = 0;
					for ( const auto& t : infos )
					{
						const Vector4 c = t.worldBoundingBox.CalcCenter();
						const Vector4 e = t.worldBoundingBox.CalcExtents();
						const Float dx = c.X - pp.X, dy = c.Y - pp.Y;
						if ( dx * dx + dy * dy <= Float( radius ) * Float( radius ) && shown < 80 )
						{
							++shown;
							std::fprintf( stderr, "[nomadplay] trigger center (%.1f, %.1f, %.1f) half (%.1f, %.1f, %.1f) incl 0x%x excl 0x%x\n", c.X, c.Y, c.Z, e.X, e.Y, e.Z, t.includeChannels, t.excludeChannels );
						}
					}
					std::fprintf( stderr, "[nomadplay] triggers: %u within %d m of (%.1f, %.1f) (of %u)\n", shown, radius, pp.X, pp.Y, Uint32( infos.Size() ) );
				}
				else
				{
					std::fprintf( stderr, "[nomadplay] triggers: player=%d manager=%d\n", player ? 1 : 0, tm ? 1 : 0 );
				}
			}
			else if ( std::strncmp( text, "ent ", 4 ) == 0 )
			{
				// [nomadplay] 'ent <NodeRef>' (e.g. ent #q000_nomad_radio_tower_circut): world position of a node-ref entity
				const world::NodeRef ref = world::NodeRef::Parse( red::StringView( text + 4 ) );
				const ent::EntityID id = RuntimeHelper::NodeRefToObjectID( ref );
				THandle< ent::Entity > e = gi ? gi->GetScene().template GetSystemRef< world::RuntimeEntityRegistry >().FindEntity( id ) : THandle< ent::Entity >();
				if ( e )
				{
					const Vector3 ep = e->GetWorldPosition().AsVector3();
					std::fprintf( stderr, "[nomadplay] ent %s at (%.2f, %.2f, %.2f) class %s\n", text + 4, ep.X, ep.Y, ep.Z, e->GetClass()->GetName().AsChar() );
				}
				else
				{
					std::fprintf( stderr, "[nomadplay] ent %s: not found (ref valid=%d)\n", text + 4, ref.IsDefined() ? 1 : 0 );
				}
			}
			else if ( std::strncmp( text, "tpcar", 5 ) == 0 )
			{
				// [nomadplay] 'tpcar x y z [yaw]': teleport the vehicle the player is mounted in (test helper for long drives)
				auto playerSystem = gi ? gi->template GetSystemPtr< game::IPlayerSystem >() : nullptr;
				auto vehicles = gi ? gi->template GetSystemPtr< game::IVehicleSystem >() : nullptr;
				auto teleport = gi ? gi->template GetSystemPtr< game::ITeleportationFacility >() : nullptr;
				THandle< game::Object > player = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				THandle< vehicle::BaseObject > v = ( player && vehicles ) ? vehicles->FindMountedVehicle( player->GetEntityID() ) : THandle< vehicle::BaseObject >();
				float x = 0, y = 0, z = 0, w = 0;
				const int n = std::sscanf( text, "tpcar %f %f %f %f", &x, &y, &z, &w );
				if ( v && teleport && n >= 3 )
				{
					const Vector3 vf = v->GetWorldForward();
					const Float yaw = n >= 4 ? w : EulerAngles::YawFromXY( vf.X, vf.Y );
					teleport->Teleport( THandle< game::Object >( v ), Vector4( x, y, z, 1.0f ), EulerAngles( 0.0f, 0.0f, yaw ) );
					std::fprintf( stderr, "[nomadplay] tpcar -> (%.2f, %.2f, %.2f) yaw %.1f\n", x, y, z, yaw );
				}
				else
				{
					std::fprintf( stderr, "[nomadplay] tpcar: mounted vehicle=%d args=%d\n", v ? 1 : 0, n );
				}
			}
			else if ( std::strncmp( text, "pos", 3 ) == 0 || std::strncmp( text, "tp ", 3 ) == 0 )
			{
				auto playerSystem = gi ? gi->template GetSystemPtr< game::IPlayerSystem >() : nullptr;
				THandle< game::Object > player = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				if ( player )
				{
					const Vector3 p = player->GetWorldPosition().AsVector3();
					const Vector3 f = player->GetWorldForward();
					const Float yaw = EulerAngles::YawFromXY( f.X, f.Y );
					float x = 0, y = 0, z = 0, w = yaw;
					const int n = std::sscanf( text, "tp %f %f %f %f", &x, &y, &z, &w );
					if ( n >= 3 )
					{
						if ( auto teleport = gi->template GetSystemPtr< game::ITeleportationFacility >() )
						{
							teleport->Teleport( player, Vector4( x, y, z, 1.0f ), EulerAngles( 0.0f, 0.0f, n >= 4 ? w : yaw ) );
							std::fprintf( stderr, "[nomadplay] tp player (%.2f, %.2f, %.2f) yaw %.1f -> (%.2f, %.2f, %.2f) yaw %.1f\n", p.X, p.Y, p.Z, yaw, x, y, z, n >= 4 ? w : yaw );
						}
					}
					else
					{
						std::fprintf( stderr, "[nomadplay] pos player (%.2f, %.2f, %.2f) yaw %.1f\n", p.X, p.Y, p.Z, yaw );
					}
				}
				else
				{
					std::fprintf( stderr, "[nomadplay] pos: no player\n" );
				}
			}
		}
		if ( now >= s_next )
		{
			s_next = now + 2.0;
			NomadplayQuestDump( false );
		}
		if ( now >= s_nextAll )
		{
			s_nextAll = now + 30.0;
			NomadplayQuestDump( true );
		}
	}

	QuestsSystem::QuestsSystem()
		: m_waitUntilGameIsRestored( false )
		, m_waitUntilPrefabsAreLoaded( false )
		, m_waitUntilEntitiesAreReady( false )
		, m_socketIndexToExecute( UNDEFINED_SOCKET_INDEX )
		, m_socketIndexToAwakeSignalAt( UNDEFINED_SOCKET_INDEX )
		, m_debugChangedNodeCount( 0 )
		, m_debugMissingNodeCount( 0 )
		, m_debugMissingPhaseResourceCount( 0 )
		, m_debugFailedToLoadSceneInstancesCount( 0 )
	{
	}

	QuestsSystem::~QuestsSystem()
	{
	}

	void QuestsSystem::OnInitialize( job::Counter& counter )
	{
		RegisterNativeFunctions();

		m_eventManager = red::CreateUniquePtr< EventManager >( *this );
		m_nodeInstanceDataManager = red::CreateUniquePtr< NodeInstanceDataManager >();
		m_factsDB = red::CreateUniquePtr< FactsDB >();

#if !defined( RED_CONFIGURATION_FINAL )
		if ( !GetGameInstance()->GetRuntimeInfo().IsGamePreview() )
#endif
		{
			m_questPrefabsHandler = CreateHandle< QuestPrefabsHandler >();
		}

		m_resourceLoader = red::CreateUniquePtr< QuestResourceLoader >();
		m_phaseFreezer = red::CreateUniquePtr< QuestPhaseFreezer >( *this );
		m_sceneDataHelper = red::CreateUniquePtr< SceneDataHelper >( *m_gameInstance );
		m_playerObjectListenerMapper = red::CreateUniquePtr< PlayerObjectListenerMapper >( *m_gameInstance, *m_eventManager );
		m_externalEventListenerRegistry = red::CreateUniquePtr< ExternalEventListenerWrapperRegistry >();
		m_questEntityCache = red::CreateUniquePtr< QuestEntityCache >( *m_gameInstance );
	}

	void QuestsSystem::OnUninitialize()
	{
		UnregisterNativeFunctions();

		m_playerObjectListenerMapper.Reset();
		m_eventManager.Reset();
		m_nodeInstanceDataManager.Reset();
		m_factsDB.Reset();
		m_questPrefabsHandler.Reset();
		m_resourceLoader.Reset();
		m_phaseFreezer.Reset();
		m_sceneDataHelper.Reset();
		m_externalEventListenerRegistry.Reset();
		m_questEntityCache.Reset();
	}

	void QuestsSystem::WorldAttached( world::RuntimeScene& scene )
	{
		TBaseClass::WorldAttached( scene );

		m_rootInstance = CreateHandle< RootInstance >();
		// m_nodePathMap - nothing to do

		// m_activeQuests - nothing to do
		// m_activeInstances - nothing to do
		// m_eventManager - nothing to do
		// m_nodeInstanceDataManager - nothing to do
		// m_factsDB - nothing to do
		if ( m_questPrefabsHandler )
		{
			m_questPrefabsHandler->WorldAttached( *GetGameInstance() );
		}

		// m_questsToRun - nothing to do
		// m_questsToStopAndRemove - nothing to do
		// m_eventsToProcess - nothing to do
		// m_eventsToSkip - nothing to do

		m_resourceLoader->WorldAttached();
		m_phaseFreezer->WorldAttached();
		m_sceneDataHelper->WorldAttached();
		m_questEntityCache->WorldAttached();

		m_isLoadingQuest.SetValue( false );
		m_waitUntilGameIsRestored = false;
		m_waitUntilPrefabsAreLoaded = false;
		m_waitUntilEntitiesAreReady = false;

		// m_listenerWrappersToDelete - nothing to do

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		m_performanceMonitor = red::CreateUniquePtr< PerformanceMonitor >( Cast< IQuestsSystem >( HandleFromThis() ) );
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

#ifdef RED_ENABLE_QUEST_DEBUGGER
		m_questDebugger = red::CreateUniquePtr< dbg::QuestDebugger >();
		m_questDebugger->Initialize( this );
#endif

#ifdef RED_ENABLE_FDB_DEBUGGER
		m_fdbDebugger = red::CreateUniquePtr< dbg::FDBDebugger >();
		m_fdbDebugger->Initialize( this );
#endif

#ifdef RED_ENABLE_QUEST_DEBUG_MONITOR
		if ( m_questPrefabsHandler )
		{
			m_debugMonitor = red::CreateUniquePtr< DebugMonitor >( *m_gameInstance, *m_questPrefabsHandler );
		}
#endif

		m_debugLogManager = red::CreateUniquePtr< DebugLogManager >();

		m_stsRecoveryHelper = red::CreateUniquePtr< StsRecoveryHelper >( *m_gameInstance );
	}

	void QuestsSystem::WorldPendingDetach( world::RuntimeScene& scene )
	{
		TBaseClass::WorldPendingDetach( scene );

		StopAll( SignalKillingReason::Shutdown );
		DeletePostponedListeners(); // explicitly delete all postponed wrappers, do not wait until it happens in destructor
	}

	void QuestsSystem::WorldDetached( world::RuntimeScene& world )
	{
		{
			RED_SCOPE_LOCK( m_phaseLock );
			m_rootInstance.Reset();
		}
		m_nodePathMap.Clear();

		m_activeQuests.Clear();
		m_activeInstances.Clear();
		m_playerObjectListenerMapper->WorldDetached();
		m_eventManager->WorldDetached();
		m_nodeInstanceDataManager->WorldDetached();
		m_factsDB->WorldDetached();
		if ( m_questPrefabsHandler )
		{
			m_questPrefabsHandler->WorldDetached();
		}

		{
			RED_SCOPE_LOCK( m_questsToRunAndStopLock );
			m_questsToRun.Clear();
			m_questsToStopAndRemove.Clear();
		}
		{
			RED_SCOPE_LOCK( m_eventsToProcessLock );
			m_eventsToProcess.Clear();
			m_eventsToSkip.Clear();
		}

		m_resourceLoader->WorldDetached();
		m_phaseFreezer->WorldDetached();
		m_sceneDataHelper->WorldDetached();
		m_externalEventListenerRegistry->WorldDetached();
		m_questEntityCache->WorldDetached();

		m_isLoadingQuest.SetValue( false );
		m_waitUntilGameIsRestored = false;
		m_waitUntilPrefabsAreLoaded = false;
		m_waitUntilEntitiesAreReady = false;

		// m_listenerWrappersToDelete - deleted in WorldPendingDetach

#ifdef RED_ENABLE_QUEST_STATS
		m_resourceStats.Clear();
#endif

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		m_performanceMonitor.Reset();
#endif

#ifdef RED_ENABLE_QUEST_DEBUG_MONITOR
		m_debugMonitor.Reset();
#endif

		m_debugLogManager.Reset();

		m_stsRecoveryHelper.Reset();

#ifdef RED_ENABLE_FDB_DEBUGGER
		if ( m_fdbDebugger )
		{
			m_fdbDebugger->Shutdown();
			m_fdbDebugger.Reset();
		}
#endif

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( m_questDebugger )
		{
			m_questDebugger->Shutdown();
			m_questDebugger.Reset();
		}
#endif

		{
			RED_SCOPE_LOCK( m_debuggerLock );
			m_nodeToExecute.Clear();
			m_socketIndexToExecute = UNDEFINED_SOCKET_INDEX;
			m_nodeToAwakeSignalAt.Clear();
			m_socketIndexToAwakeSignalAt = UNDEFINED_SOCKET_INDEX;
			m_nodeToKillSignalAt.Clear();
		}

		m_debugWarningText.Clear();
		m_debugWarningLines.Clear();
		m_debugChangedNodeCount = 0;
		m_debugMissingNodeCount = 0;
		m_debugMissingPhaseResourceCount = 0;
		m_debugFailedToLoadSceneInstancesCount = 0;

		m_debuggerRunRequests.Clear();

		// m_debugFlags - do not clear debug flags

		TBaseClass::WorldDetached( world );
	}

	static void ResourcePathHashToHexString( const Uint64 pathHash, red::String& outString )
	{
		const void* hashPtr = static_cast<const void*>( &pathHash );
		constexpr Uint32 pathHashEncodedLength = red::HexEncodedSize( sizeof( Uint64 ) ) + 1;
		outString.Resize( outString.Length() + pathHashEncodedLength );
		red::HexEncode( hashPtr, sizeof( Uint64 ), outString.AsChar(), pathHashEncodedLength );
	}

	game::IGameSystem::PreSaveResponse QuestsSystem::OnPreGameSave( const job::RunContext& context, game::MetadataCollector& collector )
	{
		if ( m_waitUntilPrefabsAreLoaded )
		{
			OnQuestPrefabNotLoaded();
		}

		static const CName ACTIVE_QUESTS = RED_NAME_CONSTEXPR( "activeQuests" );
		static const AnsiChar* LIST_DELIMITER = ";";
		static const Uint32 LIST_DELIMITER_LENGTH = static_cast<Uint32>( std::strlen( LIST_DELIMITER ) );

		red::String activeQuests;
		activeQuests.Reserve( ( red::HexEncodedSize( sizeof( Uint64 ) ) + LIST_DELIMITER_LENGTH ) * m_activeQuests.Size() );
		for ( Uint32 i = 0; i < m_activeQuests.Size(); ++i )
		{
			const Uint64 pathHash = m_activeQuests[ i ];
			if ( !activeQuests.Empty() )
			{
				activeQuests.Append( LIST_DELIMITER, LIST_DELIMITER_LENGTH );
			}
			ResourcePathHashToHexString( pathHash, activeQuests );
		}
		collector.Push< String >( ACTIVE_QUESTS, activeQuests );

		m_phaseFreezer->PreSave();

		m_sceneDataHelper->PreSave();

		const PlayerObjectGatherContext gatherCtx{ *m_rootInstance, *m_externalEventListenerRegistry };
		m_playerObjectListenerMapper->Gather( gatherCtx );

		return game::IGameSystem::PreSaveResponse::ReadyForSaving;
	}

	void QuestsSystem::OnGameSave(save::IStream& saveStream)
	{
		QuestScopedLock lockP( m_phaseLock );

		SAVE_NODE( questSystem );

		Save( saveStream );
		m_factsDB->Save( saveStream );
		ArraySerializer::Serialize( saveStream, m_activeQuests );
		m_activeInstances.Save( saveStream );
		m_eventManager->Save( saveStream );
		m_nodeInstanceDataManager->Save( saveStream );
		m_questPrefabsHandler->Save( saveStream );
		m_phaseFreezer->Save( saveStream );
		m_debugLogManager->Save( saveStream );
		m_sceneDataHelper->Save( saveStream );
		m_playerObjectListenerMapper->Save( saveStream );
	}

	void QuestsSystem::OnGameLoad( const job::RunContext& context, Bool& result, save::IStream& loadStream )
	{
		Bool quickExit = false;
		LOAD_REQUIRED_NODE( questSystem, result, quickExit );
		if ( quickExit )
		{
			return;
		}

		// system is ticked between calls to OnGameLoad and OnGameRestored, we need to prevent that
		// PMTODO it might be required to do it in other systems that quest system depend on i.e. time system, delay system, etc
		m_waitUntilGameIsRestored = true;

		if ( !Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !m_factsDB->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if( loadStream.GetSaveVersion() < save::SaveVersion::Quest_StringLess )
		{
			StringArray< Int16 > activeQuests;
			if( activeQuests.Load( loadStream ) )
			{
				for( Uint32 i = 0; i < activeQuests.GetObjectCount(); ++i )
				{
					red::String activeQuest;
					activeQuests.GetObject( i, activeQuest );
					m_activeQuests.PushBack( res::ResourcePath::Build( activeQuest ).GetHash() );
				}

				if ( !RecreateQuestInstancesAfterLoad() )
				{
					result = false;
					return;
				}
			}
			else
			{
				result = false;
				return;
			}
		}
		else
		{
			ArraySerializer::Serialize( loadStream, m_activeQuests );
			if( loadStream.IsOK() )
			{
				if ( !RecreateQuestInstancesAfterLoad() )
				{
					result = false;
					return;
				}
			}
			else
			{
				result = false;
				return;
			}
		}

		if ( m_activeInstances.Load( loadStream ) )
		{
			RecreateNodePathMapAfterLoad();
		}
		else
		{
			result = false;
			return;
		}

		if ( m_eventManager->Load( loadStream ) )
		{
			m_eventManager->RecreateFreeIndicesAfterLoad();
		}
		else
		{
			result = false;
			return;
		}

		if ( !m_nodeInstanceDataManager->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !m_questPrefabsHandler->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !m_phaseFreezer->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !m_debugLogManager->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !m_sceneDataHelper->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if( !m_playerObjectListenerMapper->Load( loadStream ) )
		{
			result = false;
			return;
		}

		if ( !RecreatePhaseInstanceTreeAfterLoad() )
		{
			result = false;
		}
	}

	Bool QuestsSystem::OnGameRestored()
	{
		m_sceneDataHelper->LoadFrozenScenes();

		// after calling OnGameRestored, quest system can be ticked
		m_waitUntilGameIsRestored = false;

		// set to true to restore signal in nodes on game load
		m_waitUntilPrefabsAreLoaded = true;
		m_waitUntilEntitiesAreReady = true;

		// this needs to be called AFTER WorldStateSystem::OnGameRestored()
		return true;
	}

	void QuestsSystem::OnGamePrepared()
	{
		// this needs to be called both when starting the game and loading the savegame, but AFTER OnGameLoad
		UpdateCensorshipFacts();
	}

	void QuestsSystem::RegisterUpdateFunctions( UpdatableSystemRegistrar& processor )
	{
		processor.Register( UpdateTickGroup::PreBuckets, *this, "QuestsSystem/Tick",
							[ this ]( const UpdateContext& updateContext, job::Builder& builder )
		{
			Tick( builder, updateContext.m_timeDelta, updateContext.m_tickInfo );
		} );
	}

	void QuestsSystem::OnRenderDebug( const RenderDebugContext& context )
	{
		auto& dd = context.debugDrawer;
		PC_SCOPE( QuestsSystem_OnRenderDebug );

		rend::FragmentBuilder builder( dd );

#ifdef RED_ENABLE_QUEST_DEBUG_MONITOR
		static rend::DebugFilter DF_QuestsSystem_PrefabLoading( "GameSystem/QuestsSystem/PrefabLoading" );
		if ( dd.GetMask().Test( DF_QuestsSystem_PrefabLoading ) )
		{
			if ( m_debugMonitor )
			{
				m_debugMonitor->OnRenderDebug( builder );
			}
		}
#endif

#ifndef RED_CONFIGURATION_FINAL
		const Int32 LINE_HEIGHT = 19;
		Uint32 y = 40;
		const Uint32 x = 800;
		builder.SetColor( Color::RED() );
		builder.EnableTextBackground( Color::BLACK() );

		if ( m_waitUntilGameIsRestored || m_waitUntilPrefabsAreLoaded )
		{
			builder.AddFlatTextf( x, y, "NOT LOADED PREFABS:" );
			y += LINE_HEIGHT;


			auto streamingSystem = m_gameInstance->GetScene().GetSystem<world::RuntimeSystemWorldStreaming>();
			auto notLoadedPrefabs = m_questPrefabsHandler->DebugGetNotLoadedPrefabs();
			String extraDebugText = "";
#ifndef RED_PLATFORM_CONSOLE
			const Bool getExtraDebugInfo = true;// notLoadedPrefabs.Size() < 10; // some random number for now
#else
			const Bool getExtraDebugInfo = false;// notLoadedPrefabs.Size() < 10; // some random number for now
#endif
			for ( const auto& notLoadedPrefab : notLoadedPrefabs )
			{
				if (getExtraDebugInfo)
				{
					world::SectorNodeLoadingDebugInfo debugInfo;
					if (streamingSystem)
					{
						if (streamingSystem->GetQuestPrefabLoadingDebugInfo(notLoadedPrefab, debugInfo))
						{
							extraDebugText = String::Printf(" %u/%u/%u|%u ", debugInfo.numUnloaded, debugInfo.numPartial, debugInfo.numFull, debugInfo.numSectorsTotal);
						}
						else
						{
							extraDebugText = " <FAILED>";
						}
					}
				}

				builder.AddFlatTextf( x, y, "%hs%hs", notLoadedPrefab.ToDebugString().AsChar(), extraDebugText.AsChar() );
				y += LINE_HEIGHT;
			}
			y += LINE_HEIGHT;

			// Draws over the prefab info, but can see if anything is making progress
			const Bool forceDrawLoadingMetrics = !dd.GetMask().Test(rend::helper::GetLoadingMetricsDebugFilter());
			if (forceDrawLoadingMetrics)
				rend::helper::DrawLoadingMetrics(dd, rend::StalledFrameParams{});
		}

		if ( !m_debugWarningText.Empty() )
		{
			builder.EnableTransparency( true );
			builder.EnableTextBackground( Color::BLACK( 200 ) );			
			builder.AddFlatTextf( 170, y, m_debugWarningText.AsChar() );

			if ( m_debugFlags.m_showLoadReport )
			{
				y += 2 * LINE_HEIGHT;
				builder.SetColor( Color::WHITE() );

				for ( const auto& line : m_debugWarningLines )
				{
					builder.AddFlatTextf( x, y, line.AsChar() );
					y += LINE_HEIGHT;
				}
			}

			builder.DisableTextBackground();
		}
#endif //RED_CONFIGURATION_FINAL

		if ( m_debugFlags.m_showInstances )
		{
			if ( m_rootInstance )
			{
				Int32 y = 100;
				m_rootInstance->OnRenderDebugShowInstances( builder, 0, 30, y );
			}
		}
		if ( m_debugFlags.m_showEvents )
		{
			if ( m_rootInstance )
			{
				Int32 y = 350;
				m_eventManager->OnRenderDebugShowEvents( builder, 30, y, m_rootInstance, m_nodePathMap );
			}
		}
		if ( m_debugFlags.m_showNodePaths )
		{
			Int32 y = 100;
			OnRenderDebugShowNodePaths( builder, 1000, y );
		}
		if ( m_debugFlags.m_showActiveInstances )
		{
			Int32 y = 100;
			OnRenderDebugShowActiveQuestAndInstances( builder, 1300, y );
		}
		if ( m_debugFlags.m_showResourceLoader )
		{
			m_resourceLoader->OnRenderDebugShowResourceLoader( m_debugFlags, context, 1100, 350 );
		}

		m_resourceLoader->OnRenderDebugShowResourceLoader( m_debugFlags, context, 1100, 350 );

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( m_performanceMonitor )
		{
			m_performanceMonitor->OnRenderDebug( m_debugFlags, context );
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		if ( m_debugLogManager )
		{
			m_debugLogManager->OnRenderDebug( context );
		}

		if ( m_phaseFreezer )
		{
			m_phaseFreezer->OnRenderDebug( context );
		}

		if ( m_questPrefabsHandler )
		{
			m_questPrefabsHandler->OnRenderDebug( context );
		}


		static rend::DebugFilter DF_QuestsSystem_QuestFacts( "GameSystem/QuestsSystem/Facts" );
		if ( dd.GetMask().Test( DF_QuestsSystem_QuestFacts ) )
		{
#ifdef RED_USE_IMGUI
			static red::DynArray< String > factNames{ game::PoolGMPL_QuestDynamic() };
			static AnsiChar addFactName[ 512 ] = { 0 };

			ImGui::Begin( "Quest Facts" );

			ImGui::Text( "Fact name" );	ImGui::SameLine();
			ImGui::InputText( "", addFactName, sizeof( addFactName ), 0 ); ImGui::SameLine();
			if ( ImGui::Button( "Show fact" ) )
			{
				if ( red::Strlen( addFactName, sizeof( addFactName ) ) > 0 )
				{
					if ( !factNames.Exist( addFactName ) )
					{
						factNames.PushBack( addFactName );
					}
					addFactName[ 0 ] = 0;
				}
			}

			ImGui::Separator();

			red::ImGui::ColumnHelper columns;
			columns.Add( 150, true ).Add( 50, true ).Begin();

			ImGui::PushStyleColor( ImGuiCol_Text, red::ImGui::YELLOW() );
			ImGui::Text( "Fact name" ); ImGui::NextColumn();
			ImGui::Text( "Value" ); ImGui::NextColumn();
			ImGui::PopStyleColor();

			for ( const auto& factName : factNames )
			{
				auto value = GetFactsDB().Get( USER_TABLE_ID(), FactID( factName ) );

				ImGui::Text( factName.AsChar() ); ImGui::NextColumn();
				ImGui::Text( ToStringDirect( value ).AsChar() ); ImGui::NextColumn();
			}

			columns.End();

			ImGui::End();
#endif //RED_USE_IMGUI
		}
	}

	void QuestsSystem::DebugOnPhasePrefabChanged( Bool start, Uint64 nodeHash, const String& path, const red::DynArray< QuestPrefabEntry >* prefabs )
	{
#ifdef RED_ENABLE_QUEST_DEBUG_MONITOR
		m_debugMonitor->OnPhasePrefabChanged( nodeHash, path, start, prefabs );
#endif
	}

	const rtti::ClassType* QuestsSystem::GetReplicatedStateClass() const
	{
		return QuestsSystemReplicatedState::GetStaticClass();
	}

	void QuestsSystem::OnPreSnapshotCaptured()
	{
		auto replicatedState = GetReplicatedState< QuestsSystemReplicatedState >();
		RED_FATAL_ASSERT( replicatedState );
		m_questPrefabsHandler->UpdateReplicatedState(replicatedState->m_replicatedQuestPrefabs);
	}

	void QuestsSystem::OnPostSnapshotApplied()
	{
		auto replicatedState = GetReplicatedState< QuestsSystemReplicatedState >();
		RED_FATAL_ASSERT( replicatedState );
		m_questPrefabsHandler->ApplyReplicatedState(replicatedState->m_replicatedQuestPrefabs);
	}

	void QuestsSystem::Run( const THandle< QuestResource >& questResource )
	{
		QuestScopedLock lockQ( m_questsToRunAndStopLock );

		m_questsToRun.PushBack( questResource );
	}

	Bool QuestsSystem::Run( const res::ResourcePath& questPath )
	{
		if ( !questPath.IsValid() )
		{
			return false;
		}

		auto questRes = Cast< quest::QuestResource >( GResourceLoader->IssueLoadingRequest( questPath )->WaitUntilLoaded() );
		if ( !questRes )
		{
			return false;
		}

		Run( questRes );
		return true;
	}

	void QuestsSystem::Pause( Bool pause )
	{
		auto delaySystem = m_gameInstance->GetSystemPtr< game::IDelaySystem >();
		RED_FATAL_ASSERT( delaySystem, "Missing delay system" );

		if ( pause )
		{
			delaySystem->PauseTickableContexts( game::DelayContext::Quest_TD );
		}
		else
		{
			delaySystem->UnpauseTickableContexts( game::DelayContext::Quest_TD );
		}
	}

	Bool QuestsSystem::IsPaused() const
	{
		auto delaySystem = m_gameInstance->GetSystemPtr< game::IDelaySystem >();
		RED_FATAL_ASSERT( delaySystem, "Missing delay system" );
		return delaySystem->IsTickableContextPaused( game::DelayContext::Quest_TD );
	}

	Bool QuestsSystem::IsStarted() const
	{
		return !m_waitUntilGameIsRestored && !m_waitUntilPrefabsAreLoaded && !m_waitUntilEntitiesAreReady;
	}

	void QuestsSystem::RunAsync( const res::ResourcePath& questPath )
	{
		m_isLoadingQuest.SetValue( true );

		if ( !questPath.IsValid() )
		{
			RED_LOG_ERROR( "Start quest '%hs' failed to run!", questPath.ToDebugString() );
			m_isLoadingQuest.SetValue( false );
			return;
		}

		m_resourceLoader->LoadResourceAsync(	questPath,
												[ this, questPath ]( THandle< QuestPhaseResource > questPhaseResource )
												{
													if ( auto questResource = Cast< QuestResource >( questPhaseResource ) )
													{
														Run( questResource );
													}
													else
													{
														RED_LOG_ERROR( "Start quest '%hs' failed to run!", questPath.ToDebugString() );
													}

													m_isLoadingQuest.SetValue( false );
												} );
	}

	void QuestsSystem::Resume( Uint32 eventIndex )
	{
		{
			RED_SCOPE_LOCK( m_eventsToProcessLock );

			GAME_ASSERT( eventIndex >= 0 && eventIndex < static_cast< Int32 >( m_eventManager->GetEventCount() ), Quests, "Incorrent event index [%d]", eventIndex );
			m_eventsToProcess.PushBack( eventIndex );
		}
	}

	void QuestsSystem::StopAndRemove( const THandle< QuestResource >& questResource )
	{
		QuestScopedLock lockQ( m_questsToRunAndStopLock );

		m_questsToStopAndRemove.PushBack( questResource );
	}

	namespace dd
	{
		static red::CrashData< Uint8 >			s_actionType( "Game/QuestsSystem", "QuestAction" );
		static red::CrashData< String, 256 >	s_questNodeIdPath( "Game/QuestsSystem", "NodePath" );
		static red::CrashData< NodeId >			s_questNodeId( "Game/QuestsSystem", "NodeId" );

		static void SetCrashData( ExecutionActionType actionType, const FullPath* questNodeIdPath, NodeId questNodeId )
		{
			if ( questNodeIdPath )
			{
				s_actionType.Set( static_cast< Uint8 >( actionType ) );
				s_questNodeIdPath.Set( quest::RuntimeHelper::GetFullPathString( *questNodeIdPath ) );
				s_questNodeId.Set( questNodeId );
			}
		}

		static void ClearCrashData()
		{
			s_actionType.Clear();
			s_questNodeIdPath.Clear();
			s_questNodeId.Clear();
		}
	}

	void QuestsSystem::StopAll( SignalKillingReason reason )
	{
		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Shutdown, INVALID_OPERATION_INDEX );
		context.PushPhase( m_rootInstance.Get() );

		{
			QuestScopedLock lockP( m_phaseLock );
			m_rootInstance->StopAll( context, reason );
		}

		const PhaseInstance* currPhase = context.PopPhase();
		GAME_ASSERT( currPhase == m_rootInstance, Quests, "Invalid phase popped from phase stack" );

		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
	}

	void QuestsSystem::StopPhase( const HashedNodePath& hashedNodePath, SignalKillingReason reason )
	{
		QuestScopedLock lockP( m_phaseLock );

		const auto* foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Freeze, INVALID_OPERATION_INDEX );
		OutputSocketArray dummy;

		dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		m_rootInstance->StopPhase( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId(), reason );
		dd::ClearCrashData();

		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
	}

	Bool QuestsSystem::CanBeFrozen( const HashedNodePath& hashedNodePath )
	{
		QuestScopedLock lockP( m_phaseLock );

		const auto* foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		Bool ret = false;

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Freeze, INVALID_OPERATION_INDEX );
		OutputSocketArray dummy;
		ret = m_rootInstance->CanBeFrozen( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );

		return ret;
	}

	void QuestsSystem::FreezeImmediately( const HashedNodePath& hashedNodePath )
	{
		QuestScopedLock lockP( m_phaseLock );

		const auto* foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Freeze, INVALID_OPERATION_INDEX );
		OutputSocketArray dummy;

		dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		m_rootInstance->FreezeImmediately( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		dd::ClearCrashData();

		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
	}

	Bool QuestsSystem::Unfreeze_MakeReady( const HashedNodePath& hashedNodePath )
	{
		PC_SCOPE( QuestsSystem_UnfreezeNodes_MakeReady );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist" );

		QuestScopedLock lockP( m_phaseLock );

		auto foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Unfreeze, UNFREEZE_NODE_OPERATION_INDEX );

		dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		Bool ret = m_rootInstance->Unfreeze_MakeReady( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
		dd::ClearCrashData();

		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );

		return ret;
	}

	void QuestsSystem::Unfreeze_Run( const HashedNodePath& hashedNodePath, const red::DynArray< CName >& inputSockets, UnfreezingStage stage )
	{
		PC_SCOPE( QuestsSystem_UnfreezeNodes_Run );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist" );

		QuestScopedLock lockP( m_phaseLock );

		auto foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Unfreeze, UNFREEZE_NODE_OPERATION_INDEX );

		// PMTODO send proper socket name scenes
		OutputSocketArray dummy;
		if ( inputSockets.Empty() )
		{
			dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
			m_rootInstance->Unfreeze_Run( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId(), NOTHING_SOCKET_NAME, dummy, stage );
			dd::ClearCrashData();
		}
		else
		{
			for ( const auto& inputSocket : inputSockets )
			{
				dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
				m_rootInstance->Unfreeze_Run( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId(), inputSocket, dummy, stage );
				dd::ClearCrashData();

				dummy.Clear();
			}
		}

		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
	}

	void QuestsSystem::UnfreezeImmediately( const HashedNodePath& hashedNodePath, const red::DynArray< CName >& inputSockets )
	{
		PC_SCOPE( QuestsSystem_UnfreezeNodes_Immediately );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist" );

		QuestScopedLock lockP( m_phaseLock );

		auto foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

		ExecutionContext context = CreateExecutionContext( ExecutionActionType::Unfreeze, UNFREEZE_NODE_OPERATION_INDEX );

		OutputSocketArray dummy;
		for ( const CName inputSocket : inputSockets )
		{
			dd::SetCrashData( context.GetActionType(), foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId() );
			m_rootInstance->UnfreezeImmediately( context, *foundFullPath, hashedNodePath.GetExtendedNodeId().GetNodeId(), inputSocket, dummy );
			dd::ClearCrashData();

			dummy.Clear();
		}
		GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
	}

	void QuestsSystem::HACK_RecoverLostSceneSignals( Bool onLoadGame )
	{
 		if ( m_debugLogManager->HasRecoveryData() )
		{
			if ( m_sceneDataHelper->IsLoadingStageFinished() )
			{
				RecreateLostSignals( onLoadGame );
			}
		}
	}

	void QuestsSystem::HACK_RecoverLostStsSignals( Bool onLoadGame )
	{
		if ( m_stsRecoveryHelper->HasRecoveryData() )
		{
			if ( m_sceneDataHelper->IsLoadingStageFinished() )
			{
				RecreateLostStsSignals( onLoadGame );
			}
		}
	}

	Bool QuestsSystem::HACK_IsStsSignalOkOrFrozen( const FullPath& fullPath ) const
	{
		THandle< PhaseInstance > phaseInstance = nullptr;

		for ( Uint32 i = 0; i < fullPath.Size(); ++i )
		{
			if ( i == 0 )
			{
				phaseInstance = m_rootInstance->GetChildByIdEx( fullPath[ i ] );
			}
			else
			{
				phaseInstance = phaseInstance->GetChildByIdEx( fullPath[ i ] );
			}

			if ( !phaseInstance )
			{
				break;
			}
		}

		if ( phaseInstance )
		{
			// sts phase exists, let's test if it contains any children
			if ( phaseInstance->GetChildrenCount() == 0 )
			{
				// no children, the signal is lost
				return false;
			}
		}
		else
		{
			// phase doesn't exist, looks like it's frozen so we can't determine if it's ok
			return false;
		}

		return true;
	}

	void QuestsSystem::RegisterExternalEventListenerWrapper( const HashedNodePath& nodePath, const red::WeakPtr< quest::EventListenerWrapper >& listenerWrapper )
	{
		m_externalEventListenerRegistry->RegisterWrapper( nodePath, listenerWrapper );
	}

	void QuestsSystem::UnregisterExternalEventListenerWrapper( const HashedNodePath& nodePath )
	{
		m_externalEventListenerRegistry->UnregisterWrapper( nodePath );
	}

	PlayerObjectType QuestsSystem::GetPlayerObjectTypeForNodePath( const HashedNodePath& nodePath ) const
	{
		return m_playerObjectListenerMapper->GetPlayerObjectTypeForNodePath( nodePath );
	}

	void QuestsSystem::RemovePlayerObjectTypePersistency( const HashedNodePath& nodePath )
	{
		m_playerObjectListenerMapper->RemovePlayerObjectTypeForNodePath( nodePath );
	}

	void QuestsSystem::PostponeListenerDeletion( red::SharedPtr< quest::EventListenerWrapper >&& listenerWrapper )
	{
		m_listenerWrappersToDelete.PushBack( std::move( listenerWrapper ) );
	}

	void QuestsSystem::PostponeRunRequest( RunRequest&& request )
	{
		m_debuggerRunRequests.PushBack( std::move( request ) );
	}

	void QuestsSystem::OnUpdateEvents( Uint32 eventIndex )
	{
		Bool removed;
		{
			RED_SCOPE_LOCK( m_eventsToProcessLock );
			removed = m_eventsToProcess.Remove( eventIndex ).IsSuccessful();
		}

		// we might be in the middle of processing events right now
		// in such case, that index does not exist on m_eventsToProcess queue, because we're already operating on a copy in TickPendingEvents()
		// we need to remember that index here and just skip it in the loop in TickPendingEvents()
		if ( !removed )
		{
			m_eventsToSkip.PushBack( eventIndex );
		}
	}

	void QuestsSystem::AddNodeInstanceData( NodeInstanceHash hash, NodeInstanceData data )
	{
		m_nodeInstanceDataManager->Add( hash, data );
	}

	void QuestsSystem::RemoveNodeInstanceData( NodeInstanceHash hash )
	{
		m_nodeInstanceDataManager->Remove( hash );
	}

	Bool QuestsSystem::GetNodeInstanceData( NodeInstanceHash hash, NodeInstanceData& data ) const
	{
		return m_nodeInstanceDataManager->Get( hash, data );
	}

	Bool QuestsSystem::NodeInstanceDataExists( NodeInstanceHash hash ) const
	{
		return m_nodeInstanceDataManager->Exists( hash );
	}

	Bool QuestsSystem::IsReady() const
	{
		if ( m_isLoadingQuest.GetValue() )
		{
			return false;
		}

		return m_sceneDataHelper->IsReady();
	}

	QuestMetrics QuestsSystem::GetQuestMetrics() const
	{
		QuestMetrics questMetrics;

#ifdef RED_ENABLE_QUEST_STATS
		questMetrics.m_loadedQuestCount = m_resourceStats.m_loadedQuestCount;
		questMetrics.m_loadedPhaseCount = m_resourceStats.m_loadedPhaseCount;
		questMetrics.m_loadedNodeCount = m_resourceStats.m_loadedNodeCount;
		questMetrics.m_maxPhaseDepth = m_resourceStats.m_maxPhaseDepth;
#endif
		questMetrics.m_activeNodeCount = m_eventManager->GetActiveEventCount();
		questMetrics.m_maxActiveNodeCount = m_eventManager->GetEventCount();

		{
			const auto poolHandle = game::PoolGMPL_Quest::GetHandle();
			red::memory::RuntimePoolMetrics poolMetrics;
			red::memory::GetRuntimePoolMetrics( poolHandle, poolMetrics );

			questMetrics.m_allocatedMemory = static_cast< Uint32 >( poolMetrics.bytesAllocated );
			questMetrics.m_allocatedMemoryPeak = static_cast< Uint32 >( poolMetrics.bytesAllocatedPeak );
		}
		
		return questMetrics;
	}

	Bool QuestsSystem::ArePrefabsLoaded( Uint32 fullPathHash ) const
	{
		auto foundFullPath = m_nodePathMap.FindPtr( fullPathHash );
		if ( !foundFullPath )
		{
			return false;
		}

		THandle< PhaseInstance > phaseInstance;

		for ( Uint32 i = 0; i < (*foundFullPath).Size(); ++i )
		{
			GenericId id = (*foundFullPath)[ i ];

			if ( i == 0 )
			{
				phaseInstance = m_rootInstance->GetChild( id );
			}
			else
			{
				Bool isReady = false;

				auto phaseNode = Cast< PhaseNodeDefinition >( phaseInstance->FindNodeById( id ) );
				if ( phaseNode )
				{
					const auto& prefabs = phaseNode->GetPhasePrefabs();
					if ( prefabs.Empty() )
					{
						// no prefab to load, it's ready
						isReady = true;
					}
					else
					{
						Uint32 readyCount = 0;
						for ( const auto& prefab : prefabs )
						{
							const world::GlobalNodeID& worldglobalNodeID = m_gameInstance->GetRuntimeInfo().GetWorldGlobalNodeID();
							const world::GlobalNodeRef prefabNodeId = RuntimeHelper::NodeRefToGlobalNodeRef( prefab.m_prefabNodeRef, worldglobalNodeID );

							if ( prefabNodeId.IsDefined() && m_questPrefabsHandler->IsQuestPrefabStreamedIn( prefabNodeId ) )
							{
								++readyCount;
							}
						}

						// when all prefabs are loaded, it's ready
						isReady = ( readyCount == prefabs.Size() );
					}
				}

				phaseInstance = phaseInstance->GetChildById( id );

				if ( !isReady )
				{
					return false;
				}
			}
		}

		// all needed is loaded if we get here
		return true;
	}

	void QuestsSystem::OnPhaseFreezingAreaEnterExit( Bool entered, const world::GlobalNodeID& triggerAreaGlobalNodeID )
	{
		m_phaseFreezer->OnPhaseFreezingAreaEnterExit( entered, triggerAreaGlobalNodeID );
	}

	res::ResourcePath QuestsSystem::HACK_GetCurrentSceneResourcePath() const
	{
		return m_currentSceneResourcePath;
	}

	void QuestsSystem::HACK_SetCurrentSceneResourcePath( res::ResourcePath sceneResourcePath )
	{
		m_currentSceneResourcePath = sceneResourcePath;
	}

	Bool QuestsSystem::HACK_IsPrefabActive( const world::GlobalNodeRef& prefabNodeId ) const
	{
		if ( m_questPrefabsHandler )
		{
			return m_questPrefabsHandler->IsPrefabActive( prefabNodeId );
		}
		return false;
	}

	void QuestsSystem::HandlePointOfNoReturn( game::IGameInstance& gameInstance, Bool rewardsAdded )
	{
		//Set a fact, so we know in quests when PonR is being loaded.
		GetFactsDB().Set( quest::USER_TABLE_ID(), quest::FactID( PONR_FACT_NAME ), 1 );

		//Add info about rewards
		if( rewardsAdded )
		{
			GetFactsDB().Set( quest::USER_TABLE_ID(), quest::FactID( PONR_REWARD_FACT_NAME ), 1 );
		}
	}

	void QuestsSystem::DebugGetPathStringsFromPathHash( const HashedNodePath& hashedNodePath, String& nodeClass, String& idPath, String& namePath ) const
	{
		nodeClass.Clear();
		idPath.Clear();
		namePath.Clear();

		auto fullPathPtr = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		if ( fullPathPtr )
		{
			if ( m_rootInstance )
			{
				red::DynArray< NodeDefinition* > nodes{ red::PoolDebug() };
				m_rootInstance->DebugGetNodesFromNodePath( *fullPathPtr, hashedNodePath.GetExtendedNodeId().GetNodeId(), nodes );

				// class
				if ( !nodes.Empty() )
				{
					auto lastNode = nodes[ nodes.Size() - 1 ];
					if ( lastNode )
					{
						nodeClass = lastNode->GetClass()->GetDescriptiveName();
					}
					else
					{
						nodeClass = "?";
					}
					nodeClass.ReplaceAll( "\n", " " );
				}

				// id path
				for ( const auto id : *fullPathPtr )
				{
					idPath += String::Printf( "%d ", id );
				}
				idPath += ToStringDirect( hashedNodePath.GetExtendedNodeId().GetNodeId() );

				// name path
				if ( !fullPathPtr->Empty() )
				{
					namePath += String::Printf( "[root%d] ", ( *fullPathPtr )[ 0 ] );
				}
				for ( const auto& node : nodes )
				{
					if ( node )
					{
						namePath += String::Printf( "[%hs] ", node->GetFriendlyName().AsChar() );
					}
					else
					{
						namePath += "? ";
					}
				}
				namePath.ReplaceAll( "\n", " " );
			}
		}
	}

	void QuestsSystem::ExecuteSingleNode( const res::ResourcePath& questResourcePath, const dbg::QuestResourceDescriptor::Path& dbgPath, Int32 socketIndex )
	{
		QuestScopedLock lockDbg( m_debuggerLock );

		CreateFullPath( m_nodeToExecute, questResourcePath, dbgPath );
		m_socketIndexToExecute = socketIndex;
	}

	void QuestsSystem::AwakeSignal( const res::ResourcePath& questResourcePath, const dbg::QuestResourceDescriptor::Path& dbgPath, Int32 socketIndex )
	{
		QuestScopedLock lockDbg( m_debuggerLock );

		CreateFullPath( m_nodeToAwakeSignalAt, questResourcePath, dbgPath );
		m_socketIndexToAwakeSignalAt = socketIndex;
	}

	void QuestsSystem::KillSignal( const res::ResourcePath& questResourcePath, const dbg::QuestResourceDescriptor::Path& dbgPath )
	{
		QuestScopedLock lockDbg( m_debuggerLock );

		CreateFullPath( m_nodeToKillSignalAt, questResourcePath, dbgPath );
	}

	void QuestsSystem::UpdateResourceStats()
	{
#ifdef RED_ENABLE_QUEST_STATS
		m_rootInstance->UpdateResourceStats( m_resourceStats );
#endif
	}

	SceneListener* QuestsSystem::GetSceneListener( const HashedNodePath& hashedNodePath ) const
	{
		auto foundFullPath = m_nodePathMap.FindPtr( hashedNodePath.GetFullPathHash() );
		if ( !foundFullPath )
		{
			return nullptr;
		}

		THandle< PhaseInstance > phaseInstance;

		for ( Uint32 i = 0; i < ( *foundFullPath ).Size(); ++i )
		{
			GenericId id = ( *foundFullPath )[ i ];

			if ( i == 0 )
			{
				phaseInstance = m_rootInstance->GetChild( id );
			}
			else
			{
				phaseInstance = phaseInstance->GetChildById( id );
			}

			if ( !phaseInstance )
			{
				// phase not found?
				return nullptr;
			}
		}

		auto node = phaseInstance->FindNodeById( hashedNodePath.GetExtendedNodeId().GetNodeId() );
		RED_FATAL_ASSERT( node, "Missing node %u : %d", hashedNodePath.GetFullPathHash(), hashedNodePath.GetExtendedNodeId().GetNodeId() );
		if ( node->GetClass()->GetName() != RED_NAME_CONSTEXPR( "questSceneNodeDefinition" ) ) // can't use rtti because of quest&scene dependencies
		{
			return nullptr;
		}

		return static_cast< SceneListener* >( phaseInstance->FindEventListener( hashedNodePath.GetExtendedNodeId() ) );
	}

	void QuestsSystem::Tick( job::Builder& builder, const Float timeDelta, const tick::Info& tickInfo )
	{
		NomadplayTick( *this, ( m_waitUntilGameIsRestored ? 1 : 0 ) | ( m_waitUntilPrefabsAreLoaded ? 2 : 0 ) | ( m_waitUntilEntitiesAreReady ? 4 : 0 ), m_gameInstance ); // [nomadplay]
		if ( m_waitUntilGameIsRestored )
		{
			return;
		}

		if ( m_waitUntilPrefabsAreLoaded )
		{
			if ( m_questPrefabsHandler->IsReady() )
			{
				// when all prefabs are loaded
				m_waitUntilPrefabsAreLoaded = false;
			}

			// keep waiting one more tick
			return;
		}

		if ( m_waitUntilEntitiesAreReady )
		{
			auto communitySystem = m_gameInstance->GetSystemPtr< game::ICommunitySystem >();
			auto populationSystem = m_gameInstance->GetSystemPtr< game::IPopulationSystem >();

			if ( communitySystem && populationSystem )
			{
				if ( !communitySystem->HasPendingRequests() && populationSystem->IsReady() )
				{
					// entities are ready, recreate signals and start ticking
					m_waitUntilEntitiesAreReady = false;
					RecreateSignalsAfterLoad();
				}
			}
			else
			{
				m_waitUntilEntitiesAreReady = false;
				RecreateSignalsAfterLoad();
			}

			// keep waiting one more tick
			return;
		}

		PC_SCOPE( QuestsSystem_Tick );

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( m_performanceMonitor )
		{
			m_performanceMonitor->OnTickStarted();
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		TickQuestEntityCache();
		TickCleanup();
		TickQuestPhaseFreezer();
		TickQuestPrefabsHandler();
		TickPendingQuests();
		TickPendingEvents();

#ifdef RED_ENABLE_QUEST_DEBUGGER
		TickDebuggerCommands();
		TickDebuggerPostponedRunRequests();
#endif

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( m_performanceMonitor )
		{
			m_performanceMonitor->OnTickFinished();
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		TickScenes( builder, timeDelta, tickInfo );

#ifdef RED_ENABLE_QUEST_DEBUGGER

		builder.DispatchJob( "QuestDebugger_Update", [this]( const job::RunContext& depCtx )
		{
			if ( m_questDebugger )
			{
				m_questDebugger->Update();
			}
		} );
#endif

#ifdef RED_ENABLE_FDB_DEBUGGER

		builder.DispatchJob( "FdbDebugger_Update", [this]( const job::RunContext& depCtx )
		{
			if ( m_fdbDebugger )
			{
				m_fdbDebugger->Update();
			}
		} );
#endif

	}

	void QuestsSystem::TickQuestPhaseFreezer()
	{
		m_phaseFreezer->OnTick();
	}

	void QuestsSystem::TickQuestPrefabsHandler()
	{
#if !defined( RED_CONFIGURATION_FINAL )	//see QuestsSystem::Initialize()
		if ( m_questPrefabsHandler )
#endif
		{
			m_questPrefabsHandler->Update();
		}
	}

	void QuestsSystem::TickCleanup()
	{
		DeletePostponedListeners();
	}

	void QuestsSystem::TickPendingQuests()
	{
		PC_SCOPE( QuestsSystem_TickPendingQuests );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist");

		QuestScopedLock lockQ( m_questsToRunAndStopLock );

		if ( !m_questsToRun.Empty() )
		{
			QuestScopedLock lockP( m_phaseLock );

			while ( !m_questsToRun.Empty() )
			{
				THandle< QuestResource > questResource = m_questsToRun.Front();
				m_questsToRun.RemoveAt( 0 );

				const Uint64 questFilenameHash = questResource->GetPath().GetHash();
				GAME_ASSERT( !m_activeQuests.Exist( questFilenameHash ), Quests, "Quest is already active" );

				ExecutionContext context = CreateExecutionContext( ExecutionActionType::Execution, START_OPERATION_INDEX );

				m_activeQuests.PushBack( questFilenameHash );
				m_rootInstance->Run( context, questResource );

				UpdateResourceStats();

				GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );

#ifdef RED_ENABLE_QUEST_DEBUG_LOG
				DebugAddLog(	String::Printf( "Started new quest [%hs] [%X], save version: %d",
								questResource->GetPath().ToDebugString(),
								questResource->GetPath().GetHash(),
								static_cast< Int32 >( save::SaveVersion::Current ) ) );
#endif
			}
		}

		if ( !m_questsToStopAndRemove.Empty() )
		{
			QuestScopedLock lockP( m_phaseLock );

			while ( !m_questsToStopAndRemove.Empty() )
			{
				THandle< QuestResource > questResource = m_questsToStopAndRemove.Front();
				m_questsToStopAndRemove.RemoveAt( 0 );

				const Uint64 questFilenameHash = questResource->GetPath().GetHash();

				const Int32 childIndex = m_rootInstance->FindIndexByQuestResource( questResource.Get() );
				const auto activeQuestsIt = std::find( m_activeQuests.Begin(), m_activeQuests.End(), questFilenameHash );

				FullPath fullPath{ red::PoolFrame() };
				if ( childIndex != -1
					 && activeQuestsIt != m_activeQuests.End()
					 && m_rootInstance->TryToGetChildFullPath( childIndex, fullPath ) )
				{
					RemoveNodePathMapping( fullPath );

					ExecutionContext context = CreateExecutionContext( ExecutionActionType::StopAndRemove, INVALID_OPERATION_INDEX );
					m_rootInstance->Stop( childIndex, context, SignalKillingReason::Shutdown );
					m_activeQuests.Remove( activeQuestsIt );
					UpdateResourceStats();
				}
			}
		}
	}

	void QuestsSystem::TickPendingEvents()
	{
		PC_SCOPE( QuestsSystem_TickPendingEvents );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist");

		red::DynArray< Uint32 > eventsToProcess{ red::PoolFrame() };
		{
			RED_SCOPE_LOCK( m_eventsToProcessLock );
			for ( auto eventIndex : m_eventsToProcess )
			{
				red::alg::PushBackUnique( eventsToProcess, eventIndex );
			}
			m_eventsToProcess.Clear();
		}

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( m_performanceMonitor )
		{
			m_performanceMonitor->SetResumedNodesCount( eventsToProcess.Size() );
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		m_eventsToSkip.Clear(); // this will be filled eventually inside Resume() below
		QuestScopedLock lockP( m_phaseLock );
		for ( Uint32 i = 0; i < eventsToProcess.Size(); ++i )
		{
			const auto eventIndex = eventsToProcess[ i ];

			if ( m_eventsToSkip.Exist( eventIndex ) || !m_eventManager->EventExists( eventIndex ) )
			{
				// event may not exist at this point - for example a case of two identical realtime conditions in a phase:
				// - both conditions are met and events are queued in m_eventsToProcess
				// - signal is resumed in the next tick from first pause and leaves the phase, the second listener is removed in PhaseInstance::OnRemove
				// - one more signal is supposed to be resumed from the second pause, but the listener doesn't exist anymore
				continue;
			}
			const HashedNodePath& nodePath = m_eventManager->GetNodePathByEventIndex( eventIndex );

			auto foundFullPath = m_nodePathMap.FindPtr( nodePath.GetFullPathHash() );
			GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::Execution, i );
			OutputSocketArray dummy;

			dd::SetCrashData( context.GetActionType(), foundFullPath, nodePath.GetExtendedNodeId().GetNodeId() );
			m_rootInstance->Resume( context, *foundFullPath, nodePath.GetExtendedNodeId().GetNodeId(), UNDEFINED_SOCKET_INDEX, dummy );
			dd::ClearCrashData();

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}
		m_eventsToSkip.Clear(); 
	}

	void QuestsSystem::TickScenes( job::Builder& builder, const Float scaledEngineTimeDelta, const tick::Info& tickInfo )
	{
		PC_SCOPE( QuestsSystem_TickScenes );

		game::ISceneSystem& sceneSystem = m_gameInstance->GetSystem< game::ISceneSystem >();

		if ( !IsPaused() )
		{
			m_sceneDataHelper->Tick( *this, sceneSystem );
		}

		sceneSystem.Tick( builder, scaledEngineTimeDelta, tickInfo );

		builder.DispatchJob< job::Fence::None >( "PhantomEntitySystem", [ this,scaledEngineTimeDelta, &tickInfo ]( const job::RunContext& depCtx )
		{
			game::IPhantomEntitySystem& phantomSystem = m_gameInstance->GetSystem< game::IPhantomEntitySystem >();
			phantomSystem.Tick( scaledEngineTimeDelta, tickInfo );
		} );

		builder.DispatchJob <job::Fence::None > ( "ObjectCarrySystem", [ this,scaledEngineTimeDelta, &tickInfo ]( const job::RunContext& depCtx )
		{
			game::IObjectCarrySystem& carrySystem = m_gameInstance->GetSystem< game::IObjectCarrySystem >();
			carrySystem.Tick( scaledEngineTimeDelta, tickInfo );
		} );

		builder.DispatchFenceExplicitly();
	}

	void QuestsSystem::TickQuestEntityCache()
	{
		PC_SCOPE( QuestsSystem_QuestEntityCache );

		m_questEntityCache->Tick();
	}

#ifdef RED_ENABLE_QUEST_DEBUGGER
	void QuestsSystem::TickDebuggerCommands()
	{
		PC_SCOPE( QuestsSystem_TickDebuggerCommands );

		GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist");

		FullPath nodeToExecute{ red::PoolFrame() }, nodeToAwakeSignalAt{ red::PoolFrame() }, nodeToKillSignalAt{ red::PoolFrame() };
		Int32 socketIndexToExecute = UNDEFINED_SOCKET_INDEX, socketIndexToAwakeSignalAt = UNDEFINED_SOCKET_INDEX;

		{
			QuestScopedLock lockDbg( m_debuggerLock );

			nodeToExecute					= std::move( m_nodeToExecute );
			socketIndexToExecute			= m_socketIndexToExecute;
			nodeToAwakeSignalAt				= std::move( m_nodeToAwakeSignalAt );
			socketIndexToAwakeSignalAt		= m_socketIndexToAwakeSignalAt;
			nodeToKillSignalAt				= std::move( m_nodeToKillSignalAt );

			m_socketIndexToExecute			= UNDEFINED_SOCKET_INDEX;
			m_socketIndexToAwakeSignalAt	= UNDEFINED_SOCKET_INDEX;
		}

		if ( !nodeToExecute.Empty() )
		{
			GAME_ASSERT( nodeToExecute.Size() >= 2, Quests, "Node path should consist of at least 2 elements" );
			NodeId nodeId = nodeToExecute.PopBack();

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::DebugCommandExecuteSingleNode, DEBUG_OPERATION_INDEX );

			{
				QuestScopedLock lockP( m_phaseLock );
				OutputSocketArray dummy;
				m_rootInstance->Resume( context, nodeToExecute, nodeId, socketIndexToExecute, dummy );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

		if ( !nodeToAwakeSignalAt.Empty() )
		{
			GAME_ASSERT( nodeToAwakeSignalAt.Size() >= 2, Quests, "Node path should consist of at least 2 elements" );
			NodeId nodeId = nodeToAwakeSignalAt.PopBack();

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::DebugCommandAwake, DEBUG_OPERATION_INDEX );

			{
				QuestScopedLock lockP( m_phaseLock );
				OutputSocketArray dummy;
				m_rootInstance->Resume( context, nodeToAwakeSignalAt, nodeId, socketIndexToAwakeSignalAt, dummy );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

		if ( !nodeToKillSignalAt.Empty() )
		{
			GAME_ASSERT( nodeToKillSignalAt.Size() >= 2, Quests, "Node path should consist of at least 2 elements" );
			NodeId nodeId = nodeToKillSignalAt.PopBack();

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::DebugCommandKill, DEBUG_OPERATION_INDEX );

			{
				QuestScopedLock lockP( m_phaseLock );
				m_rootInstance->KillSignal( context, nodeToKillSignalAt, nodeId );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}
	}

	// run requests are postponed when we want to run from node inside not activated phase which requires some prefabs
	// it's used specifically for debugger but might be useful for other purposes
	// no need to make it thread-safe atm, m_debuggerRunRequests is modified and accessed in quest system tick
	void QuestsSystem::TickDebuggerPostponedRunRequests()
	{
		if ( !m_questPrefabsHandler )
		{
			return;
		}

		{
			QuestScopedLock lockP( m_phaseLock );

			// move (in Resume below, we can add requests if prefabs are not loaded)
			auto debuggerRunRequests = std::move( m_debuggerRunRequests );

			for ( const auto& request : debuggerRunRequests )
			{
				const auto* foundFullPath = m_nodePathMap.FindPtr( request.GetPathHash() );
				GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );
	
				ExecutionContext context = CreateExecutionContext( request.GetActionType(), DEBUG_OPERATION_INDEX );
				OutputSocketArray dummy;

				dd::SetCrashData( context.GetActionType(), foundFullPath, request.GetNodeId() );
				m_rootInstance->Resume( context, *foundFullPath, request.GetNodeId(), request.GetInputSocketIndex(), dummy );
				dd::ClearCrashData();

				GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
			}
		}
	}
#endif //RED_ENABLE_QUEST_DEBUGGER

	Bool QuestsSystem::Save( save::IStream& saveStream )
	{
		SAVE_NODE( QueuedData );

		{
			red::DynArray< Uint64 > questsToRun{ job::PoolJobScope() };
			CollectQuestsToRun( questsToRun );

			ArraySerializer::Serialize( saveStream, questsToRun );
		}

		{
			RED_SCOPE_LOCK( m_eventsToProcessLock );

			Uint32 count = m_eventsToProcess.Size();
			saveStream << count;
			saveStream.Serialize( m_eventsToProcess.Data(), m_eventsToProcess.DataSize() );
		}


		return true;
	}

	Bool QuestsSystem::Load( save::IStream& loadStream )
	{
		Bool result = true;
		Bool quickExit = false;
		LOAD_REQUIRED_NODE( QueuedData, result, quickExit );
		if ( quickExit )
		{
			return result;
		}

		{
			red::DynArray< Uint64 > questsToRun{ job::PoolJobScope() };
			if( loadStream.GetSaveVersion() < save::SaveVersion::Quest_StringLess )
			{
				StringArray< Uint16 > saveableQuestsToRun{ job::PoolJobScope() };
				saveableQuestsToRun.Load( loadStream );
				for( Uint32 i = 0; i < saveableQuestsToRun.GetObjectCount(); ++i )
				{
					red::String savableQuest;
					saveableQuestsToRun.GetObject( i, savableQuest );
					questsToRun.PushBack( res::ResourcePath::Build( savableQuest ).GetHash() );
				}
			}
			else
			{
				ArraySerializer::Serialize( loadStream, questsToRun );
			}

			for ( const Uint64& questHash : questsToRun )
			{
				res::ResourcePath questResPath = res::ResourcePath::Build( questHash );
				GAME_ASSERT( questResPath.IsValid(), Quests, "Invalid resource path in QuestsSystem::Load [%hs]", questResPath.ToDebugString() );
				RunAsync( questResPath );
			}
		}

		{
			red::DynArray< Uint32 > eventsToProcess{ job::PoolJobScope() };

			Uint32 count = 0;
			loadStream << count;
			eventsToProcess.Resize( count );
			loadStream.Serialize( eventsToProcess.Data(), eventsToProcess.DataSize() );

			{
				RED_SCOPE_LOCK( m_eventsToProcessLock );
				for ( auto eventIndex : eventsToProcess )
				{
					m_eventsToProcess.PushBack( eventIndex );
				}
			}
		}

		return true;
	}

	ExecutionContext QuestsSystem::CreateExecutionContext( ExecutionActionType actionType, Int32 operationIndex, Int32 eventIndex /*= INVALID_EVENT_INDEX*/ )
	{
		return ExecutionContext(	actionType,
									operationIndex,
									eventIndex,
									*m_eventManager.Get(),
									m_activeQuests,
									m_activeInstances,
									m_nodePathMap,
									*m_gameInstance
#ifdef RED_ENABLE_QUEST_DEBUGGER
								    , m_questDebugger.Get()
#endif

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
									, m_performanceMonitor.Get()
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR
								);
	}

	void QuestsSystem::DeletePostponedListeners()
	{
		// filled in quest system tick, no need to synchronize
		m_listenerWrappersToDelete.Clear();
	}

	void QuestsSystem::RemoveNodePathMapping( const FullPath& fullPath )
	{
		for ( auto it = m_nodePathMap.Begin(); it != m_nodePathMap.End(); )
		{
			if ( it.Value()[0] == fullPath[0] )
			{
				auto result = m_nodePathMap.Remove( it );
				RED_FATAL_ASSERT( result.IsSuccessful(), "Error while removing node path mapping" );
				it = result.Iterator();
			}
		}
	}

	// similar to QuestsSystem::TickPendingQuests()
	Bool QuestsSystem::RecreateQuestInstancesAfterLoad()
	{
		for ( Uint32 i = 0; i < m_activeQuests.Size(); ++i )
		{
			const Uint64 questPathHash = m_activeQuests[ i ];

			res::ResourcePath resPath = res::ResourcePath::Build( questPathHash );
			GAME_DATA_ERROR( resPath.IsValid(), Quests, "Quest resource %s not found", resPath.ToDebugString() );

			auto questResource = Cast< quest::QuestResource >( m_resourceLoader->PreloadResourceSync( resPath ) );
			GAME_DATA_ERROR( questResource, Quests, "Quest resource %s not found", resPath.ToDebugString() );
			if ( !questResource )
			{
				return false;
			}

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::Loading, INVALID_OPERATION_INDEX );
			GAME_ASSERT( m_rootInstance, Quests, "Root instance does not exist");
			if ( !m_rootInstance->RecreateQuestInstanceAfterLoad( context, questResource ) )
			{
				GAME_DATA_ERROR( false, Quests, "RecreateQuestInstanceAfterLoad failed (changed quest structure?)" );
				// PMTODO
				// failed to recreate one of the signals, probably because of changed quest structure
				// are we going to return false here in final game? (this should never happen if there are deletion mark nodes)
			}
			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

		return true;
	}

	void QuestsSystem::RecreateNodePathMapAfterLoad()
	{
		m_nodePathMap.Reserve( m_activeInstances.GetObjectCount() );

		FullPath fullpath{ game::PoolGMPL_QuestDynamic() };
		for ( Uint32 i = 0; i < m_activeInstances.GetObjectCount(); ++i )
		{
			m_activeInstances.GetObject( i, fullpath );

			if ( fullpath.Size() == 1 )
			{
				// it should be already added by RecreateQuestInstancesAfterLoad
				continue;
			}
			auto fullPathHash = HashedNodePath::CreatePathHash( fullpath );
			GAME_ASSERT( !m_nodePathMap.KeyExist( fullPathHash ), Quests, "Key alread exists" );

			m_nodePathMap.Insert( fullPathHash, std::move( fullpath ) );
		}
	}

	// similar to QuestsSystem::TickPendingEvents()
	Bool QuestsSystem::RecreatePhaseInstanceTreeAfterLoad()
	{
		red::SortedArray< PathDescriptor > activeNodePathDescriptors{ game::PoolGMPL_QuestDynamic() };
		activeNodePathDescriptors.Reserve( m_eventManager->GetEventCount() );

		// fill activeNodePathDescriptors with unique, sorted values
		for ( Uint32 i = 0; i < m_eventManager->GetEventCount(); ++i )
		{
			const Event& event = m_eventManager->GetEventAt( i );
			if ( event.IsEmpty() )
			{
				continue;
			}

			const HashedNodePath& nodePath = event.GetNodePath();
			const FullPath* foundFullPath = m_nodePathMap.FindPtr( nodePath.GetFullPathHash() );
			GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

			PathDescriptor pathElement;
			pathElement.m_fullPath.Reserve( foundFullPath->Size() + 1 );
			pathElement.m_fullPath.PushBack( *foundFullPath );
			pathElement.m_fullPath.PushBack( nodePath.GetExtendedNodeId().GetNodeId() );

			activeNodePathDescriptors.InsertUnique( std::move( pathElement ) );
		}

		//////////////////////////////////////////////////////////////////////
		//
		//
		//
#if 0
		RED_LOG( ">>>--------------------------------------------" );
		RED_LOG( ">>> %d %d", activeNodePathDescriptors.Size(), m_eventManager->GetEventCount() );
		for ( const auto& path : activeNodePathDescriptors )
		{
			String line;
			for ( const auto& nodeId : path.m_fullPath )
			{
				if ( !line.Empty() )
				{
					line += " ";
				}
				line += ToStringDirect( nodeId );
			}
			RED_LOG( "%hs", line.AsChar() );
		}
		RED_LOG( "<<<--------------------------------------------" );
#endif
		//
		//
		//
		/////////////////////////////////////////////////////////////////////////

		{
			red::DynArray< res::ResourcePath > questResourcePaths{ game::PoolGMPL_QuestDynamic() };
			for ( Uint32 i = 0; i < m_activeQuests.Size(); ++i )
			{
				const Uint64 questPathHash = m_activeQuests[ i ];
				questResourcePaths.PushBack( res::ResourcePath::Build( questPathHash ) );
			}

			if ( !questResourcePaths.Empty() )
			{
				m_resourceLoader->LoadLimitedGraphSync( questResourcePaths[ 0 ], activeNodePathDescriptors );
			}
		}

		//for ( const auto& path : activeNodePathDescriptors )
		for ( Uint32 i = 0; i < activeNodePathDescriptors.Size(); ++i )
		{
			auto& pathDesc = activeNodePathDescriptors[ i ];

			NodeId lastNodeId = pathDesc.m_fullPath.PopBack();

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::Loading, INVALID_OPERATION_INDEX );
			if ( !m_rootInstance->RecreatePhaseInstanceTreeAfterLoad( context, pathDesc.m_fullPath, lastNodeId, CName::NONE() ) )
			{
				GAME_DATA_ERROR( false, Quests, "RecreatePhaseInstanceTreeAfterLoad failed (changed quest structure?)" );
				// PMTODO
				// failed to recreate one of the signals, probably because of changed quest structure
				// are we going to return false here in final game? (this should never happen if there are deletion mark nodes)
			}

			m_debugChangedNodeCount += context.DebugGetChangedNodeCount();
			m_debugMissingNodeCount += context.DebugGetMissingNodeCount();
			m_debugMissingPhaseResourceCount += context.DebugGetMissingPhaseResourceCount();

			const auto& loadReportText = context.DebugGetLoadReportText();
			if ( !loadReportText.Empty() )
			{
				m_debugWarningLines.PushBack( loadReportText );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

		return true;
	}

	// similar to QuestsSystem::TickPendingEvents()
	Bool QuestsSystem::RecreateSignalsAfterLoad()
	{
#define DUPLICATE_DETECTION_ENABLED 0

#if DUPLICATE_DETECTION_ENABLED
		struct DupData
		{
			red::DynArray< Uint32 > m_indices{ red::PoolDebug() };
		};
		red::HashMap< Uint64, DupData > duplicatesMap{ red::PoolDebug() };
#endif // DUPLICATE_DETECTION_ENABLED

		for ( Uint32 i = 0; i < m_eventManager->GetEventCount(); ++i )
		{
			const Event& event = m_eventManager->GetEventAt( i );
			if ( event.IsEmpty() )
			{
				continue;
			}
			const HashedNodePath& nodePath = event.GetNodePath();

#if DUPLICATE_DETECTION_ENABLED
			{
				Uint64 hash64 = nodePath.AsUint64();
				auto itt = duplicatesMap.Find( hash64 );
				if ( itt != duplicatesMap.End() )
				{
					itt.Value().m_indices.PushBack( i );
				}
				else
				{
					DupData data;
					data.m_indices.PushBack( i );
					duplicatesMap.Insert( hash64, data );
				}
			}
#endif // DUPLICATE_DETECTION_ENABLED

			auto* foundFullPath = m_nodePathMap.FindPtr( nodePath.GetFullPathHash() );
			GAME_ASSERT( foundFullPath, Quests, "Full node path not found" );

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::Loading, INVALID_OPERATION_INDEX, i );

			dd::SetCrashData( context.GetActionType(), foundFullPath, nodePath.GetExtendedNodeId().GetNodeId() );
			if ( !m_rootInstance->RecreateSignalsAfterLoad( context, *foundFullPath, nodePath.GetExtendedNodeId().GetNodeId() ) )
			{
				GAME_DATA_ERROR( false, Quests, "RecreateSignalsAfterLoad failed (changed quest structure?)" );
				// PMTODO
				// failed to recreate one of the signals, probably because of changed quest structure
				// are we going to return false here in final game? (this should never happen if there are deletion mark nodes)
			}
			dd::ClearCrashData();

			m_debugFailedToLoadSceneInstancesCount += context.DebugGetFailedToLoadSceneInstancesCount();
			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

#if DUPLICATE_DETECTION_ENABLED
		{
			RED_LOG( "------------------------ %d", duplicatesMap.Size() );
			for ( const auto& element : duplicatesMap )
			{
				if ( element.Value().m_indices.Size() > 1 )
				{
					auto nodePath = HashedNodePath( element.Key(), true );
					auto* foundFullPath = m_nodePathMap.FindPtr( nodePath.GetFullPathHash() );

					String idStr( 50U );
					for ( const auto id : *foundFullPath )
					{
						if ( !idStr.Empty() )
							idStr += "-";
						idStr += ToStringDirect( id );
					}
					idStr += String::Printf( "=%d", nodePath.GetExtendedNodeId().GetNodeId() );

					String indicesStr( 50U );
					for ( auto idx : element.Value().m_indices )
					{
						if ( !indicesStr.Empty() )
							indicesStr += " ";
						indicesStr += ToStringDirect( idx );
					}

					RED_LOG( "$$$@@@ %dx   [%hs]   %hs", element.Value().m_indices.Size(), indicesStr.AsChar(), idStr.AsChar() );
				}
			}
		}
#endif // DUPLICATE_DETECTION_ENABLED

		if ( m_debugChangedNodeCount > 0 || m_debugMissingNodeCount > 0 || m_debugMissingPhaseResourceCount > 0 || m_debugFailedToLoadSceneInstancesCount > 0 )
		{
			m_debugWarningText = String::Printf( "LOADED SAVE IS OBSOLETE AND DOESN'T MATCH CURR QUEST STRUCTURE. NUM OF CHANGED NODES/MISSING NODES/MISSING PHASES/MISSING SCENES WHERE SIGNAL COULD NOT BE RESTORED: %d/%d/%d/%d\nEXECUTE 'Quest.ShowLoadReport()' COMMAND TO DISPLAY MISSING NODES",
												 m_debugChangedNodeCount,
												 m_debugMissingNodeCount,
												 m_debugMissingPhaseResourceCount,
												 m_debugFailedToLoadSceneInstancesCount );
		}

		return true;
	}

	void QuestsSystem::RecreateLostSignals( Bool onLoadGame )
	{
#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( onLoadGame )
		{
			if ( m_performanceMonitor )
			{
				m_performanceMonitor->OnTickStarted();
			}
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		auto& recoveryData = m_debugLogManager->GetRecoveryData();

		for ( Int32 i = recoveryData.Size() - 1; i >= 0; --i )
		{
			const auto& data = recoveryData[ i ];
			if ( data.m_fullPath.Empty() ) // just in case
			{
				continue;
			}

			//////////////////////////////////////
			//// TEMPORARY, just for tests
			/*
			{
				static red::DynArray< NodeId > arr1{ game::PoolGMPL_Quest() };
				if ( arr1.Empty() )
				{
					arr1 = { 0, 2, 82, 3, 19, 2, 7, 4, 8, 12, 4, 11 };
				}
				static red::DynArray< NodeId > arr2{ game::PoolGMPL_Quest() };
				if ( arr2.Empty() )
				{
					arr2 = { 0, 2, 82, 3, 19, 18, 0, 6, 3, 43, 49, 9 };
				}

				if ( data.m_fullPath != arr1 && data.m_fullPath != arr2 )
				{
					//continue;
				}
			}
			*/
			////
			//////////////////////////////////////

			auto fullPath = data.m_fullPath; // copy
			NodeId nodeId = fullPath.PopBack();

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::ExecuteSingleNode, INVALID_OPERATION_INDEX );
			if ( m_rootInstance->RecreateLostSignals( context, fullPath, nodeId, data.m_forceWithNoInput, data.m_additionaNodeToRun ) )
			{
				// and remove from debug log
				m_debugLogManager->MarkEntryAsRecovered( data.m_fullPath );

				// remove entry
				recoveryData.RemoveAt( i );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( onLoadGame )
		{
			if ( m_performanceMonitor )
			{
				m_performanceMonitor->OnTickFinished();
			}
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR
	}

	void QuestsSystem::RecreateLostStsSignals( Bool onLoadGame )
	{
#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( onLoadGame )
		{
			if ( m_performanceMonitor )
			{
				m_performanceMonitor->OnTickStarted();
			}
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		auto& recoveryData = m_stsRecoveryHelper->GetRecoveryData();

		for ( Int32 i = recoveryData.Size() - 1; i >= 0; --i )
		{
			const auto& data = recoveryData[ i ];
			if ( data.m_fullPath.Empty() ) // just in case
			{
				continue;
			}

			ExecutionContext context = CreateExecutionContext( ExecutionActionType::Execution, INVALID_OPERATION_INDEX );
			if ( m_rootInstance->RecreateLostStsSignals( context, data.m_fullPath ) )
			{
				// and remove from debug log
				auto line = String::Printf( "STS restored {%hs}", RuntimeHelper::GetFullPathString( data.m_fullPath ).AsChar() );
				m_debugLogManager->AddLogLine( line );

				// remove entry
				recoveryData.RemoveAt( i );
			}

			GAME_ASSERT( context.IsPhaseStackEmpty(), Quests, "Phase stack not empty" );
		}

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( onLoadGame )
		{
			if ( m_performanceMonitor )
			{
				m_performanceMonitor->OnTickFinished();
			}
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR
	}

	void QuestsSystem::OnRenderDebugShowNodePaths( rend::FragmentBuilder& builder, Int32 x, Int32& y ) const
	{
		const Int32 LINE_HEIGHT = 15;

		builder.SetColor( Color::YELLOW() );
		builder.AddFlatTextf( x, y, "NODE PATH MAP" );
		y += LINE_HEIGHT;

		builder.SetColor( Color::WHITE() );
		for ( auto it = m_nodePathMap.Begin(); it != m_nodePathMap.End(); ++it )
		{
			builder.AddFlatTextf( x, y, "[%u]", it.Key() );

			String line;
			for ( auto id : it.Value() )
			{
				line += String::Printf( "%d ", id );
			}
			builder.AddFlatTextf( x + 150, y, "%s", line.AsChar() );
			y += LINE_HEIGHT;
		}
	}

	void QuestsSystem::OnRenderDebugShowActiveQuestAndInstances( rend::FragmentBuilder& builder, Int32 x, Int32& y )
	{
		const Int32 LINE_HEIGHT = 15;

		{
			Float seconds = 0;
			Float percent = 0;

			GetGameplayTimerData( seconds, percent );
			builder.SetColor( Color::GREEN() );
			builder.AddFlatTextf( x, y, "TIMER: %.2f sec, %.2f%%", seconds, percent );
			y += LINE_HEIGHT;
		}

		builder.SetColor( Color::YELLOW() );
		builder.AddFlatTextf( x, y, "ACTIVE QUESTS" );
		y += LINE_HEIGHT;

		builder.SetColor( Color::WHITE() );
		for ( Uint32 i = 0; i < m_activeQuests.Size(); ++i )
		{
			Uint64 questPathHash = m_activeQuests[ i ];

			builder.AddFlatTextf( x, y, "[%s]", res::ResourcePath::Build( questPathHash ).ToDebugString() );
			y += LINE_HEIGHT;
		}

		y += LINE_HEIGHT;

		builder.SetColor( Color::YELLOW() );
		builder.AddFlatTextf( x, y, "ACTIVE INSTANCES" );
		y += LINE_HEIGHT;

		builder.SetColor( Color::WHITE() );
		FullPath fullPath{ red::PoolFrame() };
		for ( Uint32 i = 0; i < m_activeInstances.GetObjectCount(); ++i )
		{
			m_activeInstances.GetObject( i, fullPath );

			String str;
			for ( Uint32 i = 0; i < fullPath.Size(); ++i )
			{
				str += String::Printf( "%d", fullPath[ i ] );
				if ( i < fullPath.Size() - 1 )
				{
					str += " ";
				}
			}
			builder.AddFlatTextf( x, y, "[%s]", str.AsChar() );
			y += LINE_HEIGHT;
		}
	}

	const red::DynArray< Uint64 >& QuestsSystem::ftActiveQuests() const
	{
		return m_activeQuests;
	}

	const VectorArray< FullPath::value_type, Uint16 >& QuestsSystem::ftActiveInstances() const
	{
		return m_activeInstances;
	}

	// RUNTIME QUEST INFORMATION
	const THandle< RootInstance >& QuestsSystem::ftGetRootInstance() const
	{
		return m_rootInstance;
	}

	const EventManager* QuestsSystem::ftGetEventManager() const
	{
		return m_eventManager.Get();
	}

	const FullPath* QuestsSystem::ftMapPath(const HashedNodePath& hashed) const
	{
		return m_nodePathMap.FindPtr(hashed.GetFullPathHash());
	}

	void QuestsSystem::funcGetFact( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		FINISH_PARAMETERS;
		RETURN_INT( 0 );

		if ( !factName )
		{
			SCRIPT_ERROR( stack, "GetFact() requires fact name." );
			return;
		}

		RETURN_INT( GetFactsDB().Get( USER_TABLE_ID(), FactID( factName ) ) );
	}

	void QuestsSystem::funcGetFactStr( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( String, factName, String::EMPTY() );
		FINISH_PARAMETERS;
		RETURN_INT( 0 );

		if ( factName.Empty() )
		{
			SCRIPT_ERROR( stack, "GetFactStr() requires fact name." );
			return;
		}

		RETURN_INT( GetFactsDB().Get( USER_TABLE_ID(), FactID( factName ) ) );
	}

	void QuestsSystem::funcSetFact(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		GET_PARAMETER( Int32, value, 0 );
		FINISH_PARAMETERS;

		if ( !factName )
		{
			SCRIPT_ERROR( stack, "SetFact() requires fact name." );
			return;
		}
		if ( String( factName.AsChar() ).Contains( ";" ) )
		{
			SCRIPT_ERROR( stack, "SetFact(): fact name cannot contain ';' characters" );
			return;
		}

		GetFactsDB().Set( USER_TABLE_ID(), FactID( factName ), value );
	}

	void QuestsSystem::funcSetFactStr( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( String, factName, String::EMPTY() );
		GET_PARAMETER( Int32, value, 0 );
		FINISH_PARAMETERS;

		if ( factName.Empty() )
		{
			SCRIPT_ERROR( stack, "SetFactStr() requires fact name." );
			return;
		}
		if ( factName.Contains( ";" ) )
		{
			SCRIPT_ERROR( stack, "SetFact(): fact name cannot contain ';' characters" );
			return;
		}


		GetFactsDB().Set( USER_TABLE_ID(), FactID( factName ), value );
	}

	void QuestsSystem::funcRegisterListener(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		GET_PARAMETER( THandle< IScriptable >, object, nullptr );
		GET_PARAMETER( CName, funcName, CName::NONE() );
		FINISH_PARAMETERS;

		if ( !object || !funcName )
		{
			SCRIPT_ERROR( stack, "RegisterListener() requires object and a function name." );
			return;
		}

		const auto* classType = object->GetClass();
		if ( !classType->IsScriptedClass() )
		{
			SCRIPT_ERROR( stack, "RegisterListener() called with non-scripted object." );
			return;
		}

		const CName generatedFuncName = RED_APPEND_NAME( funcName, ";Int32" );
		const auto* func = classType->FindFunction( generatedFuncName );
		if ( !func )
		{
			func = classType->FindFunction( funcName );
		}

		if ( !func )
		{
			SCRIPT_ERROR( stack, "RegisterListener(): Can't find function '%s' ('%s') in class '%s'.", funcName.AsChar(), generatedFuncName.AsChar(), classType->GetName().AsChar() );
			return;
		}

		if ( func->GetNumParameters() != 1 )
		{
			SCRIPT_ERROR( stack, "RegisterListener(): Can't register callback to function '%s' in class '%s'. Wrong number of parameters.", funcName.AsChar(), classType->GetName().AsChar() );
			return;
		}

		const auto paramType = func->GetParameter( 0 )->GetType();
		if ( paramType->GetName() != RED_NAME_CONSTEXPR( "Int32" ) )
		{
			SCRIPT_ERROR( stack, "RegisterListener(): Can't register callback to function '%s' in class '%s'. Invalid parameter type '%s' (expected: Int32)..", funcName.AsChar(), classType->GetName().AsChar(), paramType->GetName().AsChar() );
			return;
		}

		WeakHandle< IScriptable > weakObject = object;
		const Uint32 id = GetFactsDB().RegisterCallback( USER_TABLE_ID(), FactID( factName ), 
			[ weakObject, func ] ( Int32 v )
			{
				if ( auto object = weakObject.ToHandle() )
				{
					rtti::FunctionContext_ExternalParams callCtx( object.Get(), (void*)&v, nullptr, nullptr, nullptr );
					func->Call( callCtx );
				}
			}
		);

		RETURN_AUTO( id );
	}

	void QuestsSystem::funcUnregisterListener(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		GET_PARAMETER( Uint32, id, 0 );
		FINISH_PARAMETERS;

		if ( !factName || id == 0 )
		{
			SCRIPT_ERROR( stack, "RegisterListener() requires factName and a callback id returned by RegisterListener()." );
			return;
		}

		GetFactsDB().UnregisterCallback( USER_TABLE_ID(), FactID( factName ), id );
	}

	void QuestsSystem::funcRegisterEntity(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		GET_PARAMETER( ent::EntityID, entityId, ent::EntityID() );
		FINISH_PARAMETERS;

		if ( !factName )
		{
			SCRIPT_ERROR( stack, "RegisterListener() requires valid fact name." );
			return;
		}

		if ( !entityId.IsDefined() )
		{
			SCRIPT_ERROR( stack, "Null entity id" );
			return;
		}

		const game::IGameInstance* gameInstance = m_gameInstance;
		const Uint32 id = GetFactsDB().RegisterCallback( USER_TABLE_ID(), FactID( factName ), 
			[ factName, entityId, gameInstance ] ( Int32 v )
			{
				if ( auto entity = gameInstance->GetScene().GetSystemRef< world::RuntimeEntityRegistry >().FindEntity( entityId ) )
				{
					auto evt = ::CreateHandle< game::FactChangedEvent > ();
					evt->m_factName = factName;
					entity->QueueEvent( evt );
				}
			}
		);

		RETURN_AUTO( id );
	}

	void QuestsSystem::funcUnregisterEntity(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, factName, CName::NONE() );
		GET_PARAMETER( Uint32, id, 0 );
		FINISH_PARAMETERS;

		if ( !factName || id == 0 )
		{
			SCRIPT_ERROR( stack, "UnregisterEntity() requires factName and a callback id returned by RegisterEntity()." );
			return;
		}

		GetFactsDB().UnregisterCallback( USER_TABLE_ID(), FactID( factName ), id );
	}

	void QuestsSystem::funcGetGameplayTimerData( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER_REF( Float, seconds, 0 );
		GET_PARAMETER_REF( Float, percent, 0 );
		FINISH_PARAMETERS;

		GetGameplayTimerData( seconds, percent );
	}

	void QuestsSystem::CreateFullPath(FullPath& fullPath, const res::ResourcePath& questResourcePath, const dbg::QuestResourceDescriptor::Path& dbgPath)
	{
		// PMTODO consider unifying quest::FullPath and quest::dbg::QuestResourceDescriptor::Path
		GAME_ASSERT( dbgPath.Size() >= 1, Quests, "Invalid path, there should be at least one element" );

		Int32 questId = -1;
		const auto& activeQuestsIt = std::find( m_activeQuests.Begin(), m_activeQuests.End(), questResourcePath.GetHash() );
		if( activeQuestsIt != m_activeQuests.End() )
		{
			questId = (Int32) std::distance( activeQuestsIt, m_activeQuests.Begin() );
		}
		GAME_ASSERT( questId > -1, Quests, "Invalid quest id %d", questId );

		fullPath.Clear();
		fullPath.Reserve( 1 + dbgPath.Size() );
		fullPath.PushBack( questId );
		fullPath.PushBack( dbgPath );
	}

	void QuestsSystem::CollectQuestsToRun( red::DynArray < Uint64 >& questsToRun )
	{
		questsToRun.Clear();

		QuestScopedLock lockQ( m_questsToRunAndStopLock );

		for ( const auto& questToRunResource : m_questsToRun )
		{
			questsToRun.PushBack( questToRunResource->GetPath().GetHash() );
		}
	}

	void QuestsSystem::GetGameplayTimerData( Float& seconds, Float& percent )
	{
		seconds = 0;
		percent = 0;

		game::DelayID timerID;
		Float currentSeconds = 0;
		Float initialSeconds = 0;

		auto& blackboardSystem = m_gameInstance->GetSystem< game::IBlackboardSystem >();
		if ( GameplayTimerListenerWrapper::ReadTimerIdAndCurrentAndInitialDuration( &blackboardSystem, timerID, currentSeconds, initialSeconds ) )
		{
			if ( timerID.IsValid() )
			{
				seconds = currentSeconds;
				if ( initialSeconds > 0 ) // just in case
				{
					percent = currentSeconds / initialSeconds * 100;
				}
			}
		}
	}

	void QuestsSystem::OnQuestPrefabNotLoaded()
	{
		if ( !m_questPrefabsHandler->IsReady() )
		{
			String notLoadedPrefabsString;

			auto notLoadedPrefabs = m_questPrefabsHandler->DebugGetNotLoadedPrefabs();
			for ( const auto& notLoadedPrefab : notLoadedPrefabs )
			{
				notLoadedPrefabsString += String::Printf( "%hs\n", notLoadedPrefab.ToDebugString().AsChar() );
			}
			GAME_DATA_ERROR( false, Quests, "One or more prefabs could not be loaded most likely because of wrong setup:\n%hs", notLoadedPrefabsString.AsChar() );
			RED_FATAL_ASSERT( false, "One or more prefabs could not be loaded most likely because of wrong setup:\n%hs", notLoadedPrefabsString.AsChar() );
		}
	}

	void QuestsSystem::UpdateCensorshipFacts()
	{
		Uint32 flags = red::GCensorshipSystem.GetCensorFlags();
		auto& factsDB = GetFactsDB();
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_nudity" ),			( flags & CensorshipFlags::Censor_Nudity ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_oversexualized" ),	( flags & CensorshipFlags::Censor_OverSexualised ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_suggestive" ),		( flags & CensorshipFlags::Censor_Suggestive ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_homosexuality" ),		( flags & CensorshipFlags::Censor_Homosexuality ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_gore" ),				( flags & CensorshipFlags::Censor_Gore ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_drugs" ),				( flags & CensorshipFlags::Censor_Drugs ) != 0 );
		factsDB.Set( USER_TABLE_ID(), FactID( "censorship_religion" ),			( flags & CensorshipFlags::Censor_Religion ) != 0 );
	}

} // namespace quest




#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

