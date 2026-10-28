/*
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "weaponObject.h"

#include "projectileSpawnComponent.h"
#include "recoilManager.h"
#include "statsBundle.h"
#include "statsSystem.h"
#include "transactionSystem.h"
#include "statPoolsSystem.h"
#include "weaponAnimFeatures.h"
#include "weaponEvents.h"
#include "weaponFxPackage.h"
#include "equipmentManager.h"
#include "weaponReplicationHistory.h"
#include "senseManager.h"
#include "smartGunNPCHandler.h"
#include "smartGunHandler.h"
#include "attack.h"
#include "puppet.h"
#include "humanoidBody.h"

#include "../../gameCore/include/targetingSystemInterface.h"
#include "../../gameCore/include/playerAim.h"
#include "../../gameTweakDB/include/records/tweakDBAttack.h"
#include "../../gameTweakDB/include/records/tweakDBStatModifier.h"
#include "../../gameTweakDB/include/records/tweakDBTriggerMode.h"
#include "../../gameTweakDB/include/records/tweakDBWeaponItem.h"
#include "../../gameTweakDB/include/records/tweakDBWeaponEvolution.h"
#include "../../gameCore/include/inventoryManagerInterface.h"
#include "../../gameCore/include/questsHelperInterface.h"
#include "../../worldEntities/include/animatedComponent.h"
#include "../../worldEntities/include/componentRequestorInterface.h"
#include "../../worldEntities/include/componentResolverInterface.h"
#include "../../worldEntities/include/playEffectEvent.h"
#include "../../worldEntities/include/runtimeEntityRegistry.h"
#include "../../worldEntities/include/runtimeSystemEntity.h"
#include "../../worldPhysics/include/runtimeSystemPhysics.h"
#include "../../worldUserInterface/include/worldWidgetEvents.h"

#include "../../worldEntities/include/entityContext.h"

#include "../../world/include/gameInstance.h"
#include "../../world/include/worldRuntimeScene.h"

#ifndef RED_CONFIGURATION_FINAL
#include "../gameCore/include/gameDebugCheatsSystemInterface.h"
#endif
#include "weaponAudioEvents.h"
#include "../redReflection/include/mathUtils.h"
#include "../worldEntities/include/positionProvider.h"
#include <cstdio>
// [tkfix] coreplay hit probes (logging only), defined in scriptableSystemsContainer.cpp; on for ?sandbox / ?coreplay=1
extern volatile int g_coreplayHitProbe;
extern volatile int g_coreplayHitBudget[ 3 ];
extern volatile unsigned long long g_coreplayHitRing[ 8 ];
extern volatile int g_coreplayHitRingPos;
static inline bool CpHitProbeTake( int which ) { return g_coreplayHitProbe && __atomic_sub_fetch( &g_coreplayHitBudget[ which ], 1, __ATOMIC_RELAXED ) >= 0; }


#ifdef DEBUG_GAMEPLAY
#pragma optimize( "", off )
#endif

//#define WEAPONS_LOG_ENABLED
#ifdef WEAPONS_LOG_ENABLED
//#define WEAPONS_LOG( message, ... ) RED_LOG( message, ##__VA_ARGS__ )
#define WEAPONS_LOG( message, ... )   \
	printf( message, ##__VA_ARGS__ ); \
	printf( "\n" )
#else
#define WEAPONS_LOG( ... )
#endif // WEAPONS_LOG_ENABLED

RTTI_BEGIN_ENUM_IN_NAMESPACE( ReloadStatus, game, weapon );
RTTI_ENUM_OPTION( Standard );
RTTI_ENUM_OPTION( Interrupted );
RTTI_END_ENUM();

RTTI_BEGIN_TYPE_IN_NAMESPACE( Object, game, weapon );
RTTI_PARENT_TYPE( ItemObject );
RTTI_SCRIPT_ALIAS( "WeaponObject" );
RTTI_PROPERTY( m_effect ).editable();
RTTI_NATIVE_FUNCTION( "GetAttacks", funcGetAttacks );
RTTI_NATIVE_FUNCTION( "GetCurrentAttack", funcGetCurrentAttack );
RTTI_NATIVE_FUNCTION( "GetCurrentTriggerMode", funcGetCurrentTriggerMode );
RTTI_NATIVE_FUNCTION( "GetFxPackage", funcGetFxPackage );
RTTI_NATIVE_FUNCTION( "GetFxPackageQuickMelee", funcGetFxPackageQuickMelee );
RTTI_NATIVE_FUNCTION( "GetSharedData", funcGetSharedData );
RTTI_NATIVE_FUNCTION( "GetTotalAmmoCount", funcGetTotalAmmoCount );
RTTI_NATIVE_FUNCTION( "GetTriggerModes", funcGetTriggerModes );
RTTI_NATIVE_FUNCTION( "HasAmmoChangeRequest", funcHasAmmoChangeRequest );
RTTI_NATIVE_FUNCTION( "HasPendingReload", funcHasPendingReload );
RTTI_NATIVE_FUNCTION( "SetAttack", funcSetAttack );
RTTI_NATIVE_FUNCTION("StartReload", funcStartReload);
RTTI_NATIVE_FUNCTION("StopReload", funcStopReload);
RTTI_NATIVE_FUNCTION( "AI_ShootAt", funcAI_ShootAt );
RTTI_NATIVE_FUNCTION( "AI_ShootForwards", funcAI_ShootForwards );
RTTI_NATIVE_FUNCTION( "AI_ShootSelfOffScreen", funcAI_ShootSelfOffScreen);
RTTI_NATIVE_FUNCTION( "AI_SetAttackData", funcAI_SetAttackData );
RTTI_NATIVE_FUNCTION( "AI_PlayChargeStartedSound", funcAI_PlayChargeStartedSound );
RTTI_NATIVE_FUNCTION( "AI_PlayMeleeAttackSound", funcAI_PlayMeleeAttackSound );
RTTI_NATIVE_FUNCTION( "SetTriggerDown", func_SetTriggerDown );
RTTI_NATIVE_FUNCTION( "StartContinuousAttack", funcStartContinuousAttack );
RTTI_NATIVE_FUNCTION( "StopContinuousAttack", funcStopContinuousAttack );
RTTI_NATIVE_FUNCTION( "IsContinuousAttackStarted", funcIsContinuousAttackStarted );
RTTI_NATIVE_FUNCTION( "HasScope", funcHasScope );
RTTI_NATIVE_FUNCTION( "GetScopeOffset", funcGetScopeOffset );
RTTI_NATIVE_FUNCTION( "GetIronSightOffset", funcGetIronSightOffset );
RTTI_NATIVE_FUNCTION( "GetMuzzleOffset", funcGetMuzzleOffset );
RTTI_NATIVE_FUNCTION( "IsSilenced", funcIsSilenced );
RTTI_NATIVE_FUNCTION( "IsTargetLocked", funcIsTargetLocked );
RTTI_NATIVE_FUNCTION( "UpdateTargetingSight", funcUpdateTargetingSight );
RTTI_NATIVE_FUNCTION( "ShootStraight", funcShootStraight );
RTTI_NATIVE_FUNCTION( "SetupBurstFireSound", funcSetupBurstFireSound );
RED_EVENT_CONNECTOR( OnCycleTriggerMode );
RED_EVENT_CONNECTOR( OnChangeTriggerMode );
RED_EVENT_CONNECTOR( OnReloadFinished );
RED_EVENT_CONNECTOR( OnRequestAmmoChange );
RED_EVENT_CONNECTOR( OnReloadStart );
RED_EVENT_CONNECTOR( OnStopFiring );
RED_EVENT_CONNECTOR( OnStopWeaponFire );
RED_EVENT_CONNECTOR( OnHitConnected );
RED_EVENT_CONNECTOR( OnToggleAimDownSights );
RED_EVENT_CONNECTOR( OnPreFire );
RED_EVENT_CONNECTOR( OnSetMaxCharge );
RED_EVENT_CONNECTOR( OnEnableSmartGunHandlerEvent );
RED_EVENT_CONNECTOR( OnOwnerAim );
RTTI_END_TYPE();

namespace
{
	const CName c_projectileComponentName = RED_NAME_CONSTEXPR( "Projectile" );
	const CName c_weaponAudioComponentName = RED_NAME_CONSTEXPR( "WeaponAudioComponent" );
	const CName c_ironSightBoneName = RED_NAME_CONSTEXPR( "pos_ironsight" );
	const CName c_scopeBoneName = RED_NAME_CONSTEXPR( "pos_scope" );
	const CName c_muzzleBoneName = RED_NAME_CONSTEXPR( "muzzle_slot" );
	const CName c_muzzleSlotName = RED_NAME_CONSTEXPR( "Muzzle" );

	const Float c_aimingScreenAreaMultiplier = 1.0f;
	const Float c_hipScreenAreaMultiplier = 0.005f;
	const Float c_defaultScreenAreaMultiplier = 1.0f;
}

namespace Config
{
	extern MULTIPLAYER_FRAMEWORK_API TConfigVar< Int32 > cvInterpolationDelayMilliSecs;
}

const Int32 game::weapon::Object::STANDARD_RELOAD = -1;

game::weapon::Object::FxPackagesMap game::weapon::Object::s_fxPackagesMap{PoolGMPL_Items_Object()};
red::RWSpinLock game::weapon::Object::s_fxPackagesMapLock;


// Ghetto spread implementation for AI - Player's spread should be refactored
// to use world-space spread like this in the future, and the AI can ride off
// the back of that implementation.
Transform TEMP_AI_GenerateRandomSpreadTransform( const Float angularRangeMax )
{
	const Float randomAngle = math::DefaultRandom().Get< Float >( 0.0f, RED_PI_TWO );
	const Float randomSpread = math::DefaultRandom().Get< Float >( 0.0f, angularRangeMax );
	Transform xform{Transform::IDENTITY()};
	xform.SetOrientation(
		math::EulerAngles(
			0.0f,
			randomSpread * MCos( randomAngle ),
			randomSpread * MSin( randomAngle ) ) );
	return xform;
}

Vector3 TEMP_AI_GenerateForwardVectorWithSpread( const Float angularRangeMax, const Quaternion& forwards )
{
	return TEMP_AI_GenerateRandomSpreadTransform( angularRangeMax ).TransformVector( forwards.Transform( Vector3::EY() ) );
}

Vector3 TEMP_AI_GenerateForwardVectorWithSpread( const Float angularRangeMax, const Vector3& forward )
{
	return TEMP_AI_GenerateRandomSpreadTransform( angularRangeMax ).TransformVector( forward );
}

//-------------------------------------------------------------------------------------------------

game::weapon::WeaponChargeStatPoolsListener::WeaponChargeStatPoolsListener( const THandle< IBlackboard >& blackboard )
	: m_blackboard( blackboard )
	, m_itemData(nullptr)
	, m_targetingSystem(nullptr)
	, m_owner(nullptr)
{}

game::weapon::WeaponChargeStatPoolsListener::~WeaponChargeStatPoolsListener() = default;

void game::weapon::WeaponChargeStatPoolsListener::GetChargMultipliers( float chargeNormalized,
																	   float chargeMultStat,
																	   float chargeAimMultStat,
																	   float fullChargeMultStat,
																	   float fullAimChargeMultStat,
																	   float& outChargeMult,
																	   float& outChargeAimMult ) const
{
	outChargeMult = 1.0f + MathUtils::InterpolationUtils::Interpolate( 0.0f, chargeMultStat, chargeNormalized );
	outChargeAimMult = 1.0f + MathUtils::InterpolationUtils::Interpolate( 0.0f, chargeAimMultStat, chargeNormalized );
	
	if( math::Abs(1.0f - chargeNormalized) < FLT_EPSILON )
	{
		outChargeMult += fullChargeMultStat;
		outChargeAimMult += fullAimChargeMultStat;
	}
}
void game::weapon::WeaponChargeStatPoolsListener::OnStatPoolValueChanged( Float oldValue, Float newValue, Float percToPoints )
{
	// UI does not want the normalized value. 
	if( auto blackboardHandle = m_blackboard.ToHandle() )
	{
		blackboardHandle->Write( BLACKBOARD_ID( Weapon, Charge ), newValue * percToPoints );
	}

	float chargeValueNormalized = 0.0f;
	if( auto weapon = m_weapon.ToHandle() )
	{
		chargeValueNormalized = weapon->GetWeaponChargeValueNormalized( newValue );
	}

	if( auto itemData = m_itemData.ToHandle() )
	{
		if( m_playerAim )
		{
			float spreadChargeMult = 1.0f;
			float spreadAdsChargeMult = 1.0f;

			GetChargMultipliers( chargeValueNormalized,
								 itemData->GetStatValue( data::StatType::SpreadChargeMult ),
								 itemData->GetStatValue( data::StatType::SpreadAdsChargeMult ),
								 itemData->GetStatValue( data::StatType::SpreadFullChargeMult ),
								 itemData->GetStatValue( data::StatType::SpreadAdsFullChargeMult ),
								 spreadChargeMult,
								 spreadAdsChargeMult );

			m_playerAim->GetWeaponSpreadManager().SetChargeMultipliers( Vector2( spreadChargeMult ), Vector2( spreadAdsChargeMult ) );
		}

		if( auto targetingSystem = m_targetingSystem.ToHandle() )
		{
			float recoilChargeMult = 1.0f;
			float recoilChargeAimMult = 1.0f;

			GetChargMultipliers( chargeValueNormalized,
								 itemData->GetStatValue( data::StatType::RecoilChargeMult ),
								 itemData->GetStatValue( data::StatType::RecoilChargeMultADS ),
								 itemData->GetStatValue( data::StatType::RecoilFullChargeMult ),
								 itemData->GetStatValue( data::StatType::RecoilFullChargeMultADS ),
								 recoilChargeMult,
								 recoilChargeAimMult );

			targetingSystem->SetRecoilChargeMultipliers( m_owner, Vector2( recoilChargeMult ), Vector2( recoilChargeAimMult ) );
		}

		if( auto animFeature = m_animFeature.ToHandle() )
		{
			animFeature->SetChargePercentage( chargeValueNormalized );
		}
	}
}

game::weapon::WeaponOverheatStatPoolsListener::WeaponOverheatStatPoolsListener( const THandle< IBlackboard >& blackboard )
	: m_blackboard( blackboard )
{}

game::weapon::WeaponOverheatStatPoolsListener::~WeaponOverheatStatPoolsListener() = default;

void game::weapon::WeaponOverheatStatPoolsListener::OnStatPoolValueChanged( Float oldValue, Float newValue, Float percToPoints )
{
	if( auto blackboardHandle = m_blackboard.ToHandle() )
	{
		if( m_weaponAudio && newValue != oldValue )
		{
			m_weaponAudio->SetOverheat( newValue );
		}
		blackboardHandle->Write( BLACKBOARD_ID( Weapon, OverheatPercentage ), newValue );
	}
}

void game::weapon::WeaponOverheatStatPoolsListener::OnStatPoolMinValueReached( Float value )
{
	if ( auto blackboardHandle = m_blackboard.ToHandle() )
	{
		blackboardHandle->Write( BLACKBOARD_ID( Weapon, IsInForcedOverheatCooldown ), false );
	}
}

void game::weapon::WeaponOverheatStatPoolsListener::OnStatPoolMaxValueReached( Float value )
{
	if ( auto blackboardHandle = m_blackboard.ToHandle() )
	{
		blackboardHandle->Write( BLACKBOARD_ID( Weapon, IsInForcedOverheatCooldown ), true );
	}
}

void game::weapon::WeaponOverheatStatPoolsListener::OnEquip( Float initialOverheatValue )
{
	if ( auto blackboardHandle = m_blackboard.ToHandle() )
	{
		blackboardHandle->Write( BLACKBOARD_ID( Weapon, OverheatPercentage ), initialOverheatValue );

		Bool isOverheating = false;
		blackboardHandle->Read( BLACKBOARD_ID( Weapon, IsInForcedOverheatCooldown ), isOverheating );

		// If the overheat value is 0 and weapon is still overheating the overheat ended while weapon was unequip, therefore clear forced overheat
		if ( initialOverheatValue == 0 && isOverheating ) {
			blackboardHandle->Write( BLACKBOARD_ID( Weapon, IsInForcedOverheatCooldown ), false );
		}
	}
}

void game::weapon::WeaponOverheatStatPoolsListener::SetWeaponAudio( WeaponAudio* weaponAudio )
{
	m_weaponAudio = weaponAudio;
}

//-------------------------------------------------------------------------------------------------

game::weapon::WeaponStatGroupListener::WeaponStatGroupListener( Uint32 initialSize, red::Function< void() > callback )
	: m_callback(callback)	
	, m_dirty( false )	
{
	m_stats.Reserve( initialSize );
}
void game::weapon::WeaponStatGroupListener::OnStatChanged( const game::StatsObjectID& ownerID, data::StatType statType, Float diff, Float total )
{
	if( !m_dirty )
	{
		// if this is a stat we care about
		if( std::find( m_stats.Begin(), m_stats.End(), statType ) != m_stats.End() )
		{
			m_dirty = true;
		}
	}
}

void game::weapon::WeaponStatGroupListener::RegisterStatType( data::StatType statType )
{		
	// if we are already listening for this state
	if( std::find( m_stats.Begin(), m_stats.End(), statType ) != m_stats.End() )
	{
		return;
	}

	m_stats.PushBack( statType );
}

void game::weapon::WeaponStatGroupListener::Update()
{
	if( m_dirty )
	{
		m_dirty = false;
		if( m_callback )
		{
			m_callback();
		}
	}
}


//-------------------------------------------------------------------------------------------------

game::weapon::Object::Object()
	: m_magazine( {ItemID::NONE(), 0u, 0u, false} )
	, m_transactionSystem( nullptr )
	, m_statsSystem( nullptr )
	, m_questHelper( nullptr )
	, m_weaponDataAnimFeature( CreateHandle< game::weapon::AnimFeature_WeaponData >() )
	, m_reloadAnimFeature( CreateHandle< game::weapon::AnimFeature_LoopableAction >() )
	, m_chargeListener( CreateHandle< game::weapon::WeaponChargeStatPoolsListener >( HandleFromPtr( &GetSharedData() ) ) )
	, m_overheatListener( CreateHandle< game::weapon::WeaponOverheatStatPoolsListener >( HandleFromPtr( &GetSharedData() ) ) )
	, m_ammoRequest( {ItemID::NONE(), false} )
	, m_smartGunHandlerOwner( nullptr )
	, m_consumesAmmo( false )
	, m_isSilenced( false )
	, m_hasScope( false )
	, m_hasPerformedLazyInitialization( false )
{
	m_audio = CreateHandle< WeaponAudio >();
	m_overheatListener->SetWeaponAudio( m_audio.Get() );
}

game::weapon::Object::~Object()
{
	m_audio = nullptr;
}

const WeakHandle< game::data::TriggerMode_Record > game::weapon::Object::GetCurrentTriggerMode() const
{
	return m_currentTriggerMode;
}

game::data::WeaponEvolution game::weapon::Object::GetWeaponEvolution() const
{
	if( auto weaponItemRecordHandle = m_weaponItemRecord.ToHandle() )
	{
		if( auto evolutionHandle = weaponItemRecordHandle->evolutionGet().ToHandle() )
		{
			return evolutionHandle->type;
		}
	}
	return data::WeaponEvolution::Invalid;
}

Bool game::weapon::Object::IsWithinEffectiveRange( Float distance )
{
	Bool inEffectiveRange = true;
	
	if( m_statsSystem )
	{
		const float effectiveRange = m_statsSystem->GetValue( GetEntityID(), data::StatType::EffectiveRange );
		
		// If effective range is not set then treat the range as infinite
		if( effectiveRange > FLT_EPSILON )
		{
			inEffectiveRange = distance < effectiveRange;
		}
		
	}
	
	return inEffectiveRange;
}

Bool game::weapon::Object::IsReloading() const
{
	return m_reloadAnimFeature->IsActive();
}

Float game::weapon::Object::StartReload( const Float durationOverride /*= 0.0f*/ )
{
	Float reloadTime = durationOverride;

	if( reloadTime == 0.0f )
	{
		Int32 currentlyLoadedAmmo = 0;
		if ( auto owner = GetOwner() )
		{
			auto& equipmentMgr = owner->GetGame()->GetSystemPtr< game::IInventoryManager >()->GetEquipmentMgr();
			currentlyLoadedAmmo = equipmentMgr.GetAmmoInMagazine( GetItemID() );
		}

		data::StatType statType = ( currentlyLoadedAmmo == 0 ) ? data::StatType::EmptyReloadTime : data::StatType::ReloadTime;

		reloadTime = m_statsSystem->GetValue( GetEntityID(), statType );
	}

	// Logic
	{
		auto reloadEvent = CreateHandle< events::ReloadStartEvent >( reloadTime );
		QueueEventToSelfAndChildItems( reloadEvent );
		if( auto owner = GetOwner() )
		{
			owner->QueueEvent( reloadEvent );
		}
	}

	// Anim Feature
	{
		m_reloadAnimFeature->SetActive( true );
		m_reloadAnimFeature->SetLoopDuration( reloadTime );
		m_reloadAnimFeature->SetNumLoops( 1u );
		SendAnimFeature( RED_NAME_CONSTEXPR( "Reload" ), m_reloadAnimFeature );
	}

	return reloadTime;
}

void game::weapon::Object::StopReload( ReloadStatus status /*= ReloadStatus::Standard*/ )
{
	// Logic
	{
		auto reloadEvent = status == ReloadStatus::Standard ? Cast< red::Event >( CreateHandle< events::ReloadFinishEvent >( GetItemID() ) ) : Cast< red::Event >( CreateHandle< events::ReloadInterruptedEvent >() );

		QueueEventToSelfAndChildItems( reloadEvent );
		if( auto weaponOwner = GetOwner() )
		{
			weaponOwner->QueueEvent( reloadEvent );
		}
	}

	// Anim Feature
	{
		m_reloadAnimFeature->SetActive( false );
		SendAnimFeature( RED_NAME_CONSTEXPR( "Reload" ), m_reloadAnimFeature );
	}
}

void game::weapon::Object::LazyInit()
{
	// To get information from the animation it needs to be ticked
	// we postpone getting the bone information until they are asked
	if( m_hasPerformedLazyInitialization )
	{
		return;
	}

	auto& components = GetComponents();
	THandle< ent::AnimatedComponent > animatedComponent = nullptr;
	for( int i = 0, limit = components.Size(); i != limit; ++i )
	{
		animatedComponent = Cast< ent::AnimatedComponent >( components[ i ] );
		if( animatedComponent )
		{
			break;
		}
	}

	if( animatedComponent )
	{
		if( !animatedComponent->GetMetaRig() )
		{
			return; // animated components still not initialized
		}
		m_hasPerformedLazyInitialization = true;

		auto offsetExtractor = [ this, &animatedComponent ]( const CName& boneName, Vector4& offset ) -> Bool
		{
			Transform boneTransform;
			// find bone
			ent::AnimIndex boneIndex = animatedComponent->FindTransform( boneName );
			if( !boneIndex.IsValid() )
			{
				GAME_DATA_ERROR( false, Gameplay, "Weapon '%hs' missing '%hs' bone", GetDisplayName().AsChar(), boneName.AsChar() );
				offset = Vector4::ZEROS();
				return false;
			}
			else
			{
				const AnimQsTransform boneTransform = animatedComponent->GetAMetaPoseTransformMs( boneIndex );
				offset = AnimVectorToVector( boneTransform.GetTranslation() );
				return true;
			}
		};

		Vector4 ironSightOffset = Vector4::ZEROS();
		Vector4 muzzleOffset = Vector4::ZEROS();
		Vector4 scopeOffset = Vector4::ZEROS();

		const Bool hasIronSightBone = offsetExtractor( c_ironSightBoneName, ironSightOffset );
		const Bool hasMuzzleBone = offsetExtractor( c_muzzleBoneName, muzzleOffset );
		const Bool hasScopeBone = offsetExtractor( c_scopeBoneName, scopeOffset );

		m_ironSightOffset = ironSightOffset;
		m_muzzleOffset = muzzleOffset;
		if ( m_hasScope && hasScopeBone )
		{
			m_scopeOffset = scopeOffset;
		}
		else
		{
			m_scopeOffset = ironSightOffset;
		}
	}
}


void game::weapon::Object::OnInitialize( const ent::EntityInitializeContext& context, ent::EntityTickRegistrar& tickRegistrar )
{
	TBaseClass::OnInitialize( context, tickRegistrar );

	m_transactionSystem = GetGameSystem< TransactionSystem >();
	m_statsSystem = GetGameSystem< StatsSystem >();
	m_questHelper = GetGameSystem< quest::IQuestsHelper >();

	// needed to call script OnTick for continuous attack
	tickRegistrar.RegisterOnPostTickPhase();
}

void game::weapon::Object::OnAttach( const ent::EntityAttachContext& context )
{
	TBaseClass::OnAttach( context );

	m_audio->SetPhysicsSystem( context.sceneService->GetSystem< world::RuntimeSystemPhysics >() );
    if( world::RuntimeSystemEntity* system = GetSceneSystem< world::RuntimeSystemEntity >() )
    {
        system->ScheduleEntitySetLOD( HandleFromPtr( this ), ent::EntityLOD::Weapon );
    }
}

void game::weapon::Object::OnGatherEventListeners( ent::EventListenerCollector& eventReporter )
{
	TBaseClass::OnGatherEventListeners( eventReporter );
	m_audio->OnGatherEventListeners( eventReporter );
}

void game::weapon::Object::OnRequestComponents( ent::IComponentRequestor& requestor )
{
	TBaseClass::OnRequestComponents( requestor );

	requestor.RequestComponent< projectile::SpawnComponent >( c_projectileComponentName, false );
	//requestor.RequestComponent< WeaponAudioComponent >( c_weaponAudioComponentName, false );
}

void game::weapon::Object::OnTakeControl( const ent::IComponentResolver& resolver )
{
	TBaseClass::OnTakeControl( resolver );

	if( resolver.HasComponent( c_projectileComponentName ) )
	{
		m_projectile = resolver.GetComponent< projectile::SpawnComponent >( c_projectileComponentName );
	}
}

void game::weapon::Object::OnSetOwner( const WeakHandle< game::Object >& owner )
{
	TBaseClass::OnSetOwner( owner );

	// Update the attack data with new owner data
	for( Uint32 i = 0, numAttacks = m_attacks.Size(); i != numAttacks; ++i )
	{
		m_attacks[i]->SetSource( owner );
		m_attacks[i]->SetInstigator( owner );
	}
}

void game::weapon::Object::OnPostPhysicsTick( const Float deltaTime, const tick::Info& tickInfo )
{
	TBaseClass::OnPostPhysicsTick( deltaTime, tickInfo );

	SendWeaponDataAnimFeature();

	TickAttacks( deltaTime, tickInfo );
	TickSmartGun( deltaTime );

	for( Uint32 i = 0; i < m_statRangeListener.Size(); ++i )
	{
		if( m_statRangeListener[i] )
		{
			m_statRangeListener[i]->Update();
		}
	}

	if ( m_audio && m_audio->IsInitialized() )
	{
		m_audio->OnPostPhysicsTick( deltaTime );
	}
}

void game::weapon::Object::SetForcedHigestLODonComponents( Bool value )
{
	if ( value )
	{
		GetSceneSystem< world::RuntimeSystemEntity >()->ScheduleEntitySetLOD( this->HandleFromThis< ent::Entity >(), ent::EntityLOD::ForcedHighestQuality );
	}
	else
	{
		GetSceneSystem< world::RuntimeSystemEntity >()->ScheduleEntityClearLOD( this->HandleFromThis< ent::Entity >(), ent::EntityLOD::ForcedHighestQuality );
	}
}

void game::weapon::Object::OnEquip( const data::TweakDBID& slotID, const WeakHandle<game::Object>& newOwner )
{
	TBaseClass::OnEquip( slotID, newOwner );
	Int32 savedAmmoCount = -1;

	const Bool isLeftSlotWeapon = slotID == TDBID( "AttachmentSlots.WeaponLeft" );
	if( auto newOwnerPtr = newOwner.ToHandle() )
	{
		if( auto itemData = GetItemData().ToHandle() )
		{
			// send weapon spread data to playerAim and recoil data to aimAssist
			if( game::data::IsOfType< game::data::WeaponItem_Record >( itemData->GetID().GetTweakDBID() ) )
			{
				if( m_isOwnedByLocalPlayer )
				{
					const auto playerSystem = GetGameSystem< game::IPlayerSystem >();
					// Preload all effect assets to remove delays due to loading when player fires
					{
						auto preloadEvent = CreateHandle< ent::PreloadAllEffectsEvent >();
						QueueEventToSelfAndChildItems( preloadEvent );

						THandle< weapon::FxPackage > fxPackageHandle = GetFxPackage().ToHandle();
						if( fxPackageHandle )
						{
							fxPackageHandle->CacheResources();
						}
					}

					if( !isLeftSlotWeapon )
					{
						if( auto targetingSystem = newOwnerPtr->GetGame()->GetSystemPtr< game::ITargetingSystem >() )
						{
							if( game::PlayerAim* playerAim = targetingSystem->GetPlayerAim( newOwner ) )
							{
								m_chargeListener->SetPlayerAim( playerAim );
								m_chargeListener->SetItemData( GetItemData() );
								m_chargeListener->SetTargetingSystem( targetingSystem );
								m_chargeListener->SetOwner( newOwner );
								m_chargeListener->SetWeapon( HandleFromThis<game::weapon::Object>() );
								m_chargeListener->SetAnimFeature( m_weaponDataAnimFeature );
							}
						}
						SendWeaponStatsToManagers( itemData );
					}
					

					SetForcedHigestLODonComponents( true );

					

					if( auto inventoryMgr = newOwnerPtr->GetGame()->GetSystemPtr< game::IInventoryManager >() )
					{
						savedAmmoCount = inventoryMgr->GetEquipmentMgr().GetAmmoInMagazine( GetItemID() );

						game::data::TriggerMode triggerMode = inventoryMgr->GetEquipmentMgr().GetCurrentTriggerMode( GetItemID() );
						if( triggerMode != game::data::TriggerMode::Invalid )
						{
							for( auto& mode : m_triggerModes )
							{
								if( mode->type == triggerMode )
								{
									m_currentTriggerMode = mode;
									break;
								}
							}
						}
						GAME_DATA_ERROR( ( m_triggerModes.Empty() == false && m_currentTriggerMode == nullptr ) == false, Weapons, "weapon::Object::OnEquip : Invalid triggermode on %s", GetItemID().GetTweakDBID().ToString().AsChar() );

						inventoryMgr->GetEquipmentMgr().OnWeaponEquip( GetItemID() );
						inventoryMgr->GetEquipmentMgr().TEMP_SetMagCap( m_magazine.m_capacitySnapshot );
						inventoryMgr->GetEquipmentMgr().TEMP_SetTriggerModes( m_triggerModes,
							m_currentTriggerMode ? m_currentTriggerMode->type : data::TriggerMode::Invalid );
						inventoryMgr->GetEquipmentMgr().TEMP_SetEvolution( GetWeaponEvolution() );
					}

					// init statPools
					if( auto statPoolSystem = newOwnerPtr->GetGame()->GetSystemPtr< game::IStatPoolsSystem >() )
					{
						statPoolSystem->RequestRegisteringListener( GetEntityID(), game::data::StatPoolType::WeaponCharge, m_chargeListener );
						statPoolSystem->RequestRegisteringListener( GetEntityID(), game::data::StatPoolType::WeaponOverheat, m_overheatListener );

						if ( statPoolSystem->HasActiveStatPool( GetEntityID(), game::data::StatPoolType::WeaponOverheat ) ) {
							m_overheatListener->OnEquip( statPoolSystem->GetStatPoolValue( GetEntityID(), game::data::StatPoolType::WeaponOverheat ) );
						}
						else
						{
							m_overheatListener->OnEquip();
						}
					}

					UpdateWorldUITextureQuality( c_hipScreenAreaMultiplier );
				}
			}

			m_consumesAmmo = itemData->GetStatValue( data::StatType::NumShotsToFire ) != 0.f;
		}

		// smartguns had to be turned off for player's left hand 
		if( !isLeftSlotWeapon || !m_isOwnedByLocalPlayer )
		{
			EnableSmartGunHandler( newOwner );

			if( m_isOwnedByLocalPlayer )
			{
				RegisterStatListeners( GetItemData().ToHandle() );
			}
		}
	}	

	if( m_magazine.m_ammoID.IsValid() )
	{
		RequestAmmoChange( m_magazine.m_ammoID, true, true, savedAmmoCount );
	}

	auto setActiveWeaponEvent = CreateHandle< events::SetActiveWeaponEvent >( GetItemID(), slotID );
	QueueEventToSelfAndChildItems( setActiveWeaponEvent );
	if( auto owner = GetOwner() )
	{
		owner->QueueEvent( setActiveWeaponEvent );
	}

	GetSharedData().Write( BLACKBOARD_ID( Weapon, TriggerMode ), m_currentTriggerMode );

	if ( game::ObjectPtr owner = GetOwner() )
	{
		// it is possible to be called here on an uninitialised owner.
		const ent::EntityState ownerState = owner->GetState();
		if ( ownerState != ent::EntityState::Uninitialized && ownerState != ent::EntityState::Uninitializing )
		{
			// This means that we haven't been called inside OnUninitialize yet, so we can do as much as possible, but we need to protect the crashing code inside WeaponAudio::OnEquip against the uninitialised entity
			// I am not protecting the caller code because I want to minimize changes at this point of the project.
			// I am also protecting the culprit line, because, we can have only 2 scenarios:
			//		1. if the entity is Uninitialized, then we can't be here, 
			//		2. if the entity is just been created then we try to do as much as we can. In this case we expect the call to m_audio->OnUnequip to clean up after this call
			m_audio->OnEquip( m_isOwnedByLocalPlayer, m_isSilenced );
		}
		else
		{
			// This means that we got called in OnUninitialize before we got called here. Do nothing.
		}
	}
}

void game::weapon::Object::OnUnequip( const data::TweakDBID& slotID )
{
	TBaseClass::OnUnequip( slotID );

	const Bool isLeftSlotWeapon = slotID == TDBID( "AttachmentSlots.WeaponLeft" );

    if( auto owner = GetOwner() )
	{
		if( m_isOwnedByLocalPlayer )
		{
            SetForcedHigestLODonComponents( false );

			// Release preloaded effect assets 
			{
				auto releaseEvent = CreateHandle< ent::ReleasePreloadedEffectsEvent >( );
				QueueEventToSelfAndChildItems( releaseEvent );

				THandle< weapon::FxPackage > fxPackageHandle = GetFxPackage().ToHandle();
				if( fxPackageHandle )
				{
					fxPackageHandle->ReleaseResourceCache();
				}
			}

			const auto itemData = GetItemData().ToHandle();
			if( itemData && game::data::IsOfType< game::data::WeaponItem_Record >( itemData->GetID().GetTweakDBID() ) )
			{
				if( !isLeftSlotWeapon )
				{
					if( auto targetingSystem = owner->GetGame()->GetSystemPtr< game::ITargetingSystem >() )
					{
						if( game::PlayerAim* playerAim = targetingSystem->GetPlayerAim( owner ) )
						{
							if( m_transactionSystem->IsSlotLogicallyEmpty( *owner, slotID ) )
							{
								playerAim->GetWeaponSpreadManager().ResetSpreadData();
								playerAim->GetWeaponSwayManager().ResetData();
							}
						}

						targetingSystem->ResetAimAssistData( owner );
					}
				}

				if( auto inventoryMgr = owner->GetGame()->GetSystemPtr< game::IInventoryManager >() )
				{
					inventoryMgr->GetEquipmentMgr().OnWeaponUnequip( GetItemID() );
				}

				if( auto statPoolSystem = owner->GetGame()->GetSystemPtr< game::IStatPoolsSystem >() )
				{
					m_chargeListener->SetPlayerAim( nullptr );
					statPoolSystem->RequestUnregisteringListener( GetEntityID(), game::data::StatPoolType::WeaponCharge, m_chargeListener );
					statPoolSystem->RequestUnregisteringListener( GetEntityID(), game::data::StatPoolType::WeaponOverheat, m_overheatListener );
				}
			}

			UpdateWorldUITextureQuality( c_defaultScreenAreaMultiplier );
		}
	}

	DisableSmartGunHandler( m_smartGunHandlerOwner );
		

	auto removeActiveWeaponEvent = CreateHandle< events::RemoveActiveWeaponEvent >( GetItemID(), slotID );
	QueueEventToSelfAndChildItems( removeActiveWeaponEvent );
	if( auto owner = GetOwner() )
	{
		owner->QueueEvent( removeActiveWeaponEvent );
	}

	StopContinuousAttack();

	m_audio->OnUnequip();

	UnregisterStatListeners();
}

void game::weapon::Object::OnSpawnComplete()
{
	TBaseClass::OnSpawnComplete();

	// Reset data
	{
		m_currentAttack.Reset();
		m_prevAttack.Reset();
		m_attacks.Clear();
		m_currentTriggerMode.Reset();
		m_triggerModes.Clear();
		m_magazine = Magazine();
	}

	// Add logic
	{
		m_weaponItemRecord = data::GetRecord< data::WeaponItem_Record >( GetItemID().GetTweakDBID() ).ToHandle();

		ForEachItemInHierarchy( [this]( const ItemObject* part ) {
			ExtractConfigurationFromItem( part->GetItemID() );
		} );
	}

	m_audio->Init( this );


	GAME_DATA_ERROR( m_currentAttack, Weapons, "Weapon \"%s\" initialised with no valid attacks.", GetItemID().GetTweakDBID().ToString().AsChar() );
	GAME_DATA_ERROR( m_currentTriggerMode, Weapons, "Weapon \"%s\" initialised with no valid trigger modes.", GetItemID().GetTweakDBID().ToString().AsChar() );
	GAME_DATA_ERROR( m_magazine.m_ammoID.IsValid(), Weapons, "Weapon \"%s\" initialised with no valid ammo type!", GetItemID().GetTweakDBID().ToString().AsChar() );
}

void game::weapon::Object::OnCycleTriggerMode( const events::CycleTriggerModeEvent& evt )
{
	auto nextIter = std::next( std::find( m_triggerModes.Begin(), m_triggerModes.End(), m_currentTriggerMode ) );
	nextIter = ( nextIter == m_triggerModes.End() ) ? m_triggerModes.Begin() : nextIter;

	SetTriggerMode( *nextIter );
}

void game::weapon::Object::OnChangeTriggerMode( const events::ChangeTriggerModeEvent& evt )
{
	auto triggerMode = std::find_if( m_triggerModes.Begin(), m_triggerModes.End(), [&evt]( const THandle< data::TriggerMode_Record >& triggerMode ) {
		return triggerMode->type == evt.GetTriggerMode();
	} );

	if( triggerMode != m_triggerModes.End() )
	{
		SetTriggerMode( *triggerMode );
	}
}

void game::weapon::Object::OnReloadFinished( const events::ReloadFinishEvent& evt )
{
	Int32 ammoCount = STANDARD_RELOAD;
	if( auto itemData = GetItemData().ToHandle() )
	{
		const Int32 statAmmotCount = static_cast< Int32 >( std::floor( itemData->GetStatValue( data::StatType::ReloadAmount ) ) );

		if( statAmmotCount != 0 )
		{
			ammoCount = statAmmotCount;
		}
	}

	DoReload( false, ammoCount );
}

void game::weapon::Object::OnRequestAmmoChange( const events::RequestAmmoChange& evt )
{
	RequestAmmoChange( evt.GetAmmoTypeID(), evt.GetAction() == events::RequestAmmoChange::Instant, false, -1 );
}

void game::weapon::Object::OnShoot( const events::ShootEvent& evt )
{
	auto inventoryMgr = GetOwner()->GetGame()->GetSystemPtr< game::IInventoryManager >();
	
	// Update ammo
	{
		Bool bypass_mag = false;
		Bool infinite_ammo = false;
		if ( inventoryMgr )
		{
			infinite_ammo = HasInfiniteAmmo( inventoryMgr->GetEquipmentMgr() );
		}
		THandle< game::Object > owner = GetOwner();

#ifndef RED_CONFIGURATION_FINAL
		if( owner && owner->IsPlayerControlled() )
		{
			game::IDebugCheatsSystem* cheatSystem = owner->GetGameSystem< game::IDebugCheatsSystem >();
			if( cheatSystem )
			{
				bypass_mag = cheatSystem->HasCheat( owner, game::cheatsystem::BypassMagazine );
				infinite_ammo |= cheatSystem->HasCheat( owner, game::cheatsystem::InfiniteAmmo );
			}
		}
#endif

		bypass_mag |= m_magazine.m_bypassMagazine;

		Uint16 ammoToConsume = evt.GetAmmoCost();
		if( ammoToConsume )
		{
			if( !bypass_mag )
			{
				ScopedWriteLock lock( m_magazineLock );

				const Uint16 currentAmmoCount = m_magazine.m_count;
				ammoToConsume = Min( currentAmmoCount, ammoToConsume );

				const Uint16 newAmmoCount = currentAmmoCount - ammoToConsume;
				m_weaponDataAnimFeature->SetAmmoRemaining( newAmmoCount );
				m_magazine.m_count = newAmmoCount;

				if( IsOwnedByLocalPlayer() )
				{
					if( inventoryMgr )
					{
						inventoryMgr->GetEquipmentMgr().OnShoot( GetItemID(), m_magazine.m_count );
					}
				}
			}
			else
			{
				m_weaponDataAnimFeature->SetAmmoRemaining( m_transactionSystem->GetItemQuantity( *GetOwner(), m_magazine.m_ammoID ) );
			}

			GetSharedData().Write( BLACKBOARD_ID( Weapon, MagazineAmmoCount ), GetMagazineAmmoCount() );

			if( !infinite_ammo )
			{
				const Bool success = m_transactionSystem->RemoveItem( *GetOwner(), m_magazine.m_ammoID, ammoToConsume );
				RED_WARNING( success, "Failed to remove ammo from the inventory - AmmoID: %hs Quantity: %u", m_magazine.m_ammoID.GetTweakDBID().ToString().AsChar(), ammoToConsume );
			}
		}
	}

	RefreshWeaponDataAnimFeature();

	if( m_currentAttack )
	{
		AttackContext context;
		context.m_isPlayerOwned = IsOwnedByPlayer();
		context.m_hasMuzzle = GetSlotWorldPosition( c_muzzleSlotName, context.m_muzzleWorldPosition );
		context.m_chargePercentage = evt.GetChargePercentage();
		context.m_projectileParams = evt.GetProjectileParams();
		context.m_aimDataWrapper = evt.GetAimDataWrapper();
		context.m_fxPackage = m_fxPackage;
		context.m_shootTime = evt.GetShootTime();
		context.m_isTargetPlayer = false;

		if( auto targetHandle = evt.GetTarget().ToHandle() )
		{
			context.m_isTargetPlayer = targetHandle->IsPlayerControlled();
		}

		Bool success = false;
		for( Uint8 i = 0; i < evt.GetProjectilesPerShot(); ++i )
		{
			// Cache the spread for this shot so that we can call aimDataWrapper.GetAim multiple times during the shot and use the same spread each time
			context.m_aimDataWrapper.GenerateSpreadAngle();

			m_currentAttack->SetSource( evt.GetInstigator() ); // TODO: Revisit this! This was added to fix the turret scenario in Q104 (https://jira.cdprojektred.com/browse/CYB-429470).
			m_currentAttack->PrepareAttack( evt.GetInstigator() );
			m_currentAttack->SetAttackData( context );
			success |= m_currentAttack->StartAttack();
		}

		if( success )
		{
			evt.GetAimDataWrapper().IncreaseSpread();
		}
	}


	Float cycleTime = 0.5f;

	if ( auto itemData = GetItemData().ToHandle() )
	{
		cycleTime = itemData->GetStatValue( data::StatType::CycleTime );
	}

	WeaponAudio::ShootInfo shootInfo;
	shootInfo.shootEvent = &evt;
	shootInfo.ammoCount = m_magazine.m_count;
	shootInfo.fireMode = m_currentTriggerMode->type;
	shootInfo.cycleTime = cycleTime;
	m_audio->OnShoot( shootInfo );
}

void game::weapon::Object::OnOwnerAim( const events::OwnerAimEvent& evt )
{
	if( m_isOwnedByLocalPlayer )
	{
		if( evt.m_isAiming )
		{
			UpdateWorldUITextureQuality( c_aimingScreenAreaMultiplier );
		}
		else
		{
			UpdateWorldUITextureQuality( c_hipScreenAreaMultiplier );
		}
	}
}

bool game::weapon::Object::IsOwnedByPlayer() const
{
	if( world::IsServer() )
	{
		return GetOwner()->IsPlayerControlled();
	}
	return IsOwnedByLocalPlayer();
}

net::Time game::weapon::Object::CalculateLagCompensationShootTime() const
{
	if( world::IsServer() && IsOwnedByPlayer() )
	{
		const net::Time clientToServerLatency = GetOwner()->GetPlayerSocket().GetLatency();
		const net::Time interpolationDelay = net::Time::FromMilliSecs( Config::cvInterpolationDelayMilliSecs.Get() );
		return GetGame()->GetNetSession()->GetSessionTime() - clientToServerLatency - interpolationDelay;
	}
	return net::Time::Invalid;
}

net::Time game::weapon::Object::CalculateLagCompensationShootTimeMuppet() const
{
	if( world::IsServer() && IsOwnedByPlayer() )
	{
		const net::Time clientToServerLatency = GetOwner()->GetPlayerSocket().GetLatency();
		const net::Time interpolationDelay = net::Time::FromMilliSecs( Config::cvInterpolationDelayMilliSecs.Get() );
		// HACK: The 200 extra ms just happens to line up and is not a real solution.
		// The Muppet does need its own LagCompensation method though.
		return GetGame()->GetNetSession()->GetSessionTime() - clientToServerLatency - interpolationDelay - net::Time::FromMilliSecs( 200 );
	}
	return net::Time::Invalid;
}

void game::weapon::Object::Cpo_OverrideAmmoCount( Uint32 ammoCount, Bool updateBlackboard )
{
	m_magazine.m_count = ammoCount;

	if ( updateBlackboard )
	{
		GetSharedData().Write( BLACKBOARD_ID( Weapon, MagazineAmmoCount ), ammoCount );

		if ( auto inventoryMgr = GetOwner()->GetGame()->GetSystemPtr< game::IInventoryManager >() )
		{
			inventoryMgr->GetEquipmentMgr().OnReload( GetItemID(), ammoCount, GetMagazineAmmoID() );
		}

		RefreshWeaponDataAnimFeature();
	}
}

void game::weapon::Object::Cpo_SetAttack( const TweakDBID& attackRecordID )
{
	SetAttack( attackRecordID );
}

