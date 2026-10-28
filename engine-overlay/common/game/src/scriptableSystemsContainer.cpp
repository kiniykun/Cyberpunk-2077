/**
 * Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"

#include "scriptableSystem.h"
#include "scriptableSystemsContainer.h"
#include "../../../common/gameSave/include/saveVersion.h"
#include "../../../common/world/include/gameInstance.h"
#include "../../../common/redReflection/include/packageBuilder.h"
#include "../../../common/redReflection/include/packageLayoutLoader.h"
#include "../../../common/redReflection/include/packageLayoutSaver.h"
#include "../../../common/redReflection/include/packageInspector.h"

// CP2077 web port [sandbox-spawn]: ?sandbox=1 content (vehicle grid + every weapon + ammo).
#include "scriptableEquipmentSystemRequests.h"
#include "../../../common/gameCore/include/gameObject.h"
#include "../../../common/gameCore/include/entityStub.h"
#include "../../../common/gameCore/include/transactionSystemInterface.h"
#include "../../../common/gameCore/include/vehicleSystemInterface.h"
#include "../../../common/gameCore/include/populationSystemInterface.h"
#include "../../../common/gameCore/include/statusEffectSystemInterface.h"
#include "../../../common/gameCore/include/playerSystemInterface.h"
#include "../../../common/gameCore/include/teleportationFacilityInterface.h" // [sandbox-floor]
#include "../../../common/gameVehicles/include/vehicleBaseObject.h" // [flash] park spawned vehicles
#include "../../../common/gameVehicles/include/vehicleRigidBody.h" // [coreplay]
#include "../../../common/gameVehicles/include/vehicleAVBaseObject.h" // [coreplay] sandbox AV flight
#include "../../../common/gameVehicles/include/vehicleListenerCallbacks.h" // [coreplay] weapon probe
#include "../../../common/gameVehicles/include/vehicleChassisComponent.h" // [coreplay]
#include "../../../common/gameVehicles/include/vehicleCameraManager.h" // [tkfix] aim report (the camera UpdateWeapons aims with)
#include "../../../common/worldPhysics/include/runtimeSystemPhysics.h" // [tkfix] aim-ray probe
#include "../../../common/physics/include/physicsQueryFilter.h" // [tkfix]
#include "../../../common/physics/include/physicsProxy.h" // [coreplay]
#include "../../../common/gameCore/include/statPoolsSystemInterface.h" // [coreplay]
#include "../../../common/gameTweakDB/include/types/tweakDBStatPoolType.h" // [coreplay]
#include "../../../common/game/include/statsObjectID.h" // [coreplay]
#include "../../../common/world/include/cameraSystemInterface.h" // [coreplay]
#include "../../../common/gameCore/include/targetingSystemInterface.h" // [coreplay]
#include "../../../common/gameCore/include/mountingFacilityInterface.h" // [coreplay]
#include "../../../common/game/include/stimuliEventData.h" // [coreplay]
#include "../../../common/gameCore/include/mappinSystemInterface.h" // [dbgpos]
#include "../../../common/gameMappins/include/mappinsApi.h" // [dbgpos]
#include "../../../common/gameMappins/include/mappin.h" // [dbgpos] IMappin::IsPlayerTracked
#include "../../../common/gameCore/include/journalManagerInterface.h" // [dbgpos]
#include "../../../common/gameJournalManager/include/journalManagerApi.h" // [dbgpos]
#include "../../../common/gameJournalManager/include/journalEntry.h" // [dbgpos]
#include "../../../common/gameJournalManager/include/journalPath.h" // [dbgpos]
#include <cstddef> // [dbgpos] offsetof
// [coreplay] magazine count / reload probes: GetMagazineAmmoCount is private; widen access for this TU only
#define private public
#include "../../../common/game/include/weaponObject.h"
#undef private
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

volatile int g_coreplayLookAtProbe = 0; // [coreplay] read by targetingLookAtHelper.cpp overlay (lookAt raycast probe)
volatile int g_coreplayActivatorProbe = 0; // [coreplay] read by gameInteractions/src/interactionActivator.cpp overlay
alignas( 16 ) static volatile int g_cpShared[ 8 ] = {}; // [coreplay] main-thread listeners write here (see CpSharedInit)
// [tkfix] hit probes (logging only). On for ?sandbox or ?coreplay=1, &coreplayHits=0 turns them off. Read by the
// damageSystem.cpp / weaponObject.cpp / projectileController.cpp overlays (same module). Budgets are lines left this second.
volatile int g_coreplayHitProbe = 0;
volatile int g_coreplayHitBudget[ 3 ] = { 0, 0, 0 }; // [0] shoot, [1] projectile impact, [2] damage pipeline
volatile unsigned long long g_coreplayHitRing[ 8 ] = {}; // entity hashes of recent damage targets (1 Hz health poll)
volatile int g_coreplayHitRingPos = 0;

RTTI_BEGIN_TYPE_IN_NAMESPACE( ScriptableSystemsContainer, game );
	RTTI_PARENT_TYPE( game::IScriptableSystemsContainer );
	RTTI_IMPORT_ONLY();
	RTTI_SCRIPT_ALIAS( "ScriptableSystemsContainer" )
	RTTI_NATIVE_FUNCTION( "Get", funcGet );
	RTTI_NATIVE_FUNCTION( "QueueRequest", funcQueueRequest )
RTTI_END_TYPE();

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

namespace game
{
	struct ScriptableSystemsDataHeader
	{
		Uint32 systemsCount;
	};

	class ScriptableSystemsPackageSaver : public red::PackageLayoutSaver
	{
	public:
		ScriptableSystemsPackageSaver( red::ArraySpan< CName > mapping )
			: m_mapping( mapping )
		{
			m_header = { mapping.Size() };
		}

	private:
		virtual void OnWriteUserLayout( red::PackageStream & stream ) const override final
		{
			stream.Write( &m_header, sizeof( ScriptableSystemsDataHeader ) );
			stream.Write( m_mapping.Data(), m_mapping.SizeInBytes() );
		}

		virtual Uint32 OnGetUserLayoutSize() const override final
		{
			return sizeof( ScriptableSystemsDataHeader ) + m_mapping.SizeInBytes();
		}

		ScriptableSystemsDataHeader m_header;
		red::ArraySpan< CName > m_mapping;
	};

	class ScriptableSystemsPackageLoader : public red::PackageLayoutLoader
	{
	public:
		const red::DynArray< CName >& GetMapping() const { return m_mapping; }

	private:
		virtual void OnReadUserLayout( Uint32 version, red::PackageReadStream & stream ) override final
		{
			stream.Read( &m_header, sizeof( m_header ) );
			m_mapping.Resize( m_header.systemsCount );
			stream.Read( m_mapping.Data(), m_mapping.DataSize() );
		}

		ScriptableSystemsDataHeader m_header;
		red::DynArray< CName > m_mapping{  PoolGMPL_Script_ScriptableSystem() };
	};

	/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	ScriptableSystemsContainer::ScriptableSystemsContainer()
		: m_systems( PoolGMPL_Script_ScriptableSystem() )
		, m_loadVersion( -1 )
		, m_loadGameVersion( -1 )
	{
	}

	ScriptableSystemsContainer::~ScriptableSystemsContainer()
	{
	}
	
	void ScriptableSystemsContainer::CreateAndAttachAllSystems( const DataBuffer* dataSource )
	{
		// "dataSource" is not null if we're loading the game and need to RESTORE the state of systems, not just create them
		red::PackageInspector inspector;
		red::DynArray< CName > mapping{ job::PoolJobScope() };
		if ( dataSource )
		{
			ScriptableSystemsPackageLoader loader;
			red::BlobView data( dataSource->Data(), dataSource->Size() );
			loader.Initialize( data );
			loader.Load();
			mapping = loader.GetMapping();

			red::Package package = loader.Load();
			inspector.Initialize( package );
		}

		ScriptableSystemRequest::EnumerateRequestClasses();

		red::DynArray< const rtti::ClassType* > classes { job::PoolJobScope() };
		GetRttiSystem().EnumClasses( ClassID< ScriptableSystem >(), classes );

		m_systems.Clear();
		for ( auto* klass : classes )
		{
			if ( klass->IsScriptedClass() && !klass->IsAbstract() )
			{
				THandle< ScriptableSystem > object( ( ScriptableSystem* ) klass->CreateObject( klass->GetSize() ) );
				GAME_ASSERT( object, Systems, "Scriptable system '%s' creation failed.", klass->GetName().AsChar() );

				object->Initialize();

				if ( dataSource )
				{
					Int32 index = mapping.GetIndex( klass->GetName() );
					if ( index < 0 )
					{
						RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Gameplay, "Scriptable system '%s' can't be restored, no data found for it.", klass->GetName().AsChar() );
					}
					else
					{
						inspector.ReadObject( *object, Uint32( index ) );
						object->MarkAsRestored();
					}
				}
				
				m_systems.Insert( klass->GetName(), object );
			}
		}

		const auto* gameInstance = GetGameInstance();
		for ( auto it : m_systems )
		{
			it.Value()->Attach( *gameInstance );
			rtti::CallFunction( it.Value().Get(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "OnAttach;" ) ) );
		}
	}

	void ScriptableSystemsContainer::WorldAttached(world::RuntimeScene& scene)
	{
		if ( GetGameInstance()->GetRuntimeInfo().IsPreview() )
		{
			// there is no OnStreamingWorldLoaded() in preview, so we need to create systems here.
			// That's no problem, as we're no loading anything from save stream in preview.
			CreateAndAttachAllSystems( nullptr );
		}
	}

	void ScriptableSystemsContainer::WorldDetached(world::RuntimeScene& scene)
	{
		for ( auto it : m_systems )
		{
			rtti::CallFunction( it.Value().Get(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "OnDetach;" ) ) );
			it.Value()->Detach();
		}
		m_loadVersion = -1;
		m_loadGameVersion = -1;
	}

	void ScriptableSystemsContainer::OnPostWorldDetached()
	{
#ifdef DEBUG_GAME_FORCED_ASSERT_ON_LEAK
		for( auto it : m_systems )
		{
			const THandle< ScriptableSystem >& scriptableSystem = it.Value();
			ALWAYSENABLED_RED_FATAL_ASSERT( scriptableSystem.GetRefCount() == 1, "Scriptable system '%hs' leak detected", scriptableSystem->GetClass()->GetName().AsChar() );
		}
#endif
	}

	void ScriptableSystemsContainer::OnStreamingWorldLoaded( world::RuntimeScene& scene, Bool isRestoringState, const job::RunContext& context )
	{
		if ( !isRestoringState )
		{
			// when loading game, systems are created in OnGameLoad()
			// otherwise, we just create them here, with nullptr as state data source
			CreateAndAttachAllSystems( nullptr );
		}
	}

	void ScriptableSystemsContainer::OnUninitialize()
	{
		// nothing
	}

	THandle< game::ScriptableSystem > ScriptableSystemsContainer::Get( CName systemName ) const
	{
		THandle< ScriptableSystem > retVal;
		m_systems.Find( systemName, retVal );
		return retVal;
	}

	// ---------------------------------------------------------------------------------------------
	// CP2077 web port [sandbox-spawn]. Active only with ?sandbox=1 in the page URL.
	// After the player is attached (first attach of the session) and c_startDelay seconds of game
	// ticks have passed, this:
	//   - applies GameplayRestriction.NoEncumbrance (232 weapons would otherwise overburden V),
	//   - gives 9999 of every ammo type and one of every weapon in s_sandboxWeapons (silent adds),
	//   - equips + draws Items.Preset_Ajax_Default through EquipmentSystem (EquipRequest/DrawItemRequest,
	//     same as inventory.cpp's EQUIP_CHOICE_TAG path),
	//   - spawns vehicles from s_sandboxVehicles with IVehicleSystem::RequestNewVehicle (the garage path)
	//     on a grid in front of the player: c_perRow per row, c_spacing m apart, rows c_rowGap m apart,
	//     first row c_firstRow m ahead; one vehicle every c_vehicleInterval s; capped (memory) at
	//     ?sandboxVehicles=N|all (default c_defaultVehicleCap); skipped records are logged,
	//   - c_verifyDelay s later logs which stubs became entities.
	// ?sandboxWeapons=0 / ?sandboxVehicles=0 skip either part. Every line starts with "[sandbox-spawn]".
	// Lists: build/research/sandbox-vehicles.txt / sandbox-weapons.txt (generated from tweakdb.bin).
	// ---------------------------------------------------------------------------------------------
	namespace cpweb_sandbox_spawn
	{
		// Generated by scratchpad/sandboxspawn/gen_cpp.py (round-robin Car,Car,Bike,Car,Car,Panzer/AV so a capped run still varies)
		static const char* const s_sandboxVehicles[] = {
			"Vehicle.v_sport1_herrera_outlaw_player",
			"Vehicle.v_sport1_quadra_turbo_player",
			"Vehicle.v_sportbike1_yaiba_kusanagi_player",
			"Vehicle.v_sport1_quadra_turbo_r_player",
			"Vehicle.v_sport1_rayfield_aerondight_player",
			"Vehicle.v_militech_basilisk",
			"Vehicle.v_sport1_rayfield_caliburn_02_player",
			"Vehicle.v_sport1_rayfield_caliburn_player",
			"Vehicle.v_sportbike1_yaiba_kusanagi_tyger_player",
			"Vehicle.v_sport2_mizutani_shion_player",
			"Vehicle.v_sport2_mizutani_shion_nomad_02_player",
			"Vehicle.v_militech_basilisk_militech",
			"Vehicle.v_sport2_mizutani_shion_nomad_player",
			"Vehicle.v_sport2_porsche_911turbo_player",
			"Vehicle.v_sportbike2_arch_jackie_player",
			"Vehicle.v_sport2_quadra_type66_avenger_player",
			"Vehicle.v_sport2_quadra_type66_nomad_ncu_player",
			"Vehicle.v_militech_basilisk_transport",
			"Vehicle.v_sport2_quadra_type66_player",
			"Vehicle.v_sport2_quadra_type66_nomad_player",
			"Vehicle.v_sportbike2_arch_jackie_tuned_player",
			"Vehicle.v_sport2_villefort_alvarado_player",
			"Vehicle.v_sport2_villefort_alvarado_valentinos_player",
			"Vehicle.av_militech_manticore",
			"Vehicle.v_standard25_mahir_supron_player",
			"Vehicle.v_standard25_thorton_colby_pickup_02_player",
			"Vehicle.v_sportbike2_arch_player",
			"Vehicle.v_standard25_thorton_colby_pickup_player",
			"Vehicle.v_standard25_thorton_colby_nomad_player",
			"Vehicle.av_rayfield_excalibur",
			"Vehicle.v_standard25_villefort_columbus_player",
			"Vehicle.v_standard2_archer_hella_player",
			"Vehicle.v_sportbike2_arch_tyger_player",
			"Vehicle.v_standard2_archer_quartz_player",
			"Vehicle.v_standard2_chevalier_thrax_player",
			"Vehicle.mq027_news_av",
			"Vehicle.v_standard2_makigai_maimai_player",
			"Vehicle.v_standard2_thorton_colby_player",
			"Vehicle.v_sportbike3_brennan_apollo_nomad_player",
			"Vehicle.v_standard2_thorton_galena_bobas_player",
			"Vehicle.v_standard2_thorton_galena_player",
			"Vehicle.av_zetatech_atlus",
			"Vehicle.v_standard2_thorton_galena_nomad_player",
			"Vehicle.v_standard2_villefort_cortes_player",
			"Vehicle.v_sportbike3_brennan_apollo_player",
			"Vehicle.v_standard2_villefort_cortes_delamain_player",
			"Vehicle.v_standard3_chevalier_emperor_player",
			"Vehicle.av_zetatech_octant",
			"Vehicle.v_standard3_thorton_mackinaw_player",
			"Vehicle.v_standard3_thorton_mackinaw_ncu_player",
			"Vehicle.v_sportbike1_yaiba_kusanagi",
			"Vehicle.v_standard3_thorton_mackinaw_nomad_panam",
			"Vehicle.v_kaukaz_z71_aras",
			"Vehicle.av_zetatech_surveyor",
			"Vehicle.v_mahir_mt28_coach",
			"Vehicle.v_sport1_herrera_outlaw",
			"Vehicle.v_sportbike2_arch",
			"Vehicle.v_010_v_tek",
			"Vehicle.v_013_rayfield_aerodnight",
			"Vehicle.av_zetatech_valgus",
			"Vehicle.v_sport1_rayfield_aerondight_kerry",
			"Vehicle.v_sport1_rayfield_caliburn",
			"Vehicle.v_sportbike3_brennan_apollo",
			"Vehicle.v_sport2_mizutani_shion",
			"Vehicle.v_sport2_mizutani_shion_nomad",
			"Vehicle.v_sport2_quadra_type66",
			"Vehicle.v_sport2_quadra_type66_rogue",
			"Vehicle.jackie_bike",
			"Vehicle.v_sport2_quadra_type66_nomad",
			"Vehicle.v_sport2_villefort_alvarado",
			"Vehicle.v_sport2_villefort_alvarado_mox",
			"Vehicle.v_trailer_supron",
			"Vehicle.rogue_bike",
			"Vehicle.v_standard25_thorton_colby_pickup",
			"Vehicle.v_standard25_thorton_colby_nomad",
			"Vehicle.v_003_smalltruck",
			"Vehicle.v_standard25_villefort_columbus_judy_van",
			"Vehicle.santiago_bike",
			"Vehicle.v_standard2_archer_hella",
			"Vehicle.v_standard2_archer_hella_combat_cab",
			"Vehicle.v_standard2_archer_hella_ncu",
			"Vehicle.v_standard2_archer_quartz",
			"Vehicle.saul_bike",
			"Vehicle.v_standard2_archer_quartz_nomad",
			"Vehicle.v_standard2_chevalier_thrax",
			"Vehicle.v_standard2_chevalier_thrax_combat_cab",
			"Vehicle.v_standard2_chevalier_thrax_dex",
			"Vehicle.q101_troy_bike",
			"Vehicle.v_009_box_car",
			"Vehicle.v_standard2_thorton_colby",
			"Vehicle.v_standard2_thorton_colby_family",
			"Vehicle.v_006_golf",
			"Vehicle.v_standard2_thorton_galena_nomad",
			"Vehicle.v_standard2_thorton_galena_nomad_scorpion",
			"Vehicle.v_005_lego",
			"Vehicle.v_standard2_villefort_cortes_police",
			"Vehicle.v_standard2_villefort_cortes_police_border_patrol",
			"Vehicle.v_standard2_villefort_cortes_police_sheriff",
			"Vehicle.v_standard2_villefort_cortes_delamain",
			"Vehicle.v_012_corpo_suv",
			"Vehicle.v_standard3_chevalier_emperor_police",
			"Vehicle.v_standard3_thorton_mackinaw",
			"Vehicle.v_standard3_thorton_mackinaw_country",
			"Vehicle.v_utility4_kaukaz_bratsk",
			"Vehicle.v_utility4_kaukaz_bratsk_concrete",
			"Vehicle.v_utility4_kaukaz_bratsk_container",
			"Vehicle.v_utility4_kaukaz_bratsk_dumpster",
			"Vehicle.v_utility4_kaukaz_bratsk_machine",
			"Vehicle.v_utility4_kaukaz_bratsk_metalstud",
			"Vehicle.v_utility4_kaukaz_bratsk_extended",
			"Vehicle.v_utility4_kaukaz_zeya_barrels",
			"Vehicle.v_utility4_kaukaz_zeya",
			"Vehicle.v_utility4_kaukaz_zeya_container",
			"Vehicle.v_utility4_kaukaz_zeya_freight",
			"Vehicle.v_utility4_kaukaz_zeya_full",
			"Vehicle.v_utility4_militech_behemoth",
			"Vehicle.ps_interceptor",
			"Vehicle.q108_delamain_cab",
			"Vehicle.johnny_car",
			"Vehicle.judy_van",
			"Vehicle.kerry_vehicle",
			"Vehicle.kerry_vehicle_sq017",
			"Vehicle.panam_army_truck",
			"Vehicle.reginald_vehicle",
			"Vehicle.rogue_car",
			"Vehicle.sobchak_vehicle",
			"Vehicle.sobchak_ncpd_vehicle",
			"Vehicle.santiago_truck",
			"Vehicle.takemura_vehicle",
			"Vehicle.takemura_van",
		};
		static const char* const s_sandboxWeapons[] = {
			"Items.Preset_Chao_Default",
			"Items.Preset_Chao_Military",
			"Items.Preset_Chao_Neon",
			"Items.Preset_Chao_Pimp",
			"Items.Preset_Kenshin_Default",
			"Items.Preset_Kenshin_Frank",
			"Items.Preset_Kenshin_Military",
			"Items.Preset_Kenshin_Neon",
			"Items.Preset_Kenshin_Pimp",
			"Items.Preset_Kenshin_Royce",
			"Items.Preset_Kenshin_Takemura",
			"Items.Preset_Lexington_Default",
			"Items.Preset_Lexington_Military",
			"Items.Preset_Lexington_Neon",
			"Items.Preset_Lexington_Pimp",
			"Items.Preset_Lexington_Toygun",
			"Items.Preset_Lexington_Wilson",
			"Items.Preset_Liberty_Default",
			"Items.Preset_Liberty_Dex",
			"Items.Preset_Liberty_Military",
			"Items.Preset_Liberty_Neon",
			"Items.Preset_Liberty_Pimp",
			"Items.Preset_Liberty_Rogue",
			"Items.Preset_Liberty_Yorinobu",
			"Items.Preset_Nue_Arasaka_2020",
			"Items.Preset_Nue_Arasaka_2077",
			"Items.Preset_Nue_Default",
			"Items.Preset_Nue_Jackie",
			"Items.Preset_Nue_Maiko",
			"Items.Preset_Nue_Military",
			"Items.Preset_Nue_Neon",
			"Items.Preset_Nue_Pimp",
			"Items.Preset_Omaha_Default",
			"Items.Preset_Omaha_Military",
			"Items.Preset_Omaha_Neon",
			"Items.Preset_Omaha_Pimp",
			"Items.Preset_Omaha_Suzie",
			"Items.Preset_Silverhand_3516",
			"Items.Preset_Unity_Default",
			"Items.Preset_Unity_Military",
			"Items.Preset_Unity_Neon",
			"Items.Preset_Unity_Pimp",
			"Items.Preset_V_Unity",
			"Items.Preset_Yukimura_Default",
			"Items.Preset_Yukimura_Kiji",
			"Items.Preset_Yukimura_Military",
			"Items.Preset_Yukimura_Neon",
			"Items.Preset_Yukimura_Pimp",
			"Items.Preset_Yukimura_Skippy",
			"Items.Preset_Yukimura_Trauma",
			"Items.Preset_Defender_Default",
			"Items.Preset_Defender_Military",
			"Items.Preset_Defender_Neon",
			"Items.Preset_Defender_Pimp",
			"Items.Preset_Achilles_Default",
			"Items.Preset_Achilles_Military",
			"Items.Preset_Achilles_Nash",
			"Items.Preset_Achilles_Neon",
			"Items.Preset_Achilles_Pimp",
			"Items.Preset_Achilles_Tiny_Mike",
			"Items.Preset_Sor22_Default",
			"Items.Preset_Sor22_Military",
			"Items.Preset_Sor22_Neon",
			"Items.Preset_Sor22_Pimp",
			"Items.Preset_Burya_Comrade",
			"Items.Preset_Burya_Default",
			"Items.Preset_Burya_Military",
			"Items.Preset_Burya_Neon",
			"Items.Preset_Burya_Pimp",
			"Items.Preset_Nova_Default",
			"Items.Preset_Nova_Doom_Doom",
			"Items.Preset_Nova_Military",
			"Items.Preset_Nova_Neon",
			"Items.Preset_Nova_Pimp",
			"Items.Preset_Overture_Cassidy",
			"Items.Preset_Overture_Default",
			"Items.Preset_Overture_Kerry",
			"Items.Preset_Overture_Military",
			"Items.Preset_Overture_Neon",
			"Items.Preset_Overture_Pimp",
			"Items.Preset_Overture_River",
			"Items.Preset_Quasar_Default",
			"Items.Preset_Quasar_Military",
			"Items.Preset_Quasar_Neon",
			"Items.Preset_Quasar_Patricia",
			"Items.Preset_Quasar_Pimp",
			"Items.Preset_Ajax_Default",
			"Items.Preset_Ajax_Military",
			"Items.Preset_Ajax_Moron",
			"Items.Preset_Ajax_Neon",
			"Items.Preset_Ajax_Pimp",
			"Items.Preset_Copperhead_Default",
			"Items.Preset_Copperhead_Genesis",
			"Items.Preset_Copperhead_Military",
			"Items.Preset_Copperhead_Neon",
			"Items.Preset_Copperhead_Pimp",
			"Items.Preset_Masamune_Arasaka_2020",
			"Items.Preset_Masamune_Arasaka_2077",
			"Items.Preset_Masamune_Default",
			"Items.Preset_Masamune_Flashlight",
			"Items.Preset_Masamune_Military",
			"Items.Preset_Masamune_Neon",
			"Items.Preset_Masamune_Pimp",
			"Items.Preset_Masamune_Rogue",
			"Items.Preset_Masamune_Trauma",
			"Items.Preset_Sidewinder_Default",
			"Items.Preset_Sidewinder_Divided",
			"Items.Preset_Sidewinder_Military",
			"Items.Preset_Sidewinder_Neon",
			"Items.Preset_Sidewinder_Pimp",
			"Items.Preset_Carnage_Default",
			"Items.Preset_Carnage_Military",
			"Items.Preset_Carnage_Mox",
			"Items.Preset_Carnage_Neon",
			"Items.Preset_Carnage_Pimp",
			"Items.Preset_Crusher_Default",
			"Items.Preset_Crusher_Military",
			"Items.Preset_Crusher_Neon",
			"Items.Preset_Crusher_Pimp",
			"Items.Preset_Tactician_Default",
			"Items.Preset_Tactician_Headsman",
			"Items.Preset_Tactician_Military",
			"Items.Preset_Tactician_Neon",
			"Items.Preset_Tactician_Pimp",
			"Items.Preset_Zhuo_Default",
			"Items.Preset_Zhuo_Eight_Star",
			"Items.Preset_Zhuo_Military",
			"Items.Preset_Zhuo_Neon",
			"Items.Preset_Zhuo_Pimp",
			"Items.Preset_Zhuo_Trauma",
			"Items.Preset_Igla_Default",
			"Items.Preset_Igla_Military",
			"Items.Preset_Igla_Neon",
			"Items.Preset_Igla_Pimp",
			"Items.Preset_Igla_Sovereign",
			"Items.Preset_Palica_Default",
			"Items.Preset_Palica_Military",
			"Items.Preset_Palica_Neon",
			"Items.Preset_Palica_Pimp",
			"Items.Preset_Satara_Default",
			"Items.Preset_Satara_Military",
			"Items.Preset_Satara_Neon",
			"Items.Preset_Satara_Pimp",
			"Items.Preset_Testera_Default",
			"Items.Preset_Testera_Military",
			"Items.Preset_Testera_Neon",
			"Items.Preset_Testera_Pimp",
			"Items.Preset_Ashura_Default",
			"Items.Preset_Ashura_Military",
			"Items.Preset_Ashura_Neon",
			"Items.Preset_Ashura_Pimp",
			"Items.Preset_Grad_Buck",
			"Items.Preset_Grad_Default",
			"Items.Preset_Grad_Military",
			"Items.Preset_Grad_Neon",
			"Items.Preset_Grad_Panam",
			"Items.Preset_Grad_Pimp",
			"Items.Preset_Nekomata_Breakthrough",
			"Items.Preset_Nekomata_Default",
			"Items.Preset_Nekomata_Military",
			"Items.Preset_Nekomata_Neon",
			"Items.Preset_Nekomata_Pimp",
			"Items.Preset_Dian_Default",
			"Items.Preset_Dian_Military",
			"Items.Preset_Dian_Neon",
			"Items.Preset_Dian_Pimp",
			"Items.Preset_Dian_Trauma",
			"Items.Preset_Dian_Yinglong",
			"Items.Preset_Pulsar_Buzzsaw",
			"Items.Preset_Pulsar_Default",
			"Items.Preset_Pulsar_Military",
			"Items.Preset_Pulsar_Neon",
			"Items.Preset_Pulsar_Pimp",
			"Items.Preset_Saratoga_Arasaka_2020",
			"Items.Preset_Saratoga_Arasaka_2077",
			"Items.Preset_Saratoga_Default",
			"Items.Preset_Saratoga_Maelstrom",
			"Items.Preset_Saratoga_Military",
			"Items.Preset_Saratoga_Neon",
			"Items.Preset_Saratoga_Pimp",
			"Items.Preset_Saratoga_Raffen",
			"Items.Preset_Shingen_Default",
			"Items.Preset_Shingen_Military",
			"Items.Preset_Shingen_Neon",
			"Items.Preset_Shingen_Pimp",
			"Items.Preset_Shingen_Prototype",
			"Items.Preset_Knuckles_Default",
			"Items.Preset_Knuckles_Golden",
			"Items.Preset_Knuckles_Military",
			"Items.Preset_Knuckles_Pimp",
			"Items.Preset_Hammer_Default",
			"Items.Preset_Hammer_Sasquatch",
			"Items.Preset_Shovel_Caretaker",
			"Items.Preset_Katana_Arasaka_2020",
			"Items.Preset_Katana_Cocktail",
			"Items.Preset_Katana_Default",
			"Items.Preset_Katana_GoG",
			"Items.Preset_Katana_Hiromi",
			"Items.Preset_Katana_Military",
			"Items.Preset_Katana_Neon",
			"Items.Preset_Katana_Saburo",
			"Items.Preset_Katana_Surgeon",
			"Items.Preset_Katana_Takemura",
			"Items.Preset_Knife_Default",
			"Items.Preset_Knife_Military",
			"Items.Preset_Knife_Stinger",
			"Items.Preset_Tanto_Default",
			"Items.Preset_Butchers_Knife_Default",
			"Items.Preset_Chefs_Knife_Default",
			"Items.Preset_Kukri_Default",
			"Items.Preset_Kukri_Voodoo",
			"Items.Preset_Machete_Default",
			"Items.Preset_Machete_Maelstrom",
			"Items.Preset_Machete_Valentinos",
			"Items.Preset_Baton_Alpha",
			"Items.Preset_Baton_Beta",
			"Items.Preset_Baton_Gamma",
			"Items.Preset_Baton_Tinker_Bell",
			"Items.Preset_Cane_Fingers",
			"Items.Preset_Crowbar_Default",
			"Items.Preset_Dildo_Stout",
			"Items.Preset_Iron_Pipe_Default",
			"Items.Preset_Pipe_Wrench_Default",
			"Items.Preset_Tire_Iron_Default",
			"Items.Preset_Tomahawk_Default",
			"Items.Preset_Baseball_Bat_Default",
			"Items.Preset_Baseball_Bat_Denny",
			"Items.Preset_Kanabo_Default",
			"Items.ProjectileLauncher",
			"Items.MantisBlades",
			"Items.NanoWires",
			"Items.StrongArms",
		};
		static const char* const s_sandboxAmmo[] = { "Ammo.HandgunAmmo", "Ammo.RifleAmmo", "Ammo.ShotgunAmmo", "Ammo.SniperRifleAmmo" };
		static const char* const c_equipWeapon = "Items.Preset_Ajax_Default";
		static const char* const c_noEncumbrance = "GameplayRestriction.NoEncumbrance";

		constexpr Uint32 c_vehiclesTotal = Uint32( sizeof( s_sandboxVehicles ) / sizeof( s_sandboxVehicles[ 0 ] ) );
		// [coreplay] ?sandboxPick=a,b,c spawns only those list indices (in that order); slot i -> record s_pick[ i ]
		static Uint32 s_pick[ c_vehiclesTotal ];
		static Bool s_pickInit = false;
		static const char* VName( Uint32 i ) { if ( !s_pickInit ) { for ( Uint32 k = 0; k < c_vehiclesTotal; ++k ) s_pick[ k ] = k; s_pickInit = true; } return s_sandboxVehicles[ s_pick[ i < c_vehiclesTotal ? i : 0 ] ]; }
		constexpr Uint32 c_weaponsTotal = Uint32( sizeof( s_sandboxWeapons ) / sizeof( s_sandboxWeapons[ 0 ] ) );
		constexpr Uint32 c_ammoAmount = 9999;
		constexpr Uint32 c_defaultVehicleCap = 32;
		constexpr Uint32 c_perRow = 20;
		constexpr Float c_spacing = 6.0f;
		constexpr Float c_rowGap = 12.0f;
		constexpr Float c_firstRow = 10.0f;
		static Int32 s_nearFirst = 0; // [nomad] ?sandboxNear=1
		constexpr Float c_zLift = 0.5f;
		constexpr Float c_startDelay = 6.0f;
		constexpr Uint32 c_weaponsPerTick = 16;
		constexpr Float c_vehicleInterval = 0.25f;
		constexpr Float c_verifyDelay = 20.0f;

		enum class Phase { Off, WaitPlayer, Weapons, Equip, Vehicles, Verify, Done };

		struct State
		{
			Int32 enabled = -1; // -1 = URL not read yet
			Int32 weaponsEnabled = 1;
			Uint32 vehicleCap = c_defaultVehicleCap;
			Bool armedOnce = false;
			Phase phase = Phase::Off;
			WeakHandle< game::Object > player;
			Float timer = 0.0f;
			Uint32 next = 0;
			Uint32 vehicleCount = 0;
			Uint32 weaponsOk = 0;
			Uint32 weaponsFail = 0;
			Uint32 requestFail = 0;
			Vector3 origin;
			Vector3 forward;
			Vector3 right;
			Quaternion orientation;
			std::mutex lock;
			ent::EntityID ids[ c_vehiclesTotal ];
			Bool stubOk[ c_vehiclesTotal ] = {};
			Bool stubDone[ c_vehiclesTotal ] = {};
			Uint32 stubsOk = 0;
			Uint32 stubsFail = 0;
			Vector3 req[ c_vehiclesTotal ]; // [sandbox-floor] requested grid positions
		};
		static State s;

		static void ReadUrl()
		{
			if ( s.enabled >= 0 )
			{
				return;
			}
			Int32 enabled = 0;
			Int32 cap = -1;
			Int32 weapons = 1;
#ifdef __EMSCRIPTEN__
			enabled = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'sandbox' ); return ( v !== null && v !== '0' && v !== 'false' ) ? 1 : 0; } catch ( e ) { return 0; }
			} );
			cap = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'sandboxVehicles' ); if ( v === null ) return -1; if ( v === 'all' ) return 1000000; const n = parseInt( v, 10 ); return isNaN( n ) ? -1 : n; } catch ( e ) { return -1; }
			} );
			weapons = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'sandboxWeapons' ); return ( v === '0' || v === 'false' ) ? 0 : 1; } catch ( e ) { return 1; }
			} );
#endif
			s.weaponsEnabled = weapons;
#ifdef __EMSCRIPTEN__
			// [nomad] ?sandboxNear=1 (set by ?start=nomad&startVehicle=N): vehicle 0 spawns beside the player,
			// 3 m to the right, facing the player's direction, so its driver door (left side) is ~1.5 m away.
			s_nearFirst = MAIN_THREAD_EM_ASM_INT( {
				try { return new URLSearchParams( location.search ).get( 'sandboxNear' ) === '1' ? 1 : 0; } catch ( e ) { return 0; }
			} );
#endif
			s.vehicleCap = cap < 0 ? c_defaultVehicleCap : Uint32( cap );
			s.vehicleCount = s.vehicleCap < c_vehiclesTotal ? s.vehicleCap : c_vehiclesTotal;
			VName( 0 ); // init identity pick
#ifdef __EMSCRIPTEN__
			// [coreplay] ?sandboxPick=0,2,5,23,52 : spawn exactly these list indices
			const Int32 pickCount = MAIN_THREAD_EM_ASM_INT( {
				try { const v = new URLSearchParams( location.search ).get( 'sandboxPick' ); if ( !v ) return 0; return v.split( ',' ).filter( x => x !== '' ).length; } catch ( e ) { return 0; }
			} );
			if ( pickCount > 0 )
			{
				Uint32 n = 0;
				for ( Int32 k = 0; k < pickCount && n < c_vehiclesTotal; ++k )
				{
					const Int32 v = MAIN_THREAD_EM_ASM_INT( {
						try { return parseInt( new URLSearchParams( location.search ).get( 'sandboxPick' ).split( ',' ).filter( x => x !== '' )[ $0 ], 10 ); } catch ( e ) { return -1; }
					}, k );
					if ( v >= 0 && Uint32( v ) < c_vehiclesTotal )
					{
						s_pick[ n++ ] = Uint32( v );
					}
				}
				s.vehicleCount = n;
				s.vehicleCap = n;
				for ( Uint32 k = 0; k < n; ++k )
				{
					std::fprintf( stderr, "[coreplay] sandboxPick slot %u = list %u %s\n", k, s_pick[ k ], VName( k ) );
				}
			}
#endif
			s.enabled = enabled;
			{
				// [tkfix] hit probes: sandbox or ?coreplay=1; &coreplayHits=0 disables
				Int32 hits = enabled;
#ifdef __EMSCRIPTEN__
				hits = MAIN_THREAD_EM_ASM_INT( {
					try { const q = new URLSearchParams( location.search ); if ( q.get( 'coreplayHits' ) === '0' ) return 0; const c = q.get( 'coreplay' ); return ( c !== null && c !== '0' ) ? 1 : $0; } catch ( e ) { return $0; }
				}, enabled );
#endif
				g_coreplayHitProbe = hits ? 1 : 0;
				std::fprintf( stderr, "[coreplay] hit probes %s (?sandbox or ?coreplay=1; &coreplayHits=0 = off)\n", hits ? "ON" : "off" );
			}
			if ( enabled )
			{
				std::fprintf( stderr, "[sandbox-spawn] enabled: %u/%u vehicles (cap %u, ?sandboxVehicles=N|all), weapons %s (%u + %u ammo types)\n",
					s.vehicleCount, c_vehiclesTotal, s.vehicleCap, weapons ? "on" : "off", c_weaponsTotal, Uint32( sizeof( s_sandboxAmmo ) / sizeof( s_sandboxAmmo[ 0 ] ) ) );
			}
		}

		static void OnPlayerAttached( WeakHandle< game::Object > playerObject )
		{
			ReadUrl();
			if ( s.enabled <= 0 || s.armedOnce )
			{
				return;
			}
			s.armedOnce = true;
			s.player = playerObject;
			s.timer = 0.0f;
			s.phase = Phase::WaitPlayer;
			std::fprintf( stderr, "[sandbox-spawn] player attached; content starts in %.1f s\n", c_startDelay );
		}

		static void GiveWeapons( const game::IGameInstance* gi, game::Object& player )
		{
			auto transactions = gi->GetSystemPtr< game::ITransactionSystem >();
			if ( !transactions )
			{
				std::fprintf( stderr, "[sandbox-spawn] FAIL no TransactionSystem; weapons skipped\n" );
				s.next = c_weaponsTotal;
				return;
			}
			const Uint32 end = ( s.next + c_weaponsPerTick ) < c_weaponsTotal ? ( s.next + c_weaponsPerTick ) : c_weaponsTotal;
			for ( ; s.next < end; ++s.next )
			{
				const char* name = s_sandboxWeapons[ s.next ];
				const Bool ok = transactions->GiveItem( player, game::data::TweakDBID( name ), 1, game::EInventoryOperationOverrides::FlagItemAddedCallbackAsSilent );
				if ( ok )
				{
					++s.weaponsOk;
				}
				else
				{
					++s.weaponsFail;
					std::fprintf( stderr, "[sandbox-spawn] weapon FAIL %s (GiveItem returned false; skipped)\n", name );
				}
			}
		}

		static void GiveAmmoAndPerks( const game::IGameInstance* gi, game::Object& player )
		{
			if ( auto statusEffects = gi->GetSystemPtr< game::IStatusEffectSystem >() )
			{
				const Bool ok = statusEffects->ApplyStatusEffect( player.GetEntityID(), game::data::TweakDBID( c_noEncumbrance ) );
				std::fprintf( stderr, "[sandbox-spawn] %s %s\n", ok ? "applied" : "FAIL apply", c_noEncumbrance );
			}
			auto transactions = gi->GetSystemPtr< game::ITransactionSystem >();
			if ( !transactions )
			{
				return;
			}
			for ( const char* ammo : s_sandboxAmmo )
			{
				const Bool ok = transactions->GiveItem( player, game::data::TweakDBID( ammo ), c_ammoAmount, game::EInventoryOperationOverrides::FlagItemAddedCallbackAsSilent );
				std::fprintf( stderr, "[sandbox-spawn] ammo %s x%u %s\n", ammo, c_ammoAmount, ok ? "ok" : "FAIL (skipped)" );
			}
		}

		static void EquipOne( const game::IGameInstance* gi, game::Object& player )
		{
			auto transactions = gi->GetSystemPtr< game::ITransactionSystem >();
			auto scriptables = gi->GetSystemPtr< game::IScriptableSystemsContainer >();
			if ( !transactions || !scriptables )
			{
				std::fprintf( stderr, "[sandbox-spawn] equip FAIL: missing TransactionSystem/ScriptableSystemsContainer\n" );
				return;
			}
			const game::ItemID itemID = transactions->GetFirstItemID( player, game::data::TweakDBID( c_equipWeapon ) );
			if ( !itemID.IsValid() )
			{
				std::fprintf( stderr, "[sandbox-spawn] equip FAIL: %s not in inventory\n", c_equipWeapon );
				return;
			}
			THandle< ScriptableSystem > equipmentSystem = scriptables->Get( RED_NAME_CONSTEXPR( "EquipmentSystem" ) );
			if ( !equipmentSystem )
			{
				std::fprintf( stderr, "[sandbox-spawn] equip FAIL: EquipmentSystem not found\n" );
				return;
			}
			WeakHandle< game::Object > owner = s.player;
			THandle< EquipRequest > equipRequest = CreateHandle< EquipRequest >( owner, itemID, -1, false, true );
			equipmentSystem->QueueRequest( equipRequest );
			THandle< DrawItemRequest > drawRequest = CreateHandle< DrawItemRequest >( owner, itemID );
			equipmentSystem->QueueRequest( drawRequest );
			std::fprintf( stderr, "[sandbox-spawn] equip+draw queued: %s\n", c_equipWeapon );
		}

		static void CaptureGrid( game::Object& player )
		{
			s.origin = player.GetWorldPosition().AsVector3();
			s.forward = player.GetWorldForward();
			s.right = player.GetWorldRight();
			s.forward.Z = 0.0f;
			s.right.Z = 0.0f;
			s.forward.Normalize();
			s.right.Normalize();
			s.orientation = player.GetWorldOrientation();
			const Uint32 rows = ( s.vehicleCount + c_perRow - 1 ) / c_perRow;
			std::fprintf( stderr, "[sandbox-spawn] grid origin (%.2f, %.2f, %.2f) forward (%.2f, %.2f): %u vehicles, %u per row, %u rows, %.0f m wide, rows %.0f..%.0f m ahead\n",
				s.origin.X, s.origin.Y, s.origin.Z, s.forward.X, s.forward.Y, s.vehicleCount, c_perRow, rows,
				Float( ( s.vehicleCount < c_perRow ? s.vehicleCount : c_perRow ) - 1 ) * c_spacing, c_firstRow, c_firstRow + Float( rows ? rows - 1 : 0 ) * c_rowGap );
		}

		static void SpawnVehicle( const game::IGameInstance* gi, Uint32 index )
		{
			const char* name = VName( index );
			auto vehicles = gi->GetSystemPtr< game::IVehicleSystem >();
			if ( !vehicles )
			{
				++s.requestFail;
				std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s FAIL: no VehicleSystem\n", index, name );
				return;
			}
			const Uint32 row = index / c_perRow;
			const Uint32 col = index % c_perRow;
			const Uint32 remaining = s.vehicleCount - row * c_perRow;
			const Uint32 inRow = remaining < c_perRow ? remaining : c_perRow;
			const Float side = ( Float( col ) - ( Float( inRow ) - 1.0f ) * 0.5f ) * c_spacing;
			const Float ahead = c_firstRow + Float( row ) * c_rowGap;
			Vector3 pos = s.origin + s.forward * ahead + s.right * side;
			if ( s_nearFirst && index == 0 )
			{
				pos = s.origin + s.forward * 0.5f + s.right * 3.0f; // [nomad] beside the player, driver door facing them
			}
			pos.Z = s.origin.Z + c_zLift;
			s.req[ index ] = pos;

			const Uint32 captured = index;
			const Bool requested = vehicles->RequestNewVehicle( WorldTransform( pos, s.orientation ), game::data::TweakDBID( name ),
				[ captured, gi ]( game::EntityStub* stub )
				{
					if ( stub && captured < 3 )
					{
						const Vector3 sp = stub->GetEntityWorldPositionAsVector3();
						std::fprintf( stderr, "[sandbox-floor] probe stub %u world position (%.2f, %.2f, %.2f)\n", captured, sp.X, sp.Y, sp.Z );
					}
					if ( stub )
					{
						// same as the garage path (vehicleGarageComponent.cpp): force the entity to spawn from the stub
						auto population = gi->GetSystemPtr< game::IPopulationSystem >();
						const Bool always = population && population->SetEntityAlwaysSpawned( stub->GetEntityId(), true );
						std::fprintf( stderr, "[sandbox-spawn] vehicle %u SetEntityAlwaysSpawned %s\n", captured, always ? "ok" : "returned false" );
					}
					std::lock_guard< std::mutex > guard( s.lock );
					s.stubDone[ captured ] = true;
					if ( stub )
					{
						s.ids[ captured ] = stub->GetEntityId();
						s.stubOk[ captured ] = true;
						++s.stubsOk;
						std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s stub ok\n", captured, VName( captured ) );
					}
					else
					{
						++s.stubsFail;
						std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s FAIL: stub creation failed (skipped)\n", captured, VName( captured ) );
					}
				} );
			if ( requested )
			{
				std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s requested at (%.1f, %.1f, %.1f) row %u col %u\n", index, name, pos.X, pos.Y, pos.Z, row, col );
			}
			else
			{
				++s.requestFail;
				std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s FAIL: RequestNewVehicle refused (not a valid Vehicle_Record?; skipped)\n", index, name );
			}
		}

		static void Verify( const game::IGameInstance* gi )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			Uint32 entities = 0;
			Uint32 stubOnly = 0;
			Uint32 pending = 0;
			ent::EntityID ids[ c_vehiclesTotal ];
			Bool stubOk[ c_vehiclesTotal ] = {};
			Bool stubDone[ c_vehiclesTotal ] = {};
			{
				std::lock_guard< std::mutex > guard( s.lock );
				for ( Uint32 i = 0; i < c_vehiclesTotal; ++i )
				{
					ids[ i ] = s.ids[ i ];
					stubOk[ i ] = s.stubOk[ i ];
					stubDone[ i ] = s.stubDone[ i ];
				}
			}
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				if ( !stubDone[ i ] )
				{
					++pending;
					continue;
				}
				if ( !stubOk[ i ] )
				{
					continue;
				}
				if ( population )
				{
					if ( population->GetSpawnedEntity( ids[ i ] ) )
					{
						++entities;
						continue;
					}
				}
				++stubOnly;
				std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s: stub exists but NO entity after %.0f s\n", i, VName( i ), c_verifyDelay );
			}
			std::fprintf( stderr, "[sandbox-spawn] SUMMARY vehicles: %u listed, %u attempted, %u entities spawned, %u stub-only, %u stub failures, %u refused, %u callbacks pending, %u skipped by cap; weapons: %u ok, %u failed of %u\n",
				c_vehiclesTotal, s.vehicleCount, entities, stubOnly, s.stubsFail, s.requestFail, pending, c_vehiclesTotal - s.vehicleCount, s.weaponsOk, s.weaponsFail, c_weaponsTotal );
		}

		// CP2077 web [sandbox-floor] probe: where are the player and the spawned vehicles (falling = Z keeps dropping).
		static Float s_probeClock = 0.0f;
		static Float s_probeNextPlayer = 0.0f;
		static Int32 s_probeVehicleRound = 0;
		static Float s_probeNextVehicles = -1.0f;

		static void ProbeVehicles( const game::IGameInstance* gi, const char* tag )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			if ( !population )
			{
				return;
			}
			ent::EntityID ids[ c_vehiclesTotal ];
			Bool stubOk[ c_vehiclesTotal ] = {};
			{
				std::lock_guard< std::mutex > guard( s.lock );
				for ( Uint32 i = 0; i < c_vehiclesTotal; ++i )
				{
					ids[ i ] = s.ids[ i ];
					stubOk[ i ] = s.stubOk[ i ];
				}
			}
			Uint32 found = 0, below = 0;
			Float minZ = 1e9f, maxZ = -1e9f;
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				if ( !stubOk[ i ] )
				{
					continue;
				}
				THandle< ent::Entity > e = population->GetSpawnedEntity( ids[ i ] );
				if ( !e )
				{
					continue;
				}
				++found;
				const Vector3 p = e->GetWorldPosition().AsVector3();
				minZ = p.Z < minZ ? p.Z : minZ;
				maxZ = p.Z > maxZ ? p.Z : maxZ;
				if ( p.Z < -2.0f )
				{
					++below;
				}
				if ( i < 6 || p.Z < -2.0f )
				{
					std::fprintf( stderr, "[sandbox-floor] probe %s vehicle %u %s at (%.2f, %.2f, %.2f)\n", tag, i, VName( i ), p.X, p.Y, p.Z );
				}
			}
			std::fprintf( stderr, "[sandbox-floor] probe %s vehicles: %u entities, z min %.2f max %.2f, %u below z=-2 (fell through)\n", tag, found, found ? minZ : 0.0f, found ? maxZ : 0.0f, below );
		}

		// [sandbox-floor] WORKAROUND: on web the spawned vehicle entities end up at the world origin (all 32 at exactly
		// 0,0,0, piled under the player) although RequestNewVehicle got the grid transform. Teleport every entity that
		// is more than 3 m from its requested spot back onto the grid (z 0.3 above the plate).
		static void FixPositions( const game::IGameInstance* gi )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >();
			if ( !population || !teleport )
			{
				std::fprintf( stderr, "[sandbox-floor] fix FAIL: population=%d teleport=%d\n", population ? 1 : 0, teleport ? 1 : 0 );
				return;
			}
			ent::EntityID ids[ c_vehiclesTotal ];
			Bool stubOk[ c_vehiclesTotal ] = {};
			{
				std::lock_guard< std::mutex > guard( s.lock );
				for ( Uint32 i = 0; i < c_vehiclesTotal; ++i )
				{
					ids[ i ] = s.ids[ i ];
					stubOk[ i ] = s.stubOk[ i ];
				}
			}
			Uint32 moved = 0, ok = 0, notObject = 0;
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				if ( !stubOk[ i ] )
				{
					continue;
				}
				THandle< ent::Entity > e = population->GetSpawnedEntity( ids[ i ] );
				if ( !e )
				{
					continue;
				}
				const Vector3 p = e->GetWorldPosition().AsVector3();
				const Vector3 r = s.req[ i ];
				const Float dx = p.X - r.X, dy = p.Y - r.Y;
				if ( dx * dx + dy * dy <= 9.0f )
				{
					++ok;
					continue;
				}
				THandle< game::Object > obj = Cast< game::Object >( e );
				if ( !obj )
				{
					++notObject;
					continue;
				}
				teleport->Teleport( obj, Vector4( r.X, r.Y, s.origin.Z + 0.3f, 1.0f ), EulerAngles() );
				++moved;
			}
			std::fprintf( stderr, "[sandbox-floor] fix: teleported %u vehicles onto the grid (workaround for origin spawn), %u already in place, %u not game objects\n", moved, ok, notObject );
		}

		// CP2077 web [flash]: RequestNewVehicle is the garage summon path, so the spawned vehicles come up with autopilot /
		// QuestOrScene physics-disable reasons. vehicle::BaseObject::Update_PostMove then runs CheckVehicleDestructionInteraction
		// (an overlap query per chassis shape) for every vehicle every frame. Log the reasons, and with ?sandbox (unless
		// &sandboxPark=0) park each vehicle like a normal parked car: autopilot reset, only the Asleep reason left.
		static Bool ParkEnabled()
		{
			static const int v = EM_ASM_INT( {
				try { return new URLSearchParams( location.search ).get( 'sandboxPark' ) === '0' ? 0 : 1; } catch ( e ) { return 1; }
			} );
			return v != 0;
		}
		static void ParkVehicles( const game::IGameInstance* gi, const char* tag, Bool park )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			if ( !population )
			{
				return;
			}
			ent::EntityID ids[ c_vehiclesTotal ];
			Bool stubOk[ c_vehiclesTotal ] = {};
			{
				std::lock_guard< std::mutex > guard( s.lock );
				for ( Uint32 i = 0; i < c_vehiclesTotal; ++i )
				{
					ids[ i ] = s.ids[ i ];
					stubOk[ i ] = s.stubOk[ i ];
				}
			}
			Uint32 found = 0, autopilot = 0, parked = 0;
			Uint32 reasonCounts[ 10 ] = {};
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				if ( !stubOk[ i ] )
				{
					continue;
				}
				THandle< ent::Entity > e = population->GetSpawnedEntity( ids[ i ] );
				vehicle::BaseObject* veh = e ? Cast< vehicle::BaseObject >( e.Get() ) : nullptr;
				if ( !veh )
				{
					continue;
				}
				++found;
				Uint32 mask = 0;
				for ( Uint32 b = 0; b < 10; ++b )
				{
					if ( veh->IsPhysicsDisabled( vehicle::DisabledPhysicsReason( 1u << b ) ) )
					{
						mask |= 1u << b;
						++reasonCounts[ b ];
					}
				}
				const Bool ap = veh->IsAutopilotActive();
				autopilot += ap ? 1 : 0;
				if ( i < 4 )
				{
					std::fprintf( stderr, "[flash] vehicle %s %u %s: disabledPhysicsReasons=0x%x autopilot=%d\n", tag, i, VName( i ), mask, ap ? 1 : 0 );
				}
				if ( park )
				{
					if ( ap )
					{
						veh->ResetAutopilot();
					}
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::Autopilot, false );
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::QuestOrScene, false );
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::StreamingHack_InitialAsleep, false );
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::StreamingHack_StuffAroundStreamedOut, false );
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::StreamingHack_WaitingForStreaming, false );
					veh->DisablePhysics( vehicle::DisabledPhysicsReason::Asleep, true );
					++parked;
				}
			}
			std::fprintf( stderr, "[flash] vehicles %s: %u found, %u autopilot active, %u parked; reason counts traffic=%u debug=%u exploded=%u autopilot=%u asleep=%u initialAsleep=%u streamedOut=%u questOrScene=%u gizmo=%u waitingStreaming=%u\n",
				tag, found, autopilot, parked, reasonCounts[ 0 ], reasonCounts[ 1 ], reasonCounts[ 2 ], reasonCounts[ 3 ], reasonCounts[ 4 ], reasonCounts[ 5 ], reasonCounts[ 6 ], reasonCounts[ 7 ], reasonCounts[ 8 ], reasonCounts[ 9 ] );
		}

		static void Probe( const game::IGameInstance* gi, Float timeDelta )
		{
			s_probeClock += timeDelta;
			if ( s_probeClock <= 30.0f && s_probeClock >= s_probeNextPlayer )
			{
				s_probeNextPlayer += 2.0f;
				if ( THandle< game::Object > player = s.player.ToHandle() )
				{
					const Vector3 p = player->GetWorldPosition().AsVector3();
					std::fprintf( stderr, "[sandbox-floor] probe t=%.0fs player at (%.2f, %.2f, %.2f)\n", s_probeClock, p.X, p.Y, p.Z );
				}
			}
			if ( s_probeNextVehicles >= 0.0f && s_probeClock >= s_probeNextVehicles && gi )
			{
				static const char* const c_tags[] = { "verify+15s", "verify+60s" };
				ProbeVehicles( gi, c_tags[ s_probeVehicleRound ] );
				ParkVehicles( gi, c_tags[ s_probeVehicleRound ], s_probeVehicleRound == 0 && ParkEnabled() ); // park once the grid teleport has landed (verify+15s)
				if ( THandle< game::Object > player = s.player.ToHandle() )
				{
					const Vector3 p = player->GetWorldPosition().AsVector3();
					std::fprintf( stderr, "[sandbox-floor] probe %s player at (%.2f, %.2f, %.2f)\n", c_tags[ s_probeVehicleRound ], p.X, p.Y, p.Z );
				}
				++s_probeVehicleRound;
				s_probeNextVehicles = s_probeVehicleRound == 1 ? s_probeClock + 45.0f : -1.0f;
			}
		}

		// ---------------------------------------------------------------------------------------------------------
		// CP2077 web [coreplay]: core-gameplay probes + test commands for ?sandbox=1.
		// Probes (every 1 s while mounted, else on change / every 5 s): mounted vehicle (index, pos, speed,
		// disabledPhysicsReasons, driver/controls/engine), active weapon (list index, magazine, reloading),
		// shots (magazine drops), vehicle health changes (hits). Commands: the test writes HEAP32 at the index logged
		// as '[coreplay] cmd block' : [0]=seq (bump to run), [1]=op, [2]=arg, [3]=arg2.
		//   op 1 = place player beside vehicle arg on its left (driver) side, arg2 decimetres away (default 26), facing it
		//   op 2 = place player arg2 decimetres (default 120) in front of vehicle arg, facing it (shooting tests)
		//   op 3 = equip+draw test weapon arg (c_testWeapons)        op 4 = log all vehicle states now
		// ---------------------------------------------------------------------------------------------------------
		static Int32 s_cpLastSeq = 0;
		static Bool s_cpAnnounced = false;
		static Float s_cpClock = 0.0f;
		static Float s_cpNext = 0.0f;
		static Int32 s_cpMounted = -2;
		static Int32 s_cpWeapon = -3;
		static Int32 s_cpMag = -1;
		static Bool s_cpReloading = false;
		static Uint32 s_cpShots = 0;
		static Float s_cpHealth[ c_vehiclesTotal ];
		static Bool s_cpHealthInit = false;
		static Float s_cpCamLogAt = -1.0f;
		static Vector3 s_cpFace;
		// [tkfix] vehicle position watcher (>5 m per 1 s probe) + who moved it; scheduled aim report; damage-target health table
		static Vector3 s_cpLastPos[ c_vehiclesTotal ];
		static Bool s_cpPosInit[ c_vehiclesTotal ];
		static Int32 s_cpMovedOp[ c_vehiclesTotal ];
		static Float s_cpMovedAt[ c_vehiclesTotal ];
		static Float s_cpAimReportAt = -1.0f;
		static Uint32 s_cpAimReportTarget = 0;
		static unsigned long long s_cpHpHash[ 16 ] = {};
		static Float s_cpHpLast[ 16 ] = {};
		static const char* const c_testWeapons[] = {
			"Items.Preset_Unity_Default", "Items.Preset_Ajax_Default", "Items.Preset_Carnage_Default", "Items.Preset_Grad_Default",
			"Items.Preset_Yukimura_Default", "Items.Preset_Nekomata_Default", "Items.Preset_Katana_Default", "Items.MantisBlades",
			"Items.Preset_Copperhead_Default", "Items.Preset_Tactician_Default" };
		constexpr Uint32 c_testWeaponsTotal = Uint32( sizeof( c_testWeapons ) / sizeof( c_testWeapons[ 0 ] ) );

		template< class TPop > static vehicle::BaseObject* CpVehicle( TPop population, Uint32 i, THandle< ent::Entity >& keep )
		{
			if ( !population || i >= s.vehicleCount )
			{
				return nullptr;
			}
			ent::EntityID id;
			Bool ok = false;
			{
				std::lock_guard< std::mutex > guard( s.lock );
				id = s.ids[ i ];
				ok = s.stubOk[ i ];
			}
			if ( !ok )
			{
				return nullptr;
			}
			keep = population->GetSpawnedEntity( id );
			return keep ? Cast< vehicle::BaseObject >( keep.Get() ) : nullptr;
		}

		static Uint32 CpMask( vehicle::BaseObject* veh )
		{
			Uint32 mask = 0;
			for ( Uint32 b = 0; b < 10; ++b )
			{
				mask |= veh->IsPhysicsDisabled( vehicle::DisabledPhysicsReason( 1u << b ) ) ? ( 1u << b ) : 0u;
			}
			return mask;
		}

		static void CpLogVehicle( const char* tag, Uint32 i, vehicle::BaseObject* veh )
		{
			const Vector3 p = veh->GetWorldPosition().AsVector3();
			const Vector3 v = veh->GetRigidBody().GetLinearVelocity();
			const Float speed = std::sqrt( v.X * v.X + v.Y * v.Y + v.Z * v.Z );
			std::fprintf( stderr, "[coreplay] %s vehicle %u %s pos (%.2f, %.2f, %.2f) speed %.2f m/s vel (%.2f, %.2f, %.2f) physMask=0x%x playerMounted=%d playerDriver=%d controls=%d engine=%d autopilot=%d input acc %.1f brk %.1f hb %.1f turn %.1f lean %.1f shoot %d%d%d\n",
				tag, i, VName( i ), p.X, p.Y, p.Z, speed, v.X, v.Y, v.Z, CpMask( veh ), veh->IsPlayerMounted() ? 1 : 0, veh->IsPlayerDriver() ? 1 : 0,
				veh->IsPlayerDrivingControlsEnabled() ? 1 : 0, veh->IsEngineTurnedOn() ? 1 : 0, veh->IsAutopilotActive() ? 1 : 0,
				veh->GetVehicleInput().accelerate, veh->GetVehicleInput().brake, veh->GetVehicleInput().handBrake, veh->GetVehicleInput().turnLR, veh->GetVehicleInput().balanceFB,
				veh->GetVehicleInput().shootPrimary ? 1 : 0, veh->GetVehicleInput().shootSecondary ? 1 : 0, veh->GetVehicleInput().shootTertiary ? 1 : 0 );
		}

		static void CpLogCamera( const game::IGameInstance* gi, game::Object& player, const char* tag )
		{
			auto cameras = gi->GetSystemPtr< game::ICameraSystem >();
			const Vector4 cf = cameras ? cameras->GetActiveCameraForward() : Vector4( 0.0f, 0.0f, 0.0f, 0.0f );
			const Vector3 pf = player.GetWorldForward();
			const Vector3 pp = player.GetWorldPosition().AsVector3();
			std::fprintf( stderr, "[coreplay] camera %s: camFwd (%.2f, %.2f, %.2f) camYaw %.1f | playerFwd (%.2f, %.2f) playerYaw %.1f | wanted face (%.2f, %.2f) yaw %.1f | player (%.2f, %.2f, %.2f)\n",
				tag, cf.X, cf.Y, cf.Z, EulerAngles::YawFromXY( cf.X, cf.Y ), pf.X, pf.Y, EulerAngles::YawFromXY( pf.X, pf.Y ), s_cpFace.X, s_cpFace.Y,
				EulerAngles::YawFromXY( s_cpFace.X, s_cpFace.Y ), pp.X, pp.Y, pp.Z );
		}

		// ---------------------------------------------------------------------------------------------------------
		// [coreplay] SANDBOX AV FLIGHT. The engine has no player flight controller for AVs (vehicle::AVBaseObject only
		// follows autopilot/scene transforms), so in ?sandbox sessions an AV driven by the player is flown kinematically:
		//   W/S = forward/back thrust, A/D = yaw, Space or Shift = climb, C = descend (not Ctrl: Ctrl+W closes the tab), release = hover.
		// Velocities are smoothed (exponential), speed clamped, attitude kept upright with a small bank/pitch for feel,
		// altitude clamped to the ground plane it took off from (landing = descend to the floor, then hold F to exit).
		// &sandboxFlight=0 disables it.
		// ---------------------------------------------------------------------------------------------------------
		struct CpFlight
		{
			Bool active = false;
			Vector3 vel = Vector3::ZEROS();
			Float yaw = 0.0f;
			Float groundZ = 0.0f;
			Float logClock = 0.0f;
			Float maxAlt = 0.0f;
			Float maxSpeed = 0.0f;
		};
		static CpFlight s_flight;

		static Bool FlightEnabled()
		{
#ifdef __EMSCRIPTEN__
			static const int v = MAIN_THREAD_EM_ASM_INT( {
				try { return new URLSearchParams( location.search ).get( 'sandboxFlight' ) === '0' ? 0 : 1; } catch ( e ) { return 1; }
			} );
			return v != 0;
#else
			return true;
#endif
		}

		static void CpFlightTick( const game::IGameInstance* gi, Float dt )
		{
			if ( dt <= 0.0f || dt > 0.25f || !FlightEnabled() )
			{
				return;
			}
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			vehicle::BaseObject* av = nullptr;
			THandle< ent::Entity > keepAv;
			for ( Uint32 i = 0; i < s.vehicleCount && !av; ++i )
			{
				THandle< ent::Entity > keep;
				vehicle::BaseObject* veh = CpVehicle( population, i, keep );
				if ( veh && veh->IsPlayerMounted() && Cast< vehicle::AVBaseObject >( veh ) )
				{
					av = veh;
					keepAv = keep;
				}
			}
			if ( !av )
			{
				if ( s_flight.active )
				{
					std::fprintf( stderr, "[coreplay] flight END (driver left the AV): max altitude %.1f m, max speed %.1f m/s\n", s_flight.maxAlt, s_flight.maxSpeed );
					s_flight.active = false;
				}
				return;
			}
			const WorldTransform wt = av->GetWorldTransform();
			Vector3 pos = wt.GetPosition().AsVector3();
			if ( !s_flight.active )
			{
				s_flight = CpFlight();
				s_flight.active = true;
				const Vector3 f = av->GetWorldForward();
				s_flight.yaw = EulerAngles::YawFromXY( f.X, f.Y );
				s_flight.groundZ = pos.Z;
				std::fprintf( stderr, "[coreplay] flight START: AV at (%.2f, %.2f, %.2f) yaw %.1f (W/S thrust, A/D yaw, Space/Shift up, C down; Ctrl is not used: Ctrl+W closes the browser tab)\n", pos.X, pos.Y, pos.Z, s_flight.yaw );
			}
			// Input: the engine only fills vehicle::Input for a 'driver' seat with drive contexts (AV seats often are not),
			// so read the physical keys from the page (listeners installed once on the main thread; codes, layout-proof).
			// No per-frame proxy: main-thread key listeners write the bitmask into g_cpShared (see CpSharedInit).
			const Int32 keys = __atomic_load_n( &g_cpShared[ 4 ], __ATOMIC_RELAXED );
			const vehicle::Input& in = av->GetVehicleInput();
			const Float thrust = Clamp( in.accelerate - in.brake + ( ( keys & 1 ) ? 1.0f : 0.0f ) - ( ( keys & 2 ) ? 1.0f : 0.0f ), -1.0f, 1.0f );
			const Float turn = Clamp( in.turnLR + ( ( keys & 8 ) ? 1.0f : 0.0f ) - ( ( keys & 4 ) ? 1.0f : 0.0f ), -1.0f, 1.0f );
			Float lift = ( in.handBrake > 0.5f || ( keys & 16 ) || ( keys & 32 ) ) ? 1.0f : 0.0f;
			lift -= ( keys & 64 ) ? 1.0f : 0.0f;
			lift = Clamp( lift, -1.0f, 1.0f );

			constexpr Float c_maxForward = 40.0f, c_maxVertical = 10.0f, c_yawRate = 70.0f;
			s_flight.yaw -= turn * c_yawRate * dt; // D (turn +) = clockwise seen from above
			const Float yawRad = DEG2RAD( s_flight.yaw );
			const Vector3 fwd( -std::sin( yawRad ), std::cos( yawRad ), 0.0f ); // YawFromXY convention (yaw 0 = +Y)
			const Vector3 want = fwd * ( thrust * c_maxForward ) + Vector3( 0.0f, 0.0f, lift * c_maxVertical );
			const Float k = 1.0f - std::exp( -2.5f * dt );
			s_flight.vel = s_flight.vel + ( want - s_flight.vel ) * k;
			pos = pos + s_flight.vel * dt;
			if ( pos.Z < s_flight.groundZ )
			{
				pos.Z = s_flight.groundZ;
				if ( s_flight.vel.Z < 0.0f ) s_flight.vel.Z = 0.0f;
			}
			const Float fwdSpeed = s_flight.vel.X * fwd.X + s_flight.vel.Y * fwd.Y;
			const EulerAngles att( turn * 12.0f, -Clamp( fwdSpeed / c_maxForward, -1.0f, 1.0f ) * 6.0f, s_flight.yaw ); // roll, pitch, yaw
			av->ForceTransform( WorldTransform( pos, att.ToQuat() ), true );
			av->GetRigidBody().ForceSetLinearVelocity( Vector4( s_flight.vel.X, s_flight.vel.Y, s_flight.vel.Z, 0.0f ) );
			av->GetRigidBody().ForceSetAngularVelocity( Vector4( 0.0f, 0.0f, 0.0f, 0.0f ) );
			const Float alt = pos.Z - s_flight.groundZ;
			const Float speed = std::sqrt( s_flight.vel.X * s_flight.vel.X + s_flight.vel.Y * s_flight.vel.Y + s_flight.vel.Z * s_flight.vel.Z );
			s_flight.maxAlt = alt > s_flight.maxAlt ? alt : s_flight.maxAlt;
			s_flight.maxSpeed = speed > s_flight.maxSpeed ? speed : s_flight.maxSpeed;
			s_flight.logClock += dt;
			if ( s_flight.logClock >= 1.0f )
			{
				s_flight.logClock = 0.0f;
				std::fprintf( stderr, "[coreplay] flight probe: altitude %.2f m, speed %.2f m/s, pos (%.1f, %.1f, %.2f) yaw %.0f | input thrust %.2f turn %.2f lift %.2f%s\n",
					alt, speed, pos.X, pos.Y, pos.Z, s_flight.yaw, thrust, turn, lift, alt < 0.05f ? " (landed)" : "" );
			}
		}

		// op 6: what does the player look at, and what interaction components does vehicle 'index' have?
		static void CpInteractionProbe( const game::IGameInstance* gi, game::Object& player, Uint32 index )
		{
			WeakHandle< game::Object > weakPlayer = s.player;
			if ( auto targeting = gi->GetSystemPtr< game::ITargetingSystem >() )
			{
				const ent::EntityID los = targeting->GetLookAtObject( weakPlayer, true, false );
				const ent::EntityID any = targeting->GetLookAtObject( weakPlayer, false, true );
				THandle< ent::IComponent > comp = targeting->GetLookAtComponent( weakPlayer, true, false ).ToHandle();
				std::fprintf( stderr, "[coreplay] interact lookAt LOS=%s noLOS=%s component=%s (%s)\n", los.ToDebugString().AsChar(), any.ToDebugString().AsChar(),
					comp ? comp->GetName().AsChar() : "none", comp ? comp->GetClass()->GetName().AsChar() : "-" );
				const ent::Entity* owner = comp ? comp->GetEntity() : nullptr;
				std::fprintf( stderr, "[coreplay] interact lookAt component owner %p id=%s class=%s | player id=%s\n", ( const void* )owner,
					owner ? owner->GetEntityID().ToDebugString().AsChar() : "-", owner ? owner->GetClass()->GetName().AsChar() : "-", player.GetEntityID().ToDebugString().AsChar() );
			}
			else
			{
				std::fprintf( stderr, "[coreplay] interact: no TargetingSystem\n" );
			}
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			THandle< ent::Entity > keep;
			vehicle::BaseObject* veh = CpVehicle( population, index, keep );
			if ( !veh )
			{
				return;
			}
			std::fprintf( stderr, "[coreplay] interact vehicle %u %s id=%s components=%u\n", index, VName( index ), veh->GetEntityID().ToDebugString().AsChar(), Uint32( veh->GetComponents().Size() ) );
			if ( THandle< vehicle::ChassisComponent > chassis = veh->GetChassisComponent() )
			{
				const physics::ProxyID pid = chassis->GetPhysicsProxyId();
				const physics::ProxyID kid = chassis->GetKinematicProxyID();
				for ( Uint32 which = 0; which < 2; ++which )
				{
					const physics::ProxyID id = which == 0 ? pid : kid;
					const Bool alive = physics::IsAlive( id );
					const Bool inScene = alive && physics::IsInScene( id );
					const Uint32 shapes = alive ? physics::GetNbShapes( id ) : 0;
					Vector3 c3 = Vector3::ZEROS();
					if ( alive )
					{
						const Box b = physics::GetCompoundBounds( id );
						c3 = Vector3( ( b.Min.X + b.Max.X ) * 0.5f, ( b.Min.Y + b.Max.Y ) * 0.5f, ( b.Min.Z + b.Max.Z ) * 0.5f );
						const Vector3 tp = physics::GetPosition( id );
						const Vector3 vpos = veh->GetWorldPosition().AsVector3();
						std::fprintf( stderr, "[coreplay] interact   chassis %s proxy world pos (%.2f, %.2f, %.2f) vs vehicle (%.2f, %.2f, %.2f)%s\n", which == 0 ? "dynamic" : "kinematic",
							tp.X, tp.Y, tp.Z, vpos.X, vpos.Y, vpos.Z, ( std::fabs( tp.X - vpos.X ) + std::fabs( tp.Y - vpos.Y ) > 1.0f ) ? "  <-- MISMATCH" : "" );
					}
					std::fprintf( stderr, "[coreplay] interact   chassis %s proxy alive=%d inScene=%d shapes=%u kinematic=%d boundsCenter (%.1f, %.1f, %.1f)\n", which == 0 ? "dynamic" : "kinematic",
						alive ? 1 : 0, inScene ? 1 : 0, shapes, alive ? ( physics::GetIsKinematic( id ) ? 1 : 0 ) : -1, c3.X, c3.Y, c3.Z );
					for ( Uint32 sh = 0; sh < shapes && sh < 6; ++sh )
					{
						const auto preset = physics::GetFilterPreset( id, 0, sh );
						std::fprintf( stderr, "[coreplay] interact     shape %u queryable=%d sim=%d queryMask=%llx preset=%s\n", sh, physics::GetIsQueryable( id, 0, sh ) ? 1 : 0,
							physics::GetIsSimulationShape( id, 0, sh ) ? 1 : 0, ( unsigned long long )physics::GetQueryFilter( id, 0, sh ).GetQueryMask(), preset.Size() ? preset[ 0 ].AsChar() : "-" );
					}
				}
			}
			else
			{
				std::fprintf( stderr, "[coreplay] interact   NO chassis component\n" );
			}
			for ( const auto& c : veh->GetComponents() )
			{
				if ( !c )
				{
					continue;
				}
				const AnsiChar* cls = c->GetClass()->GetName().AsChar();
				const AnsiChar* nm = c->GetName().AsChar();
				if ( std::strstr( cls, "nteraction" ) || std::strstr( nm, "nteraction" ) || std::strstr( cls, "ount" ) || std::strstr( cls, "lot" ) || std::strstr( nm, "seat" ) )
				{
					std::fprintf( stderr, "[coreplay] interact   component %s (%s) enabled=%d attached=%d\n", nm, cls, c->IsEnabled() ? 1 : 0, c->IsAttached() ? 1 : 0 );
				}
			}
		}

		static void CpPlace( const game::IGameInstance* gi, Uint32 index, Int32 op, Int32 dm )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >();
			THandle< ent::Entity > keep;
			vehicle::BaseObject* veh = CpVehicle( population, index, keep );
			if ( !veh || !teleport )
			{
				std::fprintf( stderr, "[coreplay] place FAIL: vehicle %u not spawned (teleport=%d)\n", index, teleport ? 1 : 0 );
				return;
			}
			const Vector3 vp = veh->GetWorldPosition().AsVector3();
			Vector3 fwd = veh->GetWorldForward();
			Vector3 right = veh->GetWorldRight();
			fwd.Z = 0.0f;
			right.Z = 0.0f;
			fwd.Normalize();
			right.Normalize();
			const Float dist = ( dm > 0 ? Float( dm ) : ( op == 1 ? 26.0f : 120.0f ) ) * 0.1f;
			Vector3 at;
			Vector3 face;
			if ( op == 1 )
			{
				at = vp - right * dist + fwd * 0.3f; // left = driver side
				face = right;
			}
			else
			{
				at = vp + fwd * dist;
				face = -fwd;
			}
			at.Z = vp.Z + 0.2f;
			const EulerAngles ea( 0.0f, 0.0f, EulerAngles::YawFromXY( face.X, face.Y ) );
			THandle< game::Object > playerHandle = s.player.ToHandle();
			teleport->Teleport( playerHandle, Vector4( at.X, at.Y, at.Z, 1.0f ), ea );
			s_cpCamLogAt = s_cpClock + 1.5f;
			s_cpFace = face;
			std::fprintf( stderr, "[coreplay] place op %d: player -> (%.2f, %.2f, %.2f) yaw %.1f, %s of vehicle %u %s at (%.2f, %.2f, %.2f) dist %.1f m\n",
				op, at.X, at.Y, at.Z, ea.Yaw, op == 1 ? "left side" : "front", index, VName( index ), vp.X, vp.Y, vp.Z, dist );
			CpLogVehicle( "place-target", index, veh );
		}

		static void CpEquip( const game::IGameInstance* gi, game::Object& player, Uint32 index )
		{
			if ( index >= c_testWeaponsTotal )
			{
				std::fprintf( stderr, "[coreplay] equip FAIL: test weapon %u out of range\n", index );
				return;
			}
			const char* name = c_testWeapons[ index ];
			auto transactions = gi->GetSystemPtr< game::ITransactionSystem >();
			auto scriptables = gi->GetSystemPtr< game::IScriptableSystemsContainer >();
			if ( !transactions || !scriptables )
			{
				return;
			}
			game::ItemID itemID = transactions->GetFirstItemID( player, game::data::TweakDBID( name ) );
			if ( !itemID.IsValid() )
			{
				const Bool given = transactions->GiveItem( player, game::data::TweakDBID( name ), 1, game::EInventoryOperationOverrides::FlagItemAddedCallbackAsSilent );
				itemID = transactions->GetFirstItemID( player, game::data::TweakDBID( name ) );
				std::fprintf( stderr, "[coreplay] equip %s: not in inventory, GiveItem %s, item %s\n", name, given ? "ok" : "FAILED", itemID.IsValid() ? "valid" : "INVALID" );
				if ( !itemID.IsValid() )
				{
					return;
				}
			}
			THandle< ScriptableSystem > equipmentSystem = scriptables->Get( RED_NAME_CONSTEXPR( "EquipmentSystem" ) );
			if ( !equipmentSystem )
			{
				std::fprintf( stderr, "[coreplay] equip FAIL: EquipmentSystem not found\n" );
				return;
			}
			WeakHandle< game::Object > owner = s.player;
			equipmentSystem->QueueRequest( CreateHandle< EquipRequest >( owner, itemID, -1, false, true ) );
			equipmentSystem->QueueRequest( CreateHandle< DrawItemRequest >( owner, itemID ) );
			std::fprintf( stderr, "[coreplay] equip+draw queued: test weapon %u %s\n", index, name );
		}

		static Int32 CpWeaponIndex( const game::ItemID& id )
		{
			const game::data::TweakDBID tdb = id.GetTweakDBID();
			for ( Uint32 i = 0; i < c_testWeaponsTotal; ++i )
			{
				if ( tdb == game::data::TweakDBID( c_testWeapons[ i ] ) )
				{
					return 1000 + Int32( i );
				}
			}
			for ( Uint32 i = 0; i < c_weaponsTotal; ++i )
			{
				if ( tdb == game::data::TweakDBID( s_sandboxWeapons[ i ] ) )
				{
					return Int32( i );
				}
			}
			return -1;
		}

		static const char* CpWeaponName( Int32 idx )
		{
			if ( idx >= 1000 )
			{
				return c_testWeapons[ idx - 1000 ];
			}
			return idx >= 0 ? s_sandboxWeapons[ idx ] : ( idx == -2 ? "none" : "unknown(not in lists)" );
		}

		// [coreplay] SANDBOX F-ENTER FALLBACK. The native vehicle 'Enter' interaction does not fire on web yet (wheeled vehicles
		// are not hit by the interaction lookAt raycast; the Basilisk is hit but the choice still does not activate). Until
		// that is fixed: in ?sandbox, a press of F while on foot, with no native mount within 0.4 s, mounts the player into the
		// driver seat of the nearest vehicle whose body is within 2.5 m (the same MountingFacility request the vehicle script
		// issues). Exit stays native (hold F). &sandboxEnterFallback=0 disables it.
		// [coreplay] vehicle weapon probe: the vehicle's own weapon-used callback (fired from UpdateWeapons when a shot cycle starts)
		struct CpWeaponCb : public vehicle::IWeaponUsedCallback
		{
			RED_USE_MEMORY_POOL( game::PoolGMPL_Vehicles_Object );
			Uint32 m_slot = 0;
			explicit CpWeaponCb( Uint32 slot ) : m_slot( slot ) {}
			void OnWeaponUsed( Uint32 id ) override
			{
				std::fprintf( stderr, "[coreplay] VEHICLE WEAPON FIRED: vehicle %u %s weapon %u\n", m_slot, VName( m_slot ), id );
			}
		};
		static red::SharedPtr< CpWeaponCb > s_cpWeaponCbs[ c_vehiclesTotal ];
		static void CpRegisterWeaponCallbacks( const game::IGameInstance* gi )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				if ( s_cpWeaponCbs[ i ] )
				{
					continue;
				}
				THandle< ent::Entity > keep;
				if ( vehicle::BaseObject* veh = CpVehicle( population, i, keep ) )
				{
					s_cpWeaponCbs[ i ] = red::CreateSharedPtr< CpWeaponCb >( i );
					veh->RegisterCallback( s_cpWeaponCbs[ i ] );
				}
			}
		}

		static Float s_cpEnterPending = -1.0f;
		static Float s_cpEnterPoll = 0.0f;
		static Float s_cpEnterClock = 0.0f;
		static void CpEnterFallbackTick( const game::IGameInstance* gi, game::Object& player, Float dt )
		{
			s_cpEnterClock += dt;
			s_cpEnterPoll += dt;
			if ( s_cpEnterPoll >= 0.1f )
			{
				s_cpEnterPoll = 0.0f;
				// F presses are counted by the main-thread listener into g_cpShared[5] (no proxy); [6] = fallback disabled
				const Int32 pressed = g_cpShared[ 6 ] ? 0 : __atomic_exchange_n( &g_cpShared[ 5 ], 0, __ATOMIC_ACQ_REL );
				if ( pressed > 0 && s_cpMounted < 0 && s_cpEnterPending < 0.0f )
				{
					s_cpEnterPending = s_cpEnterClock + 0.4f;
				}
			}
			if ( s_cpEnterPending < 0.0f || s_cpEnterClock < s_cpEnterPending )
			{
				return;
			}
			s_cpEnterPending = -1.0f;
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			auto mounting = gi->GetSystemPtr< game::mounting::IMountingFacility >();
			Bool anyMounted = false;
			const Vector3 pp = player.GetWorldPosition().AsVector3();
			vehicle::BaseObject* best = nullptr;
			THandle< ent::Entity > keepBest;
			Uint32 bestIndex = 0;
			Float bestGap = 2.5f;
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				THandle< ent::Entity > keep;
				vehicle::BaseObject* veh = CpVehicle( population, i, keep );
				if ( !veh )
				{
					continue;
				}
				anyMounted = anyMounted || veh->IsPlayerMounted();
				const Vector3 vp = veh->GetWorldPosition().AsVector3();
				const Vector4 ext = veh->GetRigidBody().GetLocalRigidBox().CalcExtents();
				const Float radius = std::sqrt( ext.X * ext.X + ext.Y * ext.Y ) * 0.75f;
				const Float dx = vp.X - pp.X, dy = vp.Y - pp.Y;
				const Float gap = std::sqrt( dx * dx + dy * dy ) - radius;
				if ( gap < bestGap )
				{
					bestGap = gap;
					best = veh;
					keepBest = keep;
					bestIndex = i;
				}
			}
			if ( anyMounted )
			{
				return; // the native interaction (or a previous request) already mounted the player
			}
			if ( !best || !mounting )
			{
				std::fprintf( stderr, "[coreplay] F-enter fallback: no vehicle within 2.5 m of the player (%.1f, %.1f)\n", pp.X, pp.Y );
				return;
			}
			game::mounting::MountingInfo info( player.GetEntityID(), best->GetEntityID(), game::mounting::MountingSlotId( RED_NAME_CONSTEXPR( "seat_front_left" ) ) );
			THandle< game::MountEventData > data = CreateHandle< game::MountEventData >();
			data->m_mountEventOptions = CreateHandle< game::MountEventOptions >();
			const Bool ok = mounting->Mount( CreateHandle< game::mounting::MountingRequest >( info, true, data ) );
			std::fprintf( stderr, "[coreplay] F-enter fallback: F pressed on foot, no native mount -> driver seat of vehicle %u %s (gap %.1f m) Mount()=%d\n",
				bestIndex, VName( bestIndex ), bestGap, ok ? 1 : 0 );
		}

		// [coreplay] one-time setup of main-thread listeners that write into this shared block, so the game thread never
		// sync-proxies to the main thread per frame: [0] packed test command, [4] flight key bitmask, [5] F press counter,
		// [6] F-enter fallback disabled (&sandboxEnterFallback=0). Atomics.* on the module's HEAP32 view (main thread).
		static void CpSharedInit()
		{
			static Bool s_done = false;
			if ( s_done )
			{
				return;
			}
			s_done = true;
#ifdef __EMSCRIPTEN__
			MAIN_THREAD_EM_ASM( {
				const base = Number( $0 ) / 4;
				const H = () => HEAP32;
				try { if ( new URLSearchParams( location.search ).get( 'sandboxEnterFallback' ) === '0' ) Atomics.store( H(), base + 6, 1 ); } catch ( e ) {}
				const k = new Set();
				const pack = () => ( k.has( 'KeyW' ) ? 1 : 0 ) | ( k.has( 'KeyS' ) ? 2 : 0 ) | ( k.has( 'KeyA' ) ? 4 : 0 ) | ( k.has( 'KeyD' ) ? 8 : 0 ) |
					( k.has( 'Space' ) ? 16 : 0 ) | ( ( k.has( 'ShiftLeft' ) || k.has( 'ShiftRight' ) ) ? 32 : 0 ) | ( k.has( 'KeyC' ) ? 64 : 0 );
				addEventListener( 'keydown', e => { k.add( e.code ); Atomics.store( H(), base + 4, pack() ); if ( e.code === 'KeyF' && !e.repeat ) Atomics.add( H(), base + 5, 1 ); }, true );
				addEventListener( 'keyup', e => { k.delete( e.code ); Atomics.store( H(), base + 4, pack() ); }, true );
				addEventListener( 'blur', () => { k.clear(); Atomics.store( H(), base + 4, 0 ); }, true );
				setInterval( () => {
					try { const c = globalThis.cp2077CoreplayCmd; if ( c ) Atomics.store( H(), base, ( ( c[0] & 511 ) << 22 ) | ( ( c[1] & 15 ) << 18 ) | ( ( c[2] & 255 ) << 10 ) | ( c[3] & 1023 ) ); } catch ( e ) {}
				}, 100 );
			}, ( double )( uintptr_t )&g_cpShared[ 0 ] );
#endif
			std::fprintf( stderr, "[coreplay] shared input block ready (no per-frame main-thread proxies)\n" );
		}

		// [tkfix] read-only view of vehicle::BaseObject's protected weapon state (aim target, mounted weapons). Never instantiated.
		struct CpVehPeek : public vehicle::BaseObject
		{
			static Vector3 AimTarget( const vehicle::BaseObject& v ) { return v.*( &CpVehPeek::m_aimTarget ); }
			static Uint32 WeaponCount( const vehicle::BaseObject& v ) { return Uint32( ( v.*( &CpVehPeek::m_weaponsData ) ).Size() ); }
			static game::weapon::Object* Weapon( const vehicle::BaseObject& v, Uint32 i )
			{
				const auto& wd = v.*( &CpVehPeek::m_weaponsData );
				return i < wd.Size() ? wd[ i ].m_weapon.Get() : nullptr;
			}
			static Vector2 YawLimit( const vehicle::BaseObject& v, Uint32 i )
			{
				const auto& wd = v.*( &CpVehPeek::m_weaponsData );
				return i < wd.Size() ? wd[ i ].m_yawLimit : Vector2( 0.0f, 0.0f );
			}
		};

		static Float CpPitchDeg( const Vector3& f )
		{
			const Float h = std::sqrt( f.X * f.X + f.Y * f.Y );
			return std::atan2( f.Z, h ) * 57.2957795f;
		}

		// what a gameplay raycast with the vehicle-weapon filter (UpdateWeapons: Static/Terrain/Vehicle/VehicleBlocker) hits
		static void CpRayProbe( vehicle::BaseObject& veh, const Vector3& from, const Vector3& to, const char* tag, Uint32 tgtIndex, vehicle::BaseObject* tgt )
		{
			world::RuntimeSystemPhysics* phys = veh.GetSceneSystem< world::RuntimeSystemPhysics >();
			if ( !phys )
			{
				std::fprintf( stderr, "[coreplay] aim %s ray: no physics system\n", tag );
				return;
			}
			const physics::QueryFilter filter = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Static" ), RED_NAME_CONSTEXPR( "Terrain" ), RED_NAME_CONSTEXPR( "Vehicle" ), RED_NAME_CONSTEXPR( "VehicleBlocker" ) );
			physics::RaycastParams params = physics::RaycastParams::Create( from, to, filter ).SetUseCase( physics::QueryUseCase::Gameplay );
			physics::TraceResult r;
			if ( phys->SyncRaycast( params, r ) != physics::SyncResult::Hit )
			{
				std::fprintf( stderr, "[coreplay] aim %s ray: NO HIT (%.1f m)\n", tag, ( to - from ).Mag() );
				return;
			}
			THandle< ent::Entity > he = r.FindHitObject< ent::Entity >();
			const Bool isTarget = he && tgt && he.Get() == static_cast< ent::Entity* >( tgt );
			const Bool isSelf = he && he.Get() == static_cast< ent::Entity* >( &veh );
			std::fprintf( stderr, "[coreplay] aim %s ray: hit %s (%s)%s at (%.2f, %.2f, %.2f) dist %.1f m from ray start, material %s\n", tag,
				he ? he->GetEntityID().ToDebugString().AsChar() : "world/static", he ? he->GetClass()->GetName().AsChar() : "-",
				isTarget ? " = TARGET" : ( isSelf ? " = OWN VEHICLE" : "" ), r.m_worldPosition.X, r.m_worldPosition.Y, r.m_worldPosition.Z,
				( r.m_worldPosition - from ).Mag(), r.m_material.AsChar() );
			( void )tgtIndex;
		}

		// op 13: aim report for the mounted vehicle against target vehicle 'tgtIndex' (logging only)
		static void CpAimReport( const game::IGameInstance* gi, Uint32 tgtIndex, const char* tag )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			if ( s_cpMounted < 0 )
			{
				std::fprintf( stderr, "[coreplay] aim %s: player is not mounted\n", tag );
				return;
			}
			THandle< ent::Entity > k1, k2;
			vehicle::BaseObject* veh = CpVehicle( population, Uint32( s_cpMounted ), k1 );
			vehicle::BaseObject* tgt = CpVehicle( population, tgtIndex, k2 );
			if ( !veh || !tgt )
			{
				std::fprintf( stderr, "[coreplay] aim %s FAIL: mounted vehicle %d=%d target %u=%d\n", tag, s_cpMounted, veh ? 1 : 0, tgtIndex, tgt ? 1 : 0 );
				return;
			}
			THandle< vehicle::CameraManager > cm = veh->GetCameraManager();
			const Vector3 cp = cm ? cm->GetActiveCameraPosition() : veh->GetWorldPosition().AsVector3();
			const Vector3 cf = cm ? cm->GetActiveCameraRotation().GetForward3() : veh->GetWorldForward();
			const Box& box = tgt->GetRigidBody().GetLocalRigidBox();
			const Vector3 tp = tgt->GetWorldPosition().AsVector3();
			const Vector3 tc( tp.X, tp.Y, tp.Z + 0.5f * ( box.Min.Z + box.Max.Z ) );
			const Vector3 d = tc - cp;
			const Float camYaw = EulerAngles::YawFromXY( cf.X, cf.Y ), camPitch = CpPitchDeg( cf );
			const Float wantYaw = EulerAngles::YawFromXY( d.X, d.Y ), wantPitch = CpPitchDeg( d );
			const Vector3 vp = veh->GetWorldPosition().AsVector3();
			const Vector3 vf = veh->GetWorldForward();
			std::fprintf( stderr, "[coreplay] aim %s: target vehicle %u %s centre (%.2f, %.2f, %.2f) dist %.1f m from vehicle camera | vcam pos (%.2f, %.2f, %.2f) yaw %.1f pitch %.1f tpp=%d"
				" | want yaw %.1f pitch %.1f | err yaw %+.1f pitch %+.1f | hull yaw %.1f, camera %.1f m from vehicle | aim ray starts 15 m out (k_weaponTargetAimMinRange): target is %.1f m %s it\n",
				tag, tgtIndex, VName( tgtIndex ), tc.X, tc.Y, tc.Z, d.Mag(), cp.X, cp.Y, cp.Z, camYaw, camPitch, cm && cm->IsTPPActive() ? 1 : 0,
				wantYaw, wantPitch, EulerAngles::NormalizeAngle180( wantYaw - camYaw ), wantPitch - camPitch,
				EulerAngles::YawFromXY( vf.X, vf.Y ), ( cp - vp ).Mag(), std::fabs( d.Mag() - 15.0f ), d.Mag() >= 15.0f ? "beyond" : "BEFORE" );
			const Vector3 at = CpVehPeek::AimTarget( *veh );
			std::fprintf( stderr, "[coreplay] aim %s: m_aimTarget (%.2f, %.2f, %.2f) = %.1f m from target centre; weapons %u\n", tag, at.X, at.Y, at.Z, ( at - tc ).Mag(), CpVehPeek::WeaponCount( *veh ) );
			for ( Uint32 w = 0; w < CpVehPeek::WeaponCount( *veh ) && w < 3; ++w )
			{
				if ( game::weapon::Object* wo = CpVehPeek::Weapon( *veh, w ) )
				{
					const Vector3 wp = wo->GetWorldPosition().AsVector3();
					const Vector3 wf = wo->GetWorldForward();
					const Vector3 rel = tc - wp;
					const Float along = rel.Dot( wf );
					const Vector3 perp = rel - wf * along;
					const Vector2 yl = CpVehPeek::YawLimit( *veh, w );
					std::fprintf( stderr, "[coreplay] aim %s: weapon %u muzzle (%.2f, %.2f, %.2f) fwd yaw %.1f pitch %.1f (yaw limit %.0f..%.0f vs hull) | target centre %.1f m along, MISS DISTANCE %.2f m\n",
						tag, w, wp.X, wp.Y, wp.Z, EulerAngles::YawFromXY( wf.X, wf.Y ), CpPitchDeg( wf ), yl.X, yl.Y, along, perp.Mag() );
				}
			}
			const Vector3 from = cp + cf * 15.0f;
			CpRayProbe( *veh, from, from + cf * 250.0f, "camera", tgtIndex, tgt );
			Vector3 dn = d;
			dn.Normalize();
			CpRayProbe( *veh, cp + dn * 2.0f, tc + dn * 3.0f, "to-target", tgtIndex, tgt );
		}

		// op 12: place vehicle 'tgtIndex' dm metres (default 15) dead ahead of the vehicle the player is mounted in, broadside
		static void CpPlaceAhead( const game::IGameInstance* gi, Uint32 tgtIndex, Int32 dm )
		{
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >();
			THandle< ent::Entity > k1, k2;
			vehicle::BaseObject* src = s_cpMounted >= 0 ? CpVehicle( population, Uint32( s_cpMounted ), k1 ) : nullptr;
			vehicle::BaseObject* tgt = CpVehicle( population, tgtIndex, k2 );
			if ( !src || !tgt || !teleport || tgtIndex == Uint32( s_cpMounted ) )
			{
				std::fprintf( stderr, "[coreplay] place-ahead FAIL: mounted vehicle %d (%d) target %u (%d) teleport %d\n", s_cpMounted, src ? 1 : 0, tgtIndex, tgt ? 1 : 0, teleport ? 1 : 0 );
				return;
			}
			const Float dist = dm > 0 ? Float( dm ) : 15.0f;
			const Vector3 sp = src->GetWorldPosition().AsVector3();
			Vector3 f = src->GetWorldForward();
			f.Z = 0.0f;
			f.Normalize();
			const Vector3 tp0 = tgt->GetWorldPosition().AsVector3();
			const Vector3 at( sp.X + f.X * dist, sp.Y + f.Y * dist, tp0.Z + 0.3f );
			const EulerAngles ea( 0.0f, 0.0f, EulerAngles::NormalizeAngle180( EulerAngles::YawFromXY( f.X, f.Y ) + 90.0f ) ); // broadside
			teleport->Teleport( THandle< game::Object >( Cast< game::Object >( k2 ) ), Vector4( at.X, at.Y, at.Z, 1.0f ), ea );
			s_cpMovedOp[ tgtIndex ] = 12;
			s_cpMovedAt[ tgtIndex ] = s_cpClock;
			s_cpAimReportAt = s_cpClock + 2.0f;
			s_cpAimReportTarget = tgtIndex;
			auto statPools = gi->GetSystemPtr< game::IStatPoolsSystem >();
			const Float hp = statPools ? statPools->GetStatPoolValue( game::StatsObjectID( tgt->GetEntityID() ), game::data::StatPoolType::Health ) : -1.0f;
			std::fprintf( stderr, "[coreplay] target: vehicle %u %s id=%s health %.2f placed %.1f m dead ahead of mounted vehicle %d %s: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) broadside yaw %.1f (op 12)\n",
				tgtIndex, VName( tgtIndex ), tgt->GetEntityID().ToDebugString().AsChar(), hp, dist, s_cpMounted, VName( Uint32( s_cpMounted ) ), tp0.X, tp0.Y, tp0.Z, at.X, at.Y, at.Z, ea.Yaw );
		}

		// 1 Hz: health of recent damage-pipeline targets (NPCs, vehicles, anything), logged on change
		static void CpPollHitHealth( game::IStatPoolsSystem* statPools )
		{
			if ( !statPools || !g_coreplayHitProbe )
			{
				return;
			}
			for ( Uint32 r = 0; r < 8; ++r )
			{
				const unsigned long long h = __atomic_load_n( &g_coreplayHitRing[ r ], __ATOMIC_RELAXED );
				if ( h == 0 )
				{
					continue;
				}
				Int32 slot = -1;
				for ( Int32 k = 0; k < 16; ++k )
				{
					if ( s_cpHpHash[ k ] == h ) { slot = k; break; }
				}
				const Float hp = statPools->GetStatPoolValue( game::StatsObjectID( ent::EntityID( ent::EntityHashType( h ), ent::EntityID::EITHER ) ), game::data::StatPoolType::Health );
				if ( slot < 0 )
				{
					for ( Int32 k = 0; k < 16; ++k )
					{
						if ( s_cpHpHash[ k ] == 0 ) { slot = k; break; }
					}
					if ( slot < 0 )
					{
						slot = Int32( h % 16 );
					}
					s_cpHpHash[ slot ] = h;
					s_cpHpLast[ slot ] = hp;
					std::fprintf( stderr, "[coreplay] HEALTH damage target %s: %.2f (first poll after the hit)\n", ent::EntityID( ent::EntityHashType( h ), ent::EntityID::EITHER ).ToDebugString().AsChar(), hp );
					continue;
				}
				if ( hp != s_cpHpLast[ slot ] )
				{
					std::fprintf( stderr, "[coreplay] HEALTH damage target %s: %.2f -> %.2f%s\n", ent::EntityID( ent::EntityHashType( h ), ent::EntityID::EITHER ).ToDebugString().AsChar(),
						s_cpHpLast[ slot ], hp, hp <= 0.0f ? " (DEAD/DESTROYED)" : "" );
					s_cpHpLast[ slot ] = hp;
				}
			}
		}

		static void CoreplayTick( const game::IGameInstance* gi, game::Object& player, Float timeDelta )
		{
			if ( !s_cpAnnounced )
			{
				s_cpAnnounced = true;
				std::fprintf( stderr, "[coreplay] cmd channel ready: page sets globalThis.cp2077CoreplayCmd=[seq,op,arg,arg2] (seq 1..511, arg 0..255, arg2 0..1023); ops 1=beside vehicle 2=front of vehicle 3=equip test weapon 4=log vehicles 12=place target arg arg2 m ahead of mounted vehicle 13=aim report vs vehicle arg\n" );
				for ( Uint32 i = 0; i < c_testWeaponsTotal; ++i )
				{
					std::fprintf( stderr, "[coreplay] test weapon %u = %s\n", i, c_testWeapons[ i ] );
				}
			}
			// commands: the test sets globalThis.cp2077CoreplayCmd = [seq, op, arg, arg2] on the page (main thread);
			// polled every 0.5 s (HEAP32 is not exported in this build, and touching it aborts the runtime).
			// No proxy: a main-thread interval (CpSharedInit) packs globalThis.cp2077CoreplayCmd into g_cpShared[0] every 100 ms.
			CpSharedInit();
			const Int32 packed = __atomic_load_n( &g_cpShared[ 0 ], __ATOMIC_ACQUIRE );
			const Int32 seq = ( packed >> 22 ) & 511;
			if ( packed != 0 && seq != s_cpLastSeq )
			{
				s_cpLastSeq = seq;
				const Int32 op = ( packed >> 18 ) & 15;
				const Int32 arg = ( packed >> 10 ) & 255;
				const Int32 arg2 = packed & 1023;
				std::fprintf( stderr, "[coreplay] cmd seq %d op %d arg %d arg2 %d\n", seq, op, arg, arg2 );
				if ( op == 1 || op == 2 )
				{
					CpPlace( gi, Uint32( arg ), op, arg2 );
				}
				else if ( op == 3 )
				{
					CpEquip( gi, player, Uint32( arg ) );
				}
				else if ( op == 11 )
				{
					// move vehicle 'arg' to 25 m in front of vehicle 'arg2' (target for vehicle weapons)
					auto population = gi->GetSystemPtr< game::IPopulationSystem >();
					auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >();
					THandle< ent::Entity > k1, k2;
					vehicle::BaseObject* tgt = CpVehicle( population, Uint32( arg ), k1 );
					vehicle::BaseObject* src = CpVehicle( population, Uint32( arg2 ), k2 );
					if ( tgt && src && teleport )
					{
						const Vector3 sp = src->GetWorldPosition().AsVector3();
						Vector3 f = src->GetWorldForward();
						f.Z = 0.0f;
						f.Normalize();
						const Vector3 at = sp + f * 25.0f;
						teleport->Teleport( THandle< game::Object >( Cast< game::Object >( k1 ) ), Vector4( at.X, at.Y, sp.Z + 0.3f, 1.0f ), EulerAngles() );
						std::fprintf( stderr, "[coreplay] target: vehicle %u moved to (%.1f, %.1f) = 25 m in front of vehicle %u\n", Uint32( arg ), at.X, at.Y, Uint32( arg2 ) );
						if ( Uint32( arg ) < c_vehiclesTotal ) { s_cpMovedOp[ arg ] = 11; s_cpMovedAt[ arg ] = s_cpClock; } // [tkfix]
					}
				}
				else if ( op == 12 )
				{
					CpPlaceAhead( gi, Uint32( arg ), arg2 ); // [tkfix] target arg, arg2 metres (default 15) ahead of the mounted vehicle
				}
				else if ( op == 13 )
				{
					CpAimReport( gi, Uint32( arg ), "now" ); // [tkfix] aim report vs target vehicle arg
				}
				else if ( op == 10 )
				{
					// fall test: lift the player 'arg' metres straight up (fall damage / death)
					if ( auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >() )
					{
						const Vector3 p = player.GetWorldPosition().AsVector3();
						const Vector3 f = player.GetWorldForward();
						teleport->Teleport( s.player.ToHandle(), Vector4( p.X, p.Y, p.Z + Float( arg ), 1.0f ), EulerAngles( 0.0f, 0.0f, EulerAngles::YawFromXY( f.X, f.Y ) ) );
						std::fprintf( stderr, "[coreplay] fall test: player lifted %d m to z %.1f\n", arg, p.Z + Float( arg ) );
					}
				}
				else if ( op == 8 )
				{
					g_coreplayLookAtProbe = arg > 0 ? arg : 3; g_coreplayActivatorProbe = arg > 0 ? arg : 3; // next N lookAt traces + activator frames
				}
				else if ( op == 7 )
				{
					// direct mount test (bypasses the interaction prompt): like gameVehicleMountableComponent.script MountEntityToSlot
					auto population = gi->GetSystemPtr< game::IPopulationSystem >();
					auto mounting = gi->GetSystemPtr< game::mounting::IMountingFacility >();
					THandle< ent::Entity > keep;
					vehicle::BaseObject* veh = CpVehicle( population, Uint32( arg ), keep );
					const CName slotName = arg2 == 1 ? RED_NAME_CONSTEXPR( "seat_front_right" ) : RED_NAME_CONSTEXPR( "seat_front_left" );
					if ( veh && mounting )
					{
						game::mounting::MountingInfo info( player.GetEntityID(), veh->GetEntityID(), game::mounting::MountingSlotId( slotName ) );
						THandle< game::MountEventData > data = CreateHandle< game::MountEventData >();
						data->m_mountEventOptions = CreateHandle< game::MountEventOptions >();
						THandle< game::mounting::MountingRequest > req = CreateHandle< game::mounting::MountingRequest >( info, true, data );
						const Bool ok = mounting->Mount( req );
						std::fprintf( stderr, "[coreplay] direct mount vehicle %u %s slot %s -> Mount() returned %d\n", Uint32( arg ), VName( Uint32( arg ) ), slotName.AsChar(), ok ? 1 : 0 );
					}
					else
					{
						std::fprintf( stderr, "[coreplay] direct mount FAIL: vehicle=%d mountingFacility=%d\n", veh ? 1 : 0, mounting ? 1 : 0 );
					}
				}
				else if ( op == 6 )
				{
					CpInteractionProbe( gi, player, Uint32( arg ) );
				}
				else if ( op == 5 )
				{
					CpLogCamera( gi, player, "now" );
				}
				else if ( op == 4 )
				{
					auto population = gi->GetSystemPtr< game::IPopulationSystem >();
					for ( Uint32 i = 0; i < s.vehicleCount; ++i )
					{
						THandle< ent::Entity > keep;
						if ( vehicle::BaseObject* veh = CpVehicle( population, i, keep ) )
						{
							CpLogVehicle( "list", i, veh );
						}
					}
				}
			}
			// weapon state (every frame; logs only on change)
			if ( auto transactions = gi->GetSystemPtr< game::ITransactionSystem >() )
			{
				THandle< game::ItemObject > item;
				Int32 widx = -2;
				Int32 mag = -1;
				Bool reloading = false;
				if ( transactions->GetItemInSlot( player, game::data::TweakDBID( "AttachmentSlots.WeaponRight" ), item ) && item )
				{
					widx = CpWeaponIndex( item->GetItemID() );
					if ( game::weapon::Object* w = Cast< game::weapon::Object >( item.Get() ) )
					{
						mag = Int32( w->GetMagazineAmmoCount() );
						reloading = w->IsReloading();
					}
				}
				if ( widx != s_cpWeapon )
				{
					std::fprintf( stderr, "[coreplay] weapon in hand: %s (idx %d) magazine %d\n", CpWeaponName( widx ), widx, mag );
					s_cpWeapon = widx;
					s_cpMag = mag;
				}
				else if ( mag >= 0 && s_cpMag >= 0 && mag < s_cpMag )
				{
					s_cpShots += Uint32( s_cpMag - mag );
					std::fprintf( stderr, "[coreplay] shot: %s magazine %d -> %d (total shots %u)\n", CpWeaponName( widx ), s_cpMag, mag, s_cpShots );
					s_cpMag = mag;
				}
				else if ( mag != s_cpMag )
				{
					std::fprintf( stderr, "[coreplay] magazine %s %d -> %d%s\n", CpWeaponName( widx ), s_cpMag, mag, mag > s_cpMag ? " (reloaded)" : "" );
					s_cpMag = mag;
				}
				if ( reloading != s_cpReloading )
				{
					std::fprintf( stderr, "[coreplay] reload %s: %s\n", reloading ? "START" : "end", CpWeaponName( widx ) );
					s_cpReloading = reloading;
				}
			}
			CpFlightTick( gi, timeDelta );
			CpEnterFallbackTick( gi, player, timeDelta );
			{
				// on-foot locomotion probe (4 Hz, only while moving / airborne): horizontal speed and height
				static Float s_mvClock = 0.0f;
				static Vector3 s_mvLast;
				static Bool s_mvInit = false;
				s_mvClock += timeDelta;
				if ( s_mvClock >= 0.25f )
				{
					const Vector3 p = player.GetWorldPosition().AsVector3();
					if ( s_mvInit && s_cpMounted < 0 )
					{
						const Float dx = p.X - s_mvLast.X, dy = p.Y - s_mvLast.Y;
						const Float hs = std::sqrt( dx * dx + dy * dy ) / s_mvClock;
						if ( hs > 0.2f || p.Z > 0.05f || std::fabs( p.Z - s_mvLast.Z ) > 0.02f )
						{
							std::fprintf( stderr, "[coreplay] move: speed %.2f m/s z %.2f (dz %+.2f) pos (%.1f, %.1f)\n", hs, p.Z, p.Z - s_mvLast.Z, p.X, p.Y );
						}
					}
					s_mvLast = p;
					s_mvInit = true;
					s_mvClock = 0.0f;
				}
			}
			if ( s.phase == Phase::Done ) { CpRegisterWeaponCallbacks( gi ); }
			// periodic vehicle / player probe
			s_cpClock += timeDelta;
			if ( s_cpCamLogAt >= 0.0f && s_cpClock >= s_cpCamLogAt )
			{
				s_cpCamLogAt = -1.0f;
				CpLogCamera( gi, player, "after place" );
			}
			if ( s_cpAimReportAt >= 0.0f && s_cpClock >= s_cpAimReportAt )
			{
				s_cpAimReportAt = -1.0f;
				CpAimReport( gi, s_cpAimReportTarget, "after place-ahead" ); // [tkfix]
			}
			if ( s_cpClock < s_cpNext )
			{
				return;
			}
			s_cpNext = s_cpClock + 1.0f;
			auto population = gi->GetSystemPtr< game::IPopulationSystem >();
			auto statPools = gi->GetSystemPtr< game::IStatPoolsSystem >();
			Int32 mounted = -1;
			// [tkfix] position of the vehicle the player drives (for the 'moved >5 m' cause)
			Vector3 mountedPos( 1e9f, 1e9f, 1e9f );
			if ( s_cpMounted >= 0 )
			{
				THandle< ent::Entity > km;
				if ( vehicle::BaseObject* mv = CpVehicle( population, Uint32( s_cpMounted ), km ) )
				{
					mountedPos = mv->GetWorldPosition().AsVector3();
				}
			}
			for ( Uint32 i = 0; i < s.vehicleCount; ++i )
			{
				THandle< ent::Entity > keep;
				vehicle::BaseObject* veh = CpVehicle( population, i, keep );
				if ( !veh )
				{
					continue;
				}
				if ( veh->IsPlayerMounted() )
				{
					mounted = Int32( i );
					CpLogVehicle( "mounted", i, veh );
				}
				{
					// [tkfix] vehicle position watcher: log any move > 5 m since the last 1 s probe, with its cause when knowable
					const Vector3 vp = veh->GetWorldPosition().AsVector3();
					if ( s_cpPosInit[ i ] )
					{
						const Vector3 dv = vp - s_cpLastPos[ i ];
						const Float moved = dv.Mag();
						if ( moved > 5.0f )
						{
							const Vector3 v = veh->GetRigidBody().GetLinearVelocity();
							const Float speed = v.Mag();
							const Vector3 dm = vp - mountedPos;
							const Float gap = std::sqrt( dm.X * dm.X + dm.Y * dm.Y );
							char cause[ 160 ];
							if ( s_cpMovedOp[ i ] > 0 && s_cpClock - s_cpMovedAt[ i ] < 3.0f )
							{
								std::snprintf( cause, sizeof( cause ), "teleport by coreplay op %d (test command)", s_cpMovedOp[ i ] );
							}
							else if ( veh->IsPlayerMounted() )
							{
								std::snprintf( cause, sizeof( cause ), "player is driving it" );
							}
							else if ( veh->IsAutopilotActive() )
							{
								std::snprintf( cause, sizeof( cause ), "autopilot active" );
							}
							else if ( s_cpMounted >= 0 && gap < 12.0f )
							{
								std::snprintf( cause, sizeof( cause ), "physics: %.1f m from the player's vehicle %d (pushed/collision?)", gap, s_cpMounted );
							}
							else
							{
								std::snprintf( cause, sizeof( cause ), "unknown (physics; no test command, not driven)" );
							}
							std::fprintf( stderr, "[coreplay] VEHICLE MOVED: vehicle %u %s (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) = %.1f m since last probe, speed now %.1f m/s physMask=0x%x | cause: %s\n",
								i, VName( i ), s_cpLastPos[ i ].X, s_cpLastPos[ i ].Y, s_cpLastPos[ i ].Z, vp.X, vp.Y, vp.Z, moved, speed, CpMask( veh ), cause );
						}
					}
					s_cpLastPos[ i ] = vp;
					s_cpPosInit[ i ] = s.phase == Phase::Done;
				}
				if ( statPools )
				{
					const Float h = statPools->GetStatPoolValue( game::StatsObjectID( veh->GetEntityID() ), game::data::StatPoolType::Health );
					if ( s_cpHealthInit && h != s_cpHealth[ i ] )
					{
						std::fprintf( stderr, "[coreplay] HIT vehicle %u %s health %.2f -> %.2f\n", i, VName( i ), s_cpHealth[ i ], h );
					}
					s_cpHealth[ i ] = h;
				}
			}
			s_cpHealthInit = statPools != nullptr && s.phase == Phase::Done;
			CpPollHitHealth( statPools.Get() ); // [tkfix]
			if ( statPools )
			{
				// player health probe (damage / death)
				static Float s_cpPlayerHealth = -1.0f;
				const Float ph = statPools->GetStatPoolValue( game::StatsObjectID( player.GetEntityID() ), game::data::StatPoolType::Health );
				if ( ph != s_cpPlayerHealth )
				{
					std::fprintf( stderr, "[coreplay] PLAYER health %.2f -> %.2f%s\n", s_cpPlayerHealth, ph, ph <= 0.0f ? " (DEAD)" : "" );
					if ( ph <= 0.0f && s_cpPlayerHealth > 0.0f )
					{
						// SANDBOX RESPAWN: the death menu only offers 'load save', and ?sandbox disables saves, so restart the sandbox
						// session (same URL = fresh spawn, all vehicles and weapons again) 4 s after death. The death screen stays up
						// meanwhile. The timer lives on the main thread, so it still fires while the death menu pauses the game.
						// &sandboxRespawn=0 disables it.
						std::fprintf( stderr, "[coreplay] SANDBOX RESPAWN: player died; restarting the sandbox session in 4 s (&sandboxRespawn=0 to disable)\n" );
#ifdef __EMSCRIPTEN__
						MAIN_THREAD_ASYNC_EM_ASM( {
							try {
								if ( new URLSearchParams( location.search ).get( 'sandboxRespawn' ) === '0' ) { console.log( '[coreplay] sandbox respawn disabled by URL' ); return; }
								setTimeout( () => { console.log( '[coreplay] sandbox respawn: reloading ' + location.href ); location.reload(); }, 4000 );
							} catch ( e ) {}
						} );
#endif
					}
					s_cpPlayerHealth = ph;
				}
			}
			if ( mounted != s_cpMounted )
			{
				std::fprintf( stderr, "[coreplay] MOUNT state: %s %d\n", mounted >= 0 ? "mounted vehicle" : "on foot", mounted );
				s_cpMounted = mounted;
			}
			static Uint32 s_cpPlayerTick = 0;
			if ( mounted >= 0 || ( s_cpPlayerTick++ % 5 ) == 0 )
			{
				const Vector3 p = player.GetWorldPosition().AsVector3();
				std::fprintf( stderr, "[coreplay] t=%.0fs player (%.2f, %.2f, %.2f) weapon %s mag %d shots %u\n", s_cpClock, p.X, p.Y, p.Z, CpWeaponName( s_cpWeapon ), s_cpMag, s_cpShots );
			}
		}

		static Bool s_tickLogged = false;

		static void Tick( const game::IGameInstance* gi, Float timeDelta )
		{
			if ( !s_tickLogged )
			{
				// unconditional: proves the hook is linked and ticking, and that the URL read returns
				s_tickLogged = true;
				std::fprintf( stderr, "[sandbox-spawn] tick hook alive (ScriptableSystemsContainer::Tick); reading URL\n" );
				ReadUrl();
				std::fprintf( stderr, "[sandbox-spawn] URL read done: sandbox=%d vehicles=%u weapons=%d\n", s.enabled, s.vehicleCount, s.weaponsEnabled );
			}
			if ( g_coreplayHitProbe )
			{
				// [tkfix] refill the hit-probe line budgets once per second (shoot / impact / damage)
				static Float s_hitBudgetClock = 1.0f;
				s_hitBudgetClock += timeDelta;
				if ( s_hitBudgetClock >= 1.0f )
				{
					s_hitBudgetClock = 0.0f;
					__atomic_store_n( &g_coreplayHitBudget[ 0 ], 8, __ATOMIC_RELAXED );
					__atomic_store_n( &g_coreplayHitBudget[ 1 ], 6, __ATOMIC_RELAXED );
					__atomic_store_n( &g_coreplayHitBudget[ 2 ], 8, __ATOMIC_RELAXED );
				}
			}
			if ( gi == nullptr || s.enabled <= 0 )
			{
				return;
			}
			if ( !s.armedOnce )
			{
				// IScriptableSystemsContainer::OnPlayerAttached is only called for multiplayer muppets, so poll the
				// player system for the local player's main object every frame until it exists.
				auto playerSystem = gi->GetSystemPtr< game::IPlayerSystem >();
				THandle< game::Object > mainObject = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				if ( !mainObject )
				{
					return;
				}
				std::fprintf( stderr, "[sandbox-spawn] local player main object found via IPlayerSystem (poll)\n" );
				OnPlayerAttached( mainObject->WeakHandleFromThis< game::Object >() );
			}
			Probe( gi, timeDelta );
			if ( s.phase == Phase::Done || s.phase == Phase::Verify )
			{
				if ( THandle< game::Object > cpPlayer = s.player.ToHandle() )
				{
					CoreplayTick( gi, *cpPlayer, timeDelta ); // [coreplay]
				}
			}
			if ( s.phase == Phase::Off || s.phase == Phase::Done )
			{
				return;
			}
			THandle< game::Object > player = s.player.ToHandle();
			if ( !player )
			{
				if ( s.phase != Phase::WaitPlayer )
				{
					std::fprintf( stderr, "[sandbox-spawn] player object lost during phase %d; stopping\n", Int32( s.phase ) );
					s.phase = Phase::Done;
				}
				return;
			}
			s.timer += timeDelta;
			switch ( s.phase )
			{
			case Phase::WaitPlayer:
				if ( s.timer < c_startDelay )
				{
					return;
				}
				s.timer = 0.0f;
				s.next = 0;
				if ( s.weaponsEnabled )
				{
					GiveAmmoAndPerks( gi, *player );
					s.phase = Phase::Weapons;
				}
				else
				{
					std::fprintf( stderr, "[sandbox-spawn] weapons skipped (?sandboxWeapons=0)\n" );
					s.phase = Phase::Equip;
				}
				return;
			case Phase::Weapons:
				GiveWeapons( gi, *player );
				if ( s.next >= c_weaponsTotal )
				{
					std::fprintf( stderr, "[sandbox-spawn] weapons given: %u ok, %u failed of %u\n", s.weaponsOk, s.weaponsFail, c_weaponsTotal );
					s.timer = 0.0f;
					s.phase = Phase::Equip;
				}
				return;
			case Phase::Equip:
				if ( s.timer < 0.5f )
				{
					return;
				}
				if ( s.weaponsEnabled )
				{
					EquipOne( gi, *player );
				}
				CaptureGrid( *player );
				for ( Uint32 i = s.vehicleCount; i < c_vehiclesTotal; ++i )
				{
					std::fprintf( stderr, "[sandbox-spawn] vehicle %u %s skipped by cap (%u)\n", i, VName( i ), s.vehicleCap );
				}
				s.next = 0;
				s.timer = c_vehicleInterval;
				s.phase = Phase::Vehicles;
				return;
			case Phase::Vehicles:
				if ( s.next >= s.vehicleCount )
				{
					s.timer = 0.0f;
					s.phase = Phase::Verify;
					return;
				}
				if ( s.timer < c_vehicleInterval )
				{
					return;
				}
				s.timer = 0.0f;
				SpawnVehicle( gi, s.next++ );
				return;
			case Phase::Verify:
				if ( s.timer < c_verifyDelay )
				{
					return;
				}
				Verify( gi );
				ProbeVehicles( gi, "verify" );
				FixPositions( gi );
				ParkVehicles( gi, "verify", false );
				s_probeNextVehicles = s_probeClock + 15.0f;
				s.phase = Phase::Done;
				return;
			default:
				return;
			}
		}
	} // namespace cpweb_sandbox_spawn

#ifdef __EMSCRIPTEN__
	// [dbgpos] Debug state for automated playtesting: player position/yaw and the tracked objective mappin position.
	// Same thread-safety scheme as Module.cp2077TouchState (gameInput inputManager.cpp overlay): a small seqlocked
	// struct in Wasm memory, written here on the game-systems tick (<= 4 Hz), read on demand by the page through
	// Module.cp2077DebugState() (registered once). Nothing runs on the main thread unless the page calls the getter.
	namespace cpweb_debug_state
	{
		struct alignas( 8 ) DebugState
		{
			Uint32 m_seq;           // 0  odd while writing
			Int32 m_hasPlayer;      // 4
			Float m_player[ 6 ];    // 8  x y z yaw fwdX fwdY
			Int32 m_hasObjective;   // 32
			Float m_objective[ 4 ]; // 36 x y z dist
			Uint32 m_updates;       // 52
			char m_quest[ 192 ];    // 56
		};
		static_assert( offsetof( DebugState, m_player ) == 8 && offsetof( DebugState, m_hasObjective ) == 32 &&
			offsetof( DebugState, m_objective ) == 36 && offsetof( DebugState, m_updates ) == 52 && offsetof( DebugState, m_quest ) == 56,
			"debug state layout is read by Module.cp2077DebugState" );
		DebugState g_state{};
		// [teleport] ?devTeleport=1 test hook: Module.cp2077Teleport(x, y, z, yaw) moves the local player (streaming route
		// tests: walking/driving bots got stuck on props). Floats first, then the sequence bump; applied in Update (4 Hz).
		struct alignas( 8 ) TeleportRequest { Int32 m_seq; Float m_v[ 4 ]; };
		TeleportRequest g_teleport{};

		static void RegisterTeleport()
		{
			MAIN_THREAD_ASYNC_EM_ASM( {
				Module['cp2077Teleport'] = (function( at ) { return function( x, y, z, yaw ) {
					HEAPF32[ ( at + 4 ) / 4 ] = x; HEAPF32[ ( at + 8 ) / 4 ] = y; HEAPF32[ ( at + 12 ) / 4 ] = z; HEAPF32[ ( at + 16 ) / 4 ] = yaw || 0;
					return Atomics.add( HEAP32, at / 4, 1 ) + 1; }; })( Number( $0 ) );
			}, static_cast< double >( reinterpret_cast< uintptr_t >( &g_teleport ) ) );
		}

		static void Register()
		{
			static Bool s_registered = false;
			if ( s_registered ) return;
			s_registered = true;
			MAIN_THREAD_ASYNC_EM_ASM( {
				Module['cp2077DebugState'] = (function( at ) { return function() {
					for ( var tries = 0; tries < 8; tries++ ) {
						var q0 = HEAPU32[ at / 4 ];
						if ( q0 & 1 ) continue;
						var f = function( o ) { return HEAPF32[ ( at + o ) / 4 ]; };
						var player = HEAP32[ ( at + 4 ) / 4 ] ? { x: f( 8 ), y: f( 12 ), z: f( 16 ), yaw: f( 20 ), fx: f( 24 ), fy: f( 28 ) } : null;
						var objective = HEAP32[ ( at + 32 ) / 4 ] ? { x: f( 36 ), y: f( 40 ), z: f( 44 ), dist: f( 48 ) } : null;
						var n = HEAPU32[ ( at + 52 ) / 4 ];
						var quest = ''; for ( var i = 0; i < 191; i++ ) { var ch = HEAPU8[ at + 56 + i ]; if ( !ch ) break; quest += String.fromCharCode( ch ); }
						if ( HEAPU32[ at / 4 ] !== q0 ) continue;
						return { player: player, objective: objective, quest: quest || null, updates: n };
					}
					return { player: null, objective: null, quest: null, updates: -1, torn: true }; }; })( Number( $0 ) );
			}, static_cast< double >( reinterpret_cast< uintptr_t >( &g_state ) ) );
			std::fprintf( stderr, "CP2077 debug state: Module.cp2077DebugState() registered (player/objective, <= 4 Hz)\n" );
		}

		struct ObjectivePick
		{
			Vector3 player;
			Vector3 pos;
			Float dist;
			Int32 rank; // 0 none, 1 tracked mappin, 2 tracked quest mappin
			Bool hasPlayer;
		};

		static void Update( const game::IGameInstance* gi )
		{
			static double s_next = 0.0;
			const double now = emscripten_get_now();
			if ( now < s_next ) return;
			s_next = now + 250.0;
			Register();

			Int32 hasPlayer = 0;
			Float player[ 6 ] = {};
			Int32 hasObjective = 0;
			Float objective[ 4 ] = {};
			static char s_quest[ 192 ] = {};
			static const void* s_questEntry = nullptr;

			if ( gi != nullptr )
			{
				auto playerSystem = gi->GetSystemPtr< game::IPlayerSystem >();
				THandle< game::Object > obj = playerSystem ? playerSystem->GetLocalPlayerMainObject() : THandle< game::Object >();
				ObjectivePick pick{};
				if ( obj )
				{
					const Vector3 p = obj->GetWorldPosition().AsVector3();
					const Vector3 fw = obj->GetWorldForward();
					player[ 0 ] = p.X; player[ 1 ] = p.Y; player[ 2 ] = p.Z;
					player[ 3 ] = obj->GetWorldYaw();
					player[ 4 ] = fw.X; player[ 5 ] = fw.Y;
					hasPlayer = 1;
					pick.player = p;
					pick.hasPlayer = true;
					static const Int32 s_devTeleport = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'devTeleport' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
					static Int32 s_teleportSeq = 0;
					if ( s_devTeleport )
					{
						static Bool s_teleportRegistered = false;
						if ( !s_teleportRegistered ) { s_teleportRegistered = true; RegisterTeleport(); }
						const Int32 seq = __atomic_load_n( &g_teleport.m_seq, __ATOMIC_ACQUIRE );
						auto teleport = gi->GetSystemPtr< game::ITeleportationFacility >();
						if ( seq != s_teleportSeq && teleport )
						{
							s_teleportSeq = seq;
							const Float* v = g_teleport.m_v;
							teleport->Teleport( obj, Vector4( v[ 0 ], v[ 1 ], v[ 2 ], 1.0f ), EulerAngles( 0.0f, 0.0f, v[ 3 ] ) );
							std::fprintf( stderr, "CP2077 teleport %d: (%.1f, %.1f, %.1f) yaw %.0f from (%.1f, %.1f, %.1f)\n", seq, v[ 0 ], v[ 1 ], v[ 2 ], v[ 3 ], p.X, p.Y, p.Z );
						}
					}
				}
				if ( auto mappins = gi->GetSystemPtr< game::mappins::IMappinSystem >() )
				{
					ObjectivePick* pp = &pick;
					mappins->ForEachMappinInstance( [ pp ]( const THandle< game::mappins::IMappin >& m )
					{
						if ( !m || !m->IsPlayerTracked() || !m->IsActive() ) return;
						const Int32 rank = m->IsQuestMappin() ? 2 : 1;
						const Vector3 w = m->GetWorldPosition();
						Float d = m->GetDistanceToPlayer();
						if ( pp->hasPlayer )
						{
							const Float dx = w.X - pp->player.X, dy = w.Y - pp->player.Y, dz = w.Z - pp->player.Z;
							d = std::sqrt( dx * dx + dy * dy + dz * dz );
						}
						if ( rank > pp->rank || ( rank == pp->rank && d < pp->dist ) )
						{
							pp->rank = rank; pp->pos = w; pp->dist = d;
						}
					} );
				}
				if ( pick.rank > 0 )
				{
					hasObjective = 1;
					objective[ 0 ] = pick.pos.X; objective[ 1 ] = pick.pos.Y; objective[ 2 ] = pick.pos.Z; objective[ 3 ] = pick.dist;
				}
				auto journal = gi->GetSystemPtr< game::IJournalManager >();
				THandle< game::JournalEntry > entry = journal ? journal->GetTrackedQuestEntry() : THandle< game::JournalEntry >();
				const void* key = entry ? static_cast< const void* >( entry.Get() ) : nullptr;
				if ( key != s_questEntry )
				{
					s_questEntry = key;
					s_quest[ 0 ] = 0;
					if ( entry )
					{
						if ( THandle< game::JournalPath > path = journal->GetPath( entry ) )
						{
							const char* t = path->GetRealPath().AsChar();
							const size_t n = t ? strnlen( t, sizeof( s_quest ) - 1 ) : 0;
							if ( n ) std::memcpy( s_quest, t, n );
							s_quest[ n ] = 0;
						}
					}
				}
			}
			else
			{
				s_questEntry = nullptr;
				s_quest[ 0 ] = 0;
			}

			// seqlock write: everything above is gathered into locals first, the odd window is just these copies
			__atomic_fetch_add( &g_state.m_seq, 1u, __ATOMIC_SEQ_CST );
			g_state.m_hasPlayer = hasPlayer;
			std::memcpy( g_state.m_player, player, sizeof( player ) );
			g_state.m_hasObjective = hasObjective;
			std::memcpy( g_state.m_objective, objective, sizeof( objective ) );
			g_state.m_updates++;
			std::memcpy( g_state.m_quest, s_quest, sizeof( s_quest ) );
			__atomic_fetch_add( &g_state.m_seq, 1u, __ATOMIC_SEQ_CST );
		}
	} // namespace cpweb_debug_state
#endif

	void ScriptableSystemsContainer::OnPlayerAttached( WeakHandle< game::Object > playerObject )
	{
		THandle< PlayerAttachRequest > request = CreateHandle< PlayerAttachRequest >( playerObject );
		QueueRequest( request );
		cpweb_sandbox_spawn::OnPlayerAttached( playerObject );
	}

	void ScriptableSystemsContainer::OnPlayerDetached( WeakHandle< game::Object > playerObject )
	{
		THandle< PlayerDetachRequest > request = CreateHandle< PlayerDetachRequest >( playerObject );
		QueueRequest( request );
	}

	void ScriptableSystemsContainer::RegisterUpdateFunctions( UpdatableSystemRegistrar& processor )
	{
		processor.RegisterInBucket( UpdateBucket::CharacterMask(), UpdateBucketPhase::PhysicsFlushBufferedState, *this, "ScriptableSystems/Tick",
			[ this ]( UpdateBucket::Enum bucket, const UpdateContext& updateContext, job::Builder& )
			{
				const Float engineTime = updateContext.m_timeDelta;
				const tick::Info& tickInfo = updateContext.m_tickInfo; 
				Tick( engineTime, tickInfo ); 
			} 
		);
	}

	void ScriptableSystemsContainer::Tick( const Float timeDelta, const tick::Info& tickInfo )
	{
		PC_SCOPE_FUNC();
		for ( auto it : m_systems )
		{
			it.Value()->Tick( timeDelta, tickInfo );
		}
		cpweb_sandbox_spawn::Tick( GetGameInstance(), timeDelta );
#ifdef __EMSCRIPTEN__
		cpweb_debug_state::Update( GetGameInstance() ); // [dbgpos]
#endif
	}

	void ScriptableSystemsContainer::funcGet(CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( CName, systemName, CName::NONE() );
		FINISH_PARAMETERS;
		RETURN_OBJECT( Get( systemName ) );
	}

	void ScriptableSystemsContainer::funcQueueRequest( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		GET_PARAMETER( ScriptableSystem::RequestHandle, request, ScriptableSystem::RequestHandle() );
		FINISH_PARAMETERS;

		QueueRequest( request );
	}

	void ScriptableSystemsContainer::QueueRequest( const RequestHandle& request ) const
	{
		if ( !request )
		{
			RED_LOG_ERROR( "ScriptableSystemsContainer: can't queue null request." );
			return;
		}

		request->MarkAsGeneric();

		for ( auto it : m_systems )
		{
			it.Value().Get()->QueueRequest( request );
		}
	}

	IGameSystem::PreSaveResponse ScriptableSystemsContainer::OnPreGameSave( const job::RunContext& context, game::MetadataCollector& collector )
	{
		red::PackageBuilder builder;
		builder.Initialize( { red::PackagePropertyType_Persistent, PLATFORM_None, RED_KILO_BYTE( 256 ) } );

		red::DynArray< CName > mapping{ red::PoolFrame() }; 
		mapping.Reserve( m_systems.Size() );

		Uint32 index = 0;
		for ( auto it : m_systems )
		{
			const auto* classType = it.Value()->GetClass();
			const auto& props = classType->GetCachedPersistentProperties();

			if ( !props.Empty() )
			{
				mapping.PushBack( it.Key() );
				const Uint32 actualIndex = builder.WriteObject( it.Value().Get(), classType->GetDefaultObject() );
				GAME_ASSERT( index == actualIndex, Save, "id-index error" );
				++index;
			}
		}

		ScriptableSystemsPackageSaver saver( mapping );
		red::CompiledPackage generatedPackage = builder.BuildPackage( saver );
		m_savedData = DataBuffer( std::move( generatedPackage.buffer ) );

		return PreSaveResponse::ReadyForSaving;
	}

	void ScriptableSystemsContainer::OnGameSave( save::IStream& saveStream )
	{
		SAVE_NODE( ScriptableSystemsContainer );
		saveStream << m_savedData;
	}

	void ScriptableSystemsContainer::OnPostGameSave()
	{
		m_savedData.Clear();
	}

	void ScriptableSystemsContainer::OnGameLoad( const job::RunContext& context, Bool& result, save::IStream& loadStream )
	{
		if ( loadStream.GetSaveVersion() < save::SaveVersion::ScriptableSystems )
		{
			CreateAndAttachAllSystems( nullptr );
			return;
		}

		Bool quickExit = false;
		LOAD_NODE_REQUIRED_FROM_VERSION( ScriptableSystemsContainer, save::SaveVersion::ScriptableSystems, result, quickExit );
		if ( quickExit )
		{
			return;
		}

		m_loadVersion = Int32( loadStream.GetSaveVersion() );
		m_loadGameVersion = Int32( loadStream.GetGameVersion() );

		DataBuffer buffer;
		loadStream << buffer;
		
		CreateAndAttachAllSystems( &buffer );

		if ( !loadStream.IsOK() )
		{
			result = false;
		}
	}

	Bool ScriptableSystemsContainer::OnGameRestored()
	{
		for ( auto it : m_systems )
		{
			GAME_ASSERT( m_loadVersion >= save::c_minimalSaveVersion, Save, "Version under minimal." );
			GAME_ASSERT( m_loadGameVersion >= 0, Save, "Game version invalid." );
			rtti::CallFunction( it.Value().Get(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "OnRestored;Int32Int32" ) ), m_loadVersion, m_loadGameVersion );
		}
		m_loadVersion = -1;
		m_loadGameVersion = -1;
		return true;
	}

	SaveLock ScriptableSystemsContainer::IsSavingLocked( const bool isForced ) const
	{
		Bool atLeastOneLocked = false;
#if !defined( RED_CONFIGURATION_FINAL )
		red::StringBuilder< String > reasons;
#else
		red::StringView reasons{};
#endif
		for ( auto it : m_systems )
		{
			Bool thisOneLocked = false;
			if ( !rtti::CallFunctionRet< Bool >( it.Value().Get(), rtti::RawFunctionName( RED_NAME_CONSTEXPR( "IsSavingLocked;" ) ), thisOneLocked ) )
			{
				continue;
			}

			if ( thisOneLocked )
			{
#if !defined( RED_CONFIGURATION_FINAL )
				if ( atLeastOneLocked )
				{
					reasons.Append( '\n' );
				}
#endif

				atLeastOneLocked = true;

#if !defined( RED_CONFIGURATION_FINAL )
				const auto keyName = it.Key().AsStringView();
				reasons.Append( "Scriptable system: " );
				reasons.Append( keyName.Data(), keyName.Length() );
				reasons.Append( " locked saves." );
#endif
			}
		}

		return SaveLock{ atLeastOneLocked ? SaveLock::Reason::Script : SaveLock::Reason::Nothing, reasons.ToString() };
	}

} // namespace game



#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

