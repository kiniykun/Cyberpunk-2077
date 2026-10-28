/**
* Copyright (C)2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <atomic>
#include <cstdio>
// [nailfix] Set while the session's character customization state is the developer fallback preset
// (test\player_presets\default_*.ccstate, used when a new session starts without character creation:
// ?sandbox, ?start=nomad). Read by worldEntities appearanceResource.cpp, which then shows the CC default nails
// (beige) instead of the preset's green UV-test 'checker' nails. Cleared by any real (CC or save) state.
namespace cpweb_nailfix_shared { extern std::atomic< int > g_fallbackCustomization; }
namespace { thread_local bool t_nailfixInFallbackPreset = false; }
// [missionflow] ?start=nomad direct new game (no menu clicking): the main menu's PreGameRequestsHandler overlay asks
// cpweb_missionflow_BuildNomadState() for the default Nomad state (fallback preset + lifepath Nomad + new game +
// UIAttributePresets.Preset1 / 7 points, i.e. what the CC menus produce when every screen is just confirmed) and hands
// it to the menu's own StartNewGame. The new session's InitializeFromFinalizedState routes that state through the same
// CheckVersionsCompatibility + nails handling as the developer fallback preset.
namespace { std::atomic< const void* > g_missionflowStatePtr{ nullptr }; thread_local bool t_missionflowInCompat = false; }
#include "characterCustomizationSystem.h"
#include "../../gameCore/include/characterCustomizationComponentInterface.h"
#include "../../game/include/puppet.h"
#include "../../game/include/itemObject.h"
#include "../../gameCore/include/transactionSystemInterface.h"
#include "../../gameTweakDB/include/records/tweakDBItem.h"
#include "../../gameTweakDB/include/types/tweakDBGender.h"
#include "../../gameTweakDB/include/records/tweakDBLifePath.h"
#include "../../gameTweakDB/include/types/tweakDBLifePath.h"
#include "../../world/include/gameInstance.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../worldEntities/include/morphTargetManagerComponent.h"
#include "../../gameCore/include/scriptableSystemsContainerInterface.h"
#include "../../gameCore/include/questsSystemInterface.h"
#include "../../gameCore/include/fdbInterface.h"
#include "../../worldUserInterface/include/runtimeSystemUI.h"
#include "../../inkCore/include/gameNotificationsUtilities.h"
#include "../../gameCore/include/inputHandle.h"
#include "../../redReflection/include/censorshipSystem.h"
#include "../../redReflection/include/gatheredResource.h"
#include "../../game/include/puppetHelper.h"
#include "../../resource/include/resourceSaver.h"
#include "../../redReflection/include/resourceDepot.h"
#include "../../redCore/include/commandline.h"
#include "../../redMath/include/random.h"
#include "../../gameTweakDB/include/records/tweakDBCharacter.h"
#include "../../game/include/audioEvents.h"
#include "../../gameCore/include/gameISceneSystemInterface.h"
#include "../../gameSceneCore/include/scnsSceneInstanceParams.h"
#include "../../gameSceneCore/include/scnsISceneSystem.h"
#include "../../worldEntities/include/animatedComponent.h"
#include "../../worldEntities/include/runtimeSystemEntityAppearanceChanger.h"
#include "../../engine/include/baseEngine.h"
#include "../../localization/include/localizationManager.h"
#include "../../gameTweakDB/include/records/tweakDBUICharacterCreationAttributesPreset.h" // [missionflow]
#include "../../gameTweakDB/include/records/tweakDBUICharacterCreationAttribute.h" // [missionflow]
#include "../../gameTweakDB/include/records/tweakDBStat.h" // [missionflow]

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationOptionImpl, game, ui );
	RTTI_PARENT_TYPE( game::ui::CharacterCustomizationOption );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CensorshipInfo, game, ui );
	RTTI_PROPERTY( m_censorFlag );
	RTTI_PROPERTY( m_censorFlagAction );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CustomizationAppearance, game, ui );
	RTTI_PARENT_TYPE( game::ui::CensorshipInfo );
	RTTI_PROPERTY( m_name );
	RTTI_PROPERTY( m_resource );
	RTTI_PROPERTY( m_definition );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CustomizationMorph, game, ui );
	RTTI_PARENT_TYPE( game::ui::CensorshipInfo );
	RTTI_PROPERTY( m_regionName );
	RTTI_PROPERTY( m_targetName );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CustomizationGroup, game, ui );
	RTTI_PROPERTY( m_name );
	RTTI_PROPERTY( m_customization );
	RTTI_PROPERTY( m_morphs );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationPreset, game, ui );
	RTTI_PARENT_TYPE( CResource );
#ifdef CHARACTER_CUSTOMIZATION_PRESETS_ENABLED
	RTTI_PROPERTY( m_isMale );
	RTTI_PROPERTY( m_bodyGroups );
	RTTI_PROPERTY( m_headGroups );
	RTTI_PROPERTY( m_armsGroups );
	RTTI_PROPERTY( m_perspectiveInfo );
	RTTI_PROPERTY( m_tags );
	RTTI_PROPERTY( m_version );
#endif
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationState, game, ui );
	RTTI_PARENT_TYPE( game::ui::ICharacterCustomizationState );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem, game, ui );
	RTTI_PARENT_TYPE( game::ui::ICharacterCustomizationSystem );
	RTTI_IMPORT_ONLY();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SetCharacterCreationDataRequest, game, ui );
	RTTI_PARENT_TYPE( game::PlayerScriptableSystemRequest );
	RTTI_PROPERTY( m_lifepath );
	RTTI_PROPERTY( m_attributes );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem_OnInitializeOptionsListEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem_OnPresetAppliedEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem_OnRandomizeCompleteEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SwitchPair, game, ui );
	RTTI_PROPERTY( m_prevOption );
	RTTI_PROPERTY( m_currOption );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem_OnAppearanceSwitchedEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
	RTTI_PROPERTY( m_pairs );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCustomizationSystem_OnOptionUpdatedEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
	RTTI_PROPERTY( m_option );
RTTI_END_TYPE();

namespace
{
	static game::ui::CharacterCustomizationOptions s_emptyCustomizationOptions{ game::PoolGMPL_CharacterCustomization() };
		
	static constexpr const char s_tempFolder[] = "temp\\player_presets\\";
	static constexpr Uint32 s_reinitOptionsCount = 1U;

	static res::TStaticResource< game::ui::CharacterCustomizationInfoResource > s_maleCCO( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\fullscreen\\main_menu\\male_cco.inkcharcustomization" ), GRF_SkipOnHeadless | GRF_Startup );
	static res::TStaticResource< game::ui::CharacterCustomizationInfoResource > s_femaleCCO( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\fullscreen\\main_menu\\female_cco.inkcharcustomization" ), GRF_SkipOnHeadless | GRF_Startup );


	THandle<ent::MorphTargetManagerComponent> FindMorphManager( const THandle<ent::Entity>& entity )
	{
		THandle<ent::MorphTargetManagerComponent> component;

		const auto& components = entity->GetComponents();
		for ( const auto& comp : components )
		{
			component = Cast<ent::MorphTargetManagerComponent>( comp );
			if ( component ) break;
		}
		return component;
	}

	const THandle< game::ui::CharacterCustomizationOption > FindOption( const game::ui::CharacterCustomizationOptions& options, CName optName )
	{
		auto it = std::find_if( options.Begin(), options.End(),
			[ optName ]( const THandle< game::ui::CharacterCustomizationOption >& option )
		{
			return option->GetInfo()->m_name == optName;
		} );

		if ( it != options.End() )
		{
			return ( *it );
		}

		return nullptr;
	}

	Uint32 GetLinkValue( const red::HashMap< CName, Uint32 >& links, CName linkName )
	{
		Uint32 linkValue;
		if ( !linkName.Empty() && links.Find( linkName, linkValue ) )
		{
			return linkValue;
		}
		return -1;
	}
}

namespace Config
{
	TConfigVar< Float > cvMorphingTimeDilation( "CharacterCustomizationSystem", "MorphingTimeDilation", 3.f, eConsoleVarFlag_Save );
}

namespace game
{
	namespace ui
	{
		//////////////////////////////////////////////////////////////////////////

		CharacterCustomizationOptionImpl::CharacterCustomizationOptionImpl()
		{
			Reset();
		}

		void CharacterCustomizationOptionImpl::Init( CharacterCustomizationPart part, const THandle< const CharacterCustomizationInfo >& info, const Uint32 censorFlags )
		{
			m_bodyPart = part;
			m_info = info;

			Bool permanentlyInactive = ( info->m_censorFlagAction == CharacterCustomizationActionType::Deactivate ) ?
				( censorFlags & info->m_censorFlag ) == info->m_censorFlag :
				( censorFlags & info->m_censorFlag ) != info->m_censorFlag;

			Reset( permanentlyInactive );
			SetActive( m_info->m_enabled );
			if ( IsActive() )
			{
				Set( m_info->m_defaultIndex );
			}
		}

		void CharacterCustomizationOptionImpl::ReInit()
		{
			Reset( m_isCensored );
			SetActive( m_info->m_enabled );
			if ( IsActive() )
			{
				Set( m_info->m_defaultIndex );
			}
		}

		void CharacterCustomizationOptionImpl::Reset( Bool permanentlyInactive )
		{
			m_prevIndex = -1;
			m_currIndex = -1;
			m_isActive = false;
			m_isCensored = permanentlyInactive;
		}

		void CharacterCustomizationOptionImpl::Set( Uint32 currIndex )
		{
			m_prevIndex = m_currIndex;
			m_currIndex = currIndex;
		}

		const THandle<const game::ui::MorphInfo> CharacterCustomizationOptionImpl::GetMorphInfo() const
		{
			return Cast<MorphInfo>( m_info );
		}

		const THandle<const game::ui::AppearanceInfo> CharacterCustomizationOptionImpl::GetAppearanceInfo() const
		{
			return Cast<AppearanceInfo>( m_info );
		}

		const THandle<const game::ui::SwitcherInfo> CharacterCustomizationOptionImpl::GetSwitcherInfo() const
		{
			return Cast<SwitcherInfo>( m_info );
		}

		//////////////////////////////////////////////////////////////////////////

		CharacterCustomizationState::CharacterCustomizationState()
			: m_isFinalized( false )
			, m_isNewGame( true )
			, m_isMale( false )
			, m_isMaleVO( false )
			, m_attributePointsAvailable( 0 )
			, m_version( 0 )
		{
		}

		void CharacterCustomizationState::Finalize(
			const CharacterCustomizationOptions& bodyOptions,
			const CharacterCustomizationOptions& headOptions,
			const CharacterCustomizationOptions& armsOptions,
			const THandle<const CharacterCustomizationInfoResource>& info )
		{
			RED_SCOPE_LOCK( m_stateLock );

			RED_ASSERT( m_isFinalized == false );

			Finalize( bodyOptions, info->GetBodyGroups(), m_bodyCustomizationGroups );
			Finalize( headOptions, info->GetHeadGroups(), m_headCustomizationGroups );
			Finalize( armsOptions, info->GetArmsGroups(), m_armsCustomizationGroups );

			m_perspectiveInfo = info->GetPerspectiveInfo();

			m_version = info->GetVersion();

			m_isFinalized = true;
		}

		void CharacterCustomizationState::Finalize( const CharacterCustomizationOptions& options, const red::DynArray<OptionsGroup>& optionsGroup, CustomizationGroups& customizationGroups )
		{
			Uint32 index;

			for ( const auto& group : optionsGroup )
			{
				customizationGroups.EmplaceBack( group.m_name );
				auto& current = customizationGroups.Back();
				for ( const auto& option : options )
				{
					if ( group.m_options.Exist( option->GetInfo()->m_name ) )
					{
						if ( option->IsActive() )
						{
							if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
							{
								index = option->GetCurrIndex();
								if ( index < appearanceInfo->m_definitions.Size() )
								{
									const auto& definition = appearanceInfo->m_definitions[ index ];
									if ( appearanceInfo->m_resource.IsValid() && !definition.m_name.Empty() )
									{
										current.m_customization.EmplaceBack( appearanceInfo->m_name,
											appearanceInfo->m_resource,
											definition.m_name,
											appearanceInfo->m_censorFlag,
											appearanceInfo->m_censorFlagAction );

										m_tags.AddTags( definition.m_tags );
									}
								}
							}
							else if ( const auto& morphInfo = option->GetMorphInfo() )
							{
								index = option->GetCurrIndex();
								if ( index < morphInfo->m_morphNames.Size() )
								{
									const auto& morph = morphInfo->m_morphNames[ index ];
									if ( !morph.m_morphName.Empty() )
									{
										current.m_morphs.EmplaceBack( morphInfo->m_name,
											morph.m_morphName,
											morphInfo->m_censorFlag,
											morphInfo->m_censorFlagAction );

										m_tags.AddTags( morph.m_tags );
									}
								}
							}
							else if ( const auto& switcherInfo = option->GetSwitcherInfo() )
							{
								index = option->GetCurrIndex();
								if ( index < switcherInfo->m_options.Size() )
								{
									const auto& switcherOption = switcherInfo->m_options[ index ];
									if ( !switcherOption.m_name.Empty() )
									{
										m_tags.AddTags( switcherOption.m_tags );
									}
								}
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationState::Serialize( save::IStream& stream )
		{
			RED_SCOPE_LOCK( m_stateLock );

			if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_ResourceVersion )
			{
				stream << m_version;
			}
			else
			{
				m_version = 0;
			}

			stream << m_isMale;
			stream << m_isMaleVO;

			Bool isFppHeadVersion = ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_FppHead );
			Bool isFlatFeetVersion = ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_FlatFeet );

			CustomizationGroup head( RED_NAME_CONSTEXPR( "TPP" ) );
			CustomizationGroup body( RED_NAME_CONSTEXPR( "TPP" ) );
			CustomizationGroup breast( RED_NAME_CONSTEXPR( "breast" ) );
			CustomizationGroup genitals( RED_NAME_CONSTEXPR( "genitals" ) );

			if ( !isFppHeadVersion )
			{
				SerializeAppearances( stream, head.m_customization );
			}

			if ( !isFlatFeetVersion )
			{
				SerializeAppearances( stream, body.m_customization );
				SerializeAppearances( stream, breast.m_customization );
				SerializeAppearances( stream, genitals.m_customization );
			}

			if ( !isFppHeadVersion )
			{
				SerializeMorphs( stream, head.m_morphs );

				m_headCustomizationGroups.PushBack( head );
			}

			if ( !isFlatFeetVersion )
			{
				SerializeMorphs( stream, body.m_morphs );
				SerializeMorphs( stream, breast.m_morphs );
				SerializeMorphs( stream, genitals.m_morphs );

				m_bodyCustomizationGroups.PushBack( body );
				m_bodyCustomizationGroups.PushBack( breast );
				m_bodyCustomizationGroups.PushBack( genitals );
			}

			if ( isFppHeadVersion )
			{
				SerializeCustomizationGroup( stream, m_headCustomizationGroups );
			}


			if (stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_Arms )
			{
				SerializeCustomizationGroup( stream, m_armsCustomizationGroups );
			}

			if ( isFlatFeetVersion )
			{
				SerializeCustomizationGroup( stream, m_bodyCustomizationGroups );
			}

			SerializePerspectiveInfo( stream, m_perspectiveInfo );

			if ( stream.IsReader() )
			{
				m_isFinalized = true;
				m_isNewGame = false;
				m_lifepath = TweakDBID::NONE();
			}

			if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_Tags )
			{
				m_tags.Serialize( stream );
			}
		}

		void CharacterCustomizationState::SerializeAppearancesOptionsNames( save::IStream& stream, red::DynArray< CName >& array )
		{
			if ( stream.GetSaveVersion() < save::SaveVersion::CharacterCustomization_FlatFeet )
			{
				return;
			}

			if ( stream.IsWriter() )
			{
				Uint32 size = array.Size();
				stream << size;
				for ( Uint32 i = 0; i < size; ++i )
				{
					stream << array[ i ];
				}
			}

			if ( stream.IsReader() )
			{
				Uint32 size = 0;
				stream << size;
				if ( size != 0 )
				{
					array.Resize( size );
					for ( Uint32 i = 0; i < size; ++i )
					{
						stream << array[ i ];
					}
				}
			}
		}

		void CharacterCustomizationState::SerializeAppearances( save::IStream& stream, CustomizationAppearances& array )
		{
			Uint32 censorFlag, censorFlagAction;

			if ( stream.IsWriter() )
			{
				Uint32 size = array.Size();
				stream << size;
				for ( Uint32 i = 0; i < size; ++i )
				{
					array[ i ].m_resource.Serialize( stream );
					stream << array[ i ].m_definition;
					if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_CensorshipUpdate )
					{
						stream << array[ i ].m_name;
						censorFlag = array[ i ].m_censorFlag;
						censorFlagAction = array[ i ].m_censorFlagAction;
						stream << censorFlag;
						stream << censorFlagAction;
					}
				}
			}

			if ( stream.IsReader() )
			{
				Uint32 size = 0;
				stream << size;
				if ( size != 0 )
				{
					array.Resize( size );
					for ( Uint32 i = 0; i < size; ++i )
					{
						array[ i ].m_resource.Serialize( stream );
						stream << array[ i ].m_definition;
						if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_CensorshipUpdate )
						{
							stream << array[ i ].m_name;
							stream << censorFlag;
							stream << censorFlagAction;
							array[ i ].m_censorFlag = ( CensorshipFlags )censorFlag;
							array[ i ].m_censorFlagAction = ( CharacterCustomizationActionType )censorFlagAction;
						}
					}
				}
			}
		}

		void CharacterCustomizationState::SerializeMorphs( save::IStream& stream, CustomizationMorphs& array )
		{
			Uint32 censorFlag, censorFlagAction;

			if ( stream.IsWriter() )
			{
				Uint32 size = array.Size();
				stream << size;
				for ( Uint32 i = 0; i < size; ++i )
				{
					stream << array[ i ].m_regionName;
					stream << array[ i ].m_targetName;
					if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_CensorshipUpdate )
					{
						censorFlag = array[ i ].m_censorFlag;
						censorFlagAction = array[ i ].m_censorFlagAction;
						stream << censorFlag;
						stream << censorFlagAction;
					}
				}
			}

			if ( stream.IsReader() )
			{
				Uint32 size = 0;
				stream << size;
				if ( size != 0 )
				{
					array.Resize( size );
					for ( Uint32 i = 0; i < size; ++i )
					{
						stream << array[ i ].m_regionName;
						stream << array[ i ].m_targetName;
						if ( stream.GetSaveVersion() >= save::SaveVersion::CharacterCustomization_CensorshipUpdate )
						{
							stream << censorFlag;
							stream << censorFlagAction;
							array[ i ].m_censorFlag = ( CensorshipFlags )censorFlag;
							array[ i ].m_censorFlagAction = ( CharacterCustomizationActionType )censorFlagAction;
						}
					}
				}
			}
		}

		void CharacterCustomizationState::SerializeCustomizationGroup( save::IStream& stream, CustomizationGroups& groups )
		{
			if ( stream.IsWriter() )
			{
				Uint32 size = groups.Size();
				stream << size;
				for ( Uint32 i = 0; i < size; ++i )
				{
					stream << groups[ i ].m_name;
					if ( stream.GetSaveVersion() < save::SaveVersion::CharacterCustomization_CensorshipUpdate )
					{
						red::DynArray< CName > customizationNames{ red::PoolDoubleBufferedFrame() };
						SerializeAppearancesOptionsNames( stream, customizationNames );
					}
					SerializeAppearances( stream, groups[ i ].m_customization );
					SerializeMorphs( stream, groups[ i ].m_morphs );
				}
			}

			if ( stream.IsReader() )
			{
				Uint32 size = 0;
				stream << size;
				if ( size != 0 )
				{
					groups.Resize( size );
					for ( Uint32 i = 0; i < size; ++i )
					{
						stream << groups[ i ].m_name;
						if ( stream.GetSaveVersion() < save::SaveVersion::CharacterCustomization_CensorshipUpdate )
						{
							red::DynArray< CName > customizationNames{ red::PoolDoubleBufferedFrame() };
							SerializeAppearancesOptionsNames( stream, customizationNames );
						}
						SerializeAppearances( stream, groups[ i ].m_customization );
						SerializeMorphs( stream, groups[ i ].m_morphs );
					}
				}
			}
		}


		void CharacterCustomizationState::SerializePerspectiveInfo( save::IStream& stream, red::DynArray<PerspectiveInfo>& perspectiveInfo )
		{
			if ( stream.GetSaveVersion() < save::SaveVersion::CharacterCustomization_PerspectiveInfo )
			{
				return;
			}

			if ( stream.IsWriter() )
			{
				Uint32 size = perspectiveInfo.Size();
				stream << size;
				for ( Uint32 i = 0; i < size; ++i )
				{
					stream << perspectiveInfo[ i ].m_name;
					stream << perspectiveInfo[ i ].m_fpp;
					stream << perspectiveInfo[ i ].m_tpp;
				}
			}

			if ( stream.IsReader() )
			{
				Uint32 size = 0;
				stream << size;
				if ( size != 0 )
				{
					perspectiveInfo.Resize( size );
					for ( Uint32 i = 0; i < size; ++i )
					{
						stream << perspectiveInfo[ i ].m_name;
						stream << perspectiveInfo[ i ].m_fpp;
						stream << perspectiveInfo[ i ].m_tpp;
					}
				}
			}
		}

		Uint32 CharacterCustomizationState::GetVersion() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_version;
		}

		Bool CharacterCustomizationState::IsFinalized() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_isFinalized;
		}

		Bool CharacterCustomizationState::IsBodyGenderMale() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_isMale;
		}

		void CharacterCustomizationState::SetIsBodyGenderMale( Bool isMale )
		{
			RED_SCOPE_LOCK( m_stateLock );
			if ( !m_isFinalized && m_isMale != isMale )
			{
				m_isMale = isMale;
				GEngine->GetLocalizationManager()->SetBodyVariantGlobal( !isMale );
			}
		}

		Bool CharacterCustomizationState::IsBrainGenderMale() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_isMaleVO;
		}

		void CharacterCustomizationState::SetIsBrainGenderMale( Bool isMale )
		{
			RED_SCOPE_LOCK( m_stateLock );
			if ( !m_isFinalized && m_isMaleVO != isMale )
			{
				m_isMaleVO = isMale;
				GEngine->GetLocalizationManager()->SetVariantGlobal( !isMale );
			}
		}

		const game::data::TweakDBID& CharacterCustomizationState::GetLifePath() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_lifepath;
		}

		void CharacterCustomizationState::SetLifePath( const game::data::TweakDBID& lifepath )
		{
			RED_SCOPE_LOCK( m_stateLock );
			if ( !m_isFinalized || !m_lifepath.IsValid() )
			{
				m_lifepath = lifepath;
			}
		}

		Uint32 CharacterCustomizationState::GetAttributePointsAvailable() const
		{
			return m_attributePointsAvailable;
		}

		void CharacterCustomizationState::SetAttributePointsAvailable( Uint32 value )
		{
			m_attributePointsAvailable = value;
		}

		Uint32 CharacterCustomizationState::GetAttribute( game::data::StatType type ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			auto it = std::find_if( m_attributes.Begin(), m_attributes.End(), [ type ]( const CharacterCustomizationAttribute& attribute ) { return attribute.m_type == type; } );
			if ( it != m_attributes.End() )
			{
				return ( *it ).m_value;
			}
			return -1;
		}

		void CharacterCustomizationState::SetAttribute( game::data::StatType type, Uint32 value )
		{
			RED_SCOPE_LOCK( m_stateLock );

			auto it = std::find_if( m_attributes.Begin(), m_attributes.End(), [ type ]( const CharacterCustomizationAttribute& attribute ) { return attribute.m_type == type; } );
			if ( it != m_attributes.End() )
			{
				if ( value == -1 )
				{
					m_attributes.Remove( it );
				}
				else
				{
					( *it ).m_value = value;
				}
			}
			else if ( value != -1 )
			{
				m_attributes.PushBack( { type, value } );
			}
		}

		red::DynArray< game::ui::CharacterCustomizationAttribute > CharacterCustomizationState::GetAttributes() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_attributes;
		}

		void CharacterCustomizationState::ClearAttributes()
		{
			RED_SCOPE_LOCK( m_stateLock );
			m_attributes.Clear();
		}

		Bool CharacterCustomizationState::GetHeadCustomization( CName groupName, Bool isFPP, ent::CustomizationAppearances& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_headCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( std::make_pair( app.m_resource, app.m_definition ) );
					}
				}
				return true;
			}
			return false;
		}

		Bool CharacterCustomizationState::GetHeadCustomization( CName groupName, Bool isFPP, AppearanceChangeParameterKeys& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_headCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( { app.m_resource.GetPath(), app.m_definition } );
					}
				}
				return true;
			}
			return false;
		}

		Bool CharacterCustomizationState::GetBodyCustomization( CName groupName, Bool isFPP, ent::CustomizationAppearances& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_bodyCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( std::make_pair( app.m_resource, app.m_definition ) );
					}
				}
				return false;
			}
			return true;
		}

		Bool CharacterCustomizationState::GetBodyCustomization( CName groupName, Bool isFPP, AppearanceChangeParameterKeys& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_bodyCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( { app.m_resource.GetPath(), app.m_definition } );
					}
				}
				return false;
			}
			return true;
		}

		Bool CharacterCustomizationState::GetArmsCustomization( CName groupName, Bool isFPP, ent::CustomizationAppearances& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_armsCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( std::make_pair( app.m_resource, app.m_definition ) );
					}
				}
				return false;
			}
			return true;
		}

		Bool CharacterCustomizationState::GetArmsCustomization( CName groupName, Bool isFPP, AppearanceChangeParameterKeys& customization ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			if ( const auto* group = GetGroup( m_armsCustomizationGroups, groupName, isFPP ) )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

				customization.Reserve( customization.Size() + group->m_customization.Size() );
				for ( const auto& app : group->m_customization )
				{
					if ( !app.IsCensored( censorFlags ) )
					{
						customization.PushBack( { app.m_resource.GetPath(), app.m_definition } );
					}
				}
				return false;
			}
			return true;
		}

		Bool CharacterCustomizationState::MorphHead( CName groupName, Bool isFPP, const THandle< ent::Entity >& head, Bool enabled ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			if ( const auto* group = GetGroup( m_headCustomizationGroups, groupName, isFPP ) )
			{
				return ApplyMorphs( FindMorphManager( head ), group->m_morphs, enabled ? 1.f : 0.0f );
			}
			return false;
		}

		Bool CharacterCustomizationState::MorphBody( CName groupName, Bool isFPP, const THandle< ent::Entity >& body, Bool enabled /*= true */ ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			if ( const auto* group = GetGroup( m_bodyCustomizationGroups, groupName, isFPP ) )
			{
				return ApplyMorphs( FindMorphManager( body ), group->m_morphs, enabled ? 1.f : 0.0f );
			}
			return false;
		}

		Bool CharacterCustomizationState::MorphArms( CName groupName, Bool isFPP, const THandle< ent::Entity >& arms, Bool enabled ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			if ( const auto* group = GetGroup( m_armsCustomizationGroups, groupName, isFPP ) )
			{
				return ApplyMorphs( FindMorphManager( arms ), group->m_morphs, enabled ? 1.f : 0.0f );
			}
			return false;
		}

		Bool CharacterCustomizationState::IsNewGame() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_isNewGame;
		}

		void CharacterCustomizationState::SetIsNewGame( Bool isNewGame )
		{
			RED_SCOPE_LOCK( m_stateLock );
			m_isNewGame = isNewGame;
		}

		Bool CharacterCustomizationState::IsRiverRomanceable() const
		{
			return !IsBodyGenderMale() && !IsBrainGenderMale();
		}

		Bool CharacterCustomizationState::IsJudyRomanceable() const
		{
			return !IsBodyGenderMale() && !IsBrainGenderMale();
		}

		Bool CharacterCustomizationState::IsKerryRomanceable() const
		{
			return IsBodyGenderMale() && IsBrainGenderMale();
		}

		Bool CharacterCustomizationState::IsPanamRomanceable() const
		{
			return IsBodyGenderMale() && IsBrainGenderMale();
		}

		Bool CharacterCustomizationState::HasOption( CName groupName, Bool isFPP, CName optionName ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );

			const CustomizationGroup* group = GetGroup( m_bodyCustomizationGroups, groupName, isFPP );

			if ( !group )
			{
				group = GetGroup( m_headCustomizationGroups, groupName, isFPP );
			}

			if ( !group )
			{
				group = GetGroup( m_armsCustomizationGroups, groupName, isFPP );
			}

			if ( group )
			{
				auto appIt = std::find_if( group->m_customization.Begin(), group->m_customization.End(), [ optionName ]( const CustomizationAppearance& app ) { return app.m_name == optionName; } );
				if ( appIt != group->m_customization.End() )
				{
					return true;
				}

				auto morphIt = std::find_if( group->m_morphs.Begin(), group->m_morphs.End(), [ optionName ]( const CustomizationMorph& morph ) { return morph.m_regionName == optionName; } );
				if ( morphIt != group->m_morphs.End() )
				{
					return true;
				}
			}

			return false;
		}

		Bool CharacterCustomizationState::HasDifferentPerspectives( CName groupName, Bool isFPP ) const
		{
			const auto it = std::find_if( m_perspectiveInfo.Begin(), m_perspectiveInfo.End(), [ groupName ]( const PerspectiveInfo& info ) { return info.m_name == groupName; } );
			return it != m_perspectiveInfo.End();
		}

		Bool CharacterCustomizationState::HasTag( CName tag ) const
		{
			return m_tags.HasTag( tag );
		}

		const game::ui::CustomizationGroup* CharacterCustomizationState::GetGroup( const CustomizationGroups& groups, CName name, Bool isFPP ) const
		{
			{
				const auto it = std::find_if( m_perspectiveInfo.Begin(), m_perspectiveInfo.End(), [ name ]( const PerspectiveInfo& info ) { return info.m_name == name; } );
				if ( it != m_perspectiveInfo.End() )
				{
					const auto& info = ( *it );
					name = isFPP ? info.m_fpp : info.m_tpp;
				}
			}

			const CustomizationGroup* group = nullptr;

			{
				const auto it = std::find_if( groups.Begin(), groups.End(), [ name ]( const CustomizationGroup& group ) { return group.m_name == name; } );
				if ( it != groups.End() )
				{
					group = &( *it );
				}
			}

			return group;
		}

		Bool CharacterCustomizationState::ApplyMorphs( const THandle<ent::MorphTargetManagerComponent> morphManager, const CustomizationMorphs& morphs, Float value ) const
		{
			if ( morphManager )
			{
				const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();
				for ( const auto& morph : morphs )
				{
					if ( !morph.IsCensored( censorFlags ) )
					{
						morphManager->SetTargetWeightWithNormalize( morph.m_targetName, morph.m_regionName, value );
					}
				}
				return true;
			}

			return false;
		}

#ifdef CHARACTER_CUSTOMIZATION_PRESETS_ENABLED
		THandle< game::ui::CharacterCustomizationPreset > CharacterCustomizationState::Debug_CreatePreset(
			const CharacterCustomizationOptions& bodyOptions,
			const CharacterCustomizationOptions& headOptions,
			const CharacterCustomizationOptions& armsOptions,
			const THandle<const CharacterCustomizationInfoResource>& info )
		{
			Finalize( bodyOptions, headOptions, armsOptions, info );

			auto preset = CreateHandle<CharacterCustomizationPreset>();

			RED_SCOPE_LOCK( m_stateLock );
			{
				preset->m_isMale = m_isMale;

				preset->m_bodyGroups = m_bodyCustomizationGroups;
				preset->m_headGroups = m_headCustomizationGroups;
				preset->m_armsGroups = m_armsCustomizationGroups;

				preset->m_perspectiveInfo = m_perspectiveInfo;

				preset->m_tags = m_tags;

				preset->m_version = m_version;

				UnFinalize();
			}

			return preset;
		}

		void CharacterCustomizationState::Debug_InitFromPreset( const THandle< const CharacterCustomizationPreset >& preset )
		{
			RED_SCOPE_LOCK( m_stateLock );

			m_isMale = preset->m_isMale;

			CName playerVoiceoverType;
			m_isMaleVO = red::CommandLine::Get().GetFirstParam<CName>( "playerVoiceoverType", playerVoiceoverType ) ?
				playerVoiceoverType == PuppetHelper::MALE :
				m_isMale;

			m_bodyCustomizationGroups = preset->m_bodyGroups;
			m_headCustomizationGroups = preset->m_headGroups;
			m_armsCustomizationGroups = preset->m_armsGroups;

			m_perspectiveInfo = preset->m_perspectiveInfo;

			m_tags = preset->m_tags;

			m_version = preset->m_version;

			m_isFinalized = true;
			m_isNewGame = false;
			m_lifepath = TweakDBID::NONE();
		}

		void CharacterCustomizationState::UnFinalize()
		{
			m_bodyCustomizationGroups.Clear();
			m_headCustomizationGroups.Clear();
			m_armsCustomizationGroups.Clear();

			m_tags.Clear();

			m_version = 0;

			m_isFinalized = false;
		}
#endif

		//////////////////////////////////////////////////////////////////////////

		CharacterCustomizationSystem::MorphData::MorphData( CName regionName, CName targetName, Float weight, const THandle<ent::MorphTargetManagerComponent>& morphManager )
			: m_regionName( regionName ), m_targetName( targetName ), m_weight( weight ), m_morphManager( morphManager )
		{

		}

		void CharacterCustomizationSystem::MorphData::UpdateTargetWeightWithNormalize( float delta )
		{
			m_weight += delta;
			m_morphManager->SetTargetWeightWithNormalize( m_targetName, m_regionName, Clamp( m_weight, 0.f, 1.f ) );
		}

		//////////////////////////////////////////////////////////////////////////

		Bool CharacterCustomizationSystem::PendingOption::ShouldApply( const THandle< CharacterCustomizationOption >& option ) const
		{
			if ( option )
			{
				return m_option && m_option != option;
			}

			return m_option && m_time <= 0.f;
		}

		void CharacterCustomizationSystem::PendingOption::Set( const THandle< CharacterCustomizationOption >& option, Uint32 value )
		{
			m_option = option;
			m_value = value;
			m_time = 0.3f;
		}

		void CharacterCustomizationSystem::PendingOption::Update( float delta )
		{
			if ( m_option )
			{
				m_time -= delta;
			}
		}

		//////////////////////////////////////////////////////////////////////////

		CharacterCustomizationSystem::CharacterCustomizationSystem()
			: m_requestsCount( 0 )
		{
		}

		CharacterCustomizationSystem::~CharacterCustomizationSystem()
		{
		}

		void CharacterCustomizationSystem::RegisterComponent( const THandle< ICharacterCustomizationComponent >& comp )
		{
			RED_SCOPE_LOCK( m_componentsLock );
			ASSERT( !m_components.Exist( comp ) );
			m_components.PushBack( comp );
		}

		void CharacterCustomizationSystem::UnRegisterComponent( const THandle< ICharacterCustomizationComponent >& comp )
		{
			RED_SCOPE_LOCK( m_componentsLock );
			m_components.Remove( comp );
		}

		void CharacterCustomizationSystem::WorldAttached( world::RuntimeScene& scene )
		{
			TBaseClass::WorldAttached( scene );

			{
				RED_SCOPE_LOCK( m_brokenNoseStageLock );
				m_brokenNoseStage = CharacterCustomization_BrokenNoseStage::CCBN_Disabled;
			}

			{
				RED_SCOPE_LOCK( m_componentsLock );
				m_components.Clear();
			}

			{
				RED_SCOPE_LOCK( m_dataLock );
				m_entityAppearanceChanger = scene.GetSystem<world::RuntimeSystemEntityAppearanceChanger>();

				auto& sceneSystem = GetGameInstance()->GetSystem< game::ISceneSystem >().GetSceneSystem();
				m_sceneInstanceID.Init( &sceneSystem, [ weakSelf = WeakHandleFromThis<CharacterCustomizationSystem>() ]()
				{
					if ( const auto& self = weakSelf.ToHandle() )
					{
						self->EnableInput();
					}
				} );

				ResetRequestsCounter();
			}

#if !defined(RED_CONFIGURATION_FINAL)

			GetNativeFunRegistry().RegisterNativeFunction( "CharacterCustomizationSystem.SetBrokenNoseStage", "Set broken nose stage", &CharacterCustomizationSystem::Debug_SetBrokenNoseStage, this );

			GetNativeFunRegistry().RegisterNativeFunction( "CharacterCustomizationSystem.SaveAsUIPreset", "Saves current setup as an UI preset", &CharacterCustomizationSystem::Debug_SaveAsUIPreset, this );
			GetNativeFunRegistry().RegisterNativeFunction( "CharacterCustomizationSystem.LoadUIPreset", "Loads and applies UI preset", &CharacterCustomizationSystem::Debug_LoadUIPreset, this );

#if defined(CHARACTER_CUSTOMIZATION_PRESETS_ENABLED)

			GetNativeFunRegistry().RegisterNativeFunction( "CharacterCustomizationSystem.SaveAsPreset", "Saves current setup as a preset", &CharacterCustomizationSystem::Debug_SaveAsPreset, this );

#endif
#endif
		}

		void CharacterCustomizationSystem::WorldDetached( world::RuntimeScene& scene )
		{
#if !defined(RED_CONFIGURATION_FINAL)

			GetNativeFunRegistry().UnregisterNativeFunction( "CharacterCustomizationSystem.SetBrokenNoseStage" );

			GetNativeFunRegistry().UnregisterNativeFunction( "CharacterCustomizationSystem.SaveAsUIPreset" );
			GetNativeFunRegistry().UnregisterNativeFunction( "CharacterCustomizationSystem.LoadUIPreset" );

#if defined(CHARACTER_CUSTOMIZATION_PRESETS_ENABLED)

			GetNativeFunRegistry().UnregisterNativeFunction( "CharacterCustomizationSystem.SaveAsPreset" );

#endif
#endif

			{
				RED_SCOPE_LOCK( m_stateLock );
				m_state.Reset();
			}

			{
				RED_SCOPE_LOCK( m_dataLock );

				m_sceneInstanceID.DeInit();

				Internal_ResetCustomizationOptionsResource( false );
				ResetRequestsCounter();

				m_pending.Reset();

				m_entityAppearanceChanger = nullptr;
			}

			{
				RED_SCOPE_LOCK( m_componentsLock );
				m_components.Clear();
			}

			{
				RED_SCOPE_LOCK( m_brokenNoseStageLock );
				m_brokenNoseStage = CharacterCustomization_BrokenNoseStage::CCBN_Disabled;
			}

			TBaseClass::WorldDetached( scene );
		}

		void CharacterCustomizationSystem::RegisterUpdateFunctions( UpdatableSystemRegistrar & registrar )
		{
			if ( IsHeadlessMode() )
			{
				return;
			}

			const red::FixedSizeFunction<void( const UpdateContext&, job::Builder& )> func =
				[ this ]( const UpdateContext& updateContext, job::Builder& builder )
			{
				Tick( updateContext, builder );
			};

			registrar.Register( UpdateTickGroup::BlackboardCallbacks_SecondPass, *this, "Ink/CharacterCustomizationHUB", func );
		}

		game::IGameSystem::PreSaveResponse CharacterCustomizationSystem::OnPreGameSave( const job::RunContext& context, game::MetadataCollector& collector )
		{
			collector.Push< game::data::LifePath > ( RED_NAME_CONSTEXPR( "lifePath" ), FetchLifePath() );

			if ( m_state )
			{
				collector.Push< red::String >( RED_NAME_CONSTEXPR( "bodyGender" ), m_state->IsBodyGenderMale() ? "Male" : "Female" );
				collector.Push< red::String >( RED_NAME_CONSTEXPR( "brainGender" ), m_state->IsBrainGenderMale() ? "Male" : "Female" );
			}

			return PreSaveResponse::ReadyForSaving;
		}

		void CharacterCustomizationSystem::OnGameSave( save::IStream & saveStream )
		{
			SAVE_NODE( CharacetrCustomization_Appearances );

			THandle< CharacterCustomizationState > state;
			{
				RED_SCOPE_SHARED_LOCK( m_stateLock );
				if ( m_state && m_state->IsFinalized() )
				{
					state = Cast<CharacterCustomizationState>( m_state );
				}
			}

			Bool hasFinalizedState = ( state != nullptr );
			saveStream << hasFinalizedState;

			Uint32 brokenNoseStage;
			brokenNoseStage = ( Uint32 )m_brokenNoseStage;
			saveStream << brokenNoseStage;

			if ( hasFinalizedState )
			{
				state->Serialize( saveStream );
			}
		}

		void CharacterCustomizationSystem::OnGameLoad( const job::RunContext& context, Bool& result, save::IStream & loadStream )
		{
			Bool quickExit = false;
			auto version = loadStream.GetSaveVersion();
			if ( version < save::SaveVersion::CharacetrCustomization_Appearances )
			{
				if ( version >= save::SaveVersion::CharacterCustomizationSystem_Components_v2 )
				{
					LOAD_NODE_REQUIRED_FROM_VERSION( CharacterCustomizationSystem_Components_v2, save::SaveVersion::CharacterCustomizationSystem_Components_v2, result, quickExit );
					if ( quickExit )
					{
						return;
					}
				}

				DataBuffer customizedComponentsDataBuffer;
				customizedComponentsDataBuffer.Serialize( loadStream );
			}
			else
			{
				LOAD_NODE_REQUIRED_FROM_VERSION( CharacetrCustomization_Appearances, save::SaveVersion::CharacetrCustomization_Appearances, result, quickExit );
				if ( quickExit )
				{
					return;
				}

				Bool hasFinalizedState = false;
				loadStream << hasFinalizedState;

				if ( version >= save::SaveVersion::CharacterCustomization_BrokenNoseInfo )
				{
					Uint32 brokenNoseStage;
					loadStream << brokenNoseStage;
					m_brokenNoseStage = ( CharacterCustomization_BrokenNoseStage )brokenNoseStage;
				}

				if ( hasFinalizedState )
				{
					const THandle< CharacterCustomizationState > state = CreateHandle< CharacterCustomizationState>();
					state->Serialize( loadStream );

					if ( version >= save::SaveVersion::CharacterCustomization_CensorshipUpdate )
					{
						CheckVersionsCompatibility( state );
					}
					else
					{
						InitializeFromFinalizedState( state );
					}
				}
			}

			if ( !loadStream.IsOK() )
			{
				result = false;
			}
		}

		game::SaveLock CharacterCustomizationSystem::IsSavingLocked( const bool isForced ) const
		{
			// HACK: no saving in main menu world at all
			static const auto mainMenuWorld = RED_CONST_RESOURCEPATH( "base\\worlds\\04_main_menu\\04_main_menu.world" );

			const auto state = GetState();
			if ( !state || state->IsFinalized() )
			{
				const auto worldID = GetGameInstance()->GetRuntimeInfo().GetWorldID().GetWorldPath();
				if ( GetGameInstance() && worldID.IsValid() && worldID != mainMenuWorld )
					return { game::SaveLock::Reason::Nothing };
			}

			return { game::SaveLock::Reason::MainMenu, "You haven't started real game yet!" };
		}

		// [missionflow] the state built by cpweb_missionflow_BuildNomadState (non-const handle for CheckVersionsCompatibility)
		static THandle< CharacterCustomizationState > s_missionflowState;

		Bool CharacterCustomizationSystem::InitializeFromFinalizedState(const THandle< const ICharacterCustomizationState >& state)
		{
			if ( state && !t_missionflowInCompat && (const void*)state.Get() == g_missionflowStatePtr.load() && s_missionflowState )
			{
				// [missionflow] same handling as InitializeFallbackFinalizedState's preset: version upgrade + CC-default nails.
				const auto own = s_missionflowState;
				cpweb_nailfix_shared::g_fallbackCustomization.store( 1 );
				std::fprintf( stderr, "CP2077 missionflow: session CC state = default Nomad preset (version %u, cco version %u), male=%d lifepath=%s newGame=%d attributes=%u points=%u\n",
					unsigned( own->GetVersion() ), unsigned( ( own->IsBodyGenderMale() ? s_maleCCO.Get() : s_femaleCCO.Get() ) ? ( own->IsBodyGenderMale() ? s_maleCCO.Get() : s_femaleCCO.Get() )->GetVersion() : 0 ),
					int( own->IsBodyGenderMale() ), own->GetLifePath() == game::data::RecordID( "LifePaths.Nomad" ) ? "LifePaths.Nomad" : "OTHER",
					int( own->IsNewGame() ), unsigned( own->GetAttributes().Size() ), unsigned( own->GetAttributePointsAvailable() ) );
				t_missionflowInCompat = true;
				t_nailfixInFallbackPreset = true;
				CheckVersionsCompatibility( own );
				t_nailfixInFallbackPreset = false;
				t_missionflowInCompat = false;
				return true;
			}
			if ( !t_nailfixInFallbackPreset && state && state->IsFinalized() )
			{
				cpweb_nailfix_shared::g_fallbackCustomization.store( 0 ); // [nailfix] real CC / saved state
			}
			Bool init = false;
			if ( state->IsFinalized() )
			{
				RED_SCOPE_LOCK( m_stateLock );
				init = !m_state || !m_state->IsFinalized();
				if ( init )
				{
					m_state = state;

					if ( m_state->IsNewGame() )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "cp77_new_game" ), 1 );
					}

					if ( m_state->IsRiverRomanceable() )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "river_romanceable" ), 1 );
					}

					if ( m_state->IsJudyRomanceable() )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "judy_romanceable" ), 1 );
					}

					if ( m_state->IsKerryRomanceable() )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "kerry_romanceable" ), 1 );
					}

					if ( m_state->IsPanamRomanceable() )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "panam_romanceable" ), 1 );
					}

					if ( m_state->HasTag( RED_NAME_CONSTEXPR( "big_dick" ) ) )
					{
						quest::SetFact< Uint32 >( m_gameInstance->GetSystem<quest::IQuestsSystem>().GetFactsDB(), quest::USER_TABLE_ID(),
							RED_NAME_CONSTEXPR( "big_dick_is_back_in_city" ), 1 );
					}
				}
			}
			return init;
		}

#ifdef CHARACTER_CUSTOMIZATION_PRESETS_ENABLED

		res::GatheredResourceReference g_malePresetResource( RED_CONST_RESOURCEPATH( "test\\player_presets\\default_male.ccstate" ), GRF_Default );
		res::GatheredResourceReference g_femalePresetResource( RED_CONST_RESOURCEPATH( "test\\player_presets\\default_female.ccstate"  ), GRF_Default );

		Bool CharacterCustomizationSystem::InitializeFallbackFinalizedState( CName gender )
		{
			// HACK: check based on main_menu world
			static const auto mainMenuWorld = RED_CONST_RESOURCEPATH( "base\\worlds\\04_main_menu\\04_main_menu.world" );
			const auto& worldID = GetGameInstance()->GetRuntimeInfo().GetWorldID().GetWorldPath();
			if ( worldID.IsValid() && worldID != mainMenuWorld )
			{
				Bool isMale = game::PuppetHelper::NameToEnum( gender ) == game::data::Gender::Male;
				cpweb_nailfix_shared::g_fallbackCustomization.store( 1 ); // [nailfix] no character creation for this session
				std::fprintf( stderr, "CP2077 nailfix: fallback customization preset (no character creation), male=%d\n", int( isMale ) );
				auto token = GResourceLoader->IssueLoadingRequest( isMale ? g_malePresetResource.GetPath() : g_femalePresetResource.GetPath() );

				res::LoadedCallback onLoaded{ game::PoolGMPL_CharacterCustomization(),
					[ this ]( THandle< CResource > res )
				{
					if ( const auto& preset = Cast<CharacterCustomizationPreset>( res ) )
					{
						const auto& state = CreateHandle< CharacterCustomizationState >();
						state->Debug_InitFromPreset( preset );

						t_nailfixInFallbackPreset = true; // [nailfix]
						CheckVersionsCompatibility( state );
						t_nailfixInFallbackPreset = false;
					}
				} };

				token->OnLoaded( std::move( onLoaded ) );
				return true;
			}

			return false;
		}

		// [missionflow] default Nomad character for ?start=nomad: the developer fallback preset (same body/head as the
		// sessions without character creation), plus everything the CC menus add when each screen is only confirmed:
		// lifepath Nomad (newGameSpawnTag / NomadStarting build / q000 branch), new game (cp77_new_game fact),
		// attribute preset UIAttributePresets.Preset1 + 7 unspent points (characterCreationGenderBackstoryMenu.script).
		void CpwebMissionflowBuildNomadState( void ( *done )( const THandle< const IScriptable >& ) )
		{
			std::fprintf( stderr, "CP2077 missionflow: building default Nomad CC state from %s\n", "test\\player_presets\\default_male.ccstate" );
			auto token = GResourceLoader->IssueLoadingRequest( g_malePresetResource.GetPath() );
			res::LoadedCallback onLoaded{ game::PoolGMPL_CharacterCustomization(),
				[ done ]( THandle< CResource > res )
			{
				const auto& preset = Cast<CharacterCustomizationPreset>( res );
				if ( !preset )
				{
					std::fprintf( stderr, "CP2077 missionflow: ERROR default_male.ccstate did not load\n" );
					done( THandle< const IScriptable >() );
					return;
				}
				const auto state = CreateHandle< CharacterCustomizationState >();
				state->Debug_InitFromPreset( preset );
				state->SetIsNewGame( true );
				state->SetLifePath( game::data::RecordID( "LifePaths.Nomad" ) );
				state->SetAttributePointsAvailable( 7 );
				Uint32 attributeCount = 0;
				if ( const auto attrPreset = game::data::GetRecordHandle< game::data::UICharacterCreationAttributesPreset_Record >( game::data::RecordID( "UIAttributePresets.Preset1" ) ) )
				{
					red::DynArray< WeakHandle< game::data::UICharacterCreationAttribute_Record > > records{ game::PoolGMPL_CharacterCustomization() };
					attrPreset->attributesGet( records );
					for ( const auto& weak : records )
					{
						const auto record = weak.ToHandle();
						if ( !record ) continue;
						const THandle< game::data::Stat_Record > stat = record->attributeGet().ToHandle();
						if ( !stat ) continue;
						state->SetAttribute( stat->statType, Uint32( record->value() ) );
						std::fprintf( stderr, "CP2077 missionflow: attribute statType=%u value=%u\n", unsigned( stat->statType ), unsigned( record->value() ) );
						++attributeCount;
					}
				}
				else
				{
					std::fprintf( stderr, "CP2077 missionflow: WARNING UIAttributePresets.Preset1 not in TweakDB\n" );
				}
				s_missionflowState = state;
				g_missionflowStatePtr.store( (const void*)state.Get() );
				cpweb_nailfix_shared::g_fallbackCustomization.store( 1 ); // CC-default (beige) nails, as for the fallback preset
				std::fprintf( stderr, "CP2077 missionflow: Nomad CC state ready male=%d finalized=%d newGame=%d lifepathNomad=%d attributes=%u points=7 version=%u\n",
					int( state->IsBodyGenderMale() ), int( state->IsFinalized() ), int( state->IsNewGame() ),
					int( state->GetLifePath() == game::data::RecordID( "LifePaths.Nomad" ) ), attributeCount, unsigned( state->GetVersion() ) );
				done( THandle< const IScriptable >( state ) );
			} };
			token->OnLoaded( std::move( onLoaded ) );
		}
#endif

		Bool CharacterCustomizationSystem::InitializeState()
		{
			RED_SCOPE_LOCK( m_stateLock );
			Bool init = !m_state;
			if ( init )
			{
				m_state = CreateHandle< CharacterCustomizationState >();
			}
			return init;
		}

		Bool CharacterCustomizationSystem::FinalizeState()
		{
			if ( m_characterCustomizationInfoResource )
			{
				THandle< CharacterCustomizationState > state;
				{
					RED_SCOPE_SHARED_LOCK( m_stateLock );
					if ( m_state && !m_state->IsFinalized() )
					{
						state = Cast<CharacterCustomizationState>( m_state );
					}
				}

				if ( state )
				{
					state->Finalize(
						GetAllBodyOptions(),
						GetAllHeadOptions(),
						GetAllArmsOptions(),
						m_characterCustomizationInfoResource
					);

					FinalizeMorphManagers();

					return true;
				}
			}
			return false;
		}

		void CharacterCustomizationSystem::FinalizeMorphManagers()
		{
			if ( const auto& manager = m_bodyMorphManager.ToHandle() )
			{
				manager->UnloadMorphData();
			}
		}

		Bool CharacterCustomizationSystem::ClearState()
		{
			RED_SCOPE_LOCK( m_stateLock );
			bool clear = m_state && !m_state->IsFinalized();
			if ( clear )
			{
				m_state.Reset();
			}
			return clear;
		}

		const THandle< game::ui::ICharacterCustomizationState > CharacterCustomizationSystem::GetState() const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			return m_state;
		}

		const THandle< game::ui::ICharacterCustomizationState > CharacterCustomizationSystem::GetFinalizedState( Bool isMale ) const
		{
			RED_SCOPE_SHARED_LOCK( m_stateLock );
			if ( m_state && m_state->IsFinalized() && m_state->IsBodyGenderMale() == isMale )
			{
				return m_state;
			}
			return nullptr;
		}

		void CharacterCustomizationSystem::SetCustomizationOptionsResource( const THandle< game::Puppet >& puppet, Bool isMale, const UiCallback& callback )
		{
			if ( const auto& state = GetState() )
			{
				RED_SCOPE_LOCK( m_dataLock );

				if ( !m_characterCustomizationInfoResource )
				{
					m_callback = callback;
					SetUIState( CharacterCustomizationUiState::ACR_Init );

					m_characterCustomizationInfoResource = isMale ? s_maleCCO.Get() : s_femaleCCO.Get();

					InitHead( puppet );
					InitBody( puppet );
					InitArms( puppet );

					InitOptions( m_characterCustomizationInfoResource->GetHeadCustomizationOptions(), CharacterCustomizationPart::Head );
					InitOptions( m_characterCustomizationInfoResource->GetBodyCustomizationOptions(), CharacterCustomizationPart::Body );
					InitOptions( m_characterCustomizationInfoResource->GetArmsCustomizationOptions(), CharacterCustomizationPart::Arms );

					AddPostponedEvent( CreateHandle<CharacterCustomizationSystem_OnInitializeOptionsListEvent>() );
				}
			}
		}

		void CharacterCustomizationSystem::ResetCustomizationOptionsResource()
		{
			RED_SCOPE_LOCK( m_dataLock );
			Internal_ResetCustomizationOptionsResource( true );
		}

		void CharacterCustomizationSystem::Internal_ResetCustomizationOptionsResource( Bool resetAppearances )
		{
			m_defaultPreset.Reset();
			m_characterCustomizationInfoResource.Reset();

			m_blendInData.Clear();
			m_blendOutData.Clear();

			const auto& puppet = Cast< game::Puppet >(m_bodyEntity.ToHandle());
			Bool isMale = puppet ? puppet->GetResolvedGenderName() == PuppetHelper::MALE : true;

			ApplyEquipActions(m_equippedItems);
			
			if (resetAppearances)
			{
				ResetAppearances( CharacterCustomizationPart::Head, true );
				ResetAppearances( CharacterCustomizationPart::Body, true );
				ResetAppearances( CharacterCustomizationPart::Arms, true );
			}

			ResetMorphs( CharacterCustomizationPart::Head );
			ResetMorphs( CharacterCustomizationPart::Body );
			ResetMorphs( CharacterCustomizationPart::Arms );

			m_bodyMorphManager.Reset();
			m_bodyEntity.Reset();

			m_headOptions.Clear();
			m_bodyOptions.Clear();
			m_armsOptions.Clear();

			m_links.Clear();
		}

		Bool CharacterCustomizationSystem::GetBodyOptions( CName groupName, CharacterCustomizationOptions& options ) const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );

			Bool res = false;
			OptionsGroup group;

			if ( !groupName.Empty() )
			{
				if ( m_characterCustomizationInfoResource && m_characterCustomizationInfoResource->GetBodyGroup( groupName, group ) )
				{
					GetGroupedOptions( group, m_bodyOptions, options );
					res = true;
				}
			}
			else
			{
				GetGroupedOptions( m_bodyGroup, m_bodyOptions, options );
				res = true;
			}

			return res;
		}

		Bool CharacterCustomizationSystem::GetHeadOptions( CName groupName, CharacterCustomizationOptions& options ) const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );

			Bool res = false;
			OptionsGroup group;

			if ( !groupName.Empty() )
			{
				if ( m_characterCustomizationInfoResource && m_characterCustomizationInfoResource->GetHeadGroup( groupName, group ) )
				{
					GetGroupedOptions( group, m_headOptions, options );
					res = true;
				}
			}
			else
			{
				GetGroupedOptions( m_headGroup, m_headOptions, options );
				res = true;
			}

			return res;
		}


		Bool CharacterCustomizationSystem::GetArmsOptions( CName groupName, CharacterCustomizationOptions& options ) const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );

			Bool res = false;
			OptionsGroup group;

			if ( !groupName.Empty() )
			{
				if ( m_characterCustomizationInfoResource && m_characterCustomizationInfoResource->GetArmsGroup( groupName, group ) )
				{
					GetGroupedOptions( group, m_armsOptions, options );
					res = true;
				}
			}
			else
			{
				GetGroupedOptions( m_armsGroup, m_armsOptions, options );
				res = true;
			}

			return res;
		}

		void CharacterCustomizationSystem::RandomizeOptions()
		{
			RED_SCOPE_LOCK( m_dataLock );

			m_randomizationStage = RandomizationStage::SwitcherOptions;

			RandomizeSwitcherOptions( m_headOptions );
			RandomizeSwitcherOptions( m_bodyOptions );
			RandomizeSwitcherOptions( m_armsOptions );

			RandomizeNoneSwitcherOptions();
		}

		void CharacterCustomizationSystem::RandomizeSwitcherOptions( const CharacterCustomizationOptions& options )
		{
			red::HashSet< red::CNameHash > postponedOptions{ game::PoolGMPL_CharacterCustomization() };
			Uint32 count;

			const auto& excludes = m_characterCustomizationInfoResource->GetExcludedFromRandomizeOptions();

			for ( const auto& option : options )
			{
				const auto& info = option->GetInfo();

				if ( const auto& switcherInfo = option->GetSwitcherInfo() )
				{
					if ( excludes.Exist( info->m_name ) )
					{
						continue;
					}

					if ( postponedOptions.Exist( info->m_name.GetHash() ) )
					{
						m_optionsToRandomize.PushBack( option );
					}
					else if ( option->IsActive() && !option->IsCensored() && !info->m_hidden )
					{
						count = switcherInfo->m_options.Size();
						if ( count > 1 )
						{
							const auto& index = math::DefaultRandom().Get<Uint32>( 0, count );
							Apply( option, index, SetWeightType::Instant );
							QueueOptionUpdateEvent( option );

							for ( const auto& switcherOption : switcherInfo->m_options )
							{
								postponedOptions.Insert( switcherOption.m_name.GetHash() );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::RandomizeNoneSwitcherOptions()
		{
			if ( m_requestsCount.GetValue() == 0 && m_randomizationStage == RandomizationStage::SwitcherOptions )
			{
				RandomizeNoneSwitcherOptions( m_headOptions );
				m_randomizationStage = RandomizationStage::NoneSwitcherOptions_Head;
			}

			if ( m_requestsCount.GetValue() == 0 && m_randomizationStage == RandomizationStage::NoneSwitcherOptions_Head )
			{
				RandomizeNoneSwitcherOptions( m_bodyOptions );
				m_randomizationStage = RandomizationStage::NoneSwitcherOptions_Body;
			}

			if ( m_requestsCount.GetValue() == 0 && m_randomizationStage == RandomizationStage::NoneSwitcherOptions_Body )
			{
				RandomizeNoneSwitcherOptions( m_armsOptions );
				m_randomizationStage = RandomizationStage::NoneSwitcherOptions_Arms;
			}

			if ( m_requestsCount.GetValue() == 0 && m_randomizationStage == RandomizationStage::NoneSwitcherOptions_Arms )
			{
				m_randomizationStage = RandomizationStage::RandomizationDisabled;
				AddPostponedEvent( CreateHandle<CharacterCustomizationSystem_OnRandomizeCompleteEvent >() );
			}
		}

		void CharacterCustomizationSystem::RandomizeNoneSwitcherOptions( const CharacterCustomizationOptions& options )
		{
			const auto& excludes = m_characterCustomizationInfoResource->GetExcludedFromRandomizeOptions();

			Uint32 count;
			for ( const auto& option : options )
			{
				count = 0;
				const auto& info = option->GetInfo();

				if ( !option->IsActive() || option->IsCensored() || info->m_hidden || excludes.Exist( info->m_name ) )
				{
					continue;
				}

				if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
				{
					count = appearanceInfo->m_definitions.Size();
				}
				else if ( const auto& morphInfo = option->GetMorphInfo() )
				{
					count = morphInfo->m_morphNames.Size();
				}

				if ( count > 1 )
				{
					Apply( option, math::DefaultRandom().Get<Uint32>( 0, count ), SetWeightType::Postponed );
					QueueOptionUpdateEvent( option );
				}
			}
		}

		void CharacterCustomizationSystem::GetGroupedOptions( const OptionsGroup& group, const CharacterCustomizationOptions& inOptions, CharacterCustomizationOptions& outOptions ) const
		{
			for ( const auto& option : inOptions )
			{
				if ( group.m_options.Exist( option->GetInfo()->m_name ) )
				{
					outOptions.PushBack( option );
				}
			}
		}

		Bool CharacterCustomizationSystem::ApplyUIPreset( CName presetName )
		{
			Bool found = false;
			CharacterCustomizationUiPresetInfo info;
			{
				RED_SCOPE_SHARED_LOCK( m_dataLock );
				found = m_characterCustomizationInfoResource && m_characterCustomizationInfoResource->GetUIPreset( presetName, info );
			}

			if ( found )
			{
				auto token = GResourceLoader->IssueLoadingRequest( info.m_resource.GetPath() );

				res::LoadedCallback onLoaded{ game::PoolGMPL_CharacterCustomization(),
					[ this ]( THandle< CResource > res )
				{
					if ( const auto& preset = Cast<CharacterCustomizationUiPreset>( res ) )
					{
						SetDefaultUIPreset( preset );
					}
				} };

				token->OnLoaded( std::move( onLoaded ) );

				return !token->HasFailed();
			}

			return false;
		}


		void CharacterCustomizationSystem::SetDefaultUIPreset( const THandle< CharacterCustomizationUiPreset >& preset )
		{
			RED_ASSERT( preset );

			RED_SCOPE_LOCK( m_dataLock );

			m_defaultPreset = preset;

			GotoNextApplyPresetStage( ApplyPresetStage::Reset );
			if ( m_requestsCount.GetValue() == 0 ) { m_readyToNextPass.SetValue( true ); }
		}

		void CharacterCustomizationSystem::ApplyDefaultUIPreset()
		{
			const auto& state = GetState();
			if ( m_defaultPreset && state && !state->IsFinalized() )
			{
				switch ( m_applyPresetStage )
				{
					case ApplyPresetStage::Reset:
					{
						if ( IsTransgenderAllowed() )
						{
							state->SetIsBrainGenderMale( m_defaultPreset->IsBrainGenderMale() );
						}
						else
						{
							state->SetIsBrainGenderMale( state->IsBodyGenderMale() );
						}

						m_links.Clear();

						ApplyEquipActions( m_equippedItems );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetHeadApps );
						break;
					}

					case ApplyPresetStage::ResetHeadApps:
					{
						ResetAppearances( CharacterCustomizationPart::Head, false );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetBodyApps );
						break;
					}

					case ApplyPresetStage::ResetBodyApps:
					{
						ResetAppearances( CharacterCustomizationPart::Body, false );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetArmsApps );
						break;
					}

					case ApplyPresetStage::ResetArmsApps:
					{
						ResetAppearances( CharacterCustomizationPart::Arms, false );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetHeadMorphs );
						break;
					}

					case ApplyPresetStage::ResetHeadMorphs:
					{
						ResetMorphs( CharacterCustomizationPart::Head );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetBodyMorphs );
						break;
					}

					case ApplyPresetStage::ResetBodyMorphs:
					{
						ResetMorphs( CharacterCustomizationPart::Body );

						GotoNextApplyPresetStage( ApplyPresetStage::ResetArmsMorphs );
						break;
					}

					case ApplyPresetStage::ResetArmsMorphs:
					{
						ResetMorphs( CharacterCustomizationPart::Arms );

						m_optionsIterator = m_headOptions.Begin();
						GotoNextApplyPresetStage( ApplyPresetStage::InitHeadApps );
						break;
					}

					case ApplyPresetStage::InitHeadApps:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_headOptions, m_optionsIterator, CharacterCustomizationPart::Head, InitOptionsType::Appearance | InitOptionsType::Switcher );

						if ( m_optionsIterator == m_headOptions.End() )
						{
							m_optionsIterator = m_bodyOptions.Begin();
							GotoNextApplyPresetStage( ApplyPresetStage::InitBodyApps );
						}
						break;
					}

					case ApplyPresetStage::InitBodyApps:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_bodyOptions, m_optionsIterator, CharacterCustomizationPart::Body, InitOptionsType::Appearance | InitOptionsType::Switcher );
						
						if ( m_optionsIterator == m_bodyOptions.End() )
						{
							m_optionsIterator = m_armsOptions.Begin();
							GotoNextApplyPresetStage( ApplyPresetStage::InitArmsApps );
						}
						break;
					}

					case ApplyPresetStage::InitArmsApps:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_armsOptions, m_optionsIterator, CharacterCustomizationPart::Arms, InitOptionsType::Appearance | InitOptionsType::Switcher );

						if ( m_optionsIterator == m_armsOptions.End() )
						{
							m_optionsIterator = m_headOptions.Begin();
							GotoNextApplyPresetStage( ApplyPresetStage::InitHeadMorphs );
						}
						break;
					}

					case ApplyPresetStage::InitHeadMorphs:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_headOptions, m_optionsIterator, CharacterCustomizationPart::Head, InitOptionsType::Morph );

						if ( m_optionsIterator == m_headOptions.End() )
						{
							m_optionsIterator = m_bodyOptions.Begin();
							GotoNextApplyPresetStage( ApplyPresetStage::InitBodyMorphs );
						}
						break;
					}

					case ApplyPresetStage::InitBodyMorphs:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_bodyOptions, m_optionsIterator, CharacterCustomizationPart::Body, InitOptionsType::Morph );

						if ( m_optionsIterator == m_bodyOptions.End() )
						{
							m_optionsIterator = m_armsOptions.Begin();
							GotoNextApplyPresetStage( ApplyPresetStage::InitArmsMorphs );
						}
						break;
					}

					case ApplyPresetStage::InitArmsMorphs:
					{
						ReInitOptions( m_defaultPreset->GetValues(), m_armsOptions, m_optionsIterator, CharacterCustomizationPart::Arms, InitOptionsType::Morph );
						AddPostponedEvent( CreateHandle<CharacterCustomizationSystem_OnPresetAppliedEvent>() );

						GotoNextApplyPresetStage( ApplyPresetStage::ApplyPresetDisabled );
						break;
					}
				}
			}
			else
			{
				GotoNextApplyPresetStage( ApplyPresetStage::ApplyPresetDisabled );
			}

			if ( m_requestsCount.GetValue() == 0 ) { m_readyToNextPass.SetValue( true ); }
		}

		void CharacterCustomizationSystem::GotoNextApplyPresetStage( const ApplyPresetStage stage )
		{
			m_applyPresetStage = stage;

			if ( m_applyPresetStage == ApplyPresetStage::ApplyPresetDisabled )
			{
				m_defaultPreset.Reset();
				SetUIState( CharacterCustomizationUiState::ACR_None );
				
			}
			else if ( m_applyPresetStage == ApplyPresetStage::Reset )
			{
				SetUIState( CharacterCustomizationUiState::ACR_Preset );
			}
		}

		void CharacterCustomizationSystem::SetUIState( CharacterCustomizationUiState state )
		{
			m_UIState = state;
			if ( m_callback )
			{
				m_callback( m_UIState );
			}
		}

		const game::ui::CharacterCustomizationOptions& CharacterCustomizationSystem::GetAllBodyOptions() const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );
			return m_bodyOptions;
		}

		const game::ui::CharacterCustomizationOptions& CharacterCustomizationSystem::GetAllHeadOptions() const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );
			return m_headOptions;
		}

		const game::ui::CharacterCustomizationOptions& CharacterCustomizationSystem::GetAllArmsOptions() const
		{
			RED_SCOPE_SHARED_LOCK( m_dataLock );
			return m_armsOptions;
		}

		void CharacterCustomizationSystem::ApplyChangeToOption( const THandle< CharacterCustomizationOption >& option, Uint32 currIndex )
		{
			RED_SCOPE_LOCK( m_dataLock );

			if ( m_pending.ShouldApply( option ) )
			{
				Apply( m_pending.GetOption(), m_pending.GetValue(), SetWeightType::Blending );
			}

			m_pending.Set( option, currIndex );
		}

		res::GatheredResourceReference g_fallbackHeadCustomizationFpp( RED_CONST_RESOURCEPATH( "base\\characters\\common\\player_base_bodies\\appearances\\n0_000_base_fpp__neck.app" ), GRF_Default );
		res::GatheredResourceReference g_fallbackHeadCustomizationTpp( RED_CONST_RESOURCEPATH( "base\\characters\\head\\player_base_heads\\appearances\\head\\h0_000__basehead.app" ), GRF_Default );

		const ent::CustomizationAppearances CharacterCustomizationSystem::GetFallbackHeadCustomization( Bool isMale, Bool isFPP, const game::data::TweakDBID lifepath ) const
		{
			res::ResourcePath path;
			CName definition;

			path = isFPP ? g_fallbackHeadCustomizationFpp.GetPath() : g_fallbackHeadCustomizationTpp.GetPath();

			if ( lifepath.Empty() )
			{
				definition = isMale ? RED_NAME_CONSTEXPR( "pma_default" ) : RED_NAME_CONSTEXPR( "pwa_default" );
			}
			else
			{
				if ( lifepath == game::data::RecordID( "LifePaths.Nomad" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_nomad" ) : RED_NAME_CONSTEXPR( "pwa_default_nomad" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.StreetKid" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_street" ) : RED_NAME_CONSTEXPR( "pwa_default_street" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.Corporate" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_corpo" ) : RED_NAME_CONSTEXPR( "pwa_default_corpo" );
				}
			}

			return ent::CustomizationAppearances{ { std::make_pair( path, definition) }, game::PoolGMPL_CharacterCustomization() };
		}

		res::GatheredResourceReference g_fallbackBodyCustomization( RED_CONST_RESOURCEPATH( "base\\characters\\common\\player_base_bodies\\appearances\\t0_000_base__full.app" ), GRF_Default );

		const ent::CustomizationAppearances CharacterCustomizationSystem::GetFallbackBodyCustomization( Bool isMale, Bool isFPP, const game::data::TweakDBID lifepath ) const
		{
			res::ResourcePath path = g_fallbackBodyCustomization.GetPath();
			CName definition;

			if ( lifepath.Empty() )
			{
				definition = isMale ? RED_NAME_CONSTEXPR( "pma_default" ) : RED_NAME_CONSTEXPR( "pwa_default" );
			}
			else
			{
				if ( lifepath == game::data::RecordID( "LifePaths.Nomad" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_nomad" ) : RED_NAME_CONSTEXPR( "pwa_default_nomad" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.StreetKid" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_street" ) : RED_NAME_CONSTEXPR( "pwa_default_street" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.Corporate" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_corpo" ) : RED_NAME_CONSTEXPR( "pwa_default_corpo" );
				}
			}

			return ent::CustomizationAppearances{ { std::make_pair( path, definition ) }, game::PoolGMPL_CharacterCustomization() };
		}

		res::GatheredResourceReference g_fallbackArmsCustomization( RED_CONST_RESOURCEPATH( "base\\characters\\common\\player_base_bodies\\appearances\\a0_000_base__full.app" ), GRF_Default );

		const ent::CustomizationAppearances CharacterCustomizationSystem::GetFallbackArmsCustomization( Bool isMale, Bool isFPP, const game::data::TweakDBID lifepath ) const
		{
			res::ResourcePath path = g_fallbackArmsCustomization.GetPath();
			CName definition;

			if ( lifepath.Empty() )
			{
				definition = isMale ? RED_NAME_CONSTEXPR( "pma_default" ) : RED_NAME_CONSTEXPR( "pwa_default" );
			}
			else
			{
				if ( lifepath == game::data::RecordID( "LifePaths.Nomad" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_nomad" ) : RED_NAME_CONSTEXPR( "pwa_default_nomad" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.StreetKid" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_street" ) : RED_NAME_CONSTEXPR( "pwa_default_street" );
				}
				else if ( lifepath == game::data::RecordID( "LifePaths.Corporate" ) )
				{
					definition = isMale ? RED_NAME_CONSTEXPR( "pma_default_corpo" ) : RED_NAME_CONSTEXPR( "pwa_default_corpo" );
				}
			}

			return ent::CustomizationAppearances{ { std::make_pair( path, definition ) }, game::PoolGMPL_CharacterCustomization() };
		}

		res::GatheredResourceReference g_vsetSceneResource( RED_CONST_RESOURCEPATH( "base\\quest\\primary_characters\\vsets\\vset_v.scene" ), GRF_Default );

		void CharacterCustomizationSystem::TriggerVoiceToneSample()
		{
			RED_SCOPE_LOCK( m_dataLock );

			const auto& puppet = m_bodyEntity.ToHandle();
			const auto& state = GetState();
			if ( puppet && state && !state->IsFinalized() )
			{
				scn::ISceneSystem& sceneSystem = GetGameInstance()->GetSystem< game::ISceneSystem >().GetSceneSystem();

				CName sceneLocation = RED_NAME_CONSTEXPR( "around_player" );
				scn::SceneInstanceParams sceneInstanceParams;
				sceneInstanceParams.m_locParams = scn::LocParams( scn::LocParams::LocstoreStrategy::smart, scn::LocParams::LanguageSelection::automatic );
				sceneInstanceParams.m_origin = scn::WorldMarker( sceneLocation );
				sceneInstanceParams.m_stimuli.InsertStimulus( { RED_NAME_CONSTEXPR( "character_creation" ) } );
				sceneInstanceParams.m_savingStrategy = scn::SavingStrategy::dontSave;
				sceneInstanceParams.m_contextActors.PushBack( scn::ContextActor( scn::ContextualActorName::VoicesetHolder, puppet->GetEntityID() ) );

				m_sceneInstanceID.TerminateScene();

				//clone from VoicesetComponent::GenerateNextSceneInstanceOwnerId()
				const Uint64 incrementalId = m_incrementalId++;
				const Uint64 ownerId = ( incrementalId << 32ull ) + static_cast< Uint32 >( GetID().CalcHash() );

				scn::SceneId sceneID( g_vsetSceneResource.GetPath() );

				scn::SceneInstanceDebug sciDebug;
#ifndef RED_CONFIGURATION_FINAL
				sciDebug.m_debugData = "CharacterCustomizationSystem";
#endif
				scn::SceneOperationStatus sciopStatus;
				const auto& sceneInstanceID = sceneSystem.CreateSceneInstance( sceneID, scn::SceneInstanceOwnerId( ownerId ), sceneInstanceParams, scn::SceneInstancePeripherals::NoPeripherals(), std::move( sciDebug ), sciopStatus );

				if ( sceneInstanceID != scn::SceneInstanceId::invalid )
				{
					m_sceneInstanceID.SetSceneInstanceId( sceneInstanceID );

					sceneSystem.QueueRequest( sceneInstanceID, scn::Control::Request::Prepare() );
					sceneSystem.QueueRequest( sceneInstanceID, scn::Control::Request::Play() );

					DisableInput();
				}
			}
		}

		game::ui::CharacterCustomization_BrokenNoseStage CharacterCustomizationSystem::GetBrokenNoseStage() const
		{
			RED_SCOPE_SHARED_LOCK( m_brokenNoseStageLock );
			return m_brokenNoseStage;
		}

		void CharacterCustomizationSystem::SetBrokenNoseStage( CharacterCustomization_BrokenNoseStage stage )
		{
			RED_SCOPE_LOCK( m_brokenNoseStageLock );
			m_brokenNoseStage = stage;
		}

		void CharacterCustomizationSystem::UpdateLifepath()
		{
			if ( !m_state )
				return;

			game::data::LifePath lifePath = FetchLifePath();

			for ( const auto& record : game::data::GetRecords< game::data::LifePath_Record >() )
			{
				auto lifePathRecord = Cast< game::data::LifePath_Record >( record );

				if ( lifePathRecord->type == lifePath )
				{
					m_state->SetLifePath( lifePathRecord->GetID() );
				}
			}
		}

		game::ui::CharacterCustomizationOptions& CharacterCustomizationSystem::GetOptions( CharacterCustomizationPart part )
		{
			switch ( part )
			{
			case game::ui::Head:
				return m_headOptions;
				break;

			case game::ui::Body:
				return m_bodyOptions;
				break;

			case game::ui::Arms:
				return m_armsOptions;
				break;
			}

			return s_emptyCustomizationOptions;
		}

		void CharacterCustomizationSystem::Apply( const THandle< CharacterCustomizationOption >& option, Uint32 currIndex, SetWeightType type )
		{
			if ( option && option->GetCurrIndex() != currIndex )
			{
				const auto& info = option->GetInfo();

				Bool applyLink = false;
				if ( currIndex != -1 && !info->m_link.Empty() )
				{
					if ( info->m_linkController )
					{
						m_links[ info->m_link ] = currIndex;
						applyLink = true;
					}
					else
					{
						m_links.Find( info->m_link, currIndex );
					}
				}

				if ( applyLink )
				{
					ApplyLink( m_headOptions, CharacterCustomizationPart::Head, info->m_link, currIndex, type );
					ApplyLink( m_bodyOptions, CharacterCustomizationPart::Body, info->m_link, currIndex, type );
					ApplyLink( m_armsOptions, CharacterCustomizationPart::Arms, info->m_link, currIndex, type );

					if ( option->IsActive() && !option->IsCensored() )
					{
						if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
						{
							ScheduleAppearanceAction( appearanceInfo, option->GetCurrIndex() );
						}
						else if ( const auto& switcherInfo = option->GetSwitcherInfo() )
						{
							ScheduleSwitchAppearanceAction( switcherInfo, option->GetCurrIndex() );
						}
					}
				}
				else
				{
					if ( option->IsActive() )
					{
						option->Set( currIndex );

						if ( !option->IsCensored() )
						{
							const auto& part = option->GetBodyPart();
							if ( const auto& morphInfo = option->GetMorphInfo() )
							{
								SetWeight( morphInfo, part, option->GetPrevIndex(), false, type );
								SetWeight( morphInfo, part, option->GetCurrIndex(), true, type );
							}
							else if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
							{
								SetAppearance( appearanceInfo, part, option->GetCurrIndex(), option->GetPrevIndex() );
								ScheduleAppearanceAction( appearanceInfo, option->GetCurrIndex() );
							}
							else if ( const auto& switcherInfo = option->GetSwitcherInfo() )
							{
								SwitchAppearance( switcherInfo, part, option->GetCurrIndex(), option->GetPrevIndex() );
								ScheduleSwitchAppearanceAction( switcherInfo, option->GetCurrIndex() );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::ApplyLink( const CharacterCustomizationOptions& options, CharacterCustomizationPart part, CName link, Uint32 currIndex, SetWeightType type )
		{
			for ( const auto& option : options )
			{
				if ( option->GetInfo()->m_link == link && option->GetCurrIndex() != currIndex )
				{
					if ( option->IsActive() )
					{
						option->Set( currIndex );

						if ( !option->IsCensored() )
						{
							if ( const auto& morphInfo = option->GetMorphInfo() )
							{
								SetWeight( morphInfo, part, option->GetPrevIndex(), false, type );
								SetWeight( morphInfo, part, option->GetCurrIndex(), true, type );
							}
							else if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
							{
								SetAppearance( appearanceInfo, part, option->GetCurrIndex(), option->GetPrevIndex() );
							}
							else if ( const auto& switcherInfo = option->GetSwitcherInfo() )
							{
								SwitchAppearance( switcherInfo, part, option->GetCurrIndex(), option->GetPrevIndex() );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::InitHead( const THandle< game::Puppet >& puppet )
		{
			auto& transactionSystem = GetGameInstance()->GetSystem<game::ITransactionSystem>();

			THandle< ItemObject > itemObject;
			if ( transactionSystem.GetItemInSlot( *puppet, TDBID( "AttachmentSlots.TppHead" ), itemObject ) )
			{
				if ( const auto& staticData = itemObject->GetStaticData().ToHandle() )
				{
					m_characterCustomizationInfoResource->GetHeadGroup( staticData->appearanceName, m_headGroup );
				}
			}
		}

		void CharacterCustomizationSystem::InitBody( const THandle< game::Puppet >& puppet )
		{
			const auto& characterRecord = puppet->GetRecord().ToHandle();
			if ( characterRecord && m_characterCustomizationInfoResource->GetBodyGroup( characterRecord->appearanceName, m_bodyGroup ) )
			{
				m_bodyEntity = puppet;

				const auto& component = FindMorphManager( puppet );
				m_bodyMorphManager = component;

				// starting character customization - inform morph manager that morph data should not be unloaded at this point
				if ( auto bodyMorphManager = m_bodyMorphManager.ToHandle() )
				{
					bodyMorphManager->SetShouldUnloadMorphData( false );
				}
			}
		}

		void CharacterCustomizationSystem::InitArms( const THandle< game::Puppet >& puppet )
		{
			auto& transactionSystem = GetGameInstance()->GetSystem<game::ITransactionSystem>();

			THandle< ItemObject > itemObject;
			if ( transactionSystem.GetItemInSlot( *puppet, TDBID( "AttachmentSlots.RightArm" ), itemObject ) )
			{
				if ( const auto& staticData = itemObject->GetStaticData().ToHandle() )
				{
					m_characterCustomizationInfoResource->GetArmsGroup( staticData->appearanceName, m_armsGroup );
				}
			}
		}

		void CharacterCustomizationSystem::InitVirtualOptions( const red::DynArray< THandle< CharacterCustomizationInfo > >& info, CharacterCustomizationPart part )
		{
			const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

			auto& options = GetOptions( part );

			for ( const auto& i : info )
			{
				const auto& option = CreateHandle<CharacterCustomizationOptionImpl>();
				option->Init( part, i, censorFlags );
				options.PushBack( option );
			}
		}

		void CharacterCustomizationSystem::InitOptions( const red::DynArray< THandle< CharacterCustomizationInfo > >& info, CharacterCustomizationPart part )
		{
			const auto& censorFlags = red::GCensorshipSystem.GetCensorFlags();

			auto& options = GetOptions( part );

			for ( const auto& i : info )
			{
				const auto& option = CreateHandle<CharacterCustomizationOptionImpl>();
				option->Init( part, i, censorFlags );
				options.PushBack( option );
			}
		}

		void CharacterCustomizationSystem::ReInitOptions( const red::DynArray< CharacterCustomizationUiPresetValue >& values, const CharacterCustomizationOptions& options, CharacterCustomizationOptions::iterator& iterator, CharacterCustomizationPart part, Uint32 optionsType )
		{
			Uint32 count = 0;
			for ( ; iterator != options.End(); ++iterator )
			{
				if ( count >= s_reinitOptionsCount )
				{
					break;
				}

				const auto& option = Cast<CharacterCustomizationOptionImpl>( *iterator );
				option->ReInit();

				const auto& info = option->GetInfo();

				auto it = std::find_if( values.Begin(), values.End(),
																[ optionName = info->m_name ]( const CharacterCustomizationUiPresetValue& value ) { return value.m_optionName == optionName; } );

				if ( it != values.End() )
				{
					const auto& value = ( *it );
					option->SetActive( value.m_isActive );
					option->Set( value.m_value );
				}

#ifdef DUG_UI_PRESETS
				RED_LOG_INFO( "--->> PRESET VALUE %s: %d", info->m_name.AsChar(), option->GetCurrIndex() );
#endif //DEBUG_UI_PRESETS

				if ( option->IsActive() && !option->IsCensored() )
				{
					if ( info->m_linkController && !info->m_link.Empty() )
					{
						m_links[ info->m_link ] = option->GetCurrIndex();
					}

					if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
					{
						if ( ( optionsType & InitOptionsType::Appearance ) > 0 )
						{
							count++;
							SetAppearance( appearanceInfo, part, option->GetCurrIndex(), option->GetPrevIndex() );

							if ( option->GetCurrIndex() < appearanceInfo->m_definitions.Size() )
							{
								ApplyEquipActions( appearanceInfo->m_definitions[ option->GetCurrIndex() ].m_actions );
							}
						}
					}
					else if ( const auto& morphInfo = option->GetMorphInfo() )
					{
						if ( ( optionsType & InitOptionsType::Morph ) > 0 )
						{
							count++;
							SetWeight( morphInfo, part, option->GetCurrIndex(), true, SetWeightType::Instant );
						}
					}
					else if ( const auto& switcherInfo = option->GetSwitcherInfo() )
					{
						if ( ( optionsType & InitOptionsType::Switcher ) > 0 )
						{
							count++;
							if ( option->GetCurrIndex() < switcherInfo->m_options.Size() )
							{
								ApplyEquipActions( switcherInfo->m_options[ option->GetCurrIndex() ].m_actions );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::ApplyEquipActions( const red::DynArray<CharacterCustomizationAction> actions )
		{
			for ( const auto& action : actions )
			{
				if ( action.m_type == CharacterCustomizationActionType::EquipItem ||
					action.m_type == CharacterCustomizationActionType::UnequipItem )
				{
					DoAction( s_emptyCustomizationOptions, action );
				}
			}
		}

		Bool CharacterCustomizationSystem::CanApplyOption( CName name, CharacterCustomizationPart part ) const
		{
			return ( part == CharacterCustomizationPart::Body && m_bodyGroup.m_options.Exist( name ) ) ||
				( part == CharacterCustomizationPart::Arms && m_armsGroup.m_options.Exist( name ) ) ||
				( part == CharacterCustomizationPart::Head && m_headGroup.m_options.Exist( name ) );
		}

		void CharacterCustomizationSystem::SetWeight( const THandle<const MorphInfo>& morphInfo, CharacterCustomizationPart part, Uint32 index, Bool enable, SetWeightType type )
		{
			if ( CanApplyOption( morphInfo->m_name, part ) &&
				index < morphInfo->m_morphNames.Size() && !morphInfo->m_morphNames[ index ].m_morphName.Empty() )
			{
				if ( type == SetWeightType::Instant )
				{
					if ( const auto& morphManager = m_bodyMorphManager.ToHandle() )
					{
						morphManager->SetTargetWeightWithNormalize( morphInfo->m_morphNames[ index ].m_morphName, morphInfo->m_name, enable ? 1.0f : 0.0f );
					}
				}
				else if ( type == SetWeightType::Postponed )
				{
					if ( const auto& morphManager = m_bodyMorphManager.ToHandle() )
					{
						m_postponedData.EmplaceBack( morphInfo->m_name, morphInfo->m_morphNames[ index ].m_morphName, enable ? 1.0f : 0.0f, morphManager );
					}
				}
				else if ( type == SetWeightType::Blending )
				{
					if ( !enable )
					{
						MoveMorphData( m_blendInData, m_blendOutData, morphInfo->m_morphNames[ index ].m_morphName, morphInfo->m_name, 1.f, part );
					}
					else
					{
						MoveMorphData( m_blendOutData, m_blendInData, morphInfo->m_morphNames[ index ].m_morphName, morphInfo->m_name, 0.f, part );
					}
				}
			}
		}

		void CharacterCustomizationSystem::SetAppearance( const THandle<const AppearanceInfo>& appearanceInfo, CharacterCustomizationPart part, Uint32 currIndex, Uint32 prevIndex )
		{
			if ( CanApplyOption( appearanceInfo->m_name, part ) && ( currIndex != -1 || prevIndex != -1 ) )
			{
				world::EntityAppearanceChangeParameter params = {};

				const auto& path = appearanceInfo->m_resource.GetPath();

				Uint32 definitionsCount = appearanceInfo->m_definitions.Size();
				if ( prevIndex < definitionsCount )
				{
					params.appearanceToRemove.resource = path;
					params.appearanceToRemove.name = appearanceInfo->m_definitions[ prevIndex ].m_name;
				}

				if ( currIndex < definitionsCount )
				{
					params.appearanceToAdd.resource = path;
					params.appearanceToAdd.name = appearanceInfo->m_definitions[ currIndex ].m_name;
				}

				params.entity = m_bodyEntity;

				if ( m_entityAppearanceChanger &&
					( params.appearanceToRemove.resource.IsValid() || params.appearanceToAdd.resource.IsValid() ) )
				{
#ifdef DEBUG_RANDOMIZE
					RED_LOG_INFO( "--->> appearance %s: %s -->> %s", appearanceInfo->m_name.AsChar(),
						params.appearanceToRemove.resource.ToString().AsChar(), params.appearanceToAdd.resource.ToString().AsChar() );
#endif // DEBUG_RANDOMIZE

					IncrementRequestsCounter( part );
					m_entityAppearanceChanger->ScheduleAppearanceChange( params,
						[ weakSelf = WeakHandleFromThis<CharacterCustomizationSystem>() ]()
					{
						if ( const auto& self = weakSelf.ToHandle() )
						{
							self->DecrementRequestsCounter();
						}
					} );
				}
			}
		}

		void CharacterCustomizationSystem::SwitchAppearance( const THandle<const SwitcherInfo>& switcherInfo, CharacterCustomizationPart part, Uint32 currIndex, Uint32 prevIndex )
		{
			const auto& options = GetOptions( part );

			THandle<CharacterCustomizationOption> option;
			red::DynArray< SwitchPair > pairs{ game::PoolGMPL_CharacterCustomization() };
			pairs.Resize( 2 );

			world::EntityAppearanceChangeParameter params = {};

			Uint32 optionsCount = switcherInfo->m_options.Size();

			if ( prevIndex < optionsCount )
			{
				const auto& prevOptName = switcherInfo->m_options[ prevIndex ].m_name;

				auto prevIt = std::find_if( options.Begin(), options.End(), [ prevOptName ]( const THandle<CharacterCustomizationOption>& option ) { return option->GetInfo()->m_name == prevOptName; } );
				if ( prevIt != options.End() )
				{
					option = ( *prevIt );
					if ( option )
					{
						SwitchOffOption( option, params, pairs );
					}
				}
			}

			if ( currIndex < optionsCount )
			{
				const auto& currOptName = switcherInfo->m_options[ currIndex ].m_name;

				auto currIt = std::find_if( options.Begin(), options.End(), [ currOptName ]( const THandle<CharacterCustomizationOption>& option ) { return option->GetInfo()->m_name == currOptName; } );
				if ( currIt != options.End() )
				{
					option = ( *currIt );
					if ( option )
					{
						SwitchOnOption( option, params, pairs );
					}
				}
			}

			if ( CanApplyOption( switcherInfo->m_name, part ) )
			{
				params.entity = m_bodyEntity;

				if ( m_entityAppearanceChanger &&
					( params.appearanceToRemove.resource.IsValid() || params.appearanceToAdd.resource.IsValid() ) )
				{
#ifdef DEBUG_RANDOMIZE
					RED_LOG_INFO( "--->> switcher %s: %s -->> %s", switcherInfo->m_name.AsChar(),
						params.appearanceToRemove.resource.ToString().AsChar(), params.appearanceToAdd.resource.ToString().AsChar() );
#endif // DEBUG_RANDOMIZE

					IncrementRequestsCounter( part );
					m_entityAppearanceChanger->ScheduleAppearanceChange( params,
						[ weakSelf = WeakHandleFromThis<CharacterCustomizationSystem>() ]()
					{
						if ( const auto& self = weakSelf.ToHandle() )
						{
							self->DecrementRequestsCounter();
						}
					} );
				}

				if ( switcherInfo->m_switchVisibility )
				{
					if ( auto* sysUI = GetGameInstance()->GetScene().GetSystem<world::ui::RuntimeSystemUI>() )
					{
						const auto& appearanceSwitchedEvent = CreateHandle<CharacterCustomizationSystem_OnAppearanceSwitchedEvent>();
						appearanceSwitchedEvent->m_pairs = pairs;
						sysUI->QueueEvent( appearanceSwitchedEvent );
					}
				}
			}
		}

		void CharacterCustomizationSystem::SwitchOffOption( const THandle<CharacterCustomizationOption>& prevOption, world::EntityAppearanceChangeParameter &params, red::DynArray< SwitchPair >& pairs )
		{
			Uint32 count, index = prevOption->GetCurrIndex();
			THandle<CharacterCustomizationOption> option;

			Uint8 pairIndex = 0;

			if ( const auto& switcherInfo = prevOption->GetSwitcherInfo() )
			{
				count = switcherInfo->m_options.Size();
				if ( index < count )
				{
					const auto& options = GetOptions( prevOption->GetBodyPart() );
					const auto& activeOptionName = switcherInfo->m_options[ index ].m_name;

					auto it = std::find_if( options.Begin(), options.End(), [ activeOptionName ]( const THandle<CharacterCustomizationOption>& option ) { return option->GetInfo()->m_name == activeOptionName; } );
					if ( it != options.End() )
					{
						option = ( *it );

						prevOption->Set( -1 );
						prevOption->SetActive( false );

						pairs[ pairIndex ].m_prevOption = prevOption;
						pairIndex++;
					}
				}
			}
			else
			{
				pairs[ pairIndex ].m_prevOption.Reset();
				option = prevOption;
			}

			if ( const auto& appearanceInfo = option ? option->GetAppearanceInfo() : nullptr )
			{
				index = option->GetCurrIndex();
				count = appearanceInfo->m_definitions.Size();
				if ( index < count )
				{
					params.appearanceToRemove.resource = appearanceInfo->m_resource.GetPath();
					params.appearanceToRemove.name = appearanceInfo->m_definitions[ index ].m_name;

					option->Set( -1 );
					option->SetActive( false );
				}

				pairs[ pairIndex ].m_prevOption = option;
			}
		}

		void CharacterCustomizationSystem::SwitchOnOption( const THandle<CharacterCustomizationOption>& currOption, world::EntityAppearanceChangeParameter &params, red::DynArray< SwitchPair >& pairs )
		{
			Uint32 count, index = currOption->GetCurrIndex();
			THandle<CharacterCustomizationOption> option;

			Uint8 pairIndex = 0;

			if ( const auto& switcherInfo = currOption->GetSwitcherInfo() )
			{
				count = switcherInfo->m_options.Size();
				if ( !m_links.Find( switcherInfo->m_link, index ) )
				{
					index = currOption->GetCurrIndex() < count ?
						currOption->GetCurrIndex() :
						currOption->GetInfo()->m_defaultIndex;
				}

				if ( index < count )
				{
					const auto& options = GetOptions( currOption->GetBodyPart() );
					const auto& activeOptionName = switcherInfo->m_options[ index ].m_name;

					auto it = std::find_if( options.Begin(), options.End(), [ activeOptionName ]( const THandle<CharacterCustomizationOption>& option ) { return option->GetInfo()->m_name == activeOptionName; } );
					if ( it != options.End() )
					{
						option = ( *it );
						currOption->Set( index );
						currOption->SetActive( true );

						pairs[ pairIndex ].m_currOption = currOption;
						pairIndex++;
					}
				}
			}
			else
			{
				pairs[ pairIndex ].m_currOption.Reset();
				option = currOption;
			}

			if ( const auto& appearanceInfo = option->GetAppearanceInfo() )
			{
				count = appearanceInfo->m_definitions.Size();
				if ( !m_links.Find( appearanceInfo->m_link, index ) )
				{
					index = option->GetCurrIndex() < count ?
						option->GetCurrIndex() :
						option->GetInfo()->m_defaultIndex;
				}

				if ( index < count )
				{
					params.appearanceToAdd.resource = appearanceInfo->m_resource.GetPath();
					params.appearanceToAdd.name = appearanceInfo->m_definitions[ index ].m_name;

					option->Set( index );
					option->SetActive( true );
				}

				pairs[ pairIndex ].m_currOption = option;
			}
		}


		void CharacterCustomizationSystem::ScheduleAppearanceAction( const THandle<const AppearanceInfo>& appearanceInfo, Uint32 currIndex )
		{
			if ( currIndex < appearanceInfo->m_definitions.Size() )
			{
				auto& actions = appearanceInfo->m_definitions[ currIndex ].m_actions;
				if ( !actions.Empty() )
				{
					m_pendingActions.PushBack( actions );
				}
			}
		}

		void CharacterCustomizationSystem::ScheduleSwitchAppearanceAction( const THandle<const SwitcherInfo>& switcherInfo, Uint32 currIndex )
		{
			if ( currIndex < switcherInfo->m_options.Size() )
			{
				auto& actions = switcherInfo->m_options[ currIndex ].m_actions;
				if ( !actions.Empty() )
				{
					m_pendingActions.PushBack( actions );
				}
			}
		}

		Bool CharacterCustomizationSystem::DoAction( const CharacterCustomizationOptions& options, const CharacterCustomizationAction& action )
		{
			if ( action.m_type <= CharacterCustomizationActionType::Deactivate )
			{
				CName optionName = RED_NAME( action.m_params );

				auto it = std::find_if( options.Begin(), options.End(),
					[ optionName ]( const THandle<CharacterCustomizationOption>& option ) { return option->GetInfo()->m_name == optionName; } );

				Bool found = it != options.End();
				if ( found )
				{
					Bool isActivation = action.m_type == CharacterCustomizationActionType::Activate;

					const auto& option = ( *it );
					if ( option && !option->IsCensored() && option->IsActive() != isActivation )
					{
						if ( isActivation )
						{
							option->SetActive( true );

							const auto& info = option->GetInfo();
							Uint32 index = info->m_defaultIndex;
							m_links.Find( info->m_link, index );

							Apply( option, index, SetWeightType::Instant );
						}
						else
						{
							m_pendingActions.PushBack( option->GetInfo()->m_onDeactivateActions );
							Apply( option, -1, SetWeightType::Instant );
							option->SetActive( false );
						}

						QueueOptionUpdateEvent( option );

						return true;
					}
				}
			}
			else if ( action.m_type <= CharacterCustomizationActionType::UnequipItem )
			{
				if ( const auto& entity = m_bodyEntity.ToHandle() )
				{
					auto& transactionSystem = GetGameInstance()->GetSystem<game::ITransactionSystem>();

					game::ItemDataWeakList itemList{ game::PoolGMPL_CharacterCustomization() };
					transactionSystem.GetItemList( *entity, itemList );

					const auto& itemRecordID = TDBID( action.m_params.AsChar() );
					const auto& itemRecord = game::data::GetRecord< game::data::Item_Record >( itemRecordID).ToHandle();
					const auto& itemQuery = game::ItemID::CreateQuery( itemRecordID );

					if ( itemRecord->placementSlotsGetCount() > 0 )
					{
						const auto& placementSlot = itemRecord->placementSlots()[ 0 ];

						if ( action.m_type == CharacterCustomizationActionType::EquipItem )
						{
							ItemID itemID;
							if ( const auto& itemData = transactionSystem.GetItemData( *entity, itemQuery ).ToHandle() )
							{
								itemID = itemData->GetID();
							}
							else
							{
								itemID = game::ItemID::CreateFromTDBID( itemRecordID );
								transactionSystem.GiveItem( *entity, itemID, 1U );
							}

							if ( itemID.IsValid() )
							{
								game::AddItemToSlotContext itemContext
								{
									*entity,
									placementSlot,
									itemID,
									false
								};
#ifdef ENT_SPAWN_DEBUG_INFO
								itemContext.m_spawnDebugInfo = "CharacterCustomizationSystem::DoAction";
#endif
								transactionSystem.AddItemToSlot( itemContext );
							}

							m_equippedItems.PushBack( action );
							m_equippedItems.Back().m_type = CharacterCustomizationActionType::UnequipItem;
						}
						else
						{
							transactionSystem.RemoveItemFromSlot( *entity, placementSlot );
							transactionSystem.RemoveItem( *entity, itemQuery, 1U );

							for ( const auto& i : m_equippedItems.Indices() )
							{
								if ( m_equippedItems[ i ].m_params == action.m_params )
								{
									m_equippedItems.RemoveAtReorder( i );
									break;
								}
							}
						}

						return true;
					}
				}
			}

			return false;
		}

		void CharacterCustomizationSystem::QueueOptionUpdateEvent( const THandle<CharacterCustomizationOption>& option )
		{
			if ( auto* sysUI = GetGameInstance()->GetScene().GetSystem<world::ui::RuntimeSystemUI>() )
			{
				const auto& optionUpdatedEvent = CreateHandle<CharacterCustomizationSystem_OnOptionUpdatedEvent>();
				optionUpdatedEvent->m_option = option;
				sysUI->QueueEvent( optionUpdatedEvent );
			}
		}

		void CharacterCustomizationSystem::AddPostponedEvent( const THandle< red::Event >& postponedEvent )
		{
			m_postponedEvent = postponedEvent;

			if ( m_requestsCount.GetValue() == 0 )
			{
				QueuePostponedEvent();
			}
		}

		void CharacterCustomizationSystem::QueuePostponedEvent()
		{
			auto* sysUI = GetGameInstance()->GetScene().GetSystem<world::ui::RuntimeSystemUI>();
			if ( sysUI && m_postponedEvent )
			{
				sysUI->QueueEvent( m_postponedEvent );
			}
			m_postponedEvent.Reset();
		}

		void CharacterCustomizationSystem::ResetMorphs( CharacterCustomizationPart part )
		{
			if ( const auto& entity = m_bodyEntity.ToHandle() )
			{
				auto& options = GetOptions( part );
				for ( const auto& option : options )
				{
					if ( !option->IsActive() || option->IsCensored() )
					{
						continue;
					}

					if ( const auto& morphInfo = option->GetMorphInfo() )
					{
						if ( option->GetCurrIndex() < morphInfo->m_morphNames.Size() )
						{
							SetWeight( morphInfo, part, option->GetCurrIndex(), false, SetWeightType::Instant );
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::ResetAppearances( CharacterCustomizationPart part, Bool silently )
		{
			const auto& entity = m_bodyEntity.ToHandle();

			if ( entity && m_entityAppearanceChanger )
			{
				auto& options = GetOptions( part );

				for ( const auto& option : options )
				{
					if ( !option->IsActive() || option->IsCensored() )
					{
						continue;
					}

					const auto& appearanceInfo = option->GetAppearanceInfo();
					if ( appearanceInfo && CanApplyOption( appearanceInfo->m_name, part ) )
					{
						Uint32 currIndex = option->GetCurrIndex();
						if ( appearanceInfo->m_resource.IsValid() &&
							currIndex < appearanceInfo->m_definitions.Size() && appearanceInfo->m_definitions[ currIndex ].m_name )
						{
							world::EntityAppearanceChangeParameter params = {};
							params.appearanceToRemove.resource = appearanceInfo->m_resource.GetPath();
							params.appearanceToRemove.name = appearanceInfo->m_definitions[ currIndex ].m_name;
							params.entity = m_bodyEntity;

#ifdef DEBUG_RANDOMIZE
							RED_LOG_INFO( "--->> remove appearance %s: %s -->> %s", appearanceInfo->m_name.AsChar(),
								params.appearanceToRemove.resource.ToString().AsChar(), params.appearanceToAdd.resource.ToString().AsChar() );
#endif // DEBUG_RANDOMIZE

							if ( silently )
							{
								m_entityAppearanceChanger->ScheduleAppearanceChange( params );
							}
							else
							{
								IncrementRequestsCounter( part );
								m_entityAppearanceChanger->ScheduleAppearanceChange( params,
									[ weakSelf = WeakHandleFromThis<CharacterCustomizationSystem>() ]()
								{
									if ( const auto& self = weakSelf.ToHandle() )
									{
										self->DecrementRequestsCounter();
									}
								} );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::MoveMorphData( red::DynArray<MorphData>& from, red::DynArray<MorphData>& to, CName targetName, CName regionName, Float weight, CharacterCustomizationPart part )
		{
			const red::FixedSizeFunction<Bool( const MorphData& data )> pred = [ targetName, regionName ]( const MorphData& data )
			{
				return data.Is( targetName, regionName );
			};

			auto fromIt = std::remove_if( from.Begin(), from.End(), pred );
			if ( fromIt != from.End() )
			{
				weight = fromIt->GetWeight();
				from.Remove( fromIt, from.End() );
			}

			if ( const auto& morphManager = m_bodyMorphManager.ToHandle() )
			{
				auto toIt = std::find_if( to.Begin(), to.End(), pred );
				if ( toIt == to.End() )
				{
					to.EmplaceBack( regionName, targetName, weight, morphManager );
				}
			}
		}

		void CharacterCustomizationSystem::Tick( const UpdateContext& updateContext, job::Builder& builder )
		{
			const auto& deltaTime = updateContext.m_tickInfo.m_realTimeDelta;

			if ( const auto& state = GetState() )
			{
				if ( !state->IsFinalized() )
				{
					{
						RED_SCOPE_LOCK( m_dataLock );

						if ( m_readyToNextPass.GetValue() )
						{
							m_readyToNextPass.SetValue( false );

							if ( m_requestsCount.GetValue() == 0 && !m_pendingActions.Empty() )
							{
								red::DynArray<CharacterCustomizationAction> pendingActions{ game::PoolGMPL_CharacterCustomization() };
								pendingActions = std::move( m_pendingActions );

								if ( !pendingActions.Empty() )
								{
									std::sort( pendingActions.Begin(), pendingActions.End(),
										[]( const CharacterCustomizationAction& a, const CharacterCustomizationAction& b ) { return a.m_type > b.m_type; } );

									for ( const auto& action : pendingActions )
									{
										DoAction( m_headOptions, action ) ||
											DoAction( m_bodyOptions, action ) ||
											DoAction( m_armsOptions, action );
									}
								}
							}

							if ( m_requestsCount.GetValue() == 0 && m_randomizationStage > RandomizationStage::RandomizationDisabled )
							{
								if ( m_randomizationStage == RandomizationStage::SwitcherOptions && !m_optionsToRandomize.Empty() )
								{
									CharacterCustomizationOptions optionsToRandomize{ game::PoolGMPL_CharacterCustomization() };
									optionsToRandomize = std::move( m_optionsToRandomize );
									RandomizeSwitcherOptions( optionsToRandomize );
									RandomizeNoneSwitcherOptions();
								}
								else
								{
									RandomizeNoneSwitcherOptions();
								}
							}

							if ( m_requestsCount.GetValue() == 0 && m_applyPresetStage > ApplyPresetStage::ApplyPresetDisabled )
							{
								ApplyDefaultUIPreset();
							}

							if ( m_requestsCount.GetValue() == 0 )
							{
								m_waitingToken.Reset();

								input::LocalHandle inputHandle( *this );
								if ( inputHandle )
								{
									inputHandle->ToggleUIStackActive( true );
								}

								QueuePostponedEvent();
							}
						}

						if ( m_requestsCount.GetValue() == 0 )
						{
							m_pending.Update( deltaTime );

							if ( m_pending.ShouldApply( nullptr ) )
							{
								Apply( m_pending.GetOption(), m_pending.GetValue(), SetWeightType::Blending );
								m_pending.Reset();
							}
						}
					}

					Bool needUpdateBlendArrays = false;
					{
						RED_SCOPE_SHARED_LOCK( m_dataLock );
						for ( auto& data : m_blendOutData )
						{
							data.UpdateTargetWeightWithNormalize( deltaTime * -Config::cvMorphingTimeDilation.Get() );
						}

						for ( auto& data : m_blendInData )
						{
							data.UpdateTargetWeightWithNormalize( deltaTime * Config::cvMorphingTimeDilation.Get() );
						}

						needUpdateBlendArrays = !m_blendOutData.Empty() || !m_blendInData.Empty();
					}

					if ( needUpdateBlendArrays )
					{
						RED_SCOPE_LOCK( m_dataLock );

						const red::FixedSizeFunction<Bool( const MorphData& data )> pred = []( const MorphData& data )
						{
							return data.GetWeight() <= 0.f || data.GetWeight() >= 1.f;
						};

						auto outIt = std::remove_if( m_blendOutData.Begin(), m_blendOutData.End(), pred );
						m_blendOutData.Remove( outIt, m_blendOutData.End() );

						auto inIt = std::remove_if( m_blendInData.Begin(), m_blendInData.End(), pred );
						m_blendInData.Remove( inIt, m_blendInData.End() );
					}

					const Uint32 postponedDataSize = m_postponedData.Size();
					if ( postponedDataSize > 0 )
					{
						const Uint32 batchSize = 2;
						const Uint32 batchCount = math::IntCeil( postponedDataSize, batchSize );

						for ( Uint32 batchIndex = 0; batchIndex < batchCount; ++batchIndex )
						{
							builder.DispatchJob<job::Fence::Full>( "CharacterCustomizationSystem/PostponedMorphs",
								[ this, batchIndex, batchSize, postponedDataSize ]( const job::RunContext& runContext )
							{
								const Uint32 begin = batchIndex * batchSize;
								const Uint32 end = Min( postponedDataSize, begin + batchSize );

								for ( Uint32 index = begin; index < end; ++index )
								{
									m_postponedData[ index ].UpdateTargetWeightWithNormalize( 0.f );
								}

								if ( end == postponedDataSize )
								{
									m_postponedData.Clear();
								}
							} );
						}
						builder.DispatchFenceExplicitly();
					}

				}
			}

			{
				RED_SCOPE_LOCK( m_componentsLock );
				Bool remove;
				THandle<ICharacterCustomizationComponent> comp;
				auto it = m_components.Begin();
				while ( it != m_components.End() )
				{
					remove = false;
					comp = ( *it ).ToHandle();

					if ( comp )
					{
						if ( comp->IsAttached() && comp->GetEntity()->IsAttached() )
						{
							if ( comp->m_readyToUpdate )
							{
								comp->OnUpdateCharacterCustomizationComponent( deltaTime );
							}
							else
							{
								remove = comp->OnCharacterCustomizationComponentAttached();
								comp->m_readyToUpdate = !remove;
							}
						}
					}
					else remove = true;

					if ( remove )
					{
						it = m_components.Remove( it ).Iterator();
					}
					else
					{
						++it;
					}
				}
			}
		}

		void CharacterCustomizationSystem::IncrementRequestsCounter( CharacterCustomizationPart part )
		{
			if ( m_requestsCount.Increment() == 1 )
			{
				DisableInput();

				m_readyToNextPass.SetValue( false );
			}
		}

		void CharacterCustomizationSystem::DecrementRequestsCounter()
		{
			if ( m_requestsCount.GetValue() > 0 && m_requestsCount.Decrement() == 0 )
			{
#ifdef DEBUG_RANDOMIZE
				RED_LOG_INFO( "------------------------------------------------------------------------>>" );
#endif // DEBUG_RANDOMIZE

				m_readyToNextPass.SetValue( true );
			}
		}

		void CharacterCustomizationSystem::ResetRequestsCounter()
		{
			m_readyToNextPass.SetValue( false );
			m_requestsCount.SetValue( 0 );

			EnableInput();

			m_pendingActions.Clear();

			m_applyPresetStage = ApplyPresetStage::ApplyPresetDisabled;
			m_defaultPreset.Reset();

			m_randomizationStage = RandomizationStage::RandomizationDisabled;
			m_optionsToRandomize.Clear();

			m_postponedEvent.Reset();
		}

		void CharacterCustomizationSystem::ShowWaitNotification()
		{
			if ( !m_waitingToken )
			{
				if ( auto* sysUI = GetGameInstance()->GetScene().GetSystem<world::ui::RuntimeSystemUI>() )
				{
					m_waitingToken = CreateHandle<ink::GameNotificationToken>();
					const auto& data = CreateHandle<ink::GameNotificationData>();
					data->m_notificationName = RED_NAME_CONSTEXPR( "AppearanceLoading" );
					m_waitingToken->SetData( data );

					const auto& evt = CreateHandle<ink::GameNotificationEvt>( m_waitingToken );
					sysUI->QueueEvent( evt );
				}
			}
		}

		void CharacterCustomizationSystem::EnableInput()
		{
			m_waitingToken.Reset();

			input::LocalHandle inputHandle( *this );
			if ( inputHandle )
			{
				inputHandle->ToggleUIStackActive( true );
			}
		}

		void CharacterCustomizationSystem::DisableInput()
		{
			ShowWaitNotification();

			input::LocalHandle inputHandle( *this );
			if ( inputHandle )
			{
				inputHandle->ToggleUIStackActive( false );

			}
		}

		game::data::LifePath CharacterCustomizationSystem::FetchLifePath()
		{
			static const CName systemName = RED_NAME_CONSTEXPR( "PlayerDevelopmentSystem" );
			static const CName functionName = RED_NAME_CONSTEXPR( "GetLifePath" );

			const auto& scriptableSystemsContainer = m_gameInstance->GetSystem< game::IScriptableSystemsContainer >();
			const auto& playerDevelopmentSystem = scriptableSystemsContainer.Get( systemName );
			RED_ASSERT( playerDevelopmentSystem, "PlayerDevelopmentSystem does not exit" );

			auto systemClass = GetRttiSystem().FindClass( systemName );
			RED_ASSERT( systemClass, "Missing system class [%hs]", systemName.AsChar() );

			auto systemFunction = systemClass->FindFunctionByFamilyName( functionName );
			RED_ASSERT( systemFunction, "Missing system function [%hs::%hs]", systemName.AsChar(), functionName.AsChar() );

			const auto& localPlayer = m_gameInstance->GetSystem< game::IPlayerSystem >().GetLocalPlayerMainObject();
			game::data::LifePath lifePath = game::data::LifePath::Invalid;
			Bool callRet = rtti::CallFunctionRawRet( playerDevelopmentSystem.Get(), systemFunction, lifePath, localPlayer );
			RED_ASSERT( callRet, "Failed to retrieve life path" );

			return lifePath;
		}

		Bool CharacterCustomizationSystem::HasCharacterCustomizationComponent( THandle< ent::Entity > entity ) const
		{
			Bool hasComponent = false;

			if( entity )
			{
				hasComponent = ( game::engineExt::FindComponentByType< game::ui::ICharacterCustomizationComponent >( entity.Get() ) != nullptr );
			}

			return hasComponent;
		}

#if !defined( NO_DEBUG_CONSOLE )

		Bool CharacterCustomizationSystem::Debug_SetBrokenNoseStage( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if ( parameters.Size() < 1 )
			{
				invokeResult.LogLine( "Stage index required: 0: CCBN_Disabled, 1: CCBN_Stage1, 2: CCBN_Stage2" );
				return false;
			}

			if ( CharacterCustomizationSystem* characterCustomizationSystem = reinterpret_cast< CharacterCustomizationSystem* >( userData ) )
			{
				Uint32 index;
				if ( FromString( parameters[ 0 ], index ) )
				{
					index = Clamp< Uint32 >( index, CCBN_Disabled, CCBN_Stage2 );
					characterCustomizationSystem->SetBrokenNoseStage( ( CharacterCustomization_BrokenNoseStage )index );
					return true;
				}
			}

			return false;
		}

		Bool CharacterCustomizationSystem::Debug_SaveAsUIPreset( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if ( CharacterCustomizationSystem* characterCustomizationSystem = reinterpret_cast< CharacterCustomizationSystem* >( userData ) )
			{
				if ( const auto& state = characterCustomizationSystem->GetState() )
				{
					red::DynArray< CharacterCustomizationUiPresetValue > values{ game::PoolGMPL_CharacterCustomization() };

					CharacterCustomizationOptions options{ game::PoolGMPL_CharacterCustomization() };
					options.Reserve( 500 );

					options.PushBack( characterCustomizationSystem->GetAllBodyOptions() );
					options.PushBack( characterCustomizationSystem->GetAllHeadOptions() );
					options.PushBack( characterCustomizationSystem->GetAllArmsOptions() );

					SaveOptionsAsUIPreset( options, values );

					auto depot = GetResourceDepot();

					res::ResourcePath path;
					if ( parameters.Size() < 1 )
					{
						for ( Uint32 i = 0; i < 100; i++ )
						{
							path = res::ResourcePath::Build( String::Printf( "%sui_preset_%u.charcustpreset", s_tempFolder, i ) );

							if ( !depot->ResourceExists( path ) )
							{
								break;
							}
						}
					}
					else
					{
						path = res::ResourcePath::Build( String::Printf( "%s%s", s_tempFolder, ( parameters[ 0 ] + ".charcustpreset" ).AsChar() ) );
					}

					const auto& preset = CreateHandle<CharacterCustomizationUiPreset>( state->IsBrainGenderMale(), values );
					preset->Internal_SetPath( path );
					res::SaveResource( preset );
					return true;
				}
			}

			return false;
		}

		Bool CharacterCustomizationSystem::Debug_LoadUIPreset( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			CharacterCustomizationSystem* characterCustomizationSystem = reinterpret_cast< CharacterCustomizationSystem* >( userData );
			if ( parameters.Size() >= 1 && characterCustomizationSystem )
			{
				res::ResourcePath path =
					res::ResourcePath::Build( String::Printf( "%s%s", s_tempFolder, ( parameters[ 0 ] + ".charcustpreset" ).AsChar() ) );

				auto token = GResourceLoader->IssueLoadingRequest( path );

				res::LoadedCallback onLoaded{ game::PoolGMPL_CharacterCustomization(),
					[ characterCustomizationSystem ]( THandle< CResource > res )
				{
					if ( const auto& preset = Cast<CharacterCustomizationUiPreset>( res ) )
					{
						characterCustomizationSystem->SetDefaultUIPreset( preset );
					}
				} };

				token->OnLoaded( std::move( onLoaded ) );

				return true;
			}

			return false;
		}

		void CharacterCustomizationSystem::SaveOptionsAsUIPreset( const CharacterCustomizationOptions& options, red::DynArray< CharacterCustomizationUiPresetValue >& values )
		{
			for ( const auto& option : options )
			{
				const auto& info = option->GetInfo();
				values.PushBack( { info->m_name, option->IsActive(), option->GetCurrIndex() } );
			}
		}

#if defined(CHARACTER_CUSTOMIZATION_PRESETS_ENABLED)

		Bool CharacterCustomizationSystem::Debug_SaveAsPreset( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			CharacterCustomizationSystem* characterCustomizationSystem = reinterpret_cast< CharacterCustomizationSystem* >( userData );
			const auto& state = Cast<CharacterCustomizationState>( characterCustomizationSystem->GetState() );
			if ( state && characterCustomizationSystem->m_characterCustomizationInfoResource )
			{
				auto depot = GetResourceDepot();

				res::ResourcePath path;
				if ( parameters.Size() < 1 )
				{
					for ( Uint32 i = 0; i < 100; i++ )
					{
						path = res::ResourcePath::Build( String::Printf( "%scharacter_%u.ccstate", s_tempFolder, i ) );

						if ( !depot->ResourceExists( path ) )
						{
							break;
						}
					}
				}
				else
				{
					path = res::ResourcePath::Build( String::Printf( "%s%s.ccstate", s_tempFolder, parameters[ 0 ].AsChar() ) );
				}

				const auto& preset = state->Debug_CreatePreset( 
					characterCustomizationSystem->GetAllBodyOptions(),
					characterCustomizationSystem->GetAllHeadOptions(),
					characterCustomizationSystem->GetAllArmsOptions(),
					characterCustomizationSystem->m_characterCustomizationInfoResource );
				preset->Internal_SetPath( path );

				res::SaveResource( preset );
				return true;
			}
			return false;
		}

		

#endif
#endif

		void CharacterCustomizationSystem::CheckVersionsCompatibility( const THandle<CharacterCustomizationState>& state )
		{
			if ( const auto& info = state->IsBodyGenderMale() ? s_maleCCO.Get() : s_femaleCCO.Get() )
			{
				if ( info->GetVersion() > state->GetVersion() )
				{
					red::HashSet<CName> verifiedLinks{ game::PoolGMPL_CharacterCustomization() };

					InitVirtualOptions( info->GetHeadCustomizationOptions(), CharacterCustomizationPart::Head );
					InitVirtualOptions( info->GetBodyCustomizationOptions(), CharacterCustomizationPart::Body );
					InitVirtualOptions( info->GetArmsCustomizationOptions(), CharacterCustomizationPart::Arms );

					CharacterCustomizationOptions simpleOptions{ game::PoolGMPL_CharacterCustomization() };
					CharacterCustomizationOptions advancedOptions{ game::PoolGMPL_CharacterCustomization() };

					SetSimpleOptionsValues( state->GetHeadCustomizationGroups(), m_headOptions, simpleOptions, verifiedLinks );
					SetAdvancedOptionsValues( m_headOptions, simpleOptions, advancedOptions );
					SetAdvancedOptionsValues( m_headOptions, advancedOptions, simpleOptions );

					SetSimpleOptionsValues( state->GetBodyCustomizationGroups(), m_bodyOptions, simpleOptions, verifiedLinks );
					SetAdvancedOptionsValues( m_bodyOptions, simpleOptions, advancedOptions );
					SetAdvancedOptionsValues( m_bodyOptions, advancedOptions, simpleOptions );

					SetSimpleOptionsValues( state->GetArmsCustomizationGroups(), m_armsOptions, simpleOptions, verifiedLinks );
					SetAdvancedOptionsValues( m_armsOptions, simpleOptions, advancedOptions );
					SetAdvancedOptionsValues( m_armsOptions, advancedOptions, simpleOptions );

					SetLinkedOptionsValues( m_headOptions );
					SetLinkedOptionsValues( m_bodyOptions );
					SetLinkedOptionsValues( m_armsOptions );

					const auto& newState = CreateHandle< CharacterCustomizationState >();

					newState->SetIsBodyGenderMale( state->IsBodyGenderMale() );
					newState->SetIsBrainGenderMale( state->IsBrainGenderMale() );
					newState->SetIsNewGame( state->IsNewGame() );
					// [missionflow] keep lifepath / CC attributes across the upgrade (NONE / empty for the plain fallback preset)
					newState->SetLifePath( state->GetLifePath() );
					for ( const auto& attribute : state->GetAttributes() )
					{
						newState->SetAttribute( attribute.m_type, attribute.m_value );
					}
					newState->SetAttributePointsAvailable( state->GetAttributePointsAvailable() );
					if ( t_missionflowInCompat )
					{
						std::fprintf( stderr, "CP2077 missionflow: CC state upgraded %u -> %u (lifepath/attributes kept)\n", unsigned( state->GetVersion() ), unsigned( info->GetVersion() ) );
					}

					newState->Finalize(
						GetAllBodyOptions(),
						GetAllHeadOptions(),
						GetAllArmsOptions(),
						info
					);

					InitializeFromFinalizedState( newState );
				}
				else
				{
					InitializeFromFinalizedState( state );
				}
			}
		}

		void CharacterCustomizationSystem::SetSimpleOptionsValues( const CustomizationGroups& customizationGroups, const CharacterCustomizationOptions& allOptions, CharacterCustomizationOptions& simpleOptions, red::HashSet<CName>& verifiedLinks )
		{
			simpleOptions.Clear();
			simpleOptions.Reserve( allOptions.Size() );

			for ( const auto& group : customizationGroups )
			{
				for ( const auto& customization : group.m_customization )
				{
					const auto& option = FindOption( allOptions, customization.m_name );
					if ( !option ) { continue; }

					const auto& appearanceInfo = option->GetAppearanceInfo();
					if ( !appearanceInfo ) { continue; }

					auto defIt = std::find_if( appearanceInfo->m_definitions.Begin(), appearanceInfo->m_definitions.End(),
						[ defName = customization.m_definition ]( const IndexedAppearanceDefinition& appearance )
					{
						return appearance.m_name == defName;
					} );

					if ( defIt == appearanceInfo->m_definitions.End() ) { continue; }

					const Uint32 newVal = ( Uint32 )( defIt - appearanceInfo->m_definitions.Begin() );

					option->SetActive( true );
					option->Set( newVal );

					simpleOptions.PushBack( option );

					if ( !appearanceInfo->m_link.Empty() )
					{
						if ( appearanceInfo->m_linkController )
						{
							verifiedLinks.Insert( appearanceInfo->m_link );
							m_links[ appearanceInfo->m_link ] = newVal;
						}
						else
						{
							const auto& prevLinkVal = GetLinkValue( m_links, appearanceInfo->m_link );
							if ( prevLinkVal == -1 )
							{
								m_links[ appearanceInfo->m_link ] = newVal;
							}
							else if ( !verifiedLinks.Exist( appearanceInfo->m_link ) )
							{
								RED_ASSERT( prevLinkVal == newVal );
							}
						}
					}
				}

				for ( const auto& morph : group.m_morphs )
				{
					const auto& option = FindOption( allOptions, morph.m_regionName );
					if ( !option ) { continue; }

					const auto& morphInfo = option->GetMorphInfo();
					if ( !morphInfo ) { continue; }

					auto defIt = std::find_if( morphInfo->m_morphNames.Begin(), morphInfo->m_morphNames.End(),
						[ targetName = morph.m_targetName ]( const IndexedMorphName& morph )
					{
						return morph.m_morphName == targetName;
					} );

					if ( defIt == morphInfo->m_morphNames.End() ) { continue; }

					const Uint32 newVal = ( Uint32 )( defIt - morphInfo->m_morphNames.Begin() );

					option->SetActive( true );
					option->Set( newVal );

					if ( !morphInfo->m_link.Empty() )
					{
						if ( morphInfo->m_linkController )
						{
							verifiedLinks.Insert( morphInfo->m_link );
							m_links[ morphInfo->m_link ] = newVal;
						}
						else
						{
							const auto& prevLinkVal = GetLinkValue( m_links, morphInfo->m_link );
							if ( prevLinkVal == -1 )
							{
								m_links[ morphInfo->m_link ] = newVal;
							}
							else if ( !verifiedLinks.Exist( morphInfo->m_link ) )
							{
								RED_ASSERT( prevLinkVal == newVal );
							}
						}
					}
				}
			}
		}

		void CharacterCustomizationSystem::SetAdvancedOptionsValues( const CharacterCustomizationOptions& allOptions, const CharacterCustomizationOptions& simpleOptions, CharacterCustomizationOptions& advancedOptions )
		{
			advancedOptions.Clear();
			advancedOptions.Reserve( simpleOptions.Size() );

			for ( const auto& option : allOptions )
			{
				const auto& switcherInfo = option->GetSwitcherInfo();
				if ( !switcherInfo ) { continue; }

				for ( Uint32 i = 0; i < switcherInfo->m_options.Size(); i++ )
				{
					if ( FindOption( simpleOptions, switcherInfo->m_options[ i ].m_name ) )
					{
						option->SetActive( true );
						option->Set( i );

						advancedOptions.PushBack( option );

						if ( option->GetPrevIndex() != -1 && option->GetPrevIndex() != option->GetCurrIndex() )
						{
							if ( const auto prevOption = FindOption( allOptions, switcherInfo->m_options[ option->GetPrevIndex() ].m_name ) )
							{
								prevOption->SetActive( false );
							}
						}

						break;
					}
				}
			}
		}

		void CharacterCustomizationSystem::SetLinkedOptionsValues( const CharacterCustomizationOptions& allOptions )
		{
			for ( const auto& option : allOptions )
			{
				if ( option->IsActive() )
				{
					const auto& linkValue = GetLinkValue( m_links, option->GetInfo()->m_link );
					if ( linkValue != -1 && option->GetCurrIndex() != linkValue )
					{
						SwitchVirtualOption( option, allOptions, linkValue );
					}
				}
			}
		}

		void CharacterCustomizationSystem::SwitchVirtualOption( const THandle< CharacterCustomizationOption >& option, const CharacterCustomizationOptions& allOptions, Uint32 value )
		{
			option->Set( value );

			if ( const auto& switcherInfo = option->GetSwitcherInfo() )
			{
				if ( const auto& currOption = FindOption( allOptions, switcherInfo->m_options[ option->GetCurrIndex() ].m_name ) )
				{
					currOption->SetActive( true );

					Uint32 index, count = 0;
					if ( !m_links.Find( currOption->GetInfo()->m_link, index ) )
					{
						if ( const auto& appearanceInfo = currOption->GetAppearanceInfo() )
						{
							count = appearanceInfo->m_definitions.Size();
						}
						else if ( const auto& switcherInfo = currOption->GetSwitcherInfo() )
						{
							count = switcherInfo->m_options.Size();
						}

						index = currOption->GetCurrIndex() < count ?
							currOption->GetCurrIndex() :
							currOption->GetInfo()->m_defaultIndex;
					}

					SwitchVirtualOption( currOption, allOptions, index );
				}

				if ( option->GetPrevIndex() != -1 )
				{
					if ( const auto& prevOption = FindOption( allOptions, switcherInfo->m_options[ option->GetPrevIndex() ].m_name ) )
					{
						prevOption->SetActive( false );

						SwitchVirtualOption( prevOption, allOptions, -1 );
					}
				}
			}
		}

		//////////////////////////////////////////////////////////////////////////

		SetCharacterCreationDataRequest::SetCharacterCreationDataRequest() = default;
		SetCharacterCreationDataRequest::SetCharacterCreationDataRequest( const WeakHandle< Object >& owner, const data::TweakDBID& lifepath, const red::DynArray< CharacterCustomizationAttribute >& attributes )
			: PlayerScriptableSystemRequest( owner )
			, m_lifepath( lifepath )
			, m_attributes( attributes ) {}

		//////////////////////////////////////////////////////////////////////////

		void CharacterCustomizationSystem::SceneInstanceIdWrapper::Init( scn::ISceneSystem* sceneSystem, const SceneInstanceIdCallback& callback )
		{
			m_sceneSystem = sceneSystem;
			m_callback = callback;
		}

		void CharacterCustomizationSystem::SceneInstanceIdWrapper::DeInit()
		{
			if ( m_sceneInstanceId != scn::SceneInstanceId::invalid )
			{
				m_sceneSystem->DeregisterListener( m_sceneInstanceId, *this );
			}

			m_sceneSystem = nullptr;
			m_callback = nullptr;
		}

		void CharacterCustomizationSystem::SceneInstanceIdWrapper::SetSceneInstanceId( const scn::SceneInstanceId& sceneInstanceId )
		{
			RED_SCOPE_LOCK( m_lock );
			if ( sceneInstanceId != scn::SceneInstanceId::invalid )
			{
				m_sceneInstanceId = sceneInstanceId;
				m_sceneSystem->RegisterListener( m_sceneInstanceId, *this );
			}
		}

		void CharacterCustomizationSystem::SceneInstanceIdWrapper::TerminateScene()
		{
			RED_SCOPE_SHARED_LOCK( m_lock );
			if ( m_sceneInstanceId != scn::SceneInstanceId::invalid )
			{
				m_sceneSystem->QueueRequest( m_sceneInstanceId, scn::Control::Request::Terminate() );
			}
		}

		Bool CharacterCustomizationSystem::SceneInstanceIdWrapper::IsActive() const
		{
			RED_SCOPE_SHARED_LOCK( m_lock );
			return m_sceneInstanceId != scn::SceneInstanceId::invalid;
		}

		void CharacterCustomizationSystem::SceneInstanceIdWrapper::OnNotificationsPublished( const scn::SceneNotificationQueue& publishedNotifications )
		{
			Bool callback = false;

			{
				RED_SCOPE_LOCK( m_lock );
				for ( const scn::SceneNotification& notification : publishedNotifications )
				{
					if ( m_sceneInstanceId == notification.m_sceneInstanceId )
					{
						switch ( notification.m_notificationType )
						{
						case scn::SceneNotificationType::instanceReady:
						case scn::SceneNotificationType::instanceExitPoint:
							break;
						case scn::SceneNotificationType::instanceFinished:
							m_sceneSystem->QueueRequest( m_sceneInstanceId, scn::Control::Request::Terminate() );
							break;
						case scn::SceneNotificationType::instanceDisposed:
							m_sceneInstanceId = scn::SceneInstanceId::invalid;
							callback = true;
							break;
						}
					}
				}
			}

			if ( callback && m_callback )
			{
				m_callback();
			}
		}

	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif
// [missionflow] entry point for the gameFramework PreGameRequestsHandler overlay (?start=nomad direct new game).
void cpweb_missionflow_BuildNomadState( void ( *done )( const THandle< const IScriptable >& ) )
{
#ifdef CHARACTER_CUSTOMIZATION_PRESETS_ENABLED
	game::ui::CpwebMissionflowBuildNomadState( done );
#else
	std::fprintf( stderr, "CP2077 missionflow: ERROR presets disabled in this build\n" );
	done( THandle< const IScriptable >() );
#endif
}