void game::weapon::Object::SetTriggerMode( const THandle< data::TriggerMode_Record >& triggerMode )
{
	m_currentTriggerMode = triggerMode;

	GetSharedData().Write( BLACKBOARD_ID( Weapon, TriggerMode ), triggerMode );

	RefreshWeaponDataAnimFeature();

	if( IsOwnedByLocalPlayer() )
	{
		if( auto inventoryMgr = GetOwner()->GetGame()->GetSystemPtr< game::IInventoryManager >() )
		{
			inventoryMgr->GetEquipmentMgr().OnSwitchTriggerMode( triggerMode->type );
		}
	}
}

void game::weapon::Object::ExtractConfigurationFromItem( const ItemID& itemID )
{
	if( auto itemRecord = data::GetRecord< data::Item_Record >( itemID.GetTweakDBID() ).ToHandle() )
	{
		// TODO: Refactor this - temp-as-truck
		if( itemRecord->tags().Exist( RED_NAME_CONSTEXPR( "BypassMagazine" ) ) )
		{
			m_magazine.m_bypassMagazine = true;
		}
	}

	if( auto weaponItemRecord = data::GetRecord< data::WeaponItem_Record >( itemID.GetTweakDBID() ).ToHandle() )
	{
		SetupFxPackage( weaponItemRecord->fxPackageGet(), m_fxPackage );
		SetupFxPackage( weaponItemRecord->fxPackageQuickMeleeGet(), m_fxPackageQuickMelee );
		

		// Trigger Modes
		{
			red::DynArray< WeakHandle< data::TriggerMode_Record > > triggerModeList{red::PoolFrame()};
			weaponItemRecord->triggerModesGet( triggerModeList );
			m_triggerModes.Reserve( triggerModeList.Size() );
			for( const auto& triggerMode : triggerModeList )
			{
				if( auto triggerModeHandle = triggerMode.ToHandle() )
				{
					red::alg::PushBackUnique( m_triggerModes, triggerModeHandle );
				}
			}
			if( !m_currentTriggerMode && !m_triggerModes.Empty() )
			{
				m_currentTriggerMode = m_triggerModes.Front();
			}
		}

		// Attack
		{
			AttackInitContext initContext;
			initContext.m_source = GetOwner();
			initContext.m_instigator = GetOwner();
			initContext.m_weapon = HandleFromPtr( this );

			red::DynArray< WeakHandle< data::Attack_Record > > attackRecords{red::PoolFrame()};
			weaponItemRecord->attacksGet( attackRecords );
			for( const auto& attackRecord : attackRecords )
			{
				initContext.m_record = attackRecord;
				if( auto attack = IAttack::Create( initContext ) )
				{
					m_attacks.PushBack( attack );
				}
			}
			if( !m_currentAttack && !m_attacks.Empty() )
			{
				SetAttack( m_attacks.Front() );
			}
		}

		// Ammo
		{
#ifdef RED_CONFIGURATION_FINAL
			const game::ItemID itemId = game::ItemID::CreateFromTDBID( weaponItemRecord->ammo() );
#else
			const game::ItemID itemId = game::ItemID::CreateFromTDBID( weaponItemRecord->ammo(), game::ItemID::CreateFromTDBID_ReservedRngSeedValue(), "weapon::Object::ExtractConfigurationFromItem - entity : %hs , weaponID : %hs", GetEntityID().ToDebugString().AsChar(), itemID.GetTweakDBID().ToString().AsChar() );
#endif //RED_CONFIGURATION_FINAL

			m_magazine.m_ammoID = itemId;
		}
	}
}

void game::weapon::Object::SetupFxPackage( const WeakHandle< data::WeaponFxPackage_Record >& fxPackageData, THandle< FxPackage >& outPackage)
{
	const THandle< const data::WeaponFxPackage_Record > fxPackageDataHandle = fxPackageData.ToHandle();
	if( fxPackageDataHandle )
	{
		{
			RED_SCOPE_SHARED_LOCK( s_fxPackagesMapLock );
			auto it = s_fxPackagesMap.Find( fxPackageDataHandle.Get() );
			if( it != s_fxPackagesMap.End() )
			{
				outPackage = it.Value();
				return;
			}
		}

		// Crate new package.
		auto newFxPackage = CreateHandle< FxPackage >();
		newFxPackage->SetData( *fxPackageDataHandle );

		{
			RED_SCOPE_LOCK( s_fxPackagesMapLock );
			auto it = s_fxPackagesMap.Find( fxPackageDataHandle.Get() );
			if( it != s_fxPackagesMap.End() )
			{
				// Somebody added the package so use other. The new will be released after out of scope.
				outPackage = it.Value();
			}
			else
			{
				// Use and save the new one for others.
				s_fxPackagesMap.Insert( fxPackageDataHandle.Get(), newFxPackage );
				outPackage = newFxPackage;
			}
		}
	}
	else
	{
		outPackage = nullptr;
	}
}

void game::weapon::Object::DoReload( const Bool foreceCount, const Int32 savedAmmoCount /*= STANDARD_RELOAD*/ )
{
	if( HasAmmoChangeRequest() )
	{
		ServiceAmmoChangeRequest();
	}

	RefillMagazine( foreceCount, savedAmmoCount );
}

void game::weapon::Object::RefillMagazine( const Bool foreceCount, const Int32 savedAmmoCount /*= STANDARD_RELOAD*/ )
{
	if( !GetOwner() )
	{
		return;
	}

	THandle< game::ItemData > itemDataHandle = GetItemData().ToHandle();
	if( !itemDataHandle )
	{
		return;
	}

	const Uint32 magazineCapacity = Uint32( itemDataHandle->GetStatValue( game::data::StatType::MagazineCapacity ) );
	const Uint32 capacity = Max( magazineCapacity, 1u );
	Uint32 remainingAmmo = Uint32( m_transactionSystem->GetItemQuantity( *GetOwner(), GetMagazineAmmoID() ) );
	auto& equipmentMgr = GetOwner()->GetGame()->GetSystemPtr< game::IInventoryManager >()->GetEquipmentMgr();

	if( IsOwnedByLocalPlayer() )
	{
		Uint32 totalLoadedAmmo = equipmentMgr.GetTotalLoadedAmmoOfType( GetMagazineAmmoID() );
		Int32 currentlyLoadedAmmo = equipmentMgr.GetAmmoInMagazine( GetItemID() );

		if( currentlyLoadedAmmo != -1 )
		{
			totalLoadedAmmo = Max< Uint32 >( totalLoadedAmmo - currentlyLoadedAmmo, 0 );
		}

		remainingAmmo = Max< Uint32 >( remainingAmmo - totalLoadedAmmo, 0 );
	}

	m_magazine.m_capacitySnapshot = capacity;
	GetSharedData().Write( BLACKBOARD_ID( Weapon, MagazineAmmoCapacity ), capacity );

	const Bool infiniteAmmo = HasInfiniteAmmo( equipmentMgr );
	if ( infiniteAmmo )
	{
		remainingAmmo = capacity;
	}

	{
		ScopedWriteLock lock( m_magazineLock );
		if( savedAmmoCount == STANDARD_RELOAD )
		{
			m_magazine.m_count = Min( capacity, remainingAmmo );
		}
		else
		{
			if( true == foreceCount )
			{
				m_magazine.m_count = Min< Uint32 >( savedAmmoCount, capacity );
			}
			else
			{
				m_magazine.m_count = Min< Uint32 >( m_magazine.m_count + savedAmmoCount, capacity );
			}
		}

		GetSharedData().Write( BLACKBOARD_ID( Weapon, MagazineAmmoCount ), m_magazine.m_count );
	}

	if( IsOwnedByLocalPlayer() )
	{
		equipmentMgr.OnReload( GetItemID(), m_magazine.m_count, GetMagazineAmmoID() );
	}

	RefreshWeaponDataAnimFeature();
}

void game::weapon::Object::ServiceAmmoChangeRequest()
{
	// Remove stats
	if( m_magazine.m_ammoID.IsValid() )
	{
		while( !m_ammoStats.Empty() )
		{
			m_statsSystem->RemoveModifier( GetEntityID(), m_ammoStats.Back() );
			m_ammoStats.PopBack();
		}
		m_ammoStats.Clear();
	}

	const auto ammoID = m_ammoRequest.m_ammoID;
	m_magazine.m_ammoID = ammoID;
	GetSharedData().Write( BLACKBOARD_ID( Weapon, MagazineAmmoID ), ammoID );

	math::Random rand;
	// Add stats
	if( auto data = data::GetRecord< data::Item_Record >( ammoID.GetTweakDBID() ).ToHandle() )
	{
		m_ammoStats.Reserve( data->statModifiers().Size() );
		for( auto statModID : data->statModifiers() )
		{
			auto statModRec = data::GetRecord< data::StatModifier_Record >( statModID );


			if( auto modifier = StatsSystem::CreateModifier( statModRec, rand ) )
			{
				if( m_statsSystem->AddModifier( GetEntityID(), modifier ) )
				{
					m_ammoStats.PushBack( modifier );
				}
			}
		}
	}
}

void game::weapon::Object::RequestAmmoChange( const ItemID& ammoID, const Bool instant, const Bool foreceCount, const Int32 savedAmmoCount /*= STANDARD_RELOAD*/ )
{
	m_ammoRequest.m_ammoID = ammoID;
	m_ammoRequest.m_instant = instant;

	if( instant )
	{
		DoReload( foreceCount, savedAmmoCount );
	}
}

void game::weapon::Object::OnSmartGunEquipped()
{
	if( const THandle< game::Object >& smartGunHandlerOwner = m_smartGunHandlerOwner.ToHandle() )
	{
		if( smartGunHandlerOwner->IsPlayerControlledByLocalPeer() )
		{
			if( auto ts = smartGunHandlerOwner->GetGame()->GetSystemPtr< game::ITargetingSystem >().Get() )
			{
				if( auto itemData = GetItemData().ToHandle() )
				{
					SendWeaponStatsToManagers( itemData, EStatGroupTypes::Smart );
					ts->OnSmartGunEquipped( smartGunHandlerOwner );
					ts->SetVisibleTargetRange( smartGunHandlerOwner, itemData->GetStatValue( data::StatType::SmartGunTargetAcquisitionRange ) );
				}
			}
		}
		else
		{
			if( auto itemData = GetItemData().ToHandle() )
			{
				auto npcSmartGunParams = CreateSmartGunParametersDataForNPC( itemData );
				auto npcSmartGunSightParams = CreateSmartGunNPCSightAreaData( itemData );

				sense::CSenseManager* senseManager = GetGameSystem< sense::CSenseManager >();

				auto entityRegistry = GetGame()->GetScene().GetSystem< world::RuntimeEntityRegistry >();

				m_smartGunNPCHandler = red::CreateUniquePtr< game::smartGun::NPCHandler >( *HandleFromThis< game::weapon::Object >().Get(),
																							npcSmartGunParams, npcSmartGunSightParams,
																							*senseManager, *entityRegistry );
			}
		}
	}
}

void game::weapon::Object::OnSmartGunUnequipped()
{
	if( const THandle< game::Object >& smartGunHandlerOwner = m_smartGunHandlerOwner.ToHandle() )
	{
		if( smartGunHandlerOwner->IsPlayerControlledByLocalPeer() )
		{
			if( auto ts = smartGunHandlerOwner->GetGame()->GetSystemPtr< game::ITargetingSystem >().Get() )
			{
				ts->OnSmartGunUnequipped( smartGunHandlerOwner );
				ts->ResetVisibleTargetRange( smartGunHandlerOwner );
			}
		}
		else
		{
			m_smartGunNPCHandler.Reset();
		}
	}
}

void game::weapon::Object::RefreshWeaponDataAnimFeature()
{
	const auto ammoCount = GetMagazineAmmoCount();
	m_weaponDataAnimFeature->SetAmmoRemaining( ammoCount );
	m_weaponDataAnimFeature->SetMagazineFull( ammoCount == GetMagazineAmmoCapacity() );
	m_weaponDataAnimFeature->SetTriggerMode( m_currentTriggerMode ? m_currentTriggerMode->type : data::TriggerMode::Invalid );
	if( auto itemData = GetItemData().ToHandle() )
	{
		if( auto owner = GetOwner() )
		{
			m_weaponDataAnimFeature->SetCycleTime( itemData->GetStatValue( data::StatType::CycleTime ) );
		}
	}
}

void game::weapon::Object::SendWeaponDataAnimFeature()
{
	if( m_weaponDataAnimFeature->IsDirty() )
	{
		if( auto itemRecord = m_weaponItemRecord.ToHandle() )
		{
			SendAnimFeature( itemRecord->animFeatureName, m_weaponDataAnimFeature );
		}
		m_weaponDataAnimFeature->ClearDirty();
	}
}

void game::weapon::Object::SendAnimFeature( const CName tag, THandle< anim::AnimFeature > animFeature )
{
	auto animEvent = CreateHandle< ent::AnimInputSetterAnimFeature >( tag, animFeature );
	if( auto owner = GetOwner() )
	{
		owner->QueueEvent( animEvent );
	}
	QueueEventToSelfAndChildItems( animEvent );
}

void game::weapon::Object::UpdateWorldUITextureQuality( Float screenAreaMultiplier )
{
	QueueEvent( CreateHandle< game::ui::ScreenAreaMultiplierChangeEvent >( screenAreaMultiplier ) );
}

Uint32 game::weapon::Object::GetMagazineAmmoCapacity() const
{
	return m_magazine.m_capacitySnapshot;
}

Uint32 game::weapon::Object::GetMagazineAmmoCount() const
{
	Uint32 count = 0u;
	{
		ScopedReadLock lock( m_magazineLock );
		count = m_magazine.m_count;
	}
	return count;
}

game::ItemID game::weapon::Object::GetMagazineAmmoID() const
{
	return m_magazine.m_ammoID;
}

void game::weapon::Object::TickAttacks( const Float deltaTime, const tick::Info& tickInfo )
{
	if( m_currentAttack )
	{
		if( m_isContinuousAttack )
		{
			rtti::CallFunction( m_currentAttack.Get(), RED_NAME_CONSTEXPR( "OnTick" ), HandleFromThis< game::weapon::Object >() );
		}

		m_currentAttack->Tick( deltaTime, tickInfo );
	}

	//Previews can get stuck on when attacks change.. Make sure the previous attack gets a tick to turn off its preview
	if( m_prevAttack &&  m_prevAttack->IsPreviewActive() )
	{
		m_prevAttack->SetPreviewActive( false );
		m_prevAttack->Tick( deltaTime, tickInfo );
	}
}

void game::weapon::Object::TickSmartGun( const Float deltaTime )
{
	if( GetWeaponEvolution() == data::WeaponEvolution::Smart )
	{
		if( m_smartGunNPCHandler )
		{
			m_smartGunNPCHandler->UpdateLockingTimer( deltaTime );
			m_smartGunNPCHandler->UpdateTargetTracking();
		}
	}
}

void game::weapon::Object::EnableSmartGunHandler( const WeakHandle< game::Object >& smartGunHandlerOwner )
{
	if( GetWeaponEvolution() == data::WeaponEvolution::Smart )
	{
		const THandle< game::Object >& currOwner = m_smartGunHandlerOwner.ToHandle();
		const THandle< game::Object >& newOwner = ToHandleAttached( smartGunHandlerOwner );

		if( currOwner != newOwner )
		{
			OnSmartGunUnequipped();
			m_smartGunHandlerOwner = newOwner;
			OnSmartGunEquipped();
		}
	}
}

void game::weapon::Object::DisableSmartGunHandler( const WeakHandle< game::Object >& smartGunHandlerOwner )
{
	if( GetWeaponEvolution() == data::WeaponEvolution::Smart )
	{
		if( smartGunHandlerOwner.ToHandle() == m_smartGunHandlerOwner.ToHandle() )
		{
			OnSmartGunUnequipped();
			m_smartGunHandlerOwner = nullptr;
		}
	}
}

void game::weapon::Object::OnDataInitialized()
{
	// Keep order here, the flow is the following:
	// 1-Initialize weapon parts data
	// 2-Gather their stats
	// 3-Combine/Add them to the weapon

	TBaseClass::OnDataInitialized();

	if ( auto hItemData = GetItemDataHandle() )
	{
		if ( const auto* powerModulePart = hItemData->GetPartInSlot( TDBID( "AttachmentSlots.PowerModule" ) ) )
		{
			if ( auto powerModuleRecord = powerModulePart->GetStaticData().ToHandle() )
			{
				const auto* foundSilencedTag = powerModuleRecord->tags().FindPtr( RED_NAME_CONSTEXPR( "Silenced" ) );
				m_isSilenced = foundSilencedTag != nullptr;
			}
		}

		m_hasScope = hItemData->HasPartInSlot( TDBID( "AttachmentSlots.Scope" ) );

		m_hasPerformedLazyInitialization = false;
	}
}


void game::weapon::Object::RegisterStatListeners( const THandle< ItemData > itemData )
{
	if( m_statsSystem && itemData )
	{
		m_itemsStatID = itemData->GetStatsBundle()->GetStatsID();
		for( Uint32 i = 0; i < m_statRangeListener.Size(); ++i )
		{			
			if( m_statRangeListener[i] )
			{
				m_statsSystem->RegisterListener( m_itemsStatID, m_statRangeListener[i] );
			}
			
		}
	}
}
void game::weapon::Object::UnregisterStatListeners()
{
	if( m_statsSystem )
	{
		for( Uint32 i = 0; i < m_statRangeListener.Size(); ++i )
		{
			if( m_statRangeListener[i] )
			{
				m_statsSystem->UnregisterListener( m_itemsStatID, m_statRangeListener[i] );
			}
		}
	}
}

void game::weapon::Object::SendWeaponStatsToManagers( const THandle< ItemData > itemData, EStatGroupTypes specificManager )
{	
	THandle< game::Object > owner = GetOwner();
	if( !owner )
	{
		return;
	}

	if( itemData )
	{
		// send weapon spread data to playerAim and recoil data to aimAssist
		if( game::data::IsOfType< game::data::WeaponItem_Record >( itemData->GetID().GetTweakDBID() ) )
		{
			if( auto targetingSystem = owner->GetGame()->GetSystemPtr< game::ITargetingSystem >() )
			{			
				bool updateAll = specificManager == EStatGroupTypes::All;
				if( game::PlayerAim* playerAim = targetingSystem->GetPlayerAim( owner ) )
				{
					if( updateAll || specificManager == EStatGroupTypes::Spread )
					{
						playerAim->GetWeaponSpreadManager().SetSpreadData( CreateSpreadData( itemData ) );
					}
					if( updateAll || specificManager == EStatGroupTypes::Sway )
					{
						playerAim->GetWeaponSwayManager().SetData( CreateSwayData( itemData ) );
					}
				}

				if( updateAll || specificManager == EStatGroupTypes::Recoil )
				{				
					targetingSystem->SetAimAssistData( owner, CreateRecoilData( itemData ), HandleFromThis< game::weapon::Object >() );
				}

				if( ( updateAll || specificManager == EStatGroupTypes::Smart ) && GetWeaponEvolution() == data::WeaponEvolution::Smart )
				{
					if( const THandle< game::Object >& smartGunHandlerOwner = m_smartGunHandlerOwner.ToHandle() )
					{
						targetingSystem->SetSmartGunParams( smartGunHandlerOwner, CreateSmartGunParametersData( itemData, smartGunHandlerOwner ) );
					}
				}
			}
		}
	}
}

// Returns the listener only if it was created with this call
THandle< game::weapon::WeaponStatGroupListener > game::weapon::Object::CreateStatGroupListenerIfNeeded( EStatGroupTypes statGroup, Uint32 initialSize )
{
	Uint32 index = (Uint32)statGroup;
	RED_ASSERT( index < (Uint32)EStatGroupTypes::Count );

	if( m_statsSystem )
	{
		if( m_statRangeListener.Empty() )
		{
			m_statRangeListener.Resize( (Uint32)EStatGroupTypes::Count );
		}
		if( !m_statRangeListener[index] )
		{
			m_statRangeListener[index] = CreateHandle< WeaponStatGroupListener >( initialSize, [this, statGroup]()
			{
				if( auto itemData = GetItemData().ToHandle() )
				{
					SendWeaponStatsToManagers( itemData, statGroup );
				}
			} );
			
			return m_statRangeListener[index];
		}
	}

	return nullptr;
}

Float game::weapon::Object::GetStatValue( const THandle< ItemData >& itemData, data::StatType stat, const THandle< WeaponStatGroupListener >& listener )
{
	if( listener )
	{	
		listener->RegisterStatType( stat );
	}

	return itemData->GetStatValue( stat );

}


game::SpreadData game::weapon::Object::CreateSpreadData( const WeakHandle< ItemData >& itemDataWH )
{
	SpreadData spreadData;

	if( auto itemData = itemDataWH.ToHandle() )
	{
		// create the listener if we have not already, returns null if it was already created.
		// If the creation was needed, we then populate it will all the needed stats
		const THandle< WeaponStatGroupListener > newListener = CreateStatGroupListenerIfNeeded( EStatGroupTypes::Spread, 35 );

		spreadData.m_defaultVal = Vector2( GetStatValue( itemData, data::StatType::SpreadDefaultX, newListener ), GetStatValue( itemData, data::StatType::SpreadDefaultY, newListener ) );
		spreadData.m_minVal = Vector2( GetStatValue( itemData, data::StatType::SpreadMinX, newListener ), GetStatValue( itemData, data::StatType::SpreadMinY, newListener ) );
		spreadData.m_maxVal = Vector2( GetStatValue( itemData, data::StatType::SpreadMaxX, newListener ), GetStatValue( itemData, data::StatType::SpreadMaxY, newListener ) );
		spreadData.m_changePerShot = GetStatValue( itemData, data::StatType::SpreadChangePerShot, newListener );
		spreadData.m_useSpreadInAds = GetStatValue( itemData, data::StatType::SpreadUseInAds, newListener );
		spreadData.m_defaultValAds = Vector2( GetStatValue( itemData, data::StatType::SpreadAdsDefaultX, newListener ), GetStatValue( itemData, data::StatType::SpreadAdsDefaultY, newListener ) );
		spreadData.m_minValAds = Vector2( GetStatValue( itemData, data::StatType::SpreadAdsMinX, newListener ), GetStatValue( itemData, data::StatType::SpreadAdsMinY, newListener ) );
		spreadData.m_maxValAds = Vector2( GetStatValue( itemData, data::StatType::SpreadAdsMaxX, newListener ), GetStatValue( itemData, data::StatType::SpreadAdsMaxY, newListener ) );
		spreadData.m_changePerShotAds = GetStatValue( itemData, data::StatType::SpreadAdsChangePerShot, newListener );
		spreadData.m_resetSpeed = GetStatValue( itemData, data::StatType::SpreadResetSpeed, newListener );
		spreadData.m_resetTimeThreshold = GetStatValue( itemData, data::StatType::SpreadResetTimeThreshold, newListener );
		spreadData.m_randomizeOriginPoint = GetStatValue( itemData, data::StatType::SpreadRandomizeOriginPoint, newListener );
		spreadData.m_useCircularSpread = GetStatValue( itemData, data::StatType::SpreadUseCircularSpread, newListener );
		spreadData.m_useEvenDistribution = GetStatValue( itemData, data::StatType::SpreadUseEvenDistribution, newListener );
		spreadData.m_evenDistributionJitterSize = GetStatValue( itemData, data::StatType::SpreadEvenDistributionJitterSize, newListener ); // TODO: Uncomment after re-generating tweakDB!
		spreadData.m_evenDistributionRowCount = GetStatValue( itemData, data::StatType::SpreadEvenDistributionRowCount, newListener );
		spreadData.m_projectilesPerShot = GetStatValue( itemData, data::StatType::ProjectilesPerShot, newListener );
		spreadData.m_zeroSpreadOnFirstShot = GetStatValue( itemData, data::StatType::SpreadZeroOnFirstShot, newListener );

		spreadData.m_fastSpeedRange = Vector2( GetStatValue( itemData, data::StatType::SpreadFastSpeedMin, newListener ), GetStatValue( itemData, data::StatType::SpreadFastSpeedMax, newListener ) );
		spreadData.m_fastSpeedRangeAds = Vector2( GetStatValue( itemData, data::StatType::SpreadAdsFastSpeedMin, newListener ), GetStatValue( itemData, data::StatType::SpreadAdsFastSpeedMax, newListener ) );
		spreadData.m_fastSpeedAdd = Vector2( GetStatValue( itemData, data::StatType::SpreadFastSpeedMinAdd, newListener ), GetStatValue( itemData, data::StatType::SpreadFastSpeedMaxAdd, newListener ) );
		spreadData.m_fastSpeedAddAds = Vector2( GetStatValue( itemData, data::StatType::SpreadAdsFastSpeedMinAdd, newListener ), GetStatValue( itemData, data::StatType::SpreadAdsFastSpeedMaxAdd, newListener ) );

		spreadData.m_crouchDefaultMult = GetStatValue( itemData, data::StatType::SpreadCrouchDefaultMult, newListener );
		spreadData.m_crouchMaxMult = GetStatValue( itemData, data::StatType::SpreadCrouchMaxMult, newListener );

	}

	return spreadData;
}

void SetRecoilStyleFlag( bool enable, game::RecoilStyle& data, game::RecoilStyle flag )
{
	if( enable )
	{
		data = static_cast<game::RecoilStyle>(static_cast<Uint32>(data) | static_cast<Uint32>(flag));
	}
	else
	{
		data = static_cast<game::RecoilStyle>(static_cast<Uint32>(data) & ~static_cast<Uint32>(flag));
	}
}



game::RecoilData game::weapon::Object::CreateRecoilData( const WeakHandle< ItemData >& itemDataWH )
{
	RecoilData recoilData;	

	if( auto itemData = itemDataWH.ToHandle() )
	{
		// create the listener if we have not already, returns null if it was already created.
		// If the creation was needed, we then populate it will all the needed stats
		const THandle< WeaponStatGroupListener >& newListener = CreateStatGroupListenerIfNeeded( EStatGroupTypes::Recoil, 55 );

		RecoilContextData& data = recoilData.m_default;
		data.m_isValid = true;

		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableLinearX, newListener ) > 0.0f, data.m_yawStyle, RecoilStyle::Linear );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableLinearY, newListener ) > 0.0f, data.m_pitchStyle, RecoilStyle::Linear );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableCycleX, newListener ) > 0.0f, data.m_yawStyle, RecoilStyle::Cyclic );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableCycleY, newListener ) > 0.0f, data.m_pitchStyle, RecoilStyle::Cyclic );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableScaleX, newListener ) > 0.0f, data.m_yawStyle, RecoilStyle::Scale );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableScaleY, newListener ) > 0.0f, data.m_pitchStyle, RecoilStyle::Scale );

		data.m_allowSway = GetStatValue( itemData, data::StatType::RecoilAllowSway, newListener );

		data.m_startDir = GetStatValue( itemData, data::StatType::RecoilDir, newListener );
		data.m_alternateDir = GetStatValue( itemData, data::StatType::RecoilAlternateDir, newListener );
		data.m_angle = GetStatValue( itemData, data::StatType::RecoilAngle, newListener );
		data.m_kickMin = GetStatValue( itemData, data::StatType::RecoilKickMin, newListener );
		data.m_kickMax = GetStatValue( itemData, data::StatType::RecoilKickMax, newListener );
		data.m_holdDuration = GetStatValue( itemData, data::StatType::RecoilHoldDuration, newListener );

		data.m_recoilTime = Max( FLT_EPSILON, GetStatValue( itemData, data::StatType::RecoilTime, newListener ) );
		data.m_recoveryTime = Max( FLT_EPSILON, GetStatValue( itemData, data::StatType::RecoilRecoveryTime, newListener ) );
		data.m_minRecoverySpeed = GetStatValue( itemData, data::StatType::RecoilRecoveryMinSpeed, newListener );
		data.m_recoilDelay = GetStatValue( itemData, data::StatType::RecoilDelay, newListener );

		data.m_maxRecoilDistance = GetStatValue( itemData, data::StatType::RecoilMaxLength, newListener );
		data.m_cycleTime = GetStatValue( itemData, data::StatType::RecoilCycleTime, newListener );
		data.m_cycleSize = GetStatValue( itemData, data::StatType::RecoilCycleSize, newListener );
		data.m_scaleTime = GetStatValue( itemData, data::StatType::RecoilScaleTime, newListener );
		data.m_scaleMax = GetStatValue( itemData, data::StatType::RecoilScaleMax, newListener );

		data.m_driftRandomRangeMin = GetStatValue( itemData, data::StatType::RecoilDriftRandomRangeMin, newListener );
		data.m_driftRandomRangeMax = GetStatValue( itemData, data::StatType::RecoilDriftRandomRangeMax, newListener );
		data.m_recoilMagForFullDrift = GetStatValue( itemData, data::StatType::RecoilMagForFullDrift, newListener );


		if( GetStatValue( itemData, data::StatType::RecoilDirPlanSequence, newListener ) > 0.f )
		{
			data.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::Sequence;
		}
		else if( GetStatValue( itemData, data::StatType::RecoilDirPlanCycleRandDir, newListener ) > 0.f )
		{
			data.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::CycleRandDir;
		}
		else if( GetStatValue( itemData, data::StatType::RecoilDirPlanCycleRandRangeDir, newListener ) > 0.f )
		{
			data.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::CycleRandDirRange;
		}
		else
		{
			data.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::FirstDirectionOnly;
		}

		data.m_linearDirectionUpdateRange = Vector2( GetStatValue( itemData, data::StatType::LinearDirectionUpdateMin, newListener ),
													 GetStatValue( itemData, data::StatType::LinearDirectionUpdateMax, newListener ) );



		// ADS settings. 
		RecoilContextData& dataADS = recoilData.m_ADS;
		dataADS.m_isValid = static_cast<Bool>(GetStatValue( itemData, data::StatType::RecoilUseDifferentStatsInADS, newListener ));

		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableLinearXADS, newListener ) > 0.0f, dataADS.m_yawStyle, RecoilStyle::Linear );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableLinearYADS, newListener ) > 0.0f, dataADS.m_pitchStyle, RecoilStyle::Linear );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableCycleXADS, newListener ) > 0.0f, dataADS.m_yawStyle, RecoilStyle::Cyclic );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableCycleYADS, newListener ) > 0.0f, dataADS.m_pitchStyle, RecoilStyle::Cyclic );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableScaleXADS, newListener ) > 0.0f, dataADS.m_yawStyle, RecoilStyle::Scale );
		SetRecoilStyleFlag( GetStatValue( itemData, data::StatType::RecoilEnableScaleYADS, newListener ) > 0.0f, dataADS.m_pitchStyle, RecoilStyle::Scale );

		dataADS.m_allowSway = GetStatValue( itemData, data::StatType::RecoilAllowSway, newListener );

		dataADS.m_startDir = GetStatValue( itemData, data::StatType::RecoilDirADS, newListener );
		dataADS.m_alternateDir = GetStatValue( itemData, data::StatType::RecoilAlternateDirADS, newListener );

		dataADS.m_angle = GetStatValue( itemData, data::StatType::RecoilAngleADS, newListener );
		dataADS.m_kickMin = GetStatValue( itemData, data::StatType::RecoilKickMinADS, newListener );
		dataADS.m_kickMax = GetStatValue( itemData, data::StatType::RecoilKickMaxADS, newListener );
		dataADS.m_holdDuration = GetStatValue( itemData, data::StatType::RecoilHoldDurationADS, newListener );

			
		dataADS.m_recoilTime = Max( FLT_EPSILON, GetStatValue( itemData, data::StatType::RecoilTimeADS, newListener ) );
		dataADS.m_recoveryTime = Max( FLT_EPSILON, GetStatValue( itemData, data::StatType::RecoilRecoveryTimeADS, newListener ) );
		dataADS.m_maxRecoilDistance = GetStatValue( itemData, data::StatType::RecoilMaxLengthADS, newListener );
		dataADS.m_cycleTime = GetStatValue( itemData, data::StatType::RecoilCycleTimeADS, newListener );
		dataADS.m_cycleSize = GetStatValue( itemData, data::StatType::RecoilCycleSizeADS, newListener );
		dataADS.m_scaleTime = GetStatValue( itemData, data::StatType::RecoilScaleTimeADS, newListener );
		dataADS.m_scaleMax = GetStatValue( itemData, data::StatType::RecoilScaleMaxADS, newListener );
		dataADS.m_minRecoverySpeed = GetStatValue( itemData, data::StatType::RecoilRecoveryMinSpeedADS, newListener );


		//For now just use the same state for ADS..
		dataADS.m_recoilDelay = GetStatValue( itemData, data::StatType::RecoilDelay, newListener );

		// currently ads and default use the same "drift" settings
		dataADS.m_driftRandomRangeMin = data.m_driftRandomRangeMin;
		dataADS.m_driftRandomRangeMax = data.m_driftRandomRangeMax;
		dataADS.m_recoilMagForFullDrift = data.m_recoilMagForFullDrift;

		if( GetStatValue( itemData, data::StatType::RecoilDirPlanSequenceADS, newListener ) > 0.f )
		{
			dataADS.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::Sequence;
		}
		else if( GetStatValue( itemData, data::StatType::RecoilDirPlanCycleRandDirADS, newListener ) > 0.f )
		{
			dataADS.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::CycleRandDir;
		}
		else if( GetStatValue( itemData, data::StatType::RecoilDirPlanCycleRandRangeDirADS, newListener ) > 0.f )
		{
			dataADS.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::CycleRandDirRange;
		}
		else
		{
			dataADS.m_linearDirectionPlan = RecoilContextData::LinearDirectionPlan::FirstDirectionOnly;
		}

		dataADS.m_linearDirectionUpdateRange = Vector2( GetStatValue( itemData, data::StatType::LinearDirectionUpdateMinADS, newListener ),
														GetStatValue( itemData, data::StatType::LinearDirectionUpdateMaxADS, newListener ) );
		
		
	}

	return recoilData;
}


game::SwayData game::weapon::Object::CreateSwayData( const WeakHandle< ItemData >& itemDataWH )
{
	SwayData swayData;
	if( auto itemData = itemDataWH.ToHandle() )
	{
		// create the listener if we have not already, returns null if it was already created.
		// If the creation was needed, we then populate it will all the needed stats
		const THandle< WeaponStatGroupListener >& newListener = CreateStatGroupListenerIfNeeded( EStatGroupTypes::Sway, 12 );

		swayData.m_sideMinAngleDistance = GetStatValue( itemData, data::StatType::SwaySideMinimumAngleDistance, newListener );
		swayData.m_sideMaxAngleDistance = GetStatValue( itemData, data::StatType::SwaySideMaximumAngleDistance, newListener );
		swayData.m_sideTopAngleLimit = GetStatValue( itemData, data::StatType::SwaySideTopAngleLimit, newListener );
		swayData.m_sideBottomAngleLimit = GetStatValue( itemData, data::StatType::SwaySideBottomAngleLimit, newListener );
		swayData.m_centerMaxAngleOffset = GetStatValue( itemData, data::StatType::SwayCenterMaximumAngleOffset, newListener );
		swayData.m_traversalTime = GetStatValue( itemData, data::StatType::SwayTraversalTime, newListener );
		swayData.m_initialOffsetRandomFactor = GetStatValue( itemData, data::StatType::SwayInitialOffsetRandomFactor, newListener );
		swayData.m_curvatureMinFactor = GetStatValue( itemData, data::StatType::SwayCurvatureMinimumFactor, newListener );
		swayData.m_curvatureMaxFactor = GetStatValue( itemData, data::StatType::SwayCurvatureMaximumFactor, newListener );
		swayData.m_sideStepChangeMinFactor = GetStatValue( itemData, data::StatType::SwaySideStepChangeMinimumFactor, newListener );
		swayData.m_sideStepChangeMaxFactor = GetStatValue( itemData, data::StatType::SwaySideStepChangeMaximumFactor, newListener );
		swayData.m_startDelay = GetStatValue( itemData, data::StatType::SwayStartDelay, newListener );
		swayData.m_startBlendTime = GetStatValue( itemData, data::StatType::SwayStartBlendTime, newListener );
	}
	return swayData;
}

game::smartGun::Parameters game::weapon::Object::CreateSmartGunParametersData( const WeakHandle< ItemData >& itemDataWH, const THandle< game::Object >& owner )
{
	game::smartGun::Parameters params;

	if( auto itemData = itemDataWH.ToHandle() )
	{
		const THandle< WeaponStatGroupListener >& newListener = CreateStatGroupListenerIfNeeded( EStatGroupTypes::Smart, 35 );

		/////////////////////////////////////////////////
		// Smart gun SightArea data
		/////////////////////////////////////////////////
		params.m_sightArea.m_shape = game::smartGun::SightArea::Shape::Rectangle;
		params.m_sightArea.m_center = Vector2( data::TweakDB::VFloat( TDBID( "player.crosshair.screenSpaceX" ) ), data::TweakDB::VFloat( TDBID( "player.crosshair.screenSpaceY" ) ) );

		params.m_sightArea.m_data.m_hip.m_targetableAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunHipTargetableAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunHipTargetableAngleYaw, newListener ) );
		params.m_sightArea.m_data.m_hip.m_lockingAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunHipLockingAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunHipLockingAngleYaw, newListener ) );
		params.m_sightArea.m_data.m_hip.m_tagLockAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunHipTagLockAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunHipTagLockAngleYaw, newListener ) );

		params.m_sightArea.m_data.m_ads.m_targetableAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunAdsTargetableAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunAdsTargetableAngleYaw, newListener ) );
		params.m_sightArea.m_data.m_ads.m_lockingAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunAdsLockingAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunAdsLockingAngleYaw, newListener ) );
		params.m_sightArea.m_data.m_ads.m_tagLockAngles = EulerAngles( 0.f, GetStatValue( itemData, data::StatType::SmartGunAdsTagLockAnglePitch, newListener ), GetStatValue( itemData, data::StatType::SmartGunAdsTagLockAngleYaw, newListener ) );

		/////////////////////////////////////////////////
		// Smart gun targeting data
		/////////////////////////////////////////////////
		params.m_timeToLockHip = GetStatValue( itemData, data::StatType::SmartGunHipTimeToLock, newListener );
		params.m_timeToLockAds = GetStatValue( itemData, data::StatType::SmartGunAdsTimeToLock, newListener );
		params.m_timeToUnlockHip = GetStatValue( itemData, data::StatType::SmartGunHipTimeToUnlock, newListener );
		params.m_timeToUnlockAds = GetStatValue( itemData, data::StatType::SmartGunAdsTimeToUnlock, newListener );
		params.m_timeToRemoveOccludedTarget = GetStatValue( itemData, data::StatType::SmartGunTimeToRemoveOccludedTarget, newListener );

		params.m_maxLockedTargetsHip = ( Uint8 )GetStatValue( itemData, data::StatType::SmartGunHipMaxLockedTargets, newListener );
		params.m_maxLockedTargetsAds = ( Uint8 )GetStatValue( itemData, data::StatType::SmartGunAdsMaxLockedTargets, newListener );
		params.m_targetAcquisitionRange = GetStatValue( itemData, data::StatType::SmartGunTargetAcquisitionRange, newListener );
		
		params.m_useTimeBasedAccuracy = GetStatValue( itemData, data::StatType::SmartGunUseTimeBasedAccuracy, newListener ) > 0.0f;
		params.m_startingAccuracy = GetStatValue( itemData, data::StatType::SmartGunStartingAccuracy, newListener );
		params.m_timeToMaxAccuracy = GetStatValue( itemData, data::StatType::SmartGunTimeToMaxAccuracy, newListener );
		
		Bool useEvenDistributionTargeting = GetStatValue( itemData, data::StatType::SmartGunUseEvenDistributionTargeting, newListener ) > 0.0f;
		params.m_evenDistributionPeriod = GetStatValue( itemData, data::StatType::SmartGunEvenDistributionPeriod, newListener );
		params.m_trackMultipleEntitiesInADS = GetStatValue( itemData, data::StatType::SmartGunTrackMultipleEntitiesInADS, newListener ) > 0.0f;

		Bool useTagLockTargeting = GetStatValue( itemData, data::StatType::SmartGunUseTagLockTargeting, newListener ) > 0.0f;

		if( useEvenDistributionTargeting )
		{
			params.m_targetingMode = game::smartGun::TargetingMode::EvenDistribution;
		}
		else if( useTagLockTargeting )
		{
			params.m_targetingMode = game::smartGun::TargetingMode::TagLock;
		}
		else
		{
			params.m_targetingMode = game::smartGun::TargetingMode::Default;
		}
		
		params.m_userHasRequiredCyberware = owner ? m_statsSystem->GetBoolValue( owner->GetEntityID(), data::StatType::HasSmartLink ) : false;

		params.m_spreadMultiplier = GetStatValue( itemData, data::StatType::SmartGunSpreadMultiplier, newListener );

		params.m_trackAllBodyparts = GetStatValue( itemData, data::StatType::SmartGunTrackAllBodyparts, newListener ) > 0.0f;
		params.m_trackHeadComponents = GetStatValue( itemData, data::StatType::SmartGunTrackHeadComponents, newListener ) > 0.0f;
		params.m_trackLegComponents = GetStatValue( itemData, data::StatType::SmartGunTrackLegComponents, newListener ) > 0.0f;
		params.m_trackMechanicalComponents = GetStatValue( itemData, data::StatType::SmartGunTrackMechanicalComponents, newListener ) > 0.0f;

		params.m_canWeaponSnapToLimbs = GetStatValue( itemData, data::StatType::CanWeaponSnapToLimbs, newListener ) > 0.0f;
	}

	return params;
}

game::smartGun::NPCParameters game::weapon::Object::CreateSmartGunParametersDataForNPC( const WeakHandle< ItemData >& itemDataWH )
{
	game::smartGun::NPCParameters params;

	if( auto itemData = itemDataWH.ToHandle() )
	{
		params.m_lockOnTime = std::max( GetStat( itemData, data::StatType::SmartGunNPCLockOnTime ), 0.1f );
		params.m_lockTimeout = std::max( GetStat( itemData, data::StatType::SmartGunNPCLockTimeout ), 0.1f ); // precaution for division by 0 in smartGun::NPCHandler
		params.m_trajectoryCurvatureMultiplier = GetStat( itemData, data::StatType::SmartGunNPCTrajectoryCurvatureMultiplier );
		params.m_shootOnlyStraight = static_cast< Bool >( GetStat( itemData, data::StatType::SmartGunNPCShootProjectilesOnlyStraight ) );
		params.m_applySpreadAtHitplane = static_cast< Bool >( GetStat( itemData, data::StatType::SmartGunNPCApplySpreadAtHitplane ) );
		params.m_spreadMultiplier = GetStat( itemData, data::StatType::SmartGunNPCSpreadMultiplier );

		GAME_DATA_ERROR( params.m_lockOnTime >= 0.1f && params.m_lockTimeout >= 0.1f, Weapons,
						 "Smart gun stats SmartGunNPCLockOnTime and SmartGunNPCLockTimeout should be > 0.1f." );
	}

	return params;
}

game::smartGun::NPCSightArea game::weapon::Object::CreateSmartGunNPCSightAreaData( const WeakHandle< ItemData >& itemDataWH )
{
	game::smartGun::NPCSightArea area;

	if( auto itemData = itemDataWH.ToHandle() )
	{
		area.m_lockingAngles = EulerAngles( 0.f, GetStat( itemData, data::StatType::SmartGunNPCLockingAnglePitch ), GetStat( itemData, data::StatType::SmartGunNPCLockingAngleYaw ) );
		area.m_projectileStartingOrientationAngleOffset = GetStat( itemData, data::StatType::SmartGunNPCProjectileStartingOrientationAngleOffset );
	}

	return area;
}

Float game::weapon::Object::GetStat( const WeakHandle< ItemData >& itemDataWH, data::StatType statType )
{
	auto itemData = itemDataWH.ToHandle();
	return itemData ? itemData->GetStatValue( statType ) : 0.f;
}

WeakHandle< game::weapon::FxPackage > game::weapon::Object::GetFxPackage() const
{
	return m_fxPackage;
}


WeakHandle< game::weapon::FxPackage > game::weapon::Object::GetFxPackageQuickMelee() const
{
	return m_fxPackageQuickMelee;
}

void game::weapon::Object::UNSAFE_Shoot( const events::ShootEvent& evt )
{
	if( GetState() == ent::EntityState::Attached )
	{
		OnShoot( evt );
	}
}

THandle< game::weapon::events::ShootEvent > game::weapon::Object::ShootAt( const ShootAtParameters& params )
{
	const Matrix muzzleMatrix = params.m_muzzleMatrix;
	const THandle< ent::IPositionProvider > fromProvider = params.m_fromPositionProvider;
	const THandle< ent::IPositionProvider > targetPositionProvider = params.m_targetPositionProvider;
	const Float spreadAngleMax = params.m_spreadAngleMax;
	const Uint16 ammoCost = params.m_ammoCost;
	const projectile::WeaponParams projectileParams = params.m_projectileParams;
	const Uint8 projectilesPerShot = params.m_projectilesPerShot;
	const Float chargePercentage = params.m_chargePercentage;

	ent::EntityID targetID = ent::EntityID();
	if( auto targetHandle = params.m_target.ToHandle() )
	{
		if( targetHandle->IsInitialized() )
		{
			targetID = targetHandle->GetEntityID();
		}
	}

	Vector4 muzzleUp;
	muzzleMatrix.ToAngleVectors( nullptr, nullptr, &muzzleUp );

	AimDataWrapper aimDataWrapper(
		[muzzleUp, fromProvider, spreadAngleMax, targetPositionProvider ]( const Bool useDefaultAim, Vector4& fromWorldPositionOut, Vector4& forwardOut, Vector4& upOut, Quaternion& spreadOrientationInOut, const Bool generateNewSpread ) {
			fromWorldPositionOut = fromProvider->CalculatePosition();

			forwardOut = targetPositionProvider->CalculatePosition() - fromProvider->CalculatePosition();
			forwardOut.Normalize3();

			forwardOut = TEMP_AI_GenerateForwardVectorWithSpread( spreadAngleMax, forwardOut );
			upOut = muzzleUp;
		},
		nullptr );

	if ( CpHitProbeTake( 0 ) )
	{
		// [tkfix] coreplay shot probe (logging only): who fires, from where, at what
		Vector3 cpFrom = Vector3::ZEROS(); if ( fromProvider ) { cpFrom = fromProvider->CalculatePosition(); }
		Vector3 cpDir = Vector3::ZEROS(); if ( targetPositionProvider ) { cpDir = targetPositionProvider->CalculatePosition(); }
		const Vector3& cpAt = projectileParams.m_targetPosition;
		THandle< game::Object > cpOwner = GetOwner();
		std::fprintf( stderr, "[coreplay] SHOT spawn: weapon %s owner %s (%s) from (%.2f, %.2f, %.2f) toward (%.2f, %.2f, %.2f) targetPos (%.2f, %.2f, %.2f) target %s projectiles %u\n",
			GetItemID().GetTweakDBID().ToString().AsChar(), cpOwner ? cpOwner->GetEntityID().ToDebugString().AsChar() : "-", cpOwner ? cpOwner->GetClass()->GetName().AsChar() : "-",
			cpFrom.X, cpFrom.Y, cpFrom.Z, cpDir.X, cpDir.Y, cpDir.Z, cpAt.X, cpAt.Y, cpAt.Z, targetID.ToDebugString().AsChar(), Uint32( projectilesPerShot ) );
	}

	auto shootEvent = CreateHandle< weapon::events::ShootEvent >(
		params.m_instigator,
		aimDataWrapper,
		ammoCost,
		projectileParams,
		projectilesPerShot,
		params.m_target,
		chargePercentage );

	if( GetOwner() )
	{
		GetOwner()->QueueEvent( shootEvent );

		m_questHelper->OnAttacked( GetOwner()->GetEntityID(), targetID );
	}

	QueueEventToSelfAndChildItems( shootEvent );
	OnShoot( *shootEvent );

	return shootEvent;
}

