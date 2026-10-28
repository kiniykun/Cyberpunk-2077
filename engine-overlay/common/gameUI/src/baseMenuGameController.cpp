/**
* Copyright (C)2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include <atomic>
#include "baseMenuGameController.h"
#include "../../game/include/puppet.h"
#include "../../game/include/puppetHelper.h"
#include "../../gameCore/include/gameRttiExtensions.h"
#include "../../gameTweakDB/include/types/tweakDBGender.h"
#include "../../inkCore/include/menuLayerEvents.h"
#include "../gameCore/include/characterCustomizationSystemInterface.h"
#include "../gameTweakDB/include/records/tweakDBCharacter.h"
#include "../worldEntities/include/runtimeSystemEntityAnimation.h"
#include "../worldEntities/include/runtimeSystemEntity.h"

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

namespace
{
	class UIObjectsLoaderSystemListener : public game::ui::IUIObjectsLoaderSystemListener
	{
		RTTI_DECLARE_TYPE( UIObjectsLoaderSystemListener );
		RED_USE_MEMORY_POOL( ink::PoolInk_Controllers );

	public:
		UIObjectsLoaderSystemListener() {}
		UIObjectsLoaderSystemListener( const THandle<game::ui::BaseMenuGameController>& owner )
			: m_owner( owner ) {}

	private:
		virtual void OnPuppetSceneLoaded( const game::ui::PuppetSceneHandle::Ptr& ticket ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnPuppetSceneLoaded( ticket );
			}
		}

		virtual void OnPuppetSceneLoadingFailed( const game::ui::PuppetSceneHandle::Ptr& ticket ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnPuppetSceneLoadingFailed( ticket );
			}
		}

		virtual void OnItemPreviewSceneLoaded( CName sceneName ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnItemPreviewSceneLoaded( sceneName );
			}
		}

		virtual void OnItemPreviewSceneLoadingFailed( CName sceneName ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnItemPreviewSceneLoadingFailed( sceneName );
			}
		}

		virtual void OnItemEntitySpawned( CName sceneName, const WeakHandle< game::ItemObject >& rootItem )
		{
		}

		virtual void OnItemEntitySpawningFailed( CName sceneName, const game::ItemID& itemID )
		{
		}

		WeakHandle<game::ui::BaseMenuGameController> m_owner;
	};

	class MenuInstanceListener : public ink::IMenuInstanceListener
	{
	public:
		MenuInstanceListener( const THandle< game::ui::BaseMenuGameController >& owner )
			: m_owner( owner )
		{
		}

		virtual void OnVisibilityChanged( Bool isVisible ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnVisibilityChanged( isVisible );
			}
		}

		virtual void OnScenarioChanged( CName prevScenario, CName currScenario ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnScenarioChanged( prevScenario, currScenario );
			}
		}

		WeakHandle< game::ui::BaseMenuGameController > m_owner;
	};

}

RTTI_BEGIN_TYPE( UIObjectsLoaderSystemListener );
	RTTI_PARENT_TYPE( game::ui::IUIObjectsLoaderSystemListener );
RTTI_END_TYPE();

RTTI_BEGIN_ENUM_IN_NAMESPACE( PuppetGenderInfo, game, ui, BaseMenuGameController );
	RTTI_ENUM_OPTION( Male );
	RTTI_ENUM_OPTION( Female );
	RTTI_ENUM_OPTION( ShouldBeDetermined );
RTTI_END_ENUM();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetSceneInfo, game, ui, BaseMenuGameController );
	RTTI_PROPERTY( m_sceneName ).editable();
	RTTI_PROPERTY( m_markerRef ).editable();
	RTTI_PROPERTY( m_prefabRef ).editable();
	RTTI_PROPERTY( m_entityTemplate ).editable().inlined();
	RTTI_PROPERTY( m_puppetRecordId ).editable().customEditor( "TweakDBGroupInheritance;Character" );
	RTTI_PROPERTY( m_gender ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( BaseMenuGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::WidgetGameController );
	RTTI_NATIVE_FUNCTION("GetPuppet", funcGetPuppet);
	RTTI_NATIVE_FUNCTION("SpawnMenuInstanceEvent", funcSpawnMenuInstanceEvent);
	RTTI_NATIVE_FUNCTION("SpawnMenuInstanceDataEvent", funcSpawnMenuInstanceDataEvent);
	RTTI_PROPERTY( m_puppetSceneInfos ).editable().resizable();
	RED_EVENT_CONNECTOR( OnGenderChanged );
RTTI_END_TYPE();

namespace game
{
	namespace ui
	{
		//////////////////////////////////////////////////////////////////////////

		BaseMenuGameController::BaseMenuGameController()
			: m_initializationFlags( 0 )
			, m_isPuppetsVisible( false )
			, m_animationSystem(nullptr)
			, m_entitySystem(nullptr)
		{

		}

		void BaseMenuGameController::OnPuppetSceneLoaded( const game::ui::PuppetSceneHandle::Ptr & ticket )
		{
			RED_SCOPE_SHARED_LOCK( m_lock );
			if ( ticket && m_tickets.Exist( ticket ) )
			{
				InitializePuppet( ticket );
				NotifyPuppetNeedsUpdate( RED_NAME_CONSTEXPR( "OnPuppetReady" ), ticket );
			}
		}

		void BaseMenuGameController::OnPuppetSceneLoadingFailed( const game::ui::PuppetSceneHandle::Ptr & ticket )
		{
			RED_SCOPE_LOCK( m_lock );
			if ( ticket )
			{
				m_tickets.RemoveReorder( ticket );
			}
		}

		void BaseMenuGameController::OnItemPreviewSceneLoaded( CName sceneName )
		{

		}

		void BaseMenuGameController::OnItemPreviewSceneLoadingFailed( CName sceneName )
		{

		}

		void BaseMenuGameController::OnVisibilityChanged( Bool isVisible )
        {
            SetPuppetsVisible( isVisible );
#ifdef __EMSCRIPTEN__
            // The native menu listener owns visibility. Preserve its event and
            // create original actors only once their actual menu is activated.
            if(isVisible && CheckInitializationFlags(InitializationFlags::PlayerAttached) &&
                !CheckInitializationFlags(InitializationFlags::PuppetsInitialized)) {
                std::fprintf(stderr,"CP2077 puppet demand probe: phase=activate scenes=%llu\n",static_cast<unsigned long long>(m_puppetSceneInfos.Size()));
                InitializePuppets();
            }
#endif
		}

		void BaseMenuGameController::OnScenarioChanged( CName prevScenario, CName currScenario )
		{

		}

		//////////////////////////////////////////////////////////////////////////

		void BaseMenuGameController::Initialize( const InitializeContext & initContext )
		{
			TBaseClass::Initialize( initContext );

			m_animationSystem = m_sceneService->GetSystem<world::AnimationSystem>();
			m_entitySystem = m_sceneService->GetSystem<world::RuntimeSystemEntity>();

			SetInitializationFlags( InitializationFlags::ControllerInitialized );

			if ( CheckInitializationFlags( InitializationFlags::PlayerAttached ) )
			{
				InitializePuppets();
			}

			m_menuInstanceListener = red::CreateSharedPtr< MenuInstanceListener >( HandleFromThis<BaseMenuGameController>() );
			QueueEvent( CreateHandle<ink::MenuInstance_MenuInstanceListener>( true, m_menuInstanceListener ) );
		}

		void BaseMenuGameController::Uninitialize( const UninitializeContext & uninitContext )
		{
			QueueEvent( CreateHandle<ink::MenuInstance_MenuInstanceListener>( false, m_menuInstanceListener ) );
			m_menuInstanceListener.Reset();

			SetPuppetsVisible( false );

			if ( IUIObjectsLoaderSystem* uiObjectsLoaderSystem = GetSystem<IUIObjectsLoaderSystem>() )
			{
				{
					RED_SCOPE_LOCK( m_lock );
					for ( const auto& ticket : m_tickets )
					{
						UninitializePuppet( ticket );
						uiObjectsLoaderSystem->UnloadPuppetScene( ticket );
					}
					m_tickets.Clear();
				}

				uiObjectsLoaderSystem->UnregisterListener( m_uiObjectsLoaderSystemListener );
			}

			m_uiObjectsLoaderSystemListener = nullptr;
			m_initializationFlags = 0;

			TBaseClass::Uninitialize( uninitContext );
		}

		void BaseMenuGameController::OnPlayerAttach( const THandle< game::Object >& player )
		{
			TBaseClass::OnPlayerAttach( player );
#ifdef __EMSCRIPTEN__
            // Remember genuine attachment while an invisible menu is deferred.
            SetInitializationFlags(InitializationFlags::PlayerAttached);
#endif
            InitializePuppets();
		}

		void BaseMenuGameController::OnGenderChanged( const ink::MenuLayer_SetGender& evt )
		{
			InitializePuppets();
		}

		void BaseMenuGameController::InitializePuppets()
        {
#ifdef __EMSCRIPTEN__
            if(!CheckInitializationFlags(InitializationFlags::PuppetsInitialized) && !IsPuppetsVisible()) {
                static std::atomic<unsigned> reports{0};
                if(!m_puppetSceneInfos.Empty() && reports.fetch_add(1)<32)
                    std::fprintf(stderr,"CP2077 puppet demand probe: phase=defer-hidden scenes=%llu\n",static_cast<unsigned long long>(m_puppetSceneInfos.Size()));
                return;
            }
#endif
			if ( CheckInitializationFlags( InitializationFlags::ControllerInitialized ) )
			{
				if ( IUIObjectsLoaderSystem* uiObjectsLoaderSystem = GetSystem<IUIObjectsLoaderSystem>() )
				{
					if ( CheckInitializationFlags( InitializationFlags::PuppetsInitialized ) )
					{
						for ( const auto& info : m_puppetSceneInfos )
						{
							ReloadPuppets( info, uiObjectsLoaderSystem );
						}
					}
					else
					{
						m_uiObjectsLoaderSystemListener = CreateHandle<UIObjectsLoaderSystemListener>( HandleFromThis<BaseMenuGameController>() );
						uiObjectsLoaderSystem->RegisterListener( m_uiObjectsLoaderSystemListener );

						for ( const auto& info : m_puppetSceneInfos )
						{
							LoadPuppetScene( info, uiObjectsLoaderSystem );
						}

						SetInitializationFlags( InitializationFlags::PuppetsInitialized );
					}
				}
			}
			else
			{
				SetInitializationFlags( InitializationFlags::PlayerAttached );
			}
		}
		void BaseMenuGameController::InitializePuppet( const game::ui::PuppetSceneHandle::Ptr & ticket )
		{
			Bool isMale = ticket->GetInfo().m_isMale;
			const auto& puppet = ticket->GetPuppet();
			RED_ASSERT( puppet );

			const auto& characterRecord = puppet->GetRecord().ToHandle();
			RED_ASSERT( characterRecord );

			if ( const auto& state = GetSystem<ICharacterCustomizationSystem>()->GetFinalizedState( isMale ) )
			{
				state->MorphBody( characterRecord->appearanceName, false, puppet );
			}

			if ( THandle<PuppetPS> persistentState = puppet->TemplatedGetPS<PuppetPS>() )
			{
				game::data::Gender gender = isMale ? game::data::Gender::Male : game::data::Gender::Female;
				CName genderName = game::rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "gamedataGender" ), static_cast< Int64 >( gender ) );

				persistentState->SetGender( genderName );
				persistentState->SetTpp( true );
			}

			if ( IsPuppetsVisible() && m_animationSystem )
			{
				m_animationSystem->SetForcedVisible( puppet->GetEntityID(), true );

				if (m_entitySystem)
					m_entitySystem->ScheduleEntitySetLOD(puppet, ent::EntityLOD::ForcedHighestQuality);
			}
		}

		void BaseMenuGameController::UninitializePuppet( const game::ui::PuppetSceneHandle::Ptr & ticket )
		{

		}

		void BaseMenuGameController::NotifyAllPuppetsNeedUpdate( CName updateEvent )
		{
			for ( const auto& ticket : m_tickets )
			{
				NotifyPuppetNeedsUpdate( updateEvent, ticket );
			}
		}

		void BaseMenuGameController::NotifyPuppetNeedsUpdate( CName updateEvent, const game::ui::PuppetSceneHandle::Ptr & ticket )
		{
			CallEvent( updateEvent, ticket->GetInfo().m_name, ticket->GetPuppet() );
		}

		void BaseMenuGameController::DeterminePlayerRelatedData( const PuppetSceneInfo &info, Bool &isMale, ent::CustomizationAppearances &appearances )
		{
			if ( info.m_gender == PuppetGenderInfo::ShouldBeDetermined )
			{
				const auto& player = Cast<game::Puppet>( GetPlayerControlledObject() );
				isMale = player && player->GetResolvedGenderName() == PuppetHelper::MALE;
			}
			else
			{
				isMale = info.m_gender == PuppetGenderInfo::Male;
			}

			const auto& characterCustomizationSystem = GetSystem<ICharacterCustomizationSystem>();
			const auto& state = characterCustomizationSystem->GetFinalizedState( isMale );
			const auto& characterRecord = game::data::GetRecord<game::data::Character_Record>( info.m_puppetRecordId ).ToHandle();

			if ( state && characterRecord )
			{
				state->GetBodyCustomization( characterRecord->appearanceName, false, appearances );
			}
		}

		void BaseMenuGameController::LoadPuppetScene( const PuppetSceneInfo& info, IUIObjectsLoaderSystem* uiObjectsLoaderSystem )
		{
			Bool isMale = false;
			ent::CustomizationAppearances appearances{ ink::PoolInk_Controllers() };

			DeterminePlayerRelatedData( info, isMale, appearances );

			game::ui::PuppetSceneInfo i
			(
				info.m_sceneName,
				info.m_prefabRef,
				info.m_markerRef,
				info.m_entityTemplate.GetPath(),
				info.m_puppetRecordId,
				appearances,
				isMale
			);

			const auto& ticket = uiObjectsLoaderSystem->LoadPuppetScene( std::move( i ) );
			if ( ticket )
			{
				RED_SCOPE_LOCK( m_lock );
				m_tickets.PushBack( ticket );
			}
		}

		void BaseMenuGameController::ReloadPuppets( const PuppetSceneInfo& info, IUIObjectsLoaderSystem* uiObjectsLoaderSystem )
		{
			SetPuppetsVisible( false );

			if ( info.m_gender == PuppetGenderInfo::ShouldBeDetermined )
			{
				Bool isMale = false;
				ent::CustomizationAppearances appearances{ ink::PoolInk_Controllers() };

				DeterminePlayerRelatedData( info, isMale, appearances );

				RED_SCOPE_SHARED_LOCK( m_lock );
				for ( const auto& ticket : m_tickets )
				{
					if ( ticket->GetInfo().m_name == info.m_sceneName )
					{
						UninitializePuppet( ticket );
						uiObjectsLoaderSystem->ReloadPuppet( ticket, isMale, appearances );
						break;
					}
				}
			}
		}

		THandle<game::Puppet> BaseMenuGameController::GetPuppet( CName sceneName )
		{
			THandle<game::Puppet> puppet;

			RED_SCOPE_SHARED_LOCK( m_lock );
			for ( const auto& ticket : m_tickets )
			{
				if ( ticket->GetState() == PuppetSceneLoadingState::Ready && ticket->GetInfo().m_name == sceneName )
				{
					puppet = ticket->GetPuppet();
					break;
				}
			}

			return puppet;
		}

		void BaseMenuGameController::funcGetPuppet( CScriptStackFrame & stack, void * result, const rtti::IType * resultType )
		{
			GET_PARAMETER( CName, sceneName, CName::NONE() );
			FINISH_PARAMETERS;

			RETURN_HANDLE( game::Puppet, GetPuppet( sceneName ) );
		}

		void BaseMenuGameController::SetPuppetsVisible( Bool isVisible )
		{
			if ( m_isPuppetsVisible != isVisible )
			{
				m_isPuppetsVisible = isVisible;

				if ( m_animationSystem )
				{
					RED_SCOPE_SHARED_LOCK( m_lock );
					for ( const auto& ticket : m_tickets )
					{
						if ( const auto& puppet = ticket->GetPuppet() )
						{
							m_animationSystem->SetForcedVisible( puppet->GetEntityID(), isVisible );
							if (m_entitySystem)
							{
								if (isVisible)
									m_entitySystem->ScheduleEntitySetLOD(puppet, ent::EntityLOD::ForcedHighestQuality);
								else
									m_entitySystem->ScheduleEntityClearLOD(puppet, ent::EntityLOD::ForcedHighestQuality);
							}
						}
					}
				}
			}
		}

		Bool BaseMenuGameController::IsPuppetsVisible() const
		{
			return m_isPuppetsVisible;
		}

		void BaseMenuGameController::SpawnMenuInstanceEvent(const CName & name, const THandle<IScriptable>& userData)
		{
			QueueEvent(CreateHandle<ink::MenuInstance_SpawnEvent>(name, userData));
		}

		void BaseMenuGameController::funcSpawnMenuInstanceEvent(CScriptStackFrame& stack, void* result, const rtti::IType* resultType)
		{
			GET_PARAMETER(CName, eventName, CName::NONE());
			FINISH_PARAMETERS;

			SpawnMenuInstanceEvent(eventName);
		}

		void BaseMenuGameController::funcSpawnMenuInstanceDataEvent(CScriptStackFrame& stack, void* result, const rtti::IType* resultType)
		{
			GET_PARAMETER(CName, eventName, CName::NONE());
			GET_PARAMETER(THandle<IScriptable>, userData, THandle<IScriptable>());
			FINISH_PARAMETERS;

			SpawnMenuInstanceEvent(eventName, userData);
		}
	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif