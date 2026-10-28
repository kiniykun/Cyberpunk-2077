/*
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "damageSystem.h"
#include "damageSystemSharedState.h"
#include "statsBundle.h"
#include "statsSystem.h"
#include "statModifiers.h"
#include "weaponObject.h"

#include "../gameCore/include/playerManagerInterface.h"
#include "../gameCore/include/player.h"
#include "../gameCore/include/gameInstance.h"
#include "../gameCore/include/gameRttiExtensions.h"
#include "../../../common/redReflection/include/gatheredResource.h"

#ifdef RED_USE_IMGUI
#include "../../imgui/include/redImGui.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../renderData/include/debugFilter.h"

#include "attack.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../worldGame/include/runtimeSystemInspector.h"

#endif
#include "../../gameCore/include/statPoolsSystemInterface.h" // [tkfix]
#include "../../gameTweakDB/include/types/tweakDBStatPoolType.h" // [tkfix]
#include "statsObjectID.h" // [tkfix]
#include <cstdio>
// [tkfix] coreplay hit probes (logging only), defined in scriptableSystemsContainer.cpp; on for ?sandbox / ?coreplay=1
extern volatile int g_coreplayHitProbe;
extern volatile int g_coreplayHitBudget[ 3 ];
extern volatile unsigned long long g_coreplayHitRing[ 8 ];
extern volatile int g_coreplayHitRingPos;
static inline bool CpHitProbeTake( int which ) { return g_coreplayHitProbe && __atomic_sub_fetch( &g_coreplayHitBudget[ which ], 1, __ATOMIC_RELAXED ) >= 0; }


#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif


RTTI_BEGIN_TYPE_IN_NAMESPACE( DamageSystem, game );
	RTTI_PARENT_TYPE( IDamageSystem );
	RTTI_SCRIPT_ALIAS( "DamageSystem" );

	RTTI_NATIVE_FUNCTION( "StartPipeline", funcStartPipeline );
	RTTI_NATIVE_FUNCTION( "StartProjectionPipeline", funcStartProjectionPipeline );
	RTTI_NATIVE_STATIC_FUNCTION( "GetDamageModFromCurve", funcGetDamageModFromCurve );

	RTTI_NATIVE_FUNCTION( "RegisterListener", funcRegisterListener );
	RTTI_NATIVE_FUNCTION( "UnregisterListener", funcUnregisterListener );
	RTTI_NATIVE_FUNCTION( "RegisterSyncListener", funcRegisterSyncListener );
	RTTI_NATIVE_FUNCTION( "UnregisterSyncListener", funcUnregisterSyncListener );
	RTTI_NATIVE_FUNCTION( "ProcessSyncStageCallbacks", funcProcessSyncStageCallbacks );

RTTI_END_TYPE();

#ifdef RED_USE_IMGUI
static rend::DebugFilter DF_Damages( "Damage" );
#endif

namespace game
{

	static res::TStaticResource< CurveSet > s_damageCurves( RED_CONST_RESOURCEPATH( "base\\gameplay\\curves\\damage\\damage.curveset" ), GRF_Startup );

	//-------------------------------------------------------------------------------------------------
	// DamageProcess
	//-------------------------------------------------------------------------------------------------

	DamageSystem::DamageProcess::DamageProcess( DamageSystem* damageSystem )
		: m_cacheData( nullptr )
		, m_damageSystem( damageSystem )
	{
		auto rttiClass = GetRttiSystem().FindClass( RED_NAME_CONSTEXPR_NOREG( "gamedamageCacheData" ) );
		m_cacheData = rttiClass->CreateHandle< damage::CacheData >();
	}

	void DamageSystem::DamageProcess::ProcessPipeline( THandle < events::HitEvent >& hitEvent )
	{
		if( hitEvent && hitEvent->m_attackData )
		{
			if( !hitEvent->m_attackData->m_attackDefinition )
			{
				GAME_DATA_ERROR( false, Gameplay, "Attack missing attack definition, there's a problem with data please contact game designers" );
				return;
			}
			m_damageSystem->ComputeAttack( *hitEvent );

			rtti::CallFunction_UserContext( m_damageSystem, m_damageSystem->GetGameInstance()->GetScriptContext(),
											rtti::RawFunctionName( RED_NAME_CONSTEXPR( "ProcessPipeline;gameHitEventCacheData" ) ), hitEvent, m_cacheData );
		}
	}

	const THandle<damage::CacheData> DamageSystem::DamageProcess::GetCacheData() const
	{
		return m_cacheData;
	}

	void DamageSystem::DamageProcess::ProcessProjectionPipeline( THandle<events::ProjectedHitEvent>& hitEvent )
	{
		if( hitEvent && hitEvent->m_attackData )
		{
			if( !hitEvent->m_attackData->m_attackDefinition )
			{
				GAME_DATA_ERROR( false, Gameplay, "Attack missing attack definition, there's a problem with data please contact game designers" );
				return;
			}
			m_damageSystem->ComputeAttack( *hitEvent );

			rtti::CallFunction_UserContext( m_damageSystem, m_damageSystem->GetGameInstance()->GetScriptContext(),
				rtti::RawFunctionName( RED_NAME_CONSTEXPR( "ProcessProjectionPipeline;gameProjectedHitEventCacheData" ) ), hitEvent, m_cacheData );
		}
	}

	//-------------------------------------------------------------------------------------------------
	// DamageSystem
	//-------------------------------------------------------------------------------------------------


	void DamageSystem::StartPipeline( THandle< events::HitEvent >& hitEvent )
	{
		// [tkfix] coreplay damage probe (logging only): target, position, instigator/source/weapon, target health before/after
		const Bool cpProbe = hitEvent && hitEvent->m_attackData && CpHitProbeTake( 2 );
		THandle< game::Object > cpTarget = cpProbe ? hitEvent->m_target.ToHandle() : THandle< game::Object >();
		auto cpPools = cpProbe ? GetGameInstance()->GetSystemPtr< game::IStatPoolsSystem >() : nullptr;
		const Float cpHpBefore = ( cpTarget && cpPools ) ? cpPools->GetStatPoolValue( game::StatsObjectID( cpTarget->GetEntityID() ), game::data::StatPoolType::Health ) : -1.0f;

		auto damageProcess = DamageProcess( this );
		damageProcess.ProcessPipeline( hitEvent );

		if ( cpProbe )
		{
			const damage::AttackData& ad = *hitEvent->m_attackData;
			THandle< game::Object > inst = ad.m_instigator.ToHandle();
			THandle< game::Object > src = ad.m_source.ToHandle();
			THandle< game::weapon::Object > wpn = ad.m_weapon.ToHandle();
			const Float hpAfter = ( cpTarget && cpPools ) ? cpPools->GetStatPoolValue( game::StatsObjectID( cpTarget->GetEntityID() ), game::data::StatPoolType::Health ) : -1.0f;
			std::fprintf( stderr, "[coreplay] DAMAGE hit: target %s (%s) at (%.2f, %.2f, %.2f) | instigator %s (%s) source %s (%s) weapon %s attack %s | target health %.2f -> %.2f%s\n",
				cpTarget ? cpTarget->GetEntityID().ToDebugString().AsChar() : "none(non-object)", cpTarget ? cpTarget->GetClass()->GetName().AsChar() : "-",
				hitEvent->m_hitPosition.X, hitEvent->m_hitPosition.Y, hitEvent->m_hitPosition.Z,
				inst ? inst->GetEntityID().ToDebugString().AsChar() : "-", inst ? inst->GetClass()->GetName().AsChar() : "-",
				src ? src->GetEntityID().ToDebugString().AsChar() : "-", src ? src->GetClass()->GetName().AsChar() : "-",
				wpn ? wpn->GetItemID().GetTweakDBID().ToString().AsChar() : "-",
				ad.m_attackDefinition ? ad.m_attackDefinition->GetClass()->GetName().AsChar() : "-",
				cpHpBefore, hpAfter, ( cpHpBefore > 0.0f && hpAfter <= 0.0f ) ? " (KILLED/DESTROYED)" : "" );
			if ( cpTarget )
			{
				g_coreplayHitRing[ __atomic_fetch_add( &g_coreplayHitRingPos, 1, __ATOMIC_RELAXED ) & 7 ] = static_cast< unsigned long long >( cpTarget->GetEntityID().GetHash() );
			}
		}

		ProcessListeners( hitEvent, DamageListenerPipelineType::Damage );

		if( GetGameInstance()->GetRuntimeInfo().IsServer() )
		{
			// No need to store any non-player damage events as it currently is only used for displaying player damage
			THandle<game::Object> instigator = hitEvent->m_attackData->m_instigator;
			if( instigator && instigator->IsPlayerControlled() )
			{
				if( auto serverData = GatherServerData( hitEvent, damageProcess.GetCacheData() ) )
				{
					RED_SCOPE_LOCK( m_replicationStateLock );

					auto replicatedState = GetReplicatedState< DamageSystemSharedState >();
					replicatedState->m_serverHitCounter++;
					serverData->m_id = replicatedState->m_serverHitCounter;
					serverData->m_hitTime = GEngine->GetRawEngineTime();

					replicatedState->m_hitHistory.EmplaceBack( std::move( serverData ) );
				}
			}
		}



	#ifdef RED_USE_IMGUI
		auto debugData = CreateHandle< damage::HitDebugData >(); // pool for this handle?
		GatherDebugData( debugData, hitEvent, damageProcess.GetCacheData() );

		{
			red::ScopedLock< red::Mutex > lock( m_debugDataMutex );
			++m_hitCounter;
			debugData->m_id = m_hitCounter;
			m_hitDebugData.EmplaceBack( std::move( debugData ) );
		}
	#endif
	}

	void DamageSystem::StartProjectionPipeline( THandle<events::ProjectedHitEvent>& projectedHitEvent )
	{
		auto damageProcess = DamageProcess( this );
		damageProcess.ProcessProjectionPipeline( projectedHitEvent );

		ProcessListeners( projectedHitEvent, DamageListenerPipelineType::ProjectedDamage );
	}

	THandle< damage::ServerHitData > DamageSystem::GatherServerData( const THandle< events::HitEvent >& hitEvent, const THandle< damage::CacheData >& cacheData ) const
	{
		auto serverData = CreateHandle< damage::ServerHitData >();

		bool result = rtti::CallFunction_UserContext( const_cast< DamageSystem* >( this ), GetGameInstance()->GetScriptContext(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "GatherServerData;gameHitEventCacheDataServerHitData" ) ), hitEvent, cacheData, serverData );
		RED_ASSERT( result, "Unable to call DamageSystem::GatherServerData" );

		// If there was no valid damage returned we don't want to store this
		if( serverData->m_damageInfos.Empty() )
		{
			return nullptr;
		}

		// Precaution - cannot replicate handles to non-replicable entities
		Bool canReplicateDamage = !serverData->m_instigator.Expired() && serverData->m_instigator.ToHandle()->IsReplicable();
		GAME_DATA_ERROR( canReplicateDamage, Gameplay, "DamageSystem::GatherServerData() - trying to replicate damage with invalid (null or non-replicable) source." );

		return canReplicateDamage ? serverData : nullptr;
	}

	void DamageSystem::ProcessClientHit( const THandle< damage::ServerHitData >& serverData ) const
	{
		//auto& targettingSystem = GetGameInstance()->GetSystem< game::ITargetingSystem >();
		bool result = rtti::CallFunction_UserContext( const_cast< DamageSystem* >( this ), GetGameInstance()->GetScriptContext(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "ProcessClientHit;ServerHitData" ) ), serverData );
		RED_ASSERT( result, "Unable to call DamageSystem::ProcessClientHit" );

	}

	void DamageSystem::ProcessClientKill( const THandle< damage::ServerKillData >& serverData ) const
	{
		bool result = rtti::CallFunction_UserContext( const_cast< DamageSystem* >( this ), GetGameInstance()->GetScriptContext(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "ProcessClientKill;ServerKillData" ) ), serverData );
		RED_ASSERT( result, "Unable to call DamageSystem::ProcessClientKill" );

	}

#ifdef RED_USE_IMGUI
	void DamageSystem::GatherDebugData( THandle< damage::HitDebugData >& debugData, const THandle< events::HitEvent >& hitEvent, const THandle< damage::CacheData >& cacheData ) const
	{
		rtti::CallFunction_UserContext( const_cast< DamageSystem* >( this ), GetGameInstance()->GetScriptContext(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "GatherDebugData;gameHitEventCacheDataHitDebugData" ) ), hitEvent, cacheData, debugData );
	}
#endif

	void DamageSystem::funcStartPipeline( CScriptStackFrame & stack, void * result, const rtti::IType* resultType )
	{
		GET_PARAMETER( THandle< events::HitEvent >, hitEvent, THandle< events::HitEvent >() );
		FINISH_PARAMETERS;

		StartPipeline( hitEvent );

		RETURN_VOID();
	}

	void DamageSystem::funcStartProjectionPipeline( CScriptStackFrame & stack, void * result, const rtti::IType * resultType )
	{
		GET_PARAMETER( THandle< events::ProjectedHitEvent >, projectedHitEvent, THandle< events::ProjectedHitEvent >() );
		FINISH_PARAMETERS;

		StartProjectionPipeline( projectedHitEvent );

		RETURN_VOID();
	}


	const rtti::ClassType* DamageSystem::GetReplicatedStateClass() const
	{
		return DamageSystemSharedState::GetStaticClass();
	}

	DamageSystem::DamageSystem()
		: TBaseClass()
		, m_damageListeners( (Uint32) DamageCallbackType::COUNT, PoolGMPL_Hit() )
	{
		m_syncDamageListeners.Grow( (Uint32) DamagePipelineStage::COUNT );
		for ( auto & stageBucket : m_syncDamageListeners )
		{
			for ( Uint32 i = 0u; i < (Uint32) DamageCallbackType::COUNT; ++i )
			{
				stageBucket.EmplaceBack( DamageListenerBuckets::ElementType( PoolGMPL_Hit() ) );
			}
		}
	}

	void DamageSystem::OnInitialize( job::Counter & counter )
	{
		job::Builder builder;
		builder.DispatchJob( "DamageSystem/OnInitializeAsync", [ this ]( const job::RunContext& context )
		{
			InitializeDebugData();
		} );

		counter += builder.ExtractWaitCounter();
	}

	void DamageSystem::OnUninitialize()
	{
	#ifdef RED_USE_IMGUI
		m_damageTablePathInspectorList.Clear();
		m_damagesTableActionList.Clear();
		m_selectedHitDebugData = nullptr;
		m_hitDebugData.Clear();
	#endif
	}

	void DamageSystem::WorldAttached( world::RuntimeScene & scene )
	{
		RED_FATAL_ASSERT( m_damageCallbackData.Empty() );
	}

	void DamageSystem::WorldDetached( world::RuntimeScene & scene )
	{
		{
			RED_SCOPE_LOCK( m_listenerLock );
			for ( auto& listeners : m_damageListeners )
			{
				listeners.Clear();
			}

			for ( auto & stageBucket : m_syncDamageListeners )
			{
				for ( auto & listeners : stageBucket )
				{
					listeners.Clear();
				}
			}
		}

		{
			RED_SCOPE_LOCK( m_callbackDataLock );
			m_damageCallbackData.Clear();
		}
	}

	void DamageSystem::RegisterListener( const THandle< IDamageSystemListener >& listener, const ent::EntityID& registereeID, DamageCallbackType callbackType, DamageListenerPipelineType listenerCallbackPipelineType /*= DamageListenerPipelineType::Damage*/ )
	{
		RegisterListener( listener, m_damageListeners, registereeID, callbackType, listenerCallbackPipelineType );
	}

	void DamageSystem::UnregisterListener( const THandle< IDamageSystemListener >& listener, const ent::EntityID& registereeID, DamageCallbackType callbackType, DamageListenerPipelineType listenerCallbackPipelineType /*= DamageListenerPipelineType::All*/ )
	{
		UnregisterListener( listener, m_damageListeners, registereeID, callbackType, listenerCallbackPipelineType );
	}

	void DamageSystem::RegisterUpdateFunctions( UpdatableSystemRegistrar& processor )
	{
		processor.Register( UpdateTickGroup::FrameBegin, *this, "DamageSystem",
			[ this ]( const UpdateContext& updateContext, job::Builder& builder )
		{
			auto replicatedState = GetReplicatedState< DamageSystemSharedState >();
			if( replicatedState )
			{
				RED_SCOPE_LOCK( m_replicationStateLock );

				auto currentTime = GEngine->GetRawEngineTime();

				// How long to keep hits in history
				const static float HIT_LIFETIME = 1.0f;

				for( int i = replicatedState->m_hitHistory.Size() - 1; i >= 0; i-- )
				{
					if( currentTime - replicatedState->m_hitHistory[ i ]->m_hitTime >= HIT_LIFETIME )
					{
						replicatedState->m_hitHistory.RemoveAt( i );
					}
				}

				// Same but for kill history
				const static float KILL_LIFETIME = 1.0f;

				for( int i = replicatedState->m_killHistory.Size() - 1; i >= 0; i-- )
				{
					if( currentTime - replicatedState->m_killHistory[ i ]->m_killTime >= KILL_LIFETIME )
					{
						replicatedState->m_killHistory.RemoveAt( i );
					}
				}
			}

		} );

		processor.Register( UpdateTickGroup::PostBuckets, *this, "DamageSystem/CallbackUpdate",
							[ this ]( const UpdateContext& updateContext, job::Builder& builder )
		{
			ProcessCallbacks();
		} );
	}

	void DamageSystem::OnPostSnapshotApplied()
	{
		// For clients find all the damage events you haven't processed yet and call client specific processing on them
		if( GetGameInstance()->GetRuntimeInfo().IsClient() )
		{
			const Player* localPlayer = GetGameInstance()->GetSystem< IPlayerManager >().GetLocalPlayer();
			const THandle< game::Object > localPlayerObj = localPlayer ? localPlayer->GetControlledObject() : nullptr;

			if( !localPlayerObj )
			{
				return;
			}

			auto replicatedState = GetReplicatedState< DamageSystemSharedState >();
			RED_ASSERT( replicatedState );

			RED_SCOPE_SHARED_LOCK( m_replicationStateLock );
			Uint32 newHitCounterId = replicatedState->m_ClientProcessedHitCounter;
			for( THandle< damage::ServerHitData > serverHit : replicatedState->m_hitHistory )
			{
				//Filter: only allow clients own hits
				if( localPlayerObj != serverHit->m_instigator.ToHandle() )
				{
					continue;
				}

				//Filter: only process hit events once
				if( replicatedState->m_ClientProcessedHitCounter < serverHit->m_id )
				{
					newHitCounterId = serverHit->m_id;
					ProcessClientHit( serverHit );
				}
			}
			replicatedState->m_ClientProcessedHitCounter = newHitCounterId;

			Uint32 newKillCounterId = replicatedState->m_clientProcessedKillCounter;
			for( THandle< damage::ServerKillData > serverKill : replicatedState->m_killHistory )
			{
				//Filter: only allow clients own kills
				if( localPlayerObj != serverKill->m_killInfo.m_killerEntity.ToHandle() )
				{
					continue;
				}

				//Filter: only process kill events once
				if( replicatedState->m_clientProcessedKillCounter < serverKill->m_id )
				{
					newKillCounterId = serverKill->m_id;
					ProcessClientKill( serverKill );
				}
			}
			replicatedState->m_clientProcessedKillCounter = newKillCounterId;
		}
	}

	void DamageSystem::CacheAttack( game::damage::AttackData& attackData )
	{
#ifdef CACHE_STAT_MODIFIERS_FOR_PROJECTILES
		if( !attackData.m_cachedDamageStatModifiers )
		{
			attackData.m_cachedDamageStatModifiers = red::CreateSharedPtr<red::DynArray<game::DamageStatModifier>, PoolGMPL_Hit>( red::DynArray<game::DamageStatModifier>{PoolGMPL_Hit()} );
		}
		else
		{
			attackData.m_cachedDamageStatModifiers->Clear();
		}

		ComputeAttackStatModifiers( attackData, nullptr, *attackData.m_cachedDamageStatModifiers );
#endif
	}

	void DamageSystem::ComputeAttack( game::events::HitEvent& hitEvent )
	{
		StatsSystem& statsSystem = GetGameInstance()->GetSystem<StatsSystem>();

#ifdef CACHE_STAT_MODIFIERS_FOR_PROJECTILES
		auto& cachedDamageStatModifiers = hitEvent.m_attackData->m_cachedDamageStatModifiers;

		bool hasCache = static_cast< Bool >( cachedDamageStatModifiers );
		auto damageStatModifiers = hasCache ? *cachedDamageStatModifiers : red::DynArray<game::DamageStatModifier>( red::PoolFrame() );
#else
		bool hasCache = false;
		auto damageStatModifiers = red::DynArray<game::DamageStatModifier>( red::PoolFrame() );
#endif

		hitEvent.m_attackComputed = CreateHandle<game::AttackComputed>();

		if( hasCache )
		{
			StatsObjectID targetId;
			if( auto target = hitEvent.m_target.ToHandle() )
			{
				targetId = target->GetEntityID();
			}

			// if we already have cache, then we should only need to figure out the target, everything else is computed already

			for( Int32 i = damageStatModifiers.Size() - 1; i >= 0; --i )
			{
				auto& statModifier = damageStatModifiers[ i ].m_statModifier;
				// everything else is handled, we just care about target
				if( statModifier->GetDependentObject() == StatObjectsRelation::Target )
				{
					const THandle<StatsBundle> statsBundle = statsSystem.GetStatsBundle( targetId );
					if( statsBundle )
					{
						THandle<ConstantStatModifierData> constantModifier = CreateHandle<ConstantStatModifierData>();
						constantModifier->m_statType = statModifier->GetStatType();
						constantModifier->m_modifierType = statModifier->m_modifierType;
						constantModifier->m_value = statModifier->GetValue( *statsBundle, statsBundle->GetValue( statModifier->GetStatType() ) );

						damageStatModifiers[ i ].m_statModifier = constantModifier; // replace current entry
					}
					else // did target disappear? get rid of it
					{
						damageStatModifiers.RemoveAtReorder( i );
					}
				}
			}
		}
		else
		{
			ComputeAttackStatModifiers( *hitEvent.m_attackData, hitEvent.m_target, damageStatModifiers );
		}

		hitEvent.m_attackComputed->Process( damageStatModifiers );
	}

	//-------------------------------------------------------------------------------------------------
	// Listener methods
	//-------------------------------------------------------------------------------------------------

	void DamageSystem::RegisterListener( const THandle<IDamageSystemListener>& listener, DamageListenerBuckets& damageListeners, ent::EntityID registereeID, DamageCallbackType callbackType, DamageListenerPipelineType listenerCallbackPipelineType )
	{
		GAME_ASSERT( ( Uint32 )callbackType < ( Uint32 )DamageCallbackType::COUNT, Gameplay, "Invalid DamageCallbackType when registering damageSystem listener", 0 );
		RED_SCOPE_LOCK( m_listenerLock );

		auto listenerIt = std::find_if( damageListeners[ ( Uint32 ) callbackType ].Begin(), damageListeners[ ( Uint32 ) callbackType ].End(), [ newListener = listener, registereeID ]( const THandle<IDamageSystemListener>& listener )
		{
			return registereeID == listener->m_registereeID && newListener == listener;
		} );

		if( listenerIt == damageListeners[ ( Uint32 ) callbackType ].End() )
		{
			listener->m_damagePipelineType = listenerCallbackPipelineType;
			damageListeners[ ( Uint32 )callbackType ].PushBack( listener );
		}
		else
		{
			if( DamageMask::Equal( listener->m_damagePipelineType, listenerCallbackPipelineType ) )
			{
				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Gameplay, "Trying to register invalid or already registered damage listener" );
			}
			else
			{
				DamageMask::Add( listener->m_damagePipelineType, listenerCallbackPipelineType );
			}
		}
	}

	void DamageSystem::UnregisterListener( const THandle<IDamageSystemListener>& listener, DamageListenerBuckets& damageListeners, ent::EntityID registereeID, DamageCallbackType callbackType, DamageListenerPipelineType listenerCallbackPipelineType )
	{
		GAME_ASSERT( ( Uint32 )callbackType < ( Uint32 )DamageCallbackType::COUNT, Gameplay, "Invalid DamageCallbackType when unregistering damageSystem listener", 0 );
		RED_SCOPE_LOCK( m_listenerLock );

		GAME_DATA_ERROR( damageListeners[ ( Uint32 )callbackType ].FindPtr( listener ), Gameplay, "Trying to unregister invalid or never registered listener" );

		auto listenerIt = std::find_if( damageListeners[ ( Uint32 ) callbackType ].Begin(), damageListeners[ ( Uint32 ) callbackType ].End(), [ newListener = listener, registereeID ]( const THandle<IDamageSystemListener>& listener )
		{
			return registereeID == listener->m_registereeID && newListener == listener;
		} );

		if( listenerIt != damageListeners[ (Uint32) callbackType ].End() )
		{
			DamageMask::Remove( listener->m_damagePipelineType, listenerCallbackPipelineType );

			if( DamageMask::Empty( listener->m_damagePipelineType ) ) // remove only if the flags are cleared on the listener
			{
				damageListeners[ (Uint32) callbackType ].RemoveReorder( listenerIt );
			}
		}
	}

	static void ExecuteCallbacksOfType( DamageCallbackType callbackType,
										const THandle< events::HitEvent >& hitEvent,
										const DamageSystem::DamageListenerBuckets& damageListeners,
										ent::EntityID registereeID,
										DamageListenerPipelineType damagePipelineType,
										red::FixedSizeFunction< void( const THandle< events::HitEvent >&, THandle< IDamageSystemListener > ) > Callback )
	{
		red::DynArray< THandle< IDamageSystemListener > > foundListeners{ red::PoolFrame() };
		for( const auto& listener : damageListeners[ ( Uint32 ) callbackType ] )
		{
			if( listener && registereeID == listener->m_registereeID && DamageMask::Contain( listener->m_damagePipelineType, damagePipelineType ) )
			{
				foundListeners.PushBack( listener );
			}
		}
		for( auto & listener : foundListeners )
		{
			Callback( hitEvent, listener );
		}
	}

	void DamageSystem::ProcessListeners( const THandle< events::HitEvent >& hitEvent, DamageListenerPipelineType damagePipelineType )
	{
		GAME_ASSERT( damagePipelineType != DamageListenerPipelineType::All, Attacks, "Listener process type needs to be strictly defined", 0 );

		{
			RED_SCOPE_LOCK( m_listenerLock );

			// hit triggered
			{
				ent::EntityID instigatorId;
				if( auto instigator = hitEvent->m_attackData->m_instigator.ToHandle() )
				{
					instigatorId = instigator->GetEntityID();
				}
				ExecuteCallbacksOfType( DamageCallbackType::HitTriggered, hitEvent, m_damageListeners, instigatorId, damagePipelineType,
					[ this ]( const THandle< events::HitEvent >& hitEvent, THandle< IDamageSystemListener > listener )
				{
					HitTriggeredCallback( hitEvent, listener->m_registereeID );
				} );
			}

			// hit received
			{
				ent::EntityID targetId;
				red::DynArray< THandle< IDamageSystemListener > > foundListeners{ red::PoolFrame() };
				if( auto target = hitEvent->m_target.ToHandle() )
				{
					targetId = target->GetEntityID();
				}
				ExecuteCallbacksOfType( DamageCallbackType::HitReceived, hitEvent, m_damageListeners, targetId, damagePipelineType,
					[ this ]( const THandle< events::HitEvent >& hitEvent, THandle< IDamageSystemListener > listener )
				{
					HitReceivedCallback( hitEvent, listener->m_registereeID );
				} );
			}

		}

		if( damagePipelineType == DamageListenerPipelineType::Damage )
		{
			PipelineProcessedCallback( hitEvent );
		}
	}

	void DamageSystem::ProcessCallbacks()
	{
		DamageListenerBuckets listeners{ PoolGMPL_Hit() };
		red::DynArray< DamageSystemListenerData > callbackData{ PoolGMPL_Hit() };

		{
			RED_SCOPE_LOCK( m_callbackDataLock );
			callbackData = std::move( m_damageCallbackData );
		}
		{
			RED_SCOPE_SHARED_LOCK( m_listenerLock );
			listeners = m_damageListeners;
		}

		for( auto & data : callbackData )
		{
			for( auto & listener : listeners[ static_cast< Uint32 >( data.callbackType ) ] )
			{
				listener->CallTypedCallback( data );
			}
		}
	}

	void DamageSystem::ProcessSyncStageCallbacks( DamagePipelineStage stage, const THandle< events::HitEvent >& hitEvent, DamageListenerPipelineType damagePipelineType )
	{
		RED_SCOPE_LOCK( m_listenerLock );

		const auto& stageBucket = m_syncDamageListeners[ ( Uint32 )stage ];

		// hit triggered
		{
			ent::EntityID instigatorID;
			if( auto instigator = hitEvent->m_attackData->m_instigator.ToHandle() )
			{
				instigatorID = instigator->GetEntityID();
			}
			ExecuteCallbacksOfType( DamageCallbackType::HitTriggered, hitEvent, stageBucket, instigatorID, damagePipelineType,
									[]( const THandle< events::HitEvent >& hitEvent, THandle< IDamageSystemListener > listener )
			{
				listener->OnHitTriggered( hitEvent );
			} );
		}

		// hit received
		{
			ent::EntityID targetId;
			if( auto target = hitEvent->m_target.ToHandle() )
			{
				targetId = target->GetEntityID();
			}
			ExecuteCallbacksOfType( DamageCallbackType::HitReceived, hitEvent, stageBucket, targetId, damagePipelineType,
				[ this ]( const THandle< events::HitEvent >& hitEvent, THandle< IDamageSystemListener > listener )
			{
				listener->OnHitReceived( hitEvent );
			} );
		}
	}

	void DamageSystem::HitTriggeredCallback( const THandle<events::HitEvent>& hitEvent, ent::EntityID instigatorID )
	{
		RED_SCOPE_LOCK( m_callbackDataLock );
		m_damageCallbackData.EmplaceBack( hitEvent, DamageCallbackType::HitTriggered, instigatorID );
	}

	void DamageSystem::HitReceivedCallback( const THandle<events::HitEvent>& hitEvent, ent::EntityID targetID )
	{
		RED_SCOPE_LOCK( m_callbackDataLock );
		m_damageCallbackData.EmplaceBack( hitEvent, DamageCallbackType::HitReceived, targetID );
	}

	void DamageSystem::PipelineProcessedCallback( const THandle<events::HitEvent>& hitEvent )
	{
		RED_SCOPE_LOCK( m_callbackDataLock );
		m_damageCallbackData.EmplaceBack( hitEvent, DamageCallbackType::PipelineProcessed, ent::EntityID() );
	}

	//-------------------------------------------------------------------------------------------------

	void DamageSystem::ComputeAttackStatModifiers( game::damage::AttackData& attackData, const THandle<game::Object>& target, red::DynArray<DamageStatModifier>& damageStatModifiers )
	{
		if( !attackData.m_attackDefinition )
		{
			GAME_DATA_ERROR( false, Gameplay, "Attack data has no definition, this should never happen. Send to designers to make sure the attack is properly setup." );
			return;
		}

		StatsSystem& statsSystem = GetGameInstance()->GetSystem<StatsSystem>();

		StatsObjectID instigatorId;
		if( auto instigator = attackData.m_instigator.ToHandle() )
		{
			instigatorId = instigator->GetEntityID();
		}
		StatsObjectID sourceId;
		if( auto source = attackData.m_source.ToHandle() )
		{
			sourceId = source->GetEntityID();
		}
		StatsObjectID weaponId;
		if( auto weapon = attackData.m_weapon.ToHandle() )
		{
			weaponId = weapon->GetEntityID();
		}

		StatsObjectID targetId;
		if( target )
		{
			targetId = target->GetEntityID();
		}

		red::DynArray< THandle< game::StatModifierData > > statModifiers{ red::PoolFrame() };

		attackData.m_attackDefinition->GetStatModList( statModifiers );

		damageStatModifiers.Reserve( statModifiers.Size() );

		for( auto statModifier : statModifiers )
		{
			data::StatType statType = statModifier->m_statType;

			data::DamageType damageType = statsSystem.GetDamageType( statType );

			if( damageType == data::DamageType::Invalid )
			{
				continue;
			}

			StatsObjectID* statsObjectId = nullptr;

			Bool statObjectIsTarget = false;

			auto statRelation = statModifier->GetDependentObject();
			switch( statRelation )
			{
			case StatObjectsRelation::Root:
				statsObjectId = &instigatorId;
				break;
			case StatObjectsRelation::Parent:
				statsObjectId = &sourceId;
				break;
			case StatObjectsRelation::Owner:
				statsObjectId = &weaponId;
				break;
			case StatObjectsRelation::Target:
				statsObjectId = &targetId;
				statObjectIsTarget = true;
				break;
			case StatObjectsRelation::Invalid:
				// invalid is special case, basically means that you should use this directly
				// but since IAttack isn't a StatsObject, we're just allowing it if it's a Constant/RandomStatModifier
				if( THandle<ConstantStatModifierData> statModifierAsConstant = Cast<ConstantStatModifierData>( statModifier ) )
				{
					DamageStatModifier damageStatModifier;
					damageStatModifier.m_damageType = damageType;
					damageStatModifier.m_statModifier = statModifierAsConstant;
					damageStatModifiers.PushBack( damageStatModifier );
				}
				else if( THandle<RandomStatModifierData> statModifierAsRandom = Cast<RandomStatModifierData>( statModifier ) )
				{
					THandle<ConstantStatModifierData> constantStatModifier = CreateHandle<ConstantStatModifierData>();
					constantStatModifier->m_statType = statModifierAsRandom->m_statType;
					constantStatModifier->m_modifierType = statModifierAsRandom->m_modifierType;
					constantStatModifier->m_value = statModifierAsRandom->m_value;

					DamageStatModifier damageStatModifier;
					damageStatModifier.m_damageType = damageType;
					damageStatModifier.m_statModifier = constantStatModifier;
					damageStatModifiers.PushBack( damageStatModifier );
				}
				continue;
				// break;
			default:
				// Self & Player not valid either
			{
				CName statRelationsName;
				if( !GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "gameStatObjectsRelation" ) )->FindName( static_cast< Int32 >( statRelation ), statRelationsName ) )
				{
					statRelationsName = RED_NAME_CONSTEXPR( "UNKNOWN ENUM" );
				}
				GAME_DATA_ERROR( false, Gameplay, "Invalid StatObjectsRelation in Attack Pipeline: %s, ignoring", statRelationsName.AsChar() );
			}
			continue; // ignore this one
			//break;
			}

			if( statsObjectId && statsObjectId->IsDefined() )
			{
				const THandle<StatsBundle> statsBundle = statsSystem.GetStatsBundle( *statsObjectId );
				if( statsBundle ) // just make sure it still exists at this point
				{

					game::data::StatType statTypeToFetch = statType;

					if( const auto combinedStatModifier = Cast< CombinedStatModifierData >( statModifier ) )
					{
						statTypeToFetch = combinedStatModifier->m_refStatType;
					}
					else if( const auto curveStatModifier = Cast< CurveStatModifierData >( statModifier ) )
					{
						statTypeToFetch = curveStatModifier->m_curveStat;
					}

					THandle<ConstantStatModifierData> constantModifier = CreateHandle<ConstantStatModifierData>();
					constantModifier->m_statType = statModifier->m_statType;
					constantModifier->m_modifierType = statModifier->m_modifierType;
					constantModifier->m_value = statModifier->GetValue( *statsBundle, statsBundle->GetValue( statTypeToFetch ) );

					DamageStatModifier damageStatModifier;
					damageStatModifier.m_damageType = damageType;
					damageStatModifier.m_statModifier = constantModifier;

					damageStatModifiers.PushBack( damageStatModifier );
				}
			}
			else if( statObjectIsTarget )
			{
				// if it is a target, but there's no target yet (i.e. at the moment of firing) then keep the original one. It will be recalculated later
				DamageStatModifier damageStatModifier;
				damageStatModifier.m_damageType = damageType;
				damageStatModifier.m_statModifier = statModifier;

				damageStatModifiers.PushBack( damageStatModifier );
			}
		}
	}

	void DamageSystem::Server_RecordKillRewardEvent( const game::Object& killerObject, const events::KillRewardEvent& evt )
	{
		if( !killerObject.IsPlayerControlled() )
		{
			return;
		}

		RED_SCOPE_LOCK( m_replicationStateLock );

		THandle< damage::ServerKillData > serverData = CreateHandle< damage::ServerKillData >();
		auto replicatedState = GetReplicatedState< DamageSystemSharedState >();
		replicatedState->m_serverKillCounter++;
		serverData->m_id = replicatedState->m_serverKillCounter;
		serverData->m_killInfo.m_killerEntity = WeakHandleFromPtr( &killerObject );
		serverData->m_killInfo.m_victimEntity = evt.m_victim;
		serverData->m_killInfo.m_killType = evt.m_killType;
		serverData->m_killTime = GEngine->GetRawEngineTime();

		replicatedState->m_killHistory.EmplaceBack( std::move( serverData ) );
	}

	void DamageSystem::DisplayKillInfo( const game::Object& killerObject, const events::KillRewardEvent& evt ) const
	{
		if( !killerObject.IsPlayerControlled() )
		{
			return;
		}

		ui::KillInfo killInfo;

		killInfo.m_killerEntity = WeakHandleFromPtr( &killerObject );
		killInfo.m_victimEntity = evt.m_victim;
		killInfo.m_killType = evt.m_killType;

		bool result = rtti::CallFunction_UserContext( const_cast< game::Object* >( &killerObject ), GetGameInstance()->GetScriptContext(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "DisplayKillUI;KillInfo" ) ), killInfo );
		RED_ASSERT( result, "Unable to call GameObject::DisplayKillUI" );
	}

	Float DamageSystem::GetDamageModFromCurve( const CName& curveName, const Float lookupValue )
	{
		float result = 0.0f;
		if( s_damageCurves.Get() )
		{
			result = s_damageCurves->EvaluateValueForCurve( curveName, lookupValue );
		}
		return result;
	}

	void DamageSystem::funcGetDamageModFromCurve( IScriptable* scriptable, CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, attackersCurveName, CName::NONE() );
		GET_PARAMETER( Float, lookUpValue, 0.f );
		FINISH_PARAMETERS;

		{
			if( attackersCurveName != CName::NONE() )
			{
				RETURN_FLOAT( GetDamageModFromCurve( attackersCurveName, lookUpValue ) );
				return;
			}

			RETURN_FLOAT( 0.f );
		}
	}

	void DamageSystem::InitializeDebugData()
	{
	#ifdef RED_USE_IMGUI
		{
			auto& clickAction = m_damagesTableActionList.EmplaceBack();
			clickAction.name = RED_NAME_CONSTEXPR( "<click>" );
			clickAction.func = [ this ]( void* ptr )
			{
				auto* hitDebugDataPtr = reinterpret_cast<THandle< damage::HitDebugData >*>( ptr );
				m_selectedHitDebugData = *hitDebugDataPtr;
				m_isHitDebugDataInspected = true;
			};
		}

		m_damagesTable.SetLockScroll( false );

		m_damagesTable.AddColumn( "Idx" );
		m_damagesTable.AddColumn( "Instigator" );
		m_damagesTable.AddColumn( "Source" );
		m_damagesTable.AddColumn( "Target" );
		m_damagesTable.AddColumn( "Attack" );
		m_damagesTable.AddColumn( "Weapon" );
		m_damagesTable.AddColumn( "HitType" );
		m_damagesTable.AddColumn( "HitFlags" );


		m_damagesTable.SetDrawRowFunction( [ this ]( Uint32 idx, red::ImGui::ColumnSizer& sizer )
		{
			const THandle< damage::HitDebugData > dataRow = m_hitDebugData[ idx ];

			//Index
			{
				const Bool selected = m_selectedHitDebugData != nullptr && dataRow->m_id == m_selectedHitDebugData->m_id;
				const auto& actions = m_damagesTableActionList;
				void* userData = (void*) &m_hitDebugData[ idx ];
				constexpr ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;

				red::ImGui::CustomActionSelectable( String::Printf( "%d", dataRow->m_id ).AsChar(), actions, selected, userData, flags );
				red::ImGui::NextColumn( sizer );
			}

			// Instigator
			{
				ImGui::Text( dataRow->m_instigatorName.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//Source
			{
				ImGui::Text( dataRow->m_sourceName.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//Target
			{
				ImGui::Text( dataRow->m_targetName.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//Attack
			{
				ImGui::Text( dataRow->m_sourceAttackName.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//Weapon
			{
				ImGui::Text( dataRow->m_sourceWeaponName.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//HitType
			{
				ImGui::Text( dataRow->m_hitType.AsChar() );
				red::ImGui::NextColumn( sizer );
			}

			//HitFlags
			{
				ImGui::Text( dataRow->m_hitFlags.AsChar() );
				red::ImGui::NextColumn( sizer );
			}


		} );

		//Data for 'calculated damages' and 'applied damages' in separate window
		m_selectedHitCalculatedDamagesTable.AddColumn( "Damage Type" );
		m_selectedHitCalculatedDamagesTable.AddColumn( "Damage Value" );
		m_selectedHitCalculatedDamagesTable.AddColumn( "Affected StatPool" );

		m_selectedHitCalculatedDamagesTable.SetDrawRowFunction( [ this ]( Uint32 idx, red::ImGui::ColumnSizer& sizer )
		{
			const THandle< damage::DamageDebugData > dataRow = m_selectedHitDebugData->m_calculatedDamages[ idx ];

			//DamageType
			{
				ImGui::Text( rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "gamedataDamageType" ), static_cast<Int64>( dataRow->m_damageType ) ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
			//DamageValue
			{
				ImGui::Text( String::Printf( "%f", dataRow->m_value ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
			//affected StatPool
			{
				ImGui::Text( rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "gamedataStatPoolType" ), static_cast<Int64>( dataRow->m_statPoolType ) ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
		} );

		m_selectedHitAppliedDamagesTable.AddColumn( "Damage Type" );
		m_selectedHitAppliedDamagesTable.AddColumn( "Damage Value" );
		m_selectedHitAppliedDamagesTable.AddColumn( "Affected StatPool" );

		m_selectedHitAppliedDamagesTable.SetDrawRowFunction( [ this ]( Uint32 idx, red::ImGui::ColumnSizer& sizer )
		{
			const THandle< damage::DamageDebugData > dataRow = m_selectedHitDebugData->m_appliedDamages[ idx ];

			//DamageType
			{
				ImGui::Text( rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "gamedataDamageType" ), static_cast<Int64>( dataRow->m_damageType ) ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
			//DamageValue
			{
				ImGui::Text( String::Printf( "%f", dataRow->m_value ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
			//affected StatPool
			{
				ImGui::Text( rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "gamedataStatPoolType" ), static_cast<Int64>( dataRow->m_statPoolType ) ).AsChar() );
				red::ImGui::NextColumn( sizer );
			}
		} );

		//hitshapes?
	#endif
	}

#ifdef RED_USE_IMGUI

	void DamageSystem::OnRenderDebug( const RenderDebugContext& context )
	{
		PC_SCOPE( DamageSystem_OnRenderDebug );
		auto& dd = context.debugDrawer;
		rend::FragmentBuilder builder( dd );
		static const CName generalName = RED_NAME_CONSTEXPR( "Damage System" );

		//if ( red::ImGui::IsWindowRequested( generalName ) )
		if( dd.GetMask().Test( DF_Damages ) )
		{
			// select no of damages

			ImGui::TextColored( red::ImGui::LIGHT_BLUE(), "Damages list:" );

			m_damagesTable.Draw( m_hitDebugData.Size() );
			ImGui::Separator();
		}

		if( m_isHitDebugDataInspected )
		{
			Debug_DrawHit( builder );
		}
	}

	void DamageSystem::Debug_DrawHit( rend::FragmentBuilder& builder )
	{
		if( ImGui::Begin( "Damage System##Hit Inspector" ) )
		{
			ImGui::BeginChild( "calculated damages", ImVec2( 300, 0 ) );
			ImGui::TextColored( red::ImGui::LIGHT_BLUE(), "Calculated damages:" );
			m_selectedHitCalculatedDamagesTable.Draw( m_selectedHitDebugData->m_calculatedDamages.Size() );
			ImGui::EndChild();
			ImGui::SameLine();

			ImGui::BeginGroup();
			ImGui::BeginChild( "applied damages" );
			ImGui::TextColored( red::ImGui::LIGHT_BLUE(), "Applied damages:" );
			m_selectedHitAppliedDamagesTable.Draw( m_selectedHitDebugData->m_appliedDamages.Size() );
			ImGui::EndChild();
			ImGui::EndGroup();

			//Draw positions of source and target
			if( auto sourceHandle = m_selectedHitDebugData->m_source.ToHandle() )
			{
				if( auto targetHandle = m_selectedHitDebugData->m_target.ToHandle() )
				{
					Vector3 sourcePos = m_selectedHitDebugData->m_sourceHitPosition;
					Vector3 targetPos = m_selectedHitDebugData->m_targetHitPosition;
					// in case when entities exist but are no longer attached (e.g. when they die)
					if( sourceHandle->GetState() == ent::EntityState::Attached )
					{
						sourcePos = sourceHandle->GetWorldPosition().AsVector3();
					}
					if( targetHandle->GetState() == ent::EntityState::Attached )
					{
						targetPos = targetHandle->GetWorldPosition().AsVector3();
					}

					constexpr Float halfLength = 20.0f;
					const Vector4 sourceLineEnds[ 2 ] = {
						sourcePos + Vector4( 0.f, 0.f, halfLength ),
						sourcePos - Vector4( 0.f, 0.f, halfLength )
					};
					const Vector4 targetLineEnds[ 2 ] = {
						targetPos + Vector4( 0.f, 0.f, halfLength ),
						targetPos - Vector4( 0.f, 0.f, halfLength )
					};

					builder.SetColor( Color::GREEN() );
					builder.AddFatLine( sourceLineEnds[ 0 ], sourceLineEnds[ 1 ], 0.08f );

					builder.SetColor( Color::RED() );
					builder.AddFatLine( targetLineEnds[ 0 ], targetLineEnds[ 1 ], 0.08f );
				}
			}

			//Draw debug for Attack
			AttackDebugData attackDebugData = m_selectedHitDebugData->m_sourceAttackDebugData;

			builder.SetColor( Color::DARK_CYAN() );
			builder.AddSolidSphere( attackDebugData.m_pointOfViewTransform.GetPosition().AsVector3(), 0.12f );
			builder.AddAxes( attackDebugData.m_pointOfViewTransform.ToMatrix() );
			builder.SetColor( Color::LIGHT_CYAN() );
			builder.AddSolidSphere( attackDebugData.m_pointOfViewTransform.GetPosition().AsVector3() + attackDebugData.m_projectileHitplaneSpread, 0.05f );
			builder.SetColor( Color::LIGHT_YELLOW() );
			builder.AddSolidSphere( attackDebugData.m_bulletStartPosition, 0.05f );

		}

		ImGui::End();
	}

#endif


	void DamageSystem::funcRegisterListener( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( THandle< ScriptedDamageSystemListener >, scriptedListener, nullptr );
		GET_PARAMETER( ent::EntityID, registereeID, ent::EntityID() );
		GET_PARAMETER( DamageCallbackType, callbackType, DamageCallbackType::INVALID );
		GET_PARAMETER_OPT( DamageListenerPipelineType, damagePipelineType, DamageListenerPipelineType::Damage );
		FINISH_PARAMETERS;

		scriptedListener->m_registereeID = registereeID;

		RegisterListener( scriptedListener, m_damageListeners, registereeID, callbackType, damagePipelineType );

		RETURN_VOID();
	}

	void DamageSystem::funcUnregisterListener( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( THandle< ScriptedDamageSystemListener >, scriptedListener, nullptr );
		GET_PARAMETER( ent::EntityID, registereeID, ent::EntityID() );
		GET_PARAMETER( DamageCallbackType, callbackType, DamageCallbackType::INVALID );
		GET_PARAMETER_OPT( DamageListenerPipelineType, damagePipelineType, DamageListenerPipelineType::All );
		FINISH_PARAMETERS;

		scriptedListener->m_registereeID = registereeID;

		UnregisterListener( scriptedListener, m_damageListeners, registereeID, callbackType, damagePipelineType );

		RETURN_VOID();
	}

	void DamageSystem::funcRegisterSyncListener( CScriptStackFrame & stack, void * result, const rtti::IType * resultType )
	{
		GET_PARAMETER( THandle< ScriptedDamageSystemListener >, scriptedListener, nullptr );
		GET_PARAMETER( ent::EntityID, registereeID, ent::EntityID() );
		GET_PARAMETER( DamageCallbackType, callbackType, DamageCallbackType::INVALID );
		GET_PARAMETER( DamagePipelineStage, damagePipelineStage, DamagePipelineStage::INVALID );
		GET_PARAMETER_OPT( DamageListenerPipelineType, damagePipelineType, DamageListenerPipelineType::Damage );
		FINISH_PARAMETERS;

		GAME_ASSERT( ( Uint32 )damagePipelineStage < ( Uint32 )DamagePipelineStage::COUNT, Gameplay, "Invalid DamagePipelineStage when registering sync damageSystem listener.", 0 );

		scriptedListener->m_registereeID = registereeID;

		RegisterListener( scriptedListener, m_syncDamageListeners[ ( Uint32 )damagePipelineStage ], registereeID, callbackType, damagePipelineType );

		RETURN_VOID();
	}

	void DamageSystem::funcUnregisterSyncListener( CScriptStackFrame & stack, void * result, const rtti::IType * resultType )
	{
		GET_PARAMETER( THandle< ScriptedDamageSystemListener >, scriptedListener, nullptr );
		GET_PARAMETER( ent::EntityID, registereeID, ent::EntityID() );
		GET_PARAMETER( DamageCallbackType, callbackType, DamageCallbackType::INVALID );
		GET_PARAMETER( DamagePipelineStage, damagePipelineStage, DamagePipelineStage::INVALID );
		GET_PARAMETER_OPT( DamageListenerPipelineType, damagePipelineType, DamageListenerPipelineType::All );
		FINISH_PARAMETERS;

		GAME_ASSERT( ( Uint32 )damagePipelineStage < ( Uint32 )DamagePipelineStage::COUNT, Gameplay, "Invalid DamagePipelineStage when unregistering sync damageSystem listener.", 0 );

		scriptedListener->m_registereeID = registereeID;

		UnregisterListener( scriptedListener, m_syncDamageListeners[ ( Uint32 )damagePipelineStage ], registereeID, callbackType, damagePipelineType );

		RETURN_VOID();
	}

	void DamageSystem::funcProcessSyncStageCallbacks( CScriptStackFrame & stack, void * result, const rtti::IType * resultType )
	{
		GET_PARAMETER( DamagePipelineStage, damagePipelineStage, DamagePipelineStage::INVALID );
		GET_PARAMETER( THandle< events::HitEvent >, hitEvent, nullptr );
		GET_PARAMETER( DamageListenerPipelineType, damagePipelineType, DamageListenerPipelineType::None );
		FINISH_PARAMETERS;

		GAME_ASSERT( ( Uint32 )damagePipelineStage < ( Uint32 )DamagePipelineStage::COUNT, Gameplay, "Invalid DamagePipelineStage when processing sync damageSystem listeners.", 0 );

		ProcessSyncStageCallbacks( damagePipelineStage, hitEvent, damagePipelineType );

		RETURN_VOID();
	}
}


#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