void game::weapon::Object::funcGetAttacks( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( m_attacks );
}

void game::weapon::Object::funcGetCurrentAttack( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( m_currentAttack );
}

void game::weapon::Object::funcGetCurrentTriggerMode( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( m_currentTriggerMode );
}

void game::weapon::Object::funcGetFxPackage( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( GetFxPackage() );
}

void game::weapon::Object::funcGetFxPackageQuickMelee( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( GetFxPackageQuickMelee() );
}



void game::weapon::Object::funcGetSharedData( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( GetSharedData().HandleFromThis() );
}

void game::weapon::Object::funcGetTotalAmmoCount( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	Int32 ammo = 0;
	auto& equipmentMgr = GetGame()->GetSystemPtr< game::IInventoryManager >()->GetEquipmentMgr();
	const Bool infiniteAmmo = HasInfiniteAmmo( equipmentMgr );

	if ( infiniteAmmo == false )
	{
		if ( auto transactionSystem = GetGame()->GetSystemPtr< TransactionSystem >() )
		{
			if ( auto owner = GetOwner() )
			{
				ammo = transactionSystem->GetItemQuantity( *owner, GetMagazineAmmoID() );
			}
		}
	}
	else
	{
		ammo = std::numeric_limits< Int32 >::max();
	}

	RETURN_AUTO( ammo );
}

void game::weapon::Object::funcGetTriggerModes( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( m_triggerModes );
}

void game::weapon::Object::funcHasAmmoChangeRequest( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( HasAmmoChangeRequest() );
}

void game::weapon::Object::funcHasPendingReload( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;
	RETURN_AUTO( HasPendingReload() );
}

void game::weapon::Object::funcSetAttack( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( TweakDBID, attackID, TweakDBID::NONE() );
	FINISH_PARAMETERS;

	RETURN_AUTO( SetAttack( attackID ) );
}

Bool game::weapon::Object::SetAttack( const TweakDBID& attackRecordID )
{
	auto attackIter = std::find_if( m_attacks.Begin(), m_attacks.End(), [ &attackRecordID ]( const THandle< game::IAttack >& attack ) {
		if ( auto record = attack->GetRecord().ToHandle() )
		{
			return record->GetID() == attackRecordID;
		}
		return false;
	} );
	const Bool success = attackIter != m_attacks.End();

	SetAttack( success ? *attackIter : m_currentAttack );
	return success;
}

void game::weapon::Object::SetAttack( const THandle< game::IAttack >& newAttack )
{
	// first, stop potentially started continuous attack
	if( m_isContinuousAttack )
		StopContinuousAttack();

	if( m_currentAttack && newAttack != m_currentAttack )
	{
		m_prevAttack = m_currentAttack;
	}

	m_currentAttack = newAttack;
}

void game::weapon::Object::funcAI_ShootAt( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( THandle< ent::IPositionProvider >, targetPositionProvider, nullptr );
	GET_PARAMETER( game::ObjectPtr, targetObject, game::ObjectPtr() );
	GET_PARAMETER( game::ObjectWeakPtr, instigator, game::ObjectWeakPtr() );
	GET_PARAMETER( Uint16, ammoCost, 1u );
	GET_PARAMETER( projectile::WeaponParams, projectileParams, projectile::WeaponParams() );
	GET_PARAMETER( Uint8, projectilesPerShot, 1u );
	GET_PARAMETER( Float, charge, 0.0f );
	GET_PARAMETER_OPT( Float, spreadAngleMax, 0.0f );
	FINISH_PARAMETERS;

	if ( !targetPositionProvider )
	{
		SCRIPT_ERROR( stack, "Target position provider is null in WeaponObject::AI_ShootAt(...)." );
		RETURN_VOID();
	}

	Vector4 target = targetPositionProvider->CalculatePosition();

	RED_ASSERT( target.IsOk(), "Invalid target position");

	Matrix muzzleMatrix{ Matrix::IDENTITY() };
	GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix );

	THandle< ent::IPositionProvider > fromProvider = CreateHandle< ent::SlotPositionProvider >( WeakHandleFromThis< ent::Entity >(), c_muzzleSlotName, Vector4::ZEROS() );
	Vector3 from = fromProvider->CalculatePosition();

	RED_ASSERT( from.IsOk(), "Invalid from position" );
	
	if( spreadAngleMax < 0.f )
	{
		spreadAngleMax = 0.f;
	}

	Vector3 forward = target - from;
	const Float distance = forward.Mag();

	if( distance <= projectileParams.m_shootingOffset )
	{
		projectileParams.m_shootingOffset = 0.0f;
	}

	// for NPCs' smartGun to create proper projectile trajectory
	if( projectileParams.m_smartGunIsProjectileGuided )
	{
		projectileParams.m_targetPosition = target;
	}

	THandle< weapon::events::ShootEvent > shootEvent = ShootAt( { instigator, muzzleMatrix, fromProvider, targetPositionProvider, spreadAngleMax, ammoCost, projectileParams, projectilesPerShot, charge, targetObject } );

	if( m_replicationHistory )
	{
		m_replicationHistory->AddShot( *shootEvent, m_currentAttack, targetObject, target.AsVector3() );
	}

	RETURN_VOID();
}

void game::weapon::Object::funcAI_ShootForwards( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( game::ObjectWeakPtr, instigator, game::ObjectWeakPtr() );
	GET_PARAMETER( Uint16, ammoCost, 1u );
	GET_PARAMETER( projectile::WeaponParams, projectileParams, projectile::WeaponParams() );
	GET_PARAMETER( Uint8, projectilesPerShot, 1u );
	GET_PARAMETER( Float, charge, 0.0f );
	GET_PARAMETER_OPT( Vector4, overridePos , Vector4::MINUS_MAX());	//for use when player shooting from turrets
	GET_PARAMETER_OPT( Vector4, overrideForward, Vector4::MINUS_MAX() );	//for use when player shooting from turrets

	FINISH_PARAMETERS;

	Matrix muzzleMatrix{Matrix::IDENTITY()};
	GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix );

	const Float spreadAngleMax = m_statsSystem->GetValue( GetEntityID(), data::StatType::SpreadMaxAI );
	Vector3 forward = muzzleMatrix.GetAxisY();

	THandle< ent::IPositionProvider > fromProvider = nullptr;

	if( overridePos != Vector4::MINUS_MAX() )
	{
		fromProvider = CreateHandle< ent::StaticPositionProvider >( WorldPosition( overridePos ) );
	}
	else
	{
		fromProvider = CreateHandle< ent::SlotPositionProvider >( WeakHandleFromThis< ent::Entity >(), c_muzzleSlotName, Vector4::ZEROS() );
	}

	Vector3 from = fromProvider->CalculatePosition();

	RED_ASSERT( from.IsOk(), "Invalid from position" );

	if( overrideForward != Vector4::MINUS_MAX() )
	{
		forward = overrideForward.Normalized4();
	}

	THandle< ent::IPositionProvider > targetPos = CreateHandle< ent::StaticPositionProvider >( WorldPosition( from + forward ) );

	THandle< weapon::events::ShootEvent > shootEvent = ShootAt( { instigator, muzzleMatrix, fromProvider, targetPos, spreadAngleMax, ammoCost, projectileParams, projectilesPerShot, charge, nullptr } );

	if( m_replicationHistory )
	{
		m_replicationHistory->AddShot( *shootEvent, m_currentAttack, game::ObjectPtr(), Vector3::ZEROS() );
	}

	RETURN_VOID();
}

void game::weapon::Object::funcAI_ShootSelfOffScreen( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( THandle< Puppet >, targetObject, nullptr );
	GET_PARAMETER( Uint16, ammoCost, 1u );
	GET_PARAMETER( projectile::WeaponParams, projectileParams, projectile::WeaponParams() );
	GET_PARAMETER( Uint8, projectilesPerShot, 1u );
	GET_PARAMETER( Float, charge, 0.0f );
	FINISH_PARAMETERS;

	if ( !targetObject )
	{
		SCRIPT_ERROR( stack, "Target object is null in WeaponObject::AI_ShootSelfOffScreen(...)." );
		RETURN_VOID();
	}

	auto targetBody = targetObject->GetBody();

	if ( !targetBody )
	{
		SCRIPT_ERROR( stack, "Target object is missing body in WeaponObject::AI_ShootSelfOffScreen(...)." );
		RETURN_VOID();
	}

	Vector3 targetHeight = targetObject->GetWorldUp() * targetBody->GetEyesHeight();
	Vector3 targetRight = targetObject->GetWorldRight() + targetHeight;
	Vector3 targetHead = targetObject->GetWorldPosition().AsVector3() + targetHeight;

	Matrix muzzleMatrix{ Matrix::IDENTITY() };
	GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix );

	const Float spreadAngleMax = m_statsSystem->GetValue( GetEntityID(), data::StatType::SpreadMaxAI );

	THandle< ent::IPositionProvider > fromProvider = CreateHandle< ent::StaticPositionProvider >( WorldPosition( targetRight * 0.3 + targetHead ) );

	THandle< ent::IPositionProvider > targetPos = CreateHandle< ent::StaticPositionProvider >( WorldPosition ( targetHead ) );

	THandle< weapon::events::ShootEvent > shootEvent = ShootAt( { targetObject, muzzleMatrix, fromProvider, targetPos, spreadAngleMax, ammoCost, projectileParams, projectilesPerShot, charge, nullptr } );

	if ( m_replicationHistory )
	{
		m_replicationHistory->AddShot( *shootEvent, m_currentAttack, game::ObjectPtr(), Vector3::ZEROS() );
	}

	RETURN_VOID();
}

void game::weapon::Object::funcStartContinuousAttack( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( Vector4, startPos, Vector4::ZEROS() );
	GET_PARAMETER( Vector4, startDir, Vector4::FRONT() );
	FINISH_PARAMETERS;

	StartContinuousAttack( startPos, startDir );

	RETURN_BOOL( m_isContinuousAttack );
}

Float game::weapon::Object::GetLockingValue() const
{
	if( m_smartGunNPCHandler )
	{
		return m_smartGunNPCHandler->GetLockingTimerOrLockingTimeout();
	}
	else
	{
		return 0.0f;
	}
}

Bool game::weapon::Object::IsTargetLocked() const
{
	if( m_smartGunNPCHandler )
	{
		return m_smartGunNPCHandler->IsTargetLocked();
	}
	else
	{
		return false;
	}
}

void game::weapon::Object::StartContinuousAttack( const Vector4& startPos, const Vector4& startDir )
{
	if( m_currentAttack && m_isContinuousAttack )
	{
		RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
						  "StartContinuousAttack: current attack is not continuous, can't start it!!!" );

		return;
	}

	m_isContinuousAttack = false;

	Matrix muzzleMatrix{Matrix::IDENTITY()};
	Vector4 up = Vector4::UP();

	if( GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix ) )
	{
		muzzleMatrix.ToAngleVectors( nullptr, nullptr, &up );
	}

	AimDataWrapper aimDataWrapper(
		[up, startPos, startDir]( const Bool useDefaultAim, Vector4& fromWorldPositionOut, Vector4& forwardOut, Vector4& upOut, Quaternion& spreadOrientationInOut, const Bool generateNewSpread ) {
			fromWorldPositionOut = startPos;
			forwardOut = startDir;
			upOut = up;
		} );

	RED_ERROR( !IsOwnedByPlayer(), "This code path needs to be updated if will ever be used on player owned weapons!" );

	// Currently this code path is only used from AI actions script to perform
	// shooting actions. If it will be ever used on player weapons you must adjust this code
	// and pass valid shooting correction time so lag compensation will work in multiplayer.
	const net::Time shootingCorrectionTime = net::Time::Zero;

	if( m_currentAttack )
	{
		AttackContext context;
		context.m_isPlayerOwned = IsOwnedByPlayer();
		context.m_hasMuzzle = false;
		context.m_chargePercentage = 0.f;
		context.m_aimDataWrapper = aimDataWrapper;
		context.m_fxPackage = m_fxPackage;
		context.m_shootTime = shootingCorrectionTime;
		context.m_isTargetPlayer = false;

		m_currentAttack->PrepareAttack( GetOwner() );
		m_currentAttack->SetAttackData( context );
		m_currentAttack->StartAttackContinuous();
	}

	if( m_replicationHistory )
	{
		m_replicationHistory->AddContinuousAttack( m_currentAttack );
	}

	if( m_currentAttack )
	{
		if( VerifyContinousAttackClass( *m_currentAttack ) )
		{
			m_isContinuousAttack = true;

			RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
				"StartContinuousAttack: starting continuous attack." );
		}
	}
	else
	{
		RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
						  "StartContinuousAttack: m_currentAttack is empty, cant start continuous attack!!!" );
	}
}

Bool game::weapon::Object::VerifyContinousAttackClass( const IAttack& attack ) const
{
	const rtti::ClassType* const attackClass = attack.GetClass();

	const Bool hasOnTick = attackClass->FindFunction( RED_NAME_CONSTEXPR( "OnTick;WeaponObject" ) );
	const Bool hasOnStop = attackClass->FindFunction( RED_NAME_CONSTEXPR( "OnStop;WeaponObject" ) );

	if( hasOnTick && hasOnStop )
	{
		return true;
	}
#ifdef RED_LOGGING_ENABLED
	String ownerDebugName( "-" );
	if( THandle<game::Object> ownerStrong = GetOwner() )
	{
		ownerDebugName = ownerStrong->GetDebugName();
	}

	String weaponDebugName = GetDebugName();

	RED_FATAL(
		"Trying to start continous attack using class (%s) that does not implement continous attack functions (OnTick: %s, OnStop: %s)! (weapon: %s, owner: %s)",
		attackClass->GetName().AsChar(), hasOnTick ? "yes" : "no", hasOnStop ? "yes" : "no",
		weaponDebugName.AsChar(), ownerDebugName.AsChar()
	);
#endif // #ifdef RED_LOGGING_ENABLED
	return false;
}

void game::weapon::Object::funcIsContinuousAttackStarted( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	Bool attackStarted = m_currentAttack && m_isContinuousAttack;

	RETURN_BOOL( attackStarted );
}

void game::weapon::Object::funcHasScope( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	RETURN_BOOL( HasScope() );
}

void game::weapon::Object::funcGetScopeOffset( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	RETURN_AUTO( GetScopeOffset() );
}

void game::weapon::Object::funcGetIronSightOffset( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	RETURN_AUTO( GetIronSightOffset() );
}

void game::weapon::Object::funcGetMuzzleOffset( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	RETURN_AUTO( GetMuzzleOffset() );
}

void game::weapon::Object::funcIsSilenced( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	RETURN_AUTO( GetIsSilenced() );
}

void game::weapon::Object::funcIsTargetLocked( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	Bool isTargetLocked = IsTargetLocked();

	RETURN_BOOL( isTargetLocked );
}

void game::weapon::Object::funcUpdateTargetingSight( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( ent::EntityID, targetID, ent::EntityID() );
	GET_PARAMETER( Vector4, targetPosition, Vector4() );
	FINISH_PARAMETERS;

	if( m_smartGunNPCHandler )
	{
		m_smartGunNPCHandler->UpdateTargetingSight( targetID, targetPosition );
	}

	RETURN_VOID();
}

void game::weapon::Object::funcShootStraight( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( Bool, shootStraight, false );
	FINISH_PARAMETERS;

	if( m_smartGunNPCHandler )
	{
		m_smartGunNPCHandler->SetShootStraight( shootStraight );
	}

	RETURN_VOID();
}

Bool game::weapon::Object::GetShootStraight() const
{
	if( m_smartGunNPCHandler )
	{
		return m_smartGunNPCHandler->GetShootStraight();
	}
	else
	{
		return false;
	}
}

Bool game::weapon::Object::GetSmartGunNPCParameters( game::smartGun::NPCParameters& npcParameters ) const
{
	if( m_smartGunNPCHandler )
	{
		npcParameters = m_smartGunNPCHandler->GetNPCParameters();
		return true;
	}
	else
	{
		return false;
	}
}

Bool game::weapon::Object::GetSmartGunNPCSightAreaParameters( game::smartGun::NPCSightArea& npcSightAreaParameters ) const
{
	if( m_smartGunNPCHandler )
	{
		npcSightAreaParameters = m_smartGunNPCHandler->GetNPCSightAreaParameters();
		return true;
	}
	else
	{
		return false;
	}
}

Bool game::weapon::Object::GetSmartGunNPCPerspectiveTransform( WorldTransform& outTransform ) const
{
	if( m_smartGunNPCHandler )
	{
		outTransform = m_smartGunNPCHandler->GetPerspectiveTransform();
		return true;
	}
	else
	{
		return false;
	}
}

game::smartGun::Handler* game::weapon::Object::GetSmartGunHandler() const
{
	game::smartGun::Handler* smartGunHandler = nullptr;

	if( GetWeaponEvolution() == data::WeaponEvolution::Smart )
	{
		if( const THandle< game::Object >& smartGunHandlerOwner = m_smartGunHandlerOwner.ToHandle() )
		{
			if( const THandle< game::ITargetingSystem >& targetingSystem = smartGunHandlerOwner->GetGame()->GetSystemPtr< game::ITargetingSystem >() )
			{
				smartGunHandler = targetingSystem->GetSmartGunHandler( smartGunHandlerOwner );
			}
		}
	}

	return smartGunHandler;
}

void game::weapon::Object::funcStopContinuousAttack( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	StopContinuousAttack();

	RETURN_VOID();
}

void game::weapon::Object::StopContinuousAttack()
{
	if( m_currentAttack && m_isContinuousAttack )
	{
		rtti::CallFunction( m_currentAttack.Get(), RED_NAME_CONSTEXPR( "OnStop" ), HandleFromThis< game::weapon::Object >() );

		m_currentAttack->StopAttack();
		m_isContinuousAttack = false;

		if( m_replicationHistory )
		{
			m_replicationHistory->RemoveContinuousAttack();
		}

		RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
						  "StopContinuousAttack: Stopping continuous attack." );
	}
	else
	{
		RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
						  "StopContinuousAttack: current attack is not continuous!!!" );
	}
}

void game::weapon::Object::funcStartReload( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER_OPT( Float, reloadDuration, 0.0f );
	FINISH_PARAMETERS;
	RETURN_FLOAT( StartReload( reloadDuration ) );
}

void game::weapon::Object::funcStopReload( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER_OPT( ReloadStatus, reloadState, ReloadStatus::Standard );
	FINISH_PARAMETERS;
	StopReload( reloadState );
	RETURN_VOID();
}


void game::weapon::Object::funcAI_SetAttackData( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( WeakHandle< IAttack >, attack, nullptr);
	FINISH_PARAMETERS;

	Bool success = false;
	if( auto attackStrong = attack.ToHandle() )
	{
		Matrix muzzleMatrix{ Matrix::IDENTITY() };
		Vector4 up = Vector4::UP();

		if( GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix ) )
		{
			muzzleMatrix.ToAngleVectors( nullptr, nullptr, &up );
		}
		
		const Vector4& startPos = GetWorldPosition().AsVector4();
		const Vector4& startDir = GetWorldForward();

		AimDataWrapper aimDataWrapper(
			[up, startPos, startDir]( const Bool useDefaultAim, Vector4& fromWorldPositionOut, Vector4& forwardOut, Vector4& upOut, Quaternion& spreadOrientationInOut, const Bool generateNewSpread ) {
			fromWorldPositionOut = startPos;
			forwardOut = startDir;
			upOut = up;
		} );

		RED_ERROR( !IsOwnedByPlayer(), "This code path needs to be updated if will ever be used on player owned weapons!" );

		// Currently this code path is only used from AI actions script to perform
		// shooting actions. If it will be ever used on player weapons you must adjust this code
		// and pass valid shooting correction time so lag compensation will work in multiplayer.
		const net::Time shootingCorrectionTime = net::Time::Zero;

		AttackContext context;
		context.m_isPlayerOwned = IsOwnedByPlayer();
		context.m_hasMuzzle = GetSlotWorldPosition( c_muzzleSlotName, context.m_muzzleWorldPosition );
		context.m_chargePercentage = 0.f;
		context.m_aimDataWrapper = aimDataWrapper;
		context.m_fxPackage = m_fxPackage;
		context.m_shootTime = shootingCorrectionTime;

		attackStrong->SetAttackData( context );
		success = true;
	}

	RETURN_BOOL( success );
}

void game::weapon::Object::funcAI_PlayChargeStartedSound( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	FINISH_PARAMETERS;

	if ( m_audio )
	{
		m_audio->OnAIChargeStarted();
	}

	RETURN_VOID();
}

void game::weapon::Object::funcAI_PlayMeleeAttackSound( CScriptStackFrame & stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( Bool, isQuickMelee, false );
	FINISH_PARAMETERS;

	// normal melee not handled yet for NPCs
	if ( m_audio && isQuickMelee )
	{
		m_audio->OnAIQuickMeleeAttack();
	}

	RETURN_VOID();
}

void game::weapon::Object::func_SetTriggerDown( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( Bool, triggerDown, false );
	FINISH_PARAMETERS;
	m_weaponDataAnimFeature->SetTriggerDown( triggerDown );
	RETURN_VOID();
}

void game::weapon::Object::SetReplicationHistory( WeaponReplicationHistory* history )
{
	m_replicationHistory = history;
}

game::WeaponReplicationHistory* game::weapon::Object::GetReplicationHistory() const
{
	return m_replicationHistory;
}

void game::weapon::Object::HandleReplicatedShot( const ReplicatedShotData& shotData )
{
	// Set proper attack type
	auto attackIter = std::find_if( m_attacks.Begin(), m_attacks.End(), [&shotData]( const THandle< game::IAttack >& attack ) {
		if( auto record = attack->GetRecord().ToHandle() )
		{
			return record->GetID() == shotData.m_attackId;
		}
		return false;
	} );

	const Bool success = attackIter != m_attacks.End();
	if ( success )
	{
		SetAttack( *attackIter );
		ExecuteReplicatedSingleShot( shotData );
	}
	else
	{
		RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Multiplayer, "weapon::Object::HandleReplicatedShot: unknown attack type: %s, shot will be skipped",
			shotData.m_attackId.ToString().AsChar() );
	}
}

net::Time game::weapon::Object::HandleReplicatedContinuousAttack( const ReplicatedContinuousAttack& attackData, const net::Time& lastProcessingTime, const net::Time& currentTime )
{
	Bool processStart = attackData.m_startTimeStamp != net::Time::Invalid && ( lastProcessingTime == net::Time::Invalid || attackData.m_startTimeStamp > lastProcessingTime );

	Bool processStop = attackData.m_stopTimeStamp != net::Time::Invalid && ( lastProcessingTime == net::Time::Invalid || attackData.m_stopTimeStamp > lastProcessingTime );

	// if we should start and stop attacks this update, set proper order of actions first
	if( processStart && processStop )
	{
		if( attackData.m_stopTimeStamp > attackData.m_startTimeStamp )
		{
			StartReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );
			StopReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );

			return attackData.m_stopTimeStamp;
		}
		else
		{
			StopReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );
			StartReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );

			return attackData.m_startTimeStamp;
		}
	}
	else if( processStart )
	{
		StartReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );

		return attackData.m_startTimeStamp;
	}
	else if( processStop )
	{
		StopReplicatedContinuousAttack( attackData, lastProcessingTime, currentTime );

		return attackData.m_stopTimeStamp;
	}

	return lastProcessingTime;
}

void game::weapon::Object::StartReplicatedContinuousAttack( const ReplicatedContinuousAttack& attackData, const net::Time& lastProcessingTime, const net::Time& currentTime )
{
	// new attack started
	Vector4 from{Vector4::ZEROS()};
	Vector4 forward{Vector4::EY()};
	Matrix muzzleMatrix{Matrix::IDENTITY()};
	if( GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix ) )
	{
		from = muzzleMatrix.GetTranslation();
		const Quaternion muzzleQuat = muzzleMatrix.ToQuat();
		forward = muzzleQuat.Transform( Vector3::EY() );
	}

	// Set proper attack type
	auto attackIter = std::find_if( m_attacks.Begin(), m_attacks.End(), [&attackData]( const THandle< game::IAttack >& attack ) {
		if( auto record = attack->GetRecord().ToHandle() )
		{
			return record->GetID() == attackData.m_attackId;
		}
		return false;
	} );
	const Bool success = attackIter != m_attacks.End();
	RED_ASSERT( success );
	SetAttack( success ? *attackIter : m_currentAttack );

	StartContinuousAttack( from, forward );

	RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
					  "WeaponReplicatedActionsHistory: starting replicated continuous attack. Timestamp: %f, current time: %f, last processing time: %f",
					  attackData.m_startTimeStamp.ToSecs(),
					  currentTime.ToSecs(),
					  lastProcessingTime.ToSecs() );
}

void game::weapon::Object::StopReplicatedContinuousAttack( const ReplicatedContinuousAttack& attackData, const net::Time& lastProcessingTime, const net::Time& currentTime )
{
	// an attack has ended
	StopContinuousAttack();

	RED_LOG_CATEGORY( red::LoggerCategory_Multiplayer,
					  "WeaponReplicatedActionsHistory: stopping replicated continuous attack. Timestamp: %f, current time: %f, last processing time: %f",
					  attackData.m_stopTimeStamp.ToSecs(),
					  currentTime.ToSecs(),
					  lastProcessingTime.ToSecs() );
}

void game::weapon::Object::ExecuteReplicatedSingleShot( const ReplicatedShotData& shotData )
{
	// #TODO_MP Proper replicated params. Currently this is only a quick prototype to get the NPCs to shoot at all on clients. We'll worry about proper aiming later.
	Uint16 ammoCost = 1;
	projectile::WeaponParams projectileParams;
	Uint8 projectilesPerShot = 1;
	Float charge = 0.f;

	Matrix muzzleMatrix{Matrix::IDENTITY()};
	GetSlotWorldMatrix( c_muzzleSlotName, muzzleMatrix );
	const Float spreadAngleMax = m_statsSystem->GetValue( GetEntityID(), data::StatType::SpreadMaxAI );

	ent::EntityID targetId;

	THandle< ent::IPositionProvider > fromProvider = CreateHandle< ent::SlotPositionProvider >( WeakHandleFromThis< ent::Entity >(), c_muzzleSlotName, Vector4::ZEROS() );
	const Vector3 from = fromProvider->CalculatePosition();

	RED_ASSERT( from.IsOk(), "Invalid from position" );

	THandle< ent::IPositionProvider > targetPositionProvider = nullptr;
	if( const THandle< game::Object > targetObject = shotData.m_target.ToHandle() )
	{
		if( targetObject->IsInitialized() )
		{
			const WorldPosition target = targetObject->GetWorldTransform().TransformPoint( shotData.m_targetLocalOffset );
			targetPositionProvider = CreateHandle< ent::StaticPositionProvider >( target );
			targetId = targetObject->GetEntityID();
		}
		else
		{
			Vector3 forward = muzzleMatrix.GetAxisY();
			targetPositionProvider = CreateHandle< ent::StaticPositionProvider >( WorldPosition( from + forward ) );
			RED_WARNING( false, "weapon::Object: Target entity (%s) is not initialized!", targetObject->Debug_GetDescriptiveName().AsChar() );
		}
	}
	else
	{
		Vector3 forward = muzzleMatrix.GetAxisY();
		targetPositionProvider = CreateHandle< ent::StaticPositionProvider >( WorldPosition( from + forward ) );
	}

	THandle< weapon::events::ShootEvent > shootEvent = ShootAt( { GetOwner(), muzzleMatrix, fromProvider, targetPositionProvider, spreadAngleMax, ammoCost, projectileParams, projectilesPerShot, charge, shotData.m_target } );
}

void game::weapon::Object::OnReloadStart( const weapon::events::ReloadStartEvent& evt )
{
	m_audio->StartReload();
}
void game::weapon::Object::OnStopFiring( const weapon::events::StopFiringEvent& evt )
{
	m_audio->StopFiring();
}
void game::weapon::Object::OnStopWeaponFire( const audio::events::StopWeaponFire& evt )
{
	m_audio->StopFiring();
}

void game::weapon::Object::OnHitConnected( const audio::events::HitEvent& evt )
{
	m_audio->OnHit( evt );
}

void game::weapon::Object::OnToggleAimDownSights( const audio::events::ToggleAimDownSightsEvent& evt )
{
	m_audio->OnToggleAimDownSights( evt );
}

void game::weapon::Object::OnPreFire( const audio::events::PreFireEvent& evt )
{
	m_audio->OnPreFire();
}

void game::weapon::Object::OnSetMaxCharge( const weapon::events::SetMaxChargeEvent& evt )
{
	m_maxCharge = evt.m_maxCharge;
}

void game::weapon::Object::OnEnableSmartGunHandlerEvent( const weapon::events::EnableSmartGunHandlerEvent& evt )
{
	evt.m_enable ? EnableSmartGunHandler( evt.m_owner ) : DisableSmartGunHandler( evt.m_owner );
}

Float  game::weapon::Object::GetWeaponChargeValueNormalized( Float statPoolValue ) const
{
	return math::Clamp<Float>(statPoolValue /  Max( GetChargeMax(), FLT_EPSILON ), 0.0f, 1.0f);
}

void game::weapon::Object::funcSetupBurstFireSound( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
{
	GET_PARAMETER( Int32, numShotsInBurst, -1 );
	FINISH_PARAMETERS;

	m_audio->OnPreBurstFire( numShotsInBurst );

	RETURN_VOID();
}

CName game::weapon::Object::GetAudioResourceName() const
{
	if( ! m_audio )
	{
		return game::Object::GetAudioResourceName();
	}
	return m_audio->GetAudioName();
}

void game::weapon::Object::HandleAudioEvent( const ent::AudioEvent& evt )
{
	if ( m_audio->IsEquipped() )
	{
		m_audio->HandleAudioEvent( evt );
	}
	else
	{
		::game::Object::HandleAudioEvent( evt );
	}
}

void game::weapon::Object::OnUninitialize()
{
	TBaseClass::OnUninitialize();

	if ( m_audio->IsEquipped() )
	{
		m_audio->OnUnequip();
	}

	StopContinuousAttack();
}

bool game::weapon::Object::HasInfiniteAmmo( EquipmentManager& equipmentMgr )
{
	return m_isOwnedByLocalPlayer == false || equipmentMgr.HasSystemStateFlag( EEquipmentManagerState::InfiniteAmmo );
}

#undef WEAPONS_LOG_ENABLED
#undef WEAPONS_LOG

#ifdef DEBUG_GAMEPLAY
#pragma optimize( "", on )
#endif
