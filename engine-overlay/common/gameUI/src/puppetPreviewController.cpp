/**
* Copyright (C)2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include <atomic>
#include "../../gameCore/include/characterCustomizationSystemInterface.h"
#include "../../game/include/puppet.h"
#include "characterCustomizationInfoResource.h"
#include "../../redMath/include/interpolation.h"
#include "../world/include/worldGlobalNodeRuntimeResolver.h"
#include "../worldEntities/include/worldNodeInstanceEntity.h"
#include "../worldEntities/include/renderToTextureCameraComponent.h"
#include "../inkCore/include/canvasWidget.h"
#include "../inkCore/include/imageWidget.h"
#include "../inkCore/include/animationProxy.h"

#include "entityPreviewGameObject.h"
#include "puppetPreviewController.h"

#include "../game/include/itemObject.h"
#include "../worldEntities/include/runtimeSystemEntity.h"
#include "../game/include/puppetHelper.h"
#include "../gameCore/include/transactionSystemInterface.h"
#include "../worldEntities/include/runtimeSystemEntityAppearanceChanger.h"
#include "../worldEntities/include/visualComponent.h"
#include "../worldEntities/include/skinTargetComponent.h"
#include "../renderDraw/include/fragmentBuilder.h"
#include "../worldEntities/include/lightComponent.h"
#include "../worldEntities/include/effectSpawnerComponent.h"
#include "../worldEntities/include/meshComponent.h"
#include "../worldEntities/include/skinnedMeshComponent.h"
#include "../resourceMesh/include/mesh.h"
#include "../redMath/include/random.h"
#include "../gameTweakDB/include/records/tweakDBItem.h"
#include "../gameTweakDB/include/records/tweakDBEquipmentArea.h"
#include "../gameTweakDB/include/types/tweakDBEquipmentArea.h"
#include "../gameTweakDB/include/tweakDBHelpers.h"
#include "../inkCore/include/animationDataStructs.h"
#include "../inkCore/include/widgetControllerUtils.h"

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

namespace itempreview
{
	class UIObjectsLoaderSystemListener : public game::ui::IUIObjectsLoaderSystemListener
	{
		RTTI_DECLARE_TYPE( UIObjectsLoaderSystemListener );
		RED_USE_MEMORY_POOL( ink::PoolInk_Controllers );

	public:
		UIObjectsLoaderSystemListener() {}
		UIObjectsLoaderSystemListener( const THandle<game::ui::ItemPreviewGameController>& owner )
			: m_owner( owner ) {}

	private:
		virtual void OnPuppetSceneLoaded( const game::ui::PuppetSceneHandle::Ptr& ticket ) override
		{
		}

		virtual void OnPuppetSceneLoadingFailed( const game::ui::PuppetSceneHandle::Ptr& ticket ) override
		{
		}

		virtual void OnItemPreviewSceneLoaded( CName sceneName ) override
		{
		}

		virtual void OnItemPreviewSceneLoadingFailed( CName sceneName ) override
		{
		}

		virtual void OnItemEntitySpawned( CName sceneName, const WeakHandle< game::ItemObject >& rootItem )
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnItemEntitySpawned( sceneName, rootItem );
			}
		}

		virtual void OnItemEntitySpawningFailed( CName sceneName, const game::ItemID& itemID )
		{
			if ( const auto& owner = m_owner.ToHandle() )
			{
				owner->OnItemEntitySpawningFailed( sceneName, itemID );
			}
		}

		WeakHandle<game::ui::ItemPreviewGameController> m_owner;
	};
}

RTTI_BEGIN_TYPE_IN_NAMESPACE( UIObjectsLoaderSystemListener, itempreview );
	RTTI_PARENT_TYPE( game::ui::IUIObjectsLoaderSystemListener );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetPreview_ReadyToBeDisplayed, game, ui );
	RTTI_PARENT_TYPE( red::Event );
	RTTI_PROPERTY( m_isMale );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetPreview_SetCameraSetupEvent, game, ui );
	RTTI_PARENT_TYPE( red::Event );
	RTTI_PROPERTY( m_setupIndex );
	RTTI_PROPERTY( m_slotName );
	RTTI_PROPERTY( m_delayed );
RTTI_END_TYPE();

RTTI_BEGIN_ABSTRACT_TYPE_IN_NAMESPACE( PreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::MenuGameController );
	RTTI_SCRIPT_ALIAS( "inkPreviewGameController" );
	RTTI_PROPERTY( m_yawSpeed ).editable();
	RTTI_PROPERTY( m_yawDefault ).editable();
	RTTI_NATIVE_FUNCTION( "Rotate", funcRotate );
	RTTI_NATIVE_FUNCTION( "RotateVector", funcRotateVector );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( ItemPreview_WaitOneFrame, game, ui );
	RTTI_PARENT_TYPE( red::Event );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( ItemPreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::PreviewGameController );
	RTTI_SCRIPT_ALIAS( "inkItemPreviewGameController" );
	RTTI_PROPERTY( m_sceneName ).editable();
	RTTI_PROPERTY( m_cameraRef ).editable();
	RTTI_NATIVE_FUNCTION( "PreviewItem", funcPreviewItem );
	RED_EVENT_CONNECTOR( OnAnimComponentRegistered );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetPreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::PreviewGameController );
	RTTI_SCRIPT_ALIAS( "inkPuppetPreviewGameController" );
	RTTI_PROPERTY( m_cameraController ).editable();
	RTTI_NATIVE_FUNCTION( "GetGamePuppet", funcGetGamePuppet );
	RED_EVENT_CONNECTOR( OnSetCameraSetupEvent );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( GenderSelectionPuppetPreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::PuppetPreviewGameController );
	RTTI_SCRIPT_ALIAS( "inkGenderSelectionPuppetPreviewGameController" );
	RTTI_PROPERTY( m_sceneName ).editable();
	RTTI_PROPERTY( m_cameraRef ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CharacterCreationPuppetPreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::PuppetPreviewGameController );
	RTTI_SCRIPT_ALIAS( "inkCharacterCreationPuppetPreviewGameController" );
	RTTI_PROPERTY( m_maleSceneName ).editable();
	RTTI_PROPERTY( m_femaleSceneName ).editable();
	RTTI_PROPERTY( m_maleCamera01Ref ).editable();
	RTTI_PROPERTY( m_femaleCamera01Ref ).editable();
	RTTI_PROPERTY( m_root ).editable();
	RTTI_PROPERTY( m_image ).editable();
	RTTI_PROPERTY( m_animLib ).editable();
	RTTI_PROPERTY( m_animName ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( InventoryPuppetPreviewGameController, game, ui );
	RTTI_PARENT_TYPE( game::ui::PuppetPreviewGameController );
	RTTI_SCRIPT_ALIAS( "inkInventoryPuppetPreviewGameController" );
	RTTI_PROPERTY( m_sceneName ).editable();
	RTTI_PROPERTY( m_cameraRef ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetPreviewCameraSetup, game, ui );
	RTTI_PROPERTY( m_slotName ).editable();
	RTTI_PROPERTY( m_cameraZoom ).editable();
	RTTI_PROPERTY( m_interpolationTime ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( PuppetPreviewCameraController, game, ui );
	RTTI_PROPERTY( m_cameraSetup ).editable().resizable();
	RTTI_PROPERTY( m_activeSetup ).editable();
	RTTI_PROPERTY( m_transitionDelay ).editable();
RTTI_END_TYPE();

namespace game
{
	namespace ui
	{

		//////////////////////////////////////////////////////////////////////////

		PreviewGameController::PreviewGameController() : m_yawSpeed( 1.25f ), m_yawDefault( -125.f ) {}

		Bool PreviewGameController::InitCamera()
		{
			const auto& camera = AcquireCamera( m_cameraRef );
			m_camera = camera;
#ifdef __EMSCRIPTEN__
            std::fprintf(stderr,"CP2077 puppet camera probe: referenceDefined=%u acquired=%u\n",unsigned(m_cameraRef.IsDefined()),unsigned(bool(camera)));
#endif
			return camera != nullptr;
		}

		void PreviewGameController::EnableCamera( Bool enable )
		{
			if ( const auto& camera = m_camera.ToHandle() )
			{
				camera->GetCamera()->Enable( enable );
			}
		}

		const THandle<EntityPreviewGameObject> PreviewGameController::AcquireCamera( const world::NodeRef & cameraRef ) const
		{
			THandle< EntityPreviewGameObject > camera;
			auto nodeInstanceRegistry = m_sceneService->GetSystem< world::INodeInstanceRegistry >();
			if ( cameraRef.IsDefined() )
			{
				const world::GlobalNodeRef resolvedNodeRef = cameraRef.Resolve( world::GlobalNodeID::GetRoot() );
				if ( resolvedNodeRef.IsDefined() )
				{
					if ( const THandle< world::EntityNodeInstance > nodeInstance = Cast< world::EntityNodeInstance >( resolvedNodeRef.GetNodeInstance( nodeInstanceRegistry ) ) )
					{
						if ( nodeInstance->IsAttached() )
						{
							camera = Cast<EntityPreviewGameObject>( nodeInstance->GetCreatedEntity() );
						}
					}
				}
			}
			return camera;
		}

		const THandle<EntityPreviewGameObject> PreviewGameController::GetCamera() const
		{
			return m_camera.ToHandle();
		}


		void PreviewGameController::funcRotate( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
		{
			GET_PARAMETER( Float, value, 0.f );
			FINISH_PARAMETERS;

			Rotate( value );
		}

		void PreviewGameController::funcRotateVector( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
		{
			GET_PARAMETER( Vector3, value, Vector3( 0.f, 0.f, 0.f ) );
			FINISH_PARAMETERS;

			RotateVector( value );
		}

		//////////////////////////////////////////////////////////////////////////
		//////////////////////////////////////////////////////////////////////////

		ItemPreviewGameController::ItemPreviewGameController()
			: m_isInForcedHighestQuality( true )
			, m_dataIsReady( false )
			, m_isVertical( false )
			, m_makeHorizontal( false )
			, m_initialItemBBox( Box::EMPTY() )
		{
		}

		void ItemPreviewGameController::OnItemEntitySpawned( CName sceneName, const WeakHandle< game::ItemObject >& rootItem )
		{
			if ( m_sceneName == sceneName )
			{
				m_isInForcedHighestQuality = false;
				m_dataIsReady = false;
				m_rootItem = rootItem;
				m_initialItemBBox = Box::EMPTY();
			}
		}

		void ItemPreviewGameController::OnItemEntitySpawningFailed( CName sceneName, const game::ItemID& itemID )
		{
			if ( m_sceneName == sceneName )
			{
				EnableCamera( false );
			}
		}

		void ItemPreviewGameController::Initialize( const InitializeContext & initContext )
		{
			IUIObjectsLoaderSystem* uiObjectsLoaderSystem = GetSystem<IUIObjectsLoaderSystem>();
			if ( uiObjectsLoaderSystem->IsItemPreviewSceneLoaded( m_sceneName ) && InitCamera() )
			{
				m_listener = CreateHandle<itempreview::UIObjectsLoaderSystemListener>( HandleFromThis<ItemPreviewGameController>() );
			}

			TBaseClass::Initialize( initContext );
		}

		void ItemPreviewGameController::Uninitialize( const UninitializeContext & uninitContext )
		{
			TBaseClass::Uninitialize( uninitContext );

			EnableCamera( false );
			GetSystem<IUIObjectsLoaderSystem>()->UnregisterListener( m_listener );
		}

		void ItemPreviewGameController::Update( const UpdateContext& updateContext )
		{
			TBaseClass::Update( updateContext );

			if ( const auto& item = m_rootItem.ToHandle() )
			{
				if ( !m_isInForcedHighestQuality )
				{
					GetSystem<IUIObjectsLoaderSystem>()->UnregisterListener( m_listener );

					m_isInForcedHighestQuality = true;
					QueueEvent( CreateHandle<ItemPreview_WaitOneFrame>() );
				}
				else if ( m_dataIsReady )
				{
					Box bounds;
					ComputeItemBounds( item, bounds );
					SetCameraPosition( bounds );
					SetCameraDistance( bounds );
				}
			}
		}

		Bool ItemPreviewGameController::NeedsUpdate() const
		{
			return TBaseClass::NeedsUpdate() || !m_isInForcedHighestQuality || m_dataIsReady;
		}

#ifdef ITEM_PREVIEW_DEBUG
		void ItemPreviewGameController::OnRenderDebug( const RenderDebugContext& context )
		{
			if ( const auto& item = m_rootItem.ToHandle() )
			{
				rend::FragmentBuilder fb( context.debugDrawer );

				Box resultBox = Box::EMPTY();
				const auto& components = item->GetComponents();
				for ( auto& comp : components )
				{
					THandle<ent::IPlacedComponent> placed = Cast<ent::IPlacedComponent>( comp );
					if ( !placed )
						continue;

					Box localBounds = Box::EMPTY();
					if ( THandle<ent::MeshComponent> meshComp = Cast<ent::MeshComponent>( comp ) )
					{
						if ( meshComp->GetMesh() ) // safety check for weapon parts with improper paths
						{
							localBounds = meshComp->GetMesh()->GetBoundingBox();
						}
					}
					else if ( THandle<ent::SkinnedMeshComponent> meshComp = Cast<ent::SkinnedMeshComponent>( comp ) )
					{
						if ( meshComp->GetMesh() ) // safety check for weapon parts with improper paths
						{
							localBounds = meshComp->GetMesh()->GetBoundingBox();
						}
					}

					if ( !localBounds.IsEmpty() )
					{
						localBounds = placed->GetLocalTransform().TransformBox( localBounds );
						resultBox.AddBox( localBounds );

						fb.AddWireBox( item->GetWorldTransform().TransformBox( localBounds ) );
					}
				}

				if ( !resultBox.IsEmpty() )
				{
					fb.SetColor( Color::DARK_RED() );
					fb.AddWireBox( item->GetWorldTransform().TransformBox( resultBox ) );
				}
			}
		}
#endif // ITEM_PREVIEW_DEBUG

		void ItemPreviewGameController::OnAnimComponentRegistered( const ItemPreview_WaitOneFrame& evt )
		{
			if ( const auto& item = m_rootItem.ToHandle() )
			{
				world::RuntimeSystemEntity* entitySystem = m_sceneService->GetSystem<world::RuntimeSystemEntity>();
				entitySystem->ScheduleEntitySetLOD( item, ent::EntityLOD::ForcedHighestQuality );

				Box bounds;
				ComputeItemBounds( item, bounds );

				//InitRotation();
				SetCameraPosition( bounds );
				SetCameraDistance( bounds );
				EnableCamera( true );

				m_dataIsReady = true;
			}
		}

		void ItemPreviewGameController::InitRotation()
		{
			if ( const auto& item = m_rootItem.ToHandle() )
			{
				const auto& vecSize = m_initialItemBBox.CalcSize();
				m_isVertical = vecSize.Z >= vecSize.X && vecSize.Z >= vecSize.Y;
				if ( m_isVertical )
				{
					const THandle< data::Item_Record >& record = data::GetRecord<data::Item_Record>( item->GetItemID().GetTweakDBID() ).ToHandle();
					const THandle< data::EquipmentArea_Record >& equipArea = record ? record->equipAreaGet().ToHandle() : nullptr;
					m_makeHorizontal = equipArea &&
						equipArea->type != data::EquipmentArea::Consumable &&
						equipArea->type != data::EquipmentArea::QuickSlot;
				}
				else
				{
					m_makeHorizontal = false;
				}

				EulerAngles newAngles = item->GetLocalOrientation().ToEulerAngles();
				newAngles.Yaw = m_yawDefault;
				newAngles.Pitch = m_makeHorizontal ? -90.f : 0.f;
				item->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void ItemPreviewGameController::Rotate( float value )
		{
			if ( const auto& item = m_rootItem.ToHandle() )
			{
				EulerAngles newAngles = item->GetLocalOrientation().ToEulerAngles();
				newAngles.Yaw += m_yawSpeed * value;
				item->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void ItemPreviewGameController::RotateVector( Vector3 value )
		{
			if ( const auto& item = m_rootItem.ToHandle() )
			{
				EulerAngles newAngles = item->GetLocalOrientation().ToEulerAngles();
				if ( m_makeHorizontal )
				{
					newAngles.Roll += m_yawSpeed * -value.Z;
				}
				else if ( m_isVertical )
				{
					newAngles.Yaw += m_yawSpeed * value.Z;
				}
				else
				{
					newAngles.Roll += m_yawSpeed * value.Y;
					newAngles.Pitch += m_yawSpeed * value.X;
					newAngles.Yaw += m_yawSpeed * value.Z;
				}
				item->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void ItemPreviewGameController::PreviewItem( const game::ItemID& itemID )
		{
			if ( itemID.IsValid() )
			{
				IUIObjectsLoaderSystem* uiObjectsLoaderSystem = GetSystem<IUIObjectsLoaderSystem>();
				uiObjectsLoaderSystem->RegisterListener( m_listener );
				uiObjectsLoaderSystem->SpawnItemEntity( m_sceneName, itemID );
			}
			else
			{
				ClearPreview();
			}
		}

		void ItemPreviewGameController::ClearPreview()
		{
			GetSystem<IUIObjectsLoaderSystem>()->UnloadItemEntity( m_sceneName );
			m_rootItem.Reset();
			m_isInForcedHighestQuality = true;
			m_dataIsReady = false;
			m_initialItemBBox = Box::EMPTY();
		}

		void ItemPreviewGameController::ComputeItemBounds( const ent::EntityPtr& entity, Box& outBounds )
		{
			outBounds.Clear();

			const auto& components = entity->GetComponents();

			Box entityLocalBounds = Box::EMPTY();

			for ( auto& comp : components )
			{
				THandle<ent::IPlacedComponent> placed = Cast<ent::IPlacedComponent>( comp );
				if ( !placed )
					continue;

				Box localBounds = Box::EMPTY();
				if ( THandle<ent::MeshComponent> meshComp = Cast<ent::MeshComponent>( comp ) )
				{
					if( meshComp->GetMesh() ) // safety check for weapon parts with improper paths
					{
						static const auto placeholder_path1 = RED_CONST_RESOURCEPATH( "base\\weapons\\weapon_placeholder.mesh" );
						static const auto placeholder_path2 = RED_CONST_RESOURCEPATH( "base\\weapons\\weapon_placeholder_small.mesh" );
						if( meshComp->GetMeshReference().GetPath() != placeholder_path1 && meshComp->GetMeshReference().GetPath() != placeholder_path2 )
						{
							localBounds = meshComp->GetMesh()->GetBoundingBox();
						}
					}
				}
				else if ( THandle<ent::SkinnedMeshComponent> meshComp = Cast<ent::SkinnedMeshComponent>( comp ) )
				{
					if ( meshComp->GetMesh() ) // safety check for weapon parts with improper paths
					{
						localBounds = meshComp->GetMesh()->GetBoundingBox();
					}
				}

				if ( !localBounds.IsEmpty() )
				{
					localBounds = placed->GetLocalTransform().TransformBox( localBounds );
					entityLocalBounds.AddBox( localBounds );
				}
			}

			outBounds = entity->GetWorldTransform().TransformBox( entityLocalBounds );

			// Setup initial bounding box. Helps with calculations when object is rotated and it's bbox is changed.
			if ( m_initialItemBBox.IsEmpty() && !outBounds.IsEmpty() && outBounds.CalcSize().Mag3() > 0.01f )
			{
				// HACK: Entity is not properly initialized when spawned. There is currently no signal/interface
				// to use to ensure that
				static Uint8 refreshCount_HACK = 0;
				static const Uint8 maxTicks = 15;
				if ( refreshCount_HACK > maxTicks )
				{
					m_initialItemBBox = outBounds;
					refreshCount_HACK = 0;
					InitRotation();
				}
				else
				{
					refreshCount_HACK++;
				}
			}
		}

		void ItemPreviewGameController::SetCameraPosition( const Box &region )
		{
			if ( const auto& previewObject = GetCamera() )
			{
				const auto& center = region.CalcCenter();
				previewObject->MoveTarget( center, true );
			}
		}

		void ItemPreviewGameController::SetCameraDistance( const Box &region )
		{
			if ( const auto& previewObject = GetCamera() )
			{
				if ( const auto& camera = Cast<ent::RenderToTextureCameraComponent>( previewObject->GetCamera() ) )
				{
					if ( const auto& texture = camera->GetDynamicTextureHandle() )
					{
						const auto& fov = camera->GetFOV();
						const auto& sizeDesc = texture->GetSizeDesc();
						const auto& vecSize = m_initialItemBBox.CalcSize();

						Float size, aspect;
						if ( vecSize.Z >= vecSize.X && vecSize.Z >= vecSize.Y )
						{
							size = vecSize.Z;
							aspect = 1.f;
						}
						else
						{
							size = Max( vecSize.X, vecSize.Y );
							aspect = sizeDesc.width / ( Float )sizeDesc.height;
						}

						Float dist = size / ( 2 * aspect * MTan( ( fov * RED_PI ) / 360 ) );
						dist *= 1.4f; // arbitrary value, tweak if needed

						previewObject->Zoom( dist, true );
					}
				}
			}
		}

		void ItemPreviewGameController::funcPreviewItem( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
		{
			GET_PARAMETER( game::ItemID, itemID, game::ItemID() );
			FINISH_PARAMETERS;

			PreviewItem( itemID );
		}

		//////////////////////////////////////////////////////////////////////////

		void PuppetPreviewGameController::Initialize( const InitializeContext& initContext )
		{
			TBaseClass::Initialize( initContext );

			IUIObjectsLoaderSystem* uiObjectsLoaderSystem = GetSystem<IUIObjectsLoaderSystem>();
			m_ticket = uiObjectsLoaderSystem->GetPuppetScene( m_sceneName );
#ifdef __EMSCRIPTEN__
            std::fprintf(stderr,"CP2077 puppet load probe: phase=controller-init ticket=%u state=%d\n",unsigned(bool(m_ticket)),m_ticket?int(m_ticket->GetState()):-1);
#endif
			if ( m_ticket && m_ticket->GetState() == PuppetSceneLoadingState::Ready && InitCamera() )
			{
				CallEvent( RED_NAME_CONSTEXPR( "OnPuppetAttached" ) );

				InitCameraSetup();
				InitRotation();

				EnableCamera( true );
			}
		}

		void PuppetPreviewGameController::Uninitialize( const UninitializeContext & uninitContext )
		{
			TBaseClass::Uninitialize( uninitContext );

			EnableCamera( false );
			m_ticket = nullptr;
		}

		void PuppetPreviewGameController::Update( const UpdateContext& updateContext )
		{
			TBaseClass::Update( updateContext );
			m_cameraController.Update( updateContext.tickInfo.m_realTimeDelta );
		}

		Bool PuppetPreviewGameController::NeedsUpdate() const
		{
			return TBaseClass::NeedsUpdate() || m_cameraController.IsInTransition() || m_cameraController.IsInPendingTransition();
		}

		void PuppetPreviewGameController::InitCameraSetup()
		{
			if ( const auto& entity = m_ticket->GetEntity() )
			{
				red::DynArray< THandle< ent::SlotComponent > > slotComponents{ red::PoolFrame() };
				game::engineExt::FindComponentsByType< ent::SlotComponent >( entity.Get(), slotComponents );

				const PuppetPreviewCameraController::CameraCallback callback =
					[ weakSelf = WeakHandleFromThis< PuppetPreviewGameController >() ]( Uint32 index, const PuppetPreviewCameraSetup& setup )
				{
					if ( const auto& self = weakSelf.ToHandle() )
					{
						self->CallEvent( RED_NAME_CONSTEXPR( "OnSetCameraSetupEvent" ), index, setup.m_slotName );
					}
				};

				m_cameraController.InitCameraSetup( slotComponents, GetCamera(), callback );
			}
		}

		void PuppetPreviewGameController::InitRotation()
		{
			if ( const auto& transformDriver = m_ticket->GetTransformDriver() )
			{
				EulerAngles newAngles = transformDriver->GetLocalOrientation().ToEulerAngles();
				newAngles.Yaw = m_yawDefault;
				transformDriver->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void PuppetPreviewGameController::Rotate( float value )
		{
			if ( const auto& transformDriver = m_ticket ? m_ticket->GetTransformDriver() : nullptr )
			{
				EulerAngles newAngles = transformDriver->GetLocalOrientation().ToEulerAngles();
				newAngles.Yaw += m_yawSpeed * value;
				transformDriver->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void PuppetPreviewGameController::RotateVector( Vector3 value )
		{
			if ( const auto& transformDriver = m_ticket ? m_ticket->GetTransformDriver() : nullptr )
			{
				EulerAngles newAngles = transformDriver->GetLocalOrientation().ToEulerAngles();
				newAngles.Roll += m_yawSpeed * value.Y;
				newAngles.Pitch += m_yawSpeed * value.X;
				newAngles.Yaw += m_yawSpeed * value.Z;
				transformDriver->SetLocalOrientation( newAngles.ToQuat() );
			}
		}

		void PuppetPreviewGameController::OnSetCameraSetupEvent( const PuppetPreview_SetCameraSetupEvent& evt )
		{
			if ( evt.GetSlotName().Empty() )
			{
				m_cameraController.SetCameraSetup( evt.GetSetupIndex(), evt.IsDelayed() );
			}
			else
			{
				m_cameraController.SetCameraSetup( evt.GetSlotName(), evt.IsDelayed() );
			}
		}

		void PuppetPreviewGameController::funcGetGamePuppet( CScriptStackFrame & stack, void * result, const rtti::IType* resultType )
		{
			FINISH_PARAMETERS;
			RETURN_WEAK_HANDLE( game::Puppet, m_ticket ? m_ticket->GetPuppet() : nullptr );
		}

		//////////////////////////////////////////////////////////////////////////

		void GenderSelectionPuppetPreviewGameController::Initialize( const InitializeContext& initContext )
		{
			TBaseClass::Initialize( initContext );

			if ( m_ticket && GetCamera() )
			{
				SetDefaultCustomization();
			}
		}

		void GenderSelectionPuppetPreviewGameController::Uninitialize( const UninitializeContext & uninitContext )
		{
			if ( m_ticket )
			{
				ResetDefaultCustomization();
			}

			TBaseClass::Uninitialize( uninitContext );
		}

		void GenderSelectionPuppetPreviewGameController::SetDefaultCustomization()
		{
			auto* characterCustomizationSystem = GetSystem<ICharacterCustomizationSystem>();
			const auto& state = characterCustomizationSystem->GetState();
			const auto& puppet = m_ticket->GetPuppet();
			if ( state && puppet )
			{
				Bool isMale = puppet->GetResolvedGenderName() == PuppetHelper::MALE;
				Bool isFPP = game::PuppetHelper::GetCameraPerspectiveName( puppet ) == PuppetHelper::FPP;
				const auto& lifepath = state->GetLifePath();

				const SetDefaultCallback callback =
					[ weakSelf = WeakHandleFromThis<GenderSelectionPuppetPreviewGameController>(), isMale ]()
				{
					if ( const auto& self = weakSelf.ToHandle() )
					{
                        const auto remaining=self->m_defaultRequestsCount.Decrement();
#ifdef __EMSCRIPTEN__
                        std::fprintf(stderr,"CP2077 puppet customization probe: phase=completed remaining=%u\n",unsigned(remaining));
#endif
                        if ( remaining == 0 )
						{
							self->QueueEvent( CreateHandle<PuppetPreview_ReadyToBeDisplayed>( isMale ) );
						}
					}
				};

				ent::CustomizationAppearances appearances{ ink::PoolInk_Controllers() };
				appearances.PushBack( characterCustomizationSystem->GetFallbackBodyCustomization( isMale, isFPP, lifepath ) );
				appearances.PushBack( characterCustomizationSystem->GetFallbackHeadCustomization( isMale, isFPP, lifepath ) );
				appearances.PushBack( characterCustomizationSystem->GetFallbackArmsCustomization( isMale, isFPP, lifepath ) );

				SetDefaultCustomization( puppet, appearances, callback );
			}
		}

		void GenderSelectionPuppetPreviewGameController::SetDefaultCustomization( const THandle< ent::Entity >& entity, const ent::CustomizationAppearances& appearances, const SetDefaultCallback& callback )
		{
			auto* entityAppearanceChanger = m_sceneService->GetSystem<world::RuntimeSystemEntityAppearanceChanger>();
#ifdef __EMSCRIPTEN__
            std::fprintf(stderr,"CP2077 puppet customization probe: phase=schedule changer=%u appearances=%llu\n",unsigned(bool(entityAppearanceChanger)),static_cast<unsigned long long>(appearances.Size()));
#endif
			if ( entityAppearanceChanger )
			{
				for ( const auto& appearance : appearances )
				{
					world::EntityAppearanceChangeParameter params = {};
					params.appearanceToAdd.resource = appearance.first.GetPath();
					params.appearanceToAdd.name = appearance.second;
					params.entity = entity;

					m_defaultRequestsCount.Increment();
					entityAppearanceChanger->ScheduleAppearanceChange( params, callback );
				}
			}
		}

		void GenderSelectionPuppetPreviewGameController::ResetDefaultCustomization()
		{
			auto* characterCustomizationSystem = GetSystem<ICharacterCustomizationSystem>();
			const auto& state = characterCustomizationSystem->GetState();
			const auto& puppet = m_ticket->GetPuppet();
			if ( state && puppet )
			{
				Bool isMale = puppet->GetResolvedGenderName() == PuppetHelper::MALE;
				Bool isFPP = game::PuppetHelper::GetCameraPerspectiveName( puppet ) == PuppetHelper::FPP;
				const auto& lifepath = state->GetLifePath();

				ent::CustomizationAppearances appearances{ ink::PoolInk_Controllers() };
				appearances.PushBack( characterCustomizationSystem->GetFallbackBodyCustomization( isMale, isFPP, lifepath ) );
				appearances.PushBack( characterCustomizationSystem->GetFallbackHeadCustomization( isMale, isFPP, lifepath ) );
				appearances.PushBack( characterCustomizationSystem->GetFallbackArmsCustomization( isMale, isFPP, lifepath ) );

				ResetDefaultCustomization( puppet, appearances );
			}
		}

		void GenderSelectionPuppetPreviewGameController::ResetDefaultCustomization( const THandle< ent::Entity >& entity, const ent::CustomizationAppearances& appearances )
		{
			auto* entityAppearanceChanger = m_sceneService->GetSystem<world::RuntimeSystemEntityAppearanceChanger>();
			if ( entityAppearanceChanger )
			{
				for ( const auto& appearance : appearances )
				{
					world::EntityAppearanceChangeParameter params = {};
					params.appearanceToRemove.resource = appearance.first.GetPath();
					params.appearanceToRemove.name = appearance.second;
					params.entity = entity;

					entityAppearanceChanger->ScheduleAppearanceChange( params );
				}
			}
		}

		//////////////////////////////////////////////////////////////////////////

		void CharacterCreationPuppetPreviewGameController::Initialize( const InitializeContext & initContext )
		{
			auto* characterCustomizationSystem = GetSystem<ICharacterCustomizationSystem>();
			const auto& state = characterCustomizationSystem->GetState();

			if ( state )
			{
				m_sceneName = state->IsBodyGenderMale() ? m_maleSceneName : m_femaleSceneName;
			}

			if ( auto image = m_image.Get() )
			{
				image->SetVisible( false );
			}

			if ( const auto& itemInstance = SpawnFromExternal( m_animLib.m_widgetLibrary.GetPath(), m_animLib.m_widgetItem, m_root.Get() ) )
			{
				if ( const auto& animRoot = itemInstance->GetRootWidget() )
				{
					animRoot->SetAnchor( ink::EAnchor::Fill );
					animRoot->SetAnchorPoint( Vector2::HALVES() );
					animRoot->SetVisible( false );
					m_animRoot = animRoot;
				}
			}

			TBaseClass::Initialize( initContext );

			if ( state && m_ticket && GetCamera() )
			{
				const UiCallback callback = [ weakSelf = WeakHandleFromThis<CharacterCreationPuppetPreviewGameController>() ]( CharacterCustomizationUiState state )
				{
					if ( const auto& self = weakSelf.ToHandle() )
					{
						self->OnCharacterCustomizationUiStateChanged( state );
					}
				};

				characterCustomizationSystem->SetCustomizationOptionsResource( m_ticket->GetPuppet(), state->IsBodyGenderMale(), callback );
			}
		}

		void CharacterCreationPuppetPreviewGameController::Uninitialize( const UninitializeContext & uninitContext )
		{
			ICharacterCustomizationSystem* characterCustomizationSystem = GetSystem<ICharacterCustomizationSystem>();
			if ( m_ticket )
			{
				characterCustomizationSystem->ResetCustomizationOptionsResource();
			}

			TBaseClass::Uninitialize( uninitContext );
		}

		Bool CharacterCreationPuppetPreviewGameController::InitCamera()
		{
			THandle<EntityPreviewGameObject> camera;

			if ( const auto& state = GetSystem<ICharacterCustomizationSystem>()->GetState() )
			{
				camera = AcquireCamera( state->IsBodyGenderMale() ? m_maleCamera01Ref : m_femaleCamera01Ref );
				m_camera = camera;
			}

			return camera != nullptr;
		}

		void CharacterCreationPuppetPreviewGameController::OnCharacterCustomizationUiStateChanged( CharacterCustomizationUiState state )
		{
			if ( state == CharacterCustomizationUiState::ACR_None )
			{
				if ( auto image = m_image.Get() )
				{
					image->SetVisible( true );
				}

				if ( const auto& animRoot = m_animRoot.ToHandle() )
				{
					animRoot->SetVisible( false );
				}

				if ( m_animProxy )
				{
					m_animProxy->Stop();
					m_animProxy.Reset();
				}
			}
			else if ( state == CharacterCustomizationUiState::ACR_Init || state == CharacterCustomizationUiState::ACR_Preset )
			{
				if ( !m_animProxy )
				{
					if ( auto image = m_image.Get() )
					{
						image->SetVisible( false );
					}

					if ( const auto& animRoot = m_animRoot.ToHandle() )
					{
						animRoot->SetVisible( true );

						ink::anim::PlaybackOptions options;
						options.m_loopType = ink::anim::LoopType::Cycle;

						m_animProxy = ink::PlayLibraryAnimation( animRoot, animRoot->GetWidgetLibrary(), m_animName, options );
					}
				}
			}
		}

		void PuppetPreviewCameraController::InitCameraSetup( const red::DynArray< THandle< ent::SlotComponent > >& slotComponents, const THandle< EntityPreviewGameObject >& camera, const CameraCallback& callback )
		{
			if ( !camera )
				return;

			for ( auto& setup : m_cameraSetup )
			{
				for ( const auto& comp : slotComponents )
				{
					const ent::SlotId& id = comp->Find( setup.m_slotName );
					if ( id != -1 )
					{
						WorldTransform transform = WorldTransform::IDENTITY();
						if ( comp->GetTransformWS( id, transform ) )
						{
							setup.m_slotPos = transform.GetPosition().AsVector3();

							if ( setup.m_cameraZoom < 0.f )
							{
								setup.m_cameraZoom = camera->GetCameraSettings().m_zoomDefault;
							}

							break;
						}
					}
				}
			}

			m_callback = callback;

			if ( !m_cameraSetup.Empty() )
			{
				m_activeSetup = m_activeSetup < m_cameraSetup.Size() ? m_activeSetup : 0;

				auto& setup = m_cameraSetup[ m_activeSetup ];

				if ( m_callback )
				{
					m_callback( m_activeSetup, setup );
				}

				camera->MoveTarget( setup.m_slotPos, true );
				camera->Zoom( setup.m_cameraZoom, true );

				m_camera = camera;
			}
		}

		void PuppetPreviewCameraController::SetCameraSetup( CName slotName, Bool delayed /*= false */ )
		{
			for ( Uint32 i = 0; i < m_cameraSetup.Size(); i++ )
			{
				if ( m_cameraSetup[ i ].m_slotName == slotName )
				{
					SetCameraSetup( i, delayed );
					break;
				}
			}
		}

		void PuppetPreviewCameraController::SetCameraSetup( Uint32 index, Bool delayed /*= false */ )
		{
			const auto& camera = m_camera.ToHandle();
			if ( camera && index < m_cameraSetup.Size() )
			{
				if ( delayed )
				{
					if ( m_pendingSetup != index )
					{
						m_isInPendingTransition = true;

						m_pendingSetup = index;
						m_delay = m_transitionDelay;
					}
				}
				else
				{
					if ( m_activeSetup != index )
					{
						m_isInTransition = true;

						if ( m_callback )
						{
							m_callback( index, m_cameraSetup[ index ] );
						}

						m_pendingSetup = -1;
						m_delay = -1;
						m_activeSetup = index;
						m_startPos = camera->GetTaretPointPos();
						m_startZoom = camera->GetZoom();
						m_time = 0.f;
					}
				}
			}
		}

		void PuppetPreviewCameraController::Update( Float deltaTime )
		{
			if ( m_isInPendingTransition )
			{
				m_delay -= deltaTime;
				m_isInPendingTransition = m_delay > 0.f;

				if ( !m_isInPendingTransition )
				{
					SetCameraSetup( m_pendingSetup );
				}
			}

			if ( m_isInTransition )
			{
				auto& setup = m_cameraSetup[ m_activeSetup ];

				m_time += deltaTime;
				m_isInTransition = m_time < setup.m_interpolationTime;

				if ( const auto& camera = m_camera.ToHandle() )
				{
					Float time = Max( 1.0f - ( m_time / setup.m_interpolationTime ), 0.f );
					time = time * time * time;
					time = 1.0f - time;

					const auto& newPos = math::Interpolation<Vector3>::Linear( m_startPos, setup.m_slotPos, time );
					const auto& newZoom = math::Interpolation<Float>::Linear( m_startZoom, setup.m_cameraZoom, time );

					camera->MoveTarget( newPos, true );
					camera->Zoom( newZoom, true );
				}
			}
		}

		//////////////////////////////////////////////////////////////////////////

		PuppetPreview_SetCameraSetupEvent::PuppetPreview_SetCameraSetupEvent()
			: m_setupIndex( -1 ), m_delayed( false ) {}

		PuppetPreview_SetCameraSetupEvent::PuppetPreview_SetCameraSetupEvent( CName slotName, Bool delayed )
			: m_setupIndex( -1 ), m_slotName( slotName ), m_delayed( delayed ) {}

		PuppetPreview_SetCameraSetupEvent::PuppetPreview_SetCameraSetupEvent( Uint32 setupIndex, Bool delayed )
			: m_setupIndex( setupIndex ), m_delayed( delayed ) {}

		//////////////////////////////////////////////////////////////////////////

		PuppetPreview_ReadyToBeDisplayed::PuppetPreview_ReadyToBeDisplayed() : m_isMale( false ) {}
		PuppetPreview_ReadyToBeDisplayed::PuppetPreview_ReadyToBeDisplayed( Bool isMale ) : m_isMale( isMale ) {}

		} // namespace ui
} // namespace world

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif