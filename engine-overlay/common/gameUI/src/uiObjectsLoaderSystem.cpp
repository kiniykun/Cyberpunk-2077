/**
* Copyright (C)2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include <atomic>
#include "uiObjectsLoaderSystem.h"
#include "entityPreviewGameObject.h"
#include "../../game/include/puppet.h"
#include "../../game/include/puppetHelper.h"
#include "../../game/include/itemObject.h"
#include "../../game/include/itemDataUnique.h"
#include "../../game/include/inventory.h"
#include "../../game/src/inventoryManager.h"
#include "../../gameCore/include/itemFactorySystemInterface.h"
#include "../../game/include/itemDynamicQualifierGenerator.h"
#include "../../game/include/movingAgent.h"
#include "../../gameCore/include/transactionSystemInterface.h"
#include "../../gameCore/include/playerSystemInterface.h"
#include "../../gameSpawn/include/spawnSet.h"
#include "../../gameQuest/include/questRuntimeHelper.h"
#include "../../gameTweakDB/include/records/tweakDBItem.h"
#include "../../world/include/worldGlobalNodeRuntimeResolver.h"
#include "../../world/include/gameInstance.h"
#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"
#include "../../worldEntities/include/runtimeSystemEntity.h"
#include "../../worldEntities/include/entityBinding.h"
#include "../../worldStatic/include/staticMarkerNodeInstance.h"
#include "../game/include/itemFactorySystemLowLevel.h"
#include "../worldEntities/include/runtimeSystemEntityAnimation.h"

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

namespace
{
	class QuestPrefabStateListener : public world::IQuestPrefabStateListener
	{
		RTTI_DECLARE_TYPE( QuestPrefabStateListener );
		RED_USE_MEMORY_POOL( ink::PoolInk_System );

	public:
		QuestPrefabStateListener() {}
		QuestPrefabStateListener( const THandle<game::ui::UIObjectsLoaderSystem>& owner ) : m_owner( owner ) {}

	private:
		virtual void OnQuestPrefabStateChanged( const world::GlobalNodeRef& prefabNodeRef, world::QuestPrefabLoadState state ) override
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				if ( state == world::QuestPrefabLoadState::FullyLoaded )
				{
					owner->OnQuestPrefabLoaded( prefabNodeRef );
				}
				else if ( state == world::QuestPrefabLoadState::Failed )
				{
					owner->OnQuestPrefabLoadingFailed( prefabNodeRef );
				}
			}
		}

		WeakHandle<game::ui::UIObjectsLoaderSystem> m_owner;
	};
}

RTTI_BEGIN_TYPE( QuestPrefabStateListener )
	RTTI_PARENT_TYPE( world::IQuestPrefabStateListener );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( UIObjectsLoaderSystem, game, ui );
	RTTI_PARENT_TYPE( game::ui::IUIObjectsLoaderSystem );
RTTI_END_TYPE();

namespace game
{
	namespace ui
	{
		UIObjectsLoaderSystem::UIObjectsLoaderSystem()
			: m_streamingSystem( nullptr )
			, m_entitySystem( nullptr )
			, m_nodeInstanceRegistry( nullptr )
			, m_spawnSet( red::CreateUniquePtr<spawn::Set, ink::PoolInk_System, ink::PoolInk_System>( ink::PoolInk_System(), "game::ui::UIObjectsLoaderSystem" ) )
		{
		}

		UIObjectsLoaderSystem::~UIObjectsLoaderSystem()
		{
		}

		void UIObjectsLoaderSystem::OnQuestPrefabLoaded( const world::GlobalNodeRef & prefabNodeRef )
		{
			red::DynArray<PuppetSceneHandle::Ptr> tickets{ ink::PoolInk_System() };
			red::DynArray<CName> sceneNames{ ink::PoolInk_System() };

			{
				RED_SCOPE_SHARED_LOCK( m_ticketsLock );
				for ( const auto& ticket : m_tickets )
				{
					if ( ticket->IsGlobalPrefabRef( prefabNodeRef ) && ticket->GetState() == PuppetSceneLoadingState::Prefab )
					{
						tickets.PushBack( ticket );
					}
				}
			}

			{
				RED_SCOPE_SHARED_LOCK( m_itemPreviewScenesLock );
				for ( auto it = m_itemPreviewScenes.Begin(); it != m_itemPreviewScenes.End(); ++it )
				{
					CName sceneName = it.Key();
					ItemSceneInfo& sceneInfo = it.Value();
					if ( sceneInfo.m_prefabRef == prefabNodeRef )
					{
						sceneInfo.m_isLoaded = true;
						sceneNames.PushBack( sceneName );
					}
				}
			}

			for ( const auto& ticket : tickets )
			{
				OnQuestPrefabLoaded( ticket );
			}

			for ( const auto& sceneName : sceneNames )
			{
				NotifyItemPreviewSceneLoaded( sceneName );
			}
		}

		void UIObjectsLoaderSystem::OnQuestPrefabLoadingFailed( const world::GlobalNodeRef & prefabNodeRef )
		{
			red::DynArray<PuppetSceneHandle::Ptr> tickets{ ink::PoolInk_System() };
			red::DynArray<CName> sceneNames{ ink::PoolInk_System() };

			{
				RED_SCOPE_SHARED_LOCK( m_ticketsLock );
				for ( const auto& ticket : m_tickets )
				{
					if ( ticket->IsGlobalPrefabRef( prefabNodeRef ) && ticket->GetState() == PuppetSceneLoadingState::Prefab )
					{
						tickets.PushBack( ticket );
					}
				}
			}

			{
				RED_SCOPE_SHARED_LOCK( m_itemPreviewScenesLock );
				for ( auto it = m_itemPreviewScenes.Begin(); it != m_itemPreviewScenes.End(); ++it )
				{
					CName sceneName = it.Key();
					ItemSceneInfo& sceneInfo = it.Value();
					if ( sceneInfo.m_prefabRef == prefabNodeRef )
					{
						sceneInfo.m_isLoaded = false;
						sceneNames.PushBack( sceneName );
					}
				}
			}

			for ( const auto& ticket : tickets )
			{
				ticket->SetErrorType( PuppetSceneLoadingErrorType::PrefabLoadingFailed );
				OnQuestPrefabLoadingFailed( ticket );
			}

			for ( const auto& sceneName : sceneNames )
			{
				NotifyItemPreviewSceneLoadingFailed( sceneName );
			}
		}

		void UIObjectsLoaderSystem::WorldAttached( world::RuntimeScene & scene )
		{
			TBaseClass::WorldAttached( scene );

			m_questPrefabStateListener = CreateHandle<QuestPrefabStateListener>( HandleFromThis<UIObjectsLoaderSystem>() );

			m_animationSystem = scene.GetSystem<world::AnimationSystem>();
			m_entitySystem = scene.GetSystem<world::RuntimeSystemEntity>();
			m_nodeInstanceRegistry = scene.GetSystem< world::INodeInstanceRegistry >();
			m_streamingSystem = scene.GetSystem<world::RuntimeSystemWorldStreaming>();

			if ( m_streamingSystem )
			{
				m_streamingSystem->RegisterPrefabStateListener( m_questPrefabStateListener );
			}

			if ( const auto* gameInstance = GetGameInstance() )
			{
				m_spawnSet->Initialize( *gameInstance );
			}
		}

		void UIObjectsLoaderSystem::WorldDetached( world::RuntimeScene& scene )
		{
			if ( m_streamingSystem )
			{
				m_streamingSystem->UnregisterPrefabStateListener( m_questPrefabStateListener );
			}

			m_entitySystem = nullptr;
			m_nodeInstanceRegistry = nullptr;
			m_streamingSystem = nullptr;

			m_questPrefabStateListener.Reset();

			if ( m_spawnSet->IsInitialized() )
			{
				m_spawnSet->Deinitialize();
			}

			{
				RED_SCOPE_LOCK( m_listenersLock );
				m_listeners.Clear();
			}

			{
				RED_SCOPE_LOCK( m_ticketsLock );
				m_tickets.Clear();
			}

			{
				RED_SCOPE_LOCK( m_itemPreviewScenesLock );
				m_itemPreviewScenes.Clear();
			}

			TBaseClass::WorldDetached( scene );
		}

		void UIObjectsLoaderSystem::RegisterListener( const THandle<IUIObjectsLoaderSystemListener>& listener )
		{
			RED_SCOPE_LOCK( m_listenersLock );
			if ( !m_listeners.Exist( listener ) )
			{
				m_listeners.PushBack( listener );
			}
		}

		void UIObjectsLoaderSystem::UnregisterListener( const THandle<IUIObjectsLoaderSystemListener>& listener )
		{
			RED_SCOPE_LOCK( m_listenersLock );
			m_listeners.RemoveReorder( listener );
		}

		const PuppetSceneHandle::Ptr UIObjectsLoaderSystem::LoadPuppetScene( PuppetSceneInfo&& info )
		{
			PuppetSceneHandle::Ptr ticket;
#ifdef __EMSCRIPTEN__
            std::fprintf(stderr,"CP2077 puppet load probe: phase=request prefab=%u marker=%u record=%u entityTemplate=%u\n",unsigned(info.m_prefabRef.IsDefined()),unsigned(info.m_markerRef.IsDefined()),unsigned(info.m_puppetRecordId.IsValid()),unsigned(info.m_entityTemplatePath.IsValid()));
#endif

			if ( info.m_prefabRef.IsDefined() && info.m_markerRef.IsDefined()
				&& info.m_puppetRecordId.IsValid() && info.m_entityTemplatePath.IsValid() )
			{
				ticket = red::CreateSharedPtr< PuppetSceneHandle >( std::move( info ) );
				ticket->SetState( PuppetSceneLoadingState::Prefab );
				ticket->SetErrorType( PuppetSceneLoadingErrorType::Successful );
				{
					RED_SCOPE_LOCK( m_ticketsLock );
					m_tickets.PushBack( ticket );
				}

				StreamInQuestPrefab( ticket );
			}

			return ticket;
		}

		void UIObjectsLoaderSystem::UnloadPuppetScene( const PuppetSceneHandle::Ptr& ticket )
		{
			if ( ticket )
			{
				const auto puppet = ticket->GetPuppet();
				const auto entity = ticket->GetEntity();

				ticket->SetPuppet( nullptr );
				ticket->SetEntity( nullptr );

				if ( puppet )
				{
					puppet->Internal_UnbindExternalTransformDriver( false );
				}

				DespawnObject( puppet );
				DespawnObject( entity );

				DisableQuestPrefab( ticket->GetGlobalPrefabRef() );

				{
					RED_SCOPE_LOCK( m_ticketsLock );
					m_tickets.RemoveReorder( ticket );
				}
			}
		}

		void UIObjectsLoaderSystem::ReloadPuppet( const PuppetSceneHandle::Ptr& ticket, Bool isMale, const ent::CustomizationAppearances &appearances )
		{
			if ( ticket && ticket->GetState() == PuppetSceneLoadingState::Ready )
			{
				ticket->UpdatePlayerRelatedInfo( isMale, appearances );

				const world::QuestPrefabLoadState prefabState = m_streamingSystem->GetQuestPrefabLoadingState( ticket->GetGlobalPrefabRef() );
				if ( prefabState == world::QuestPrefabLoadState::FullyLoaded )
				{
					const auto puppet = ticket->GetPuppet();
					const auto entity = ticket->GetEntity();

					ticket->SetPuppet( nullptr );
					ticket->SetEntity( nullptr );

					if ( puppet )
					{
						puppet->Internal_UnbindExternalTransformDriver( false );
					}

					DespawnObject( puppet );
					DespawnObject( entity );

					OnQuestPrefabLoaded( ticket );
				}
			}
		}

		const PuppetSceneHandle::Ptr UIObjectsLoaderSystem::GetPuppetScene( CName name ) const
		{
			RED_SCOPE_SHARED_LOCK( m_ticketsLock );
			PuppetSceneHandle::Ptr result;

			for ( const PuppetSceneHandle::Ptr& ticket : m_tickets )
			{
				if ( ticket->GetInfo().m_name == name )
				{
					result = ticket;
					break;
				}
			}

			return result;
		}

		void UIObjectsLoaderSystem::LoadItemPreviewScene( CName sceneName, CName puppetSceneName, const world::NodeRef& prefabRef, const world::NodeRef& markerRef )
		{
			if ( !m_streamingSystem || !prefabRef.IsDefined() || !markerRef.IsDefined() )
			{
				NotifyItemPreviewSceneLoadingFailed( sceneName );
				return;
			}

			const auto& globalPrefabRef = quest::RuntimeHelper::NodeRefToGlobalNodeRef( prefabRef );
			if ( !globalPrefabRef.IsDefined() )
			{
				NotifyItemPreviewSceneLoadingFailed( sceneName );
				return;
			}

			ItemSceneInfo itemSceneInfo;
			if ( IsItemPreviewSceneLoaded(sceneName, itemSceneInfo) )
			{
				if ( itemSceneInfo.m_prefabRef == globalPrefabRef )
				{
					NotifyItemPreviewSceneLoaded( sceneName );
				}
				else
				{
					NotifyItemPreviewSceneLoadingFailed( sceneName );
				}
				return;
			}

			const world::QuestPrefabLoadState prefabState = m_streamingSystem->GetQuestPrefabLoadingState( globalPrefabRef );
			if ( prefabState == world::QuestPrefabLoadState::FullyLoaded )
			{
				NotifyItemPreviewSceneLoaded( sceneName );
			}
			else
			{
				m_streamingSystem->SetQuestPrefabLoadingMode( globalPrefabRef, world::QuestPrefabLoadingMode::ForceLoad );
			}

			{
				RED_SCOPE_LOCK( m_itemPreviewScenesLock );
				m_itemPreviewScenes.Emplace( sceneName, puppetSceneName, globalPrefabRef, markerRef );
			}
		}

		void UIObjectsLoaderSystem::UnloadItemPreviewScene( CName sceneName )
		{
			RED_SCOPE_LOCK( m_itemPreviewScenesLock );

			ItemSceneInfo itemSceneInfo;
			if ( m_itemPreviewScenes.Find( sceneName, itemSceneInfo ) )
			{
				DisableQuestPrefab( itemSceneInfo.m_prefabRef );
				m_itemPreviewScenes.Remove( sceneName );
			}
		}

		Bool UIObjectsLoaderSystem::IsItemPreviewSceneLoaded( CName sceneName ) const
		{
			ItemSceneInfo itemSceneInfo;
			return IsItemPreviewSceneLoaded( sceneName, itemSceneInfo );
		}

		Bool UIObjectsLoaderSystem::IsItemPreviewSceneLoaded( CName sceneName, ItemSceneInfo& itemSceneInfo ) const
		{
			RED_SCOPE_SHARED_LOCK( m_itemPreviewScenesLock );
			return m_itemPreviewScenes.Find( sceneName, itemSceneInfo ) && itemSceneInfo.m_isLoaded;
		}

		Bool UIObjectsLoaderSystem::IsItemPreviewable( const THandle<game::ItemData>& itemData, Bool& isGarment ) const
		{
			if ( itemData )
			{
				const auto& staticData = itemData->GetStaticData();
				if ( staticData && !staticData->entityName().Empty() )
				{
					isGarment = game::ItemFactorySystemLowLevel::GetRequestType( itemData, ItemPoolReservationID::invalid ) == ItemFactorySystemLowLevel::RequestType::Garment;
					return !isGarment;
				}
			}

			return false;
		}

		void UIObjectsLoaderSystem::SpawnItemEntity( CName sceneName, const game::ItemID& itemID )
		{
			auto& itemFactory = GetGameInstance()->GetSystem< game::IItemFactorySystem >();
			auto& transactionSystem = GetGameInstance()->GetSystem< game::ITransactionSystem >();
			auto& playerSystem = GetGameInstance()->GetSystem< game::IPlayerSystem >();

			ItemSceneInfo itemSceneInfo;
			if ( !IsItemPreviewSceneLoaded( sceneName, itemSceneInfo ) )
			{
				NotifyItemEntitySpawningFailed( sceneName, itemID );
				return;
			}

			if ( itemSceneInfo.m_previewItemData && itemSceneInfo.m_previewItemData->GetID() == itemID )
			{
				NotifyItemEntitySpawned( sceneName, itemSceneInfo.m_rootItem );
				return;
			}

			const auto& ownerScene = GetPuppetScene( itemSceneInfo.m_puppetSceneName );
			const auto& owner = ownerScene ? ownerScene->GetPuppet() : nullptr;
			if ( !owner )
			{
				NotifyItemEntitySpawningFailed( sceneName, itemID );
				return;
			}

			const THandle< game::Object > player = playerSystem.GetLocalPlayerCurrentlyControlledObject();
			THandle< ItemData > itemData = transactionSystem.GetItemData( *player, itemID.GetTweakDBID() ).ToHandle();
			if ( !itemData )
			{
				itemData = game::Inventory::CreateItemData( &transactionSystem, game::ItemModParams{ itemID, 1 }, owner, false, true );
			}

			THandle< PreviewItemData > previewItemData = game::CreatePreviewItemData( itemData );

			Bool isGarment;
			if ( !IsItemPreviewable( previewItemData, isGarment ) )
			{
				NotifyItemEntitySpawningFailed( sceneName, itemID );
				return;
			}

			game::IItemFactorySystem::SpawnContext itemSpawnContext;
			if ( !GetMarkerPosition( itemSpawnContext.m_transform, itemSceneInfo.m_markerRef ) )
			{
				NotifyItemEntitySpawningFailed( sceneName, itemID );
				return;
			}

			if ( itemSceneInfo.m_previewItemData && itemSceneInfo.m_previewItemData->GetID().IsValid() )
			{
				DisposePreviewItem( owner->GetEntityID(), itemSceneInfo.m_previewItemData, itemSceneInfo.m_rootItem );
			}

			itemSpawnContext.m_attachToScene = true;
			itemSpawnContext.m_owner = owner;
			itemSpawnContext.m_callback =
				[ weakSelf = WeakHandleFromThis<UIObjectsLoaderSystem>(), sceneName ]( game::IItemFactoryTicket& nativeTicket )
			{
				if ( const auto& self = weakSelf.ToHandle() )
				{
					self->OnItemEntitySpawned( sceneName, nativeTicket );
				}
			};
#ifdef ENT_SPAWN_DEBUG_INFO
			itemSpawnContext.m_spawnDebugInfo = "UIObjectsLoaderSystem::SpawnItemEntity";
#endif

			const auto& ticket = itemFactory.SpawnItem( 
				std::move( itemSpawnContext ),
				previewItemData,
				game::itemDynamicQualifierGenerator::GenerateDynamicQualifier( owner, nullptr, data::GetRecordHandle< data::Item_Record >( itemID.GetTweakDBID() ), previewItemData->GetID() ) );
			{
				RED_SCOPE_LOCK( m_itemPreviewScenesLock );
				itemSceneInfo.m_previewItemData = previewItemData;
				itemSceneInfo.m_rootItem.Reset();
				m_itemPreviewScenes[ sceneName ] = itemSceneInfo;
			}
		}

		void UIObjectsLoaderSystem::UnloadItemEntity( CName sceneName )
		{
			ItemSceneInfo itemSceneInfo;
			if ( IsItemPreviewSceneLoaded( sceneName, itemSceneInfo ) )
			{
				const auto& ownerScene = GetPuppetScene( itemSceneInfo.m_puppetSceneName );
				const auto& owner = ownerScene ? ownerScene->GetPuppet() : nullptr;

				if ( owner && itemSceneInfo.m_previewItemData && itemSceneInfo.m_previewItemData->GetID().IsValid() )
				{
					DisposePreviewItem( owner->GetEntityID(), itemSceneInfo.m_previewItemData, itemSceneInfo.m_rootItem );
					{
						RED_SCOPE_LOCK( m_itemPreviewScenesLock );
						itemSceneInfo.m_previewItemData.Reset();
						itemSceneInfo.m_rootItem.Reset();
						m_itemPreviewScenes[ sceneName ] = itemSceneInfo;
					}
				}
			}
		}

		void UIObjectsLoaderSystem::StreamInQuestPrefab( const PuppetSceneHandle::Ptr& ticket )
		{
			const auto& globalPrefabRef = quest::RuntimeHelper::NodeRefToGlobalNodeRef( ticket->GetInfo().m_prefabRef );
			if ( m_streamingSystem && globalPrefabRef.IsDefined() )
			{
				ticket->SetGlobalPrefabRef( globalPrefabRef );

				const world::QuestPrefabLoadState prefabState = m_streamingSystem->GetQuestPrefabLoadingState( globalPrefabRef );
				if ( prefabState == world::QuestPrefabLoadState::FullyLoaded )
				{
					OnQuestPrefabLoaded( ticket );
				}
				else
				{
					m_streamingSystem->SetQuestPrefabLoadingMode( globalPrefabRef, world::QuestPrefabLoadingMode::ForceLoad );
				}
			}
			else
			{
				ticket->SetErrorType( PuppetSceneLoadingErrorType::PrefabLookupFailed );
				OnQuestPrefabLoadingFailed( ticket );
			}
		}

		void UIObjectsLoaderSystem::DisableQuestPrefab( const world::GlobalNodeRef& prefabNodeRef )
		{
			if ( m_streamingSystem && prefabNodeRef.IsDefined() )
			{
				m_streamingSystem->SetQuestPrefabLoadingMode( prefabNodeRef, world::QuestPrefabLoadingMode::Disable );
			}
		}

		void UIObjectsLoaderSystem::SpawnEntity( const PuppetSceneHandle::Ptr& ticket )
		{
			spawn::Context spawnContext;

			const auto& weakSelf = WeakHandleFromThis<UIObjectsLoaderSystem>();
			const PuppetSceneHandle::WeakPtr weakTicket = ticket;

			spawnContext.m_callback = [ weakSelf, weakTicket ]( const spawn::Ticket& t )
			{
				const auto& self = weakSelf.ToHandle();
				const auto& ticket = weakTicket.Lock();
				if ( self && ticket )
				{
					if ( t.IsSuccess() )
					{
						self->SetSpawnedEntity( t.GetSpawnedEntity(), ticket );
					}
					else
					{
						ticket->SetErrorType( PuppetSceneLoadingErrorType::EntitySpawnFailed );
						self->OnObjectSpawningFailed( ticket );
					}
				}
			};
#ifdef ENT_SPAWN_DEBUG_INFO
			spawnContext.m_spawnDebugInfo = "UIObjectsLoaderSystem::SpawnEntity";
#endif

			if ( GetMarkerPosition( spawnContext.m_transform, ticket->GetInfo().m_markerRef ) &&
				m_spawnSet->IsInitialized() &&
				m_spawnSet->SpawnObject( std::move( spawnContext ), ticket->GetInfo().m_entityTemplatePath ) == nullptr )
			{
				ticket->SetErrorType( PuppetSceneLoadingErrorType::EntitySpawnFailed );
				OnObjectSpawningFailed( ticket );
			}
		}

		void UIObjectsLoaderSystem::SpawnPuppet( const PuppetSceneHandle::Ptr& ticket )
		{
			spawn::Context spawnContext;
			spawnContext.m_genderName = ticket->GetInfo().m_isMale ? game::PuppetHelper::MALE : game::PuppetHelper::FEMALE;

			const auto& weakSelf = WeakHandleFromThis<UIObjectsLoaderSystem>();
			const PuppetSceneHandle::WeakPtr weakTicket = ticket;

			spawnContext.m_callback = [ weakSelf, weakTicket ]( const spawn::Ticket& t )
			{
				const auto& self = weakSelf.ToHandle();
				const auto& ticket = weakTicket.Lock();
				if ( self && ticket )
				{
					if ( t.IsSuccess() )
					{
						self->SetSpawnedPuppet( t.GetSpawnedEntity(), ticket );
					}
					else
					{
						ticket->SetErrorType( PuppetSceneLoadingErrorType::PuppetSpawnFailed );
						self->OnObjectSpawningFailed( ticket );
					}
				}
			};
#ifdef ENT_SPAWN_DEBUG_INFO
			spawnContext.m_spawnDebugInfo = "UIObjectsLoaderSystem::SpawnPuppet";
#endif

			if ( m_spawnSet->IsInitialized() &&
				m_spawnSet->SpawnCustomizedObject( std::move( spawnContext ), ticket->GetInfo().m_puppetRecordId, ticket->GetInfo().m_appearances ) == nullptr )
			{
				ticket->SetErrorType( PuppetSceneLoadingErrorType::PuppetSpawnFailed );
				OnObjectSpawningFailed( ticket );
			}
		}

		void UIObjectsLoaderSystem::SetSpawnedEntity( const ent::EntityPtr & entity, const PuppetSceneHandle::Ptr& ticket )
		{
			if ( entity )
			{
				ticket->SetEntity( entity );
				OnObjectSpawnedSuccessfully( ticket );
			}
			else
			{
				ticket->SetErrorType( PuppetSceneLoadingErrorType::EntitySpawnFailed );
				OnObjectSpawningFailed( ticket );
			}
		}

		void UIObjectsLoaderSystem::SetSpawnedPuppet( const ent::EntityPtr & entity, const PuppetSceneHandle::Ptr& ticket )
		{
			if ( const auto& puppet = Cast< game::Puppet >( entity ) )
			{
				ticket->SetPuppet( puppet );
				puppet->SetIgnoreGlobalTimeDilation( true );
				OnObjectSpawnedSuccessfully( ticket );
			}
			else
			{
				DespawnObject( entity );

				ticket->SetErrorType( PuppetSceneLoadingErrorType::PuppetSpawnFailed );
				OnObjectSpawningFailed( ticket );
			}
		}

		Bool UIObjectsLoaderSystem::GetMarkerPosition( Transform& transform, const world::NodeRef& markerRef ) const
		{
			if ( m_nodeInstanceRegistry )
			{
				const auto& globalMarkerRef = quest::RuntimeHelper::NodeRefToGlobalNodeRef( markerRef );
				if ( globalMarkerRef.IsDefined() )
				{
					if ( const THandle< world::StaticMarkerNodeInstance > nodeInstance = Cast< world::StaticMarkerNodeInstance >( globalMarkerRef.GetNodeInstance( m_nodeInstanceRegistry ) ) )
					{
						if ( nodeInstance->IsAttached() )
						{
							transform = nodeInstance->GetInitialTransform();
							return true;
						}
					}
				}
			}
			return false;
		}

		void UIObjectsLoaderSystem::DespawnObject( const ent::EntityPtr& entity )
		{
			if ( m_spawnSet->IsInitialized() && entity )
			{
				m_spawnSet->DespawnObject( *entity.Get() );
			}
		}

		void UIObjectsLoaderSystem::TryInitialize( const PuppetSceneHandle::Ptr & ticket )
		{
			const auto& puppet = ticket->GetPuppet();
			const auto& entity = ticket->GetEntity();

#ifdef __EMSCRIPTEN__
            static std::atomic<unsigned> probes{0};
            if(probes.fetch_add(1)<24)std::fprintf(stderr,"CP2077 puppet load probe: phase=initialize state=%u puppet=%u entity=%u\n",unsigned(ticket->GetState()),unsigned(bool(puppet)),unsigned(bool(entity)));
#endif
			if ( ticket->GetState() == PuppetSceneLoadingState::Entities && puppet && entity )
			{
				THandle<ent::TargetPointComponent> targetPoint;
				for ( const auto& comp : entity->GetComponents() )
				{
					targetPoint = Cast<ent::TargetPointComponent>( comp );
					if ( targetPoint )
						break;
				}

				if ( targetPoint )
					puppet->Internal_BindExternalTransformDriver( targetPoint, ent::TransformBindingContext() );
				else
					puppet->Internal_BindExternalTransformDriver( *( entity.Get() ), ent::TransformBindingContext() );

				ticket->SetTransformDriver( targetPoint );
				ticket->SetState( PuppetSceneLoadingState::Ready );
#ifdef __EMSCRIPTEN__
                std::fprintf(stderr,"CP2077 puppet ready probe: phase=scene-ready\n");
#endif

				puppet->GetMovingAgent().TeleportLocalSpace( Transform::IDENTITY(), false, false );

				NotifyPuppetSceneLoaded( ticket );
			}
		}

		void UIObjectsLoaderSystem::OnItemEntitySpawned( CName sceneName, game::IItemFactoryTicket& nativeTicket )
		{
			ItemSceneInfo itemSceneInfo;
			if ( !IsItemPreviewSceneLoaded( sceneName, itemSceneInfo ) )
			{
				return;
			}

			if ( itemSceneInfo.m_previewItemData )
			{
				if ( nativeTicket.IsSuccess() )
				{
					const auto& rootItem = nativeTicket.GetRootItem();
					const auto& itemID = rootItem->GetItemID();

					if ( itemSceneInfo.m_previewItemData->GetID() == itemID )
					{
						if ( m_animationSystem )
						{
							m_animationSystem->SetForcedVisible( rootItem->GetEntityID(), true );
						}

						itemSceneInfo.m_rootItem = rootItem;
						NotifyItemEntitySpawned( sceneName, itemSceneInfo.m_rootItem );

						{
							RED_SCOPE_LOCK( m_itemPreviewScenesLock );
							m_itemPreviewScenes[ sceneName ] = itemSceneInfo;
						}
					}
					else
					{
						const auto& ownerScene = GetPuppetScene( itemSceneInfo.m_puppetSceneName );
						if ( const auto& owner = ownerScene ? ownerScene->GetPuppet() : nullptr )
						{
							DisposePreviewItem( owner->GetEntityID(), itemSceneInfo.m_previewItemData, rootItem->WeakHandleFromThis<game::ItemObject>() );
						}
					}
				}
				else
				{
					NotifyItemEntitySpawningFailed( sceneName, itemSceneInfo.m_previewItemData->GetID() );
				}
			}
		}

		void UIObjectsLoaderSystem::OnQuestPrefabLoaded( const PuppetSceneHandle::Ptr& ticket )
		{
			ticket->SetState( PuppetSceneLoadingState::Entities );
			SpawnEntity( ticket );
			SpawnPuppet( ticket );
		}

		void UIObjectsLoaderSystem::OnQuestPrefabLoadingFailed( const PuppetSceneHandle::Ptr& ticket )
		{
			ticket->SetState( PuppetSceneLoadingState::Failed );
			{
				RED_SCOPE_LOCK( m_ticketsLock );
				m_tickets.RemoveReorder( ticket );
			}
			NotifyPuppetSceneLoadingFailed( ticket );
		}

		void UIObjectsLoaderSystem::OnObjectSpawnedSuccessfully( const PuppetSceneHandle::Ptr& ticket )
		{
			TryInitialize( ticket );
		}

		void UIObjectsLoaderSystem::OnObjectSpawningFailed( const PuppetSceneHandle::Ptr& ticket )
		{
			ticket->SetState( PuppetSceneLoadingState::Failed );
			{
				RED_SCOPE_LOCK( m_ticketsLock );
				m_tickets.RemoveReorder( ticket );
			}
			NotifyPuppetSceneLoadingFailed( ticket );
		}

		void UIObjectsLoaderSystem::NotifyPuppetSceneLoaded( const PuppetSceneHandle::Ptr & ticket )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnPuppetSceneLoaded( ticket );
				}
			}
		}

		void UIObjectsLoaderSystem::NotifyPuppetSceneLoadingFailed( const PuppetSceneHandle::Ptr & ticket )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnPuppetSceneLoadingFailed( ticket );
				}
			}
		}

		void UIObjectsLoaderSystem::NotifyItemPreviewSceneLoaded( CName sceneName )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnItemPreviewSceneLoaded( sceneName );
				}
			}
		}

		void UIObjectsLoaderSystem::NotifyItemPreviewSceneLoadingFailed( CName sceneName )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnItemPreviewSceneLoadingFailed( sceneName );
				}
			}
		}

		void UIObjectsLoaderSystem::NotifyItemEntitySpawned( CName sceneName, const WeakHandle< game::ItemObject >& rootItem )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnItemEntitySpawned( sceneName, rootItem );
				}
			}
		}

		void UIObjectsLoaderSystem::NotifyItemEntitySpawningFailed( CName sceneName, const game::ItemID& itemID )
		{
			RED_SCOPE_SHARED_LOCK( m_listenersLock );
			for ( const auto& l : m_listeners )
			{
				if ( const auto& listener = l.ToHandle() )
				{
					listener->OnItemEntitySpawningFailed( sceneName, itemID );
				}
			}
		}

		void UIObjectsLoaderSystem::DisposePreviewItem( const ent::EntityID& ownerID, const THandle< game::PreviewItemData >& previewItemData, const WeakHandle<ItemObject>& rootItem )
		{
			const auto& item = rootItem.ToHandle();
			if ( item && m_animationSystem )
			{
				m_animationSystem->SetForcedVisible( item->GetEntityID(), false );
			}

			auto& itemFactory = GetGameInstance()->GetSystem< game::IItemFactorySystem >();
			if ( !itemFactory.TryToUncacheItem( ownerID, previewItemData, rootItem ) )
			{
				itemFactory.ReleaseItem( ownerID, previewItemData, rootItem );
			}
		}

		ItemSceneInfo::ItemSceneInfo() = default;
		ItemSceneInfo::ItemSceneInfo( CName puppetSceneName, const world::GlobalNodeRef & prefabRef, const world::NodeRef & markerRef )
			: m_puppetSceneName( puppetSceneName )
			, m_prefabRef( prefabRef )
			, m_markerRef( markerRef ) {}

	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif