#include <cstdio>
#include <atomic>
/**
 * Copyright (c) 2018-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"

#include "runtimeSystemUI.h"
#include "advertisementWidgetComponent.h"
#include "gameCameraDescriptor.h"
#include "widgetHudComponentInterface.h"
#include "widgetMenuComponentInterface.h"
#include "worldWidgetComponent.h"
#include "../../engine/include/nativeFunctionsRegistry.h"
#include "../../inkCore/include/advertisementsLayer.h"
#include "../../inkCore/include/hudLayer.h"
#include "../../inkCore/include/inkSystem.h"
#include "../../inkCore/include/menuLayer.h"
#include "../../inkCore/include/nullCameraDescriptor.h"
#include "../../inkCore/include/widgetDebugUtils.h"
#include "../../renderData/include/renderFrameInfo.h"
#include "../../renderData/include/viewport.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../world/include/cameraSystemInterface.h"
#include "../../world/include/gameInstance.h"
#include "../../world/include/runtimeSystemRendering.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../worldEntities/include/entity.h"
#include "../../worldEntities/include/runtimeSystemEntity.h"
#include "advertisementWidgetComponentWrapper.h"
#include "../../inkCore/include/menuLayerEvents.h"
#include "streetSignWidgetComponent.h"
#include "streetSignWidgetComponentWrapper.h"
#include "../../localization/include/localizationManager.h"
#include "../../inkCore/include/photoModeLayer.h"
#include "../../redCore/include/commandline.h"
#include "../../inkCore/include/streetSignsLayer.h"
#include "../../inkCore/include/fullscreenCompositionManager.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../worldPhysics/include/runtimeSystemPhysics.h"
#include "uiEnvParamManager.h"
// CP2077 ui runtime probe counters: [0] = wrapper present, [1] = wrapper null.
namespace { inline bool CP2077UiProbeDue( unsigned n ) { return ( n & ( n - 1 ) ) == 0 || ( n % 4096 ) == 0; } }
namespace { std::atomic<unsigned> s_cp2077Fullscreen[2]; std::atomic<unsigned> s_cp2077World[2]; std::atomic<unsigned> s_cp2077Render[2]; }
#include "../../inkCore/include/stateMachine.h"
#include "advertisementNodeInstance.h"
#include "advertisementWidgetNodeInstanceWrapper.h"
#include "../../resourceMaterial/include/bitmapTexture.h"
#include "tweakDBAdvertUtils.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeSystemUI, world, ui );
RTTI_PARENT_TYPE( world::IRuntimeSystem );
RED_EVENT_CONNECTOR( OnPushContextRequest );
RED_EVENT_CONNECTOR( OnPopContextRequest );
RED_EVENT_CONNECTOR( OnSwapContextRequest );
RED_EVENT_CONNECTOR( OnResetContextRequest );
RTTI_END_TYPE();

namespace world
{
	namespace ui
	{
		using RunningMode = ink::RunningMode;

		RuntimeSystemUI::RuntimeSystemUI()
			: m_menuComponent( nullptr )
			, m_hudComponent( nullptr )
			, m_controlledViewportWrapper()
			, m_screenResolution( 1.f, 1.f )
			, m_emptyEntries( ink::PoolInk_Layers() )
			, m_runningMode( ink::RunningMode::Unknown )
			, m_viewportId( std::numeric_limits<Uint64>::max() )
			, m_hiddenLayers( ink::PoolInk_Layers() )
			, m_pendingEvents( ink::PoolInk_Events() )
			, m_blackboardSystem( nullptr )
			, m_renderingSystem( nullptr )
			, m_gameContextStack( ink::PoolInk_System() )
			, m_compositionEffectsStates( ink::PoolInk_System() )
			, m_exclusiveUIRenderingMode( false )
		{
			m_cameraDescriptor = red::CreateUniquePtr < ink::NullCameraDescriptor >();
			m_elementManager = red::CreateSharedPtr< ElementManager >();
			m_gameContextStack.PushBack( game::ui::Context::Default );

			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is created" );

			m_compositionEffectsStates.PushBack( RED_NAME_CONSTEXPR( "inkGameState" ) );
			m_compositionEffectsStates.PushBack( RED_NAME_CONSTEXPR( "LoadingScreen" ) );
		}

		RuntimeSystemUI::~RuntimeSystemUI()
		{
			ReleaseViewportWrapper();
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is destroyed" );
		}

		SystemID RuntimeSystemUI::GetSystemID() const
		{
			return SYSTEM_ID;
		}

		void RuntimeSystemUI::OnInitialize( RuntimeScene& scene, const RuntimeSystemInitInfo& initInfo, job::Counter& counter )
		{
			m_renderingSystem = scene.GetSystem< world::RuntimeSystemRendering >();

			SetCorrectRunningMode( initInfo.mode );

			RegisterNativeFunctions();

			GetInkSystem().SetSoundSystem( GEngine->GetSoundSystem() );
			GetInkSystem().SetAdvertisementsTWDBConnector( red::CreateUniquePtr< ink::AdvertisementsTWDBWorldUIConnector >() );

			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is initialized" );
		}

		void RuntimeSystemUI::OnUninitialize( RuntimeScene& scene )
		{
			m_renderingSystem = nullptr;

			ReleaseViewportWrapper();

			m_defaultGameData.SetGameData( nullptr, nullptr, nullptr );

			UnregisterNativeFunctions();

			GetInkSystem().SetSoundSystem( nullptr );

			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is shutted down" );
		}

		void RuntimeSystemUI::RegisterUpdateFunctions( UpdatableSystemRegistrar& processor )
		{
			WeakHandle< RuntimeSystemUI > weakSelf = WeakHandleFromThis< RuntimeSystemUI >();

			// audio handlers for UI have to be updated in different tick group than any other UI logic - the best is to update it at the beggining of frame
			processor.Register( UpdateTickGroup::PreBuckets, *this, "UI/AudioHandlersUpdate",
				[weakSelf]( const UpdateContext& updateContext, job::Builder& builder )
			{
				if( THandle< RuntimeSystemUI > self = weakSelf.ToHandle() )
				{
					self->AudioHandlersUpdate( updateContext.m_tickInfo, builder );
				}
			} );

			// world UI has to be processed after CameraUpdate tick group because it needs updated camera info
			processor.Register( UpdateTickGroup::PlayerAimUpdate, *this, "UI/World",
				[weakSelf]( const UpdateContext& updateContext, job::Builder& builder )
			{
				if (THandle< RuntimeSystemUI > self = weakSelf.ToHandle())
				{
					job::Builder internalBuilder;
					self->EstablishUISystemState( updateContext.m_tickInfo, internalBuilder);
					self->BroadcastUIState( internalBuilder);
					self->ProcessWorldUI( updateContext.m_tickInfo, internalBuilder);
					self->m_asyncUpdateCounter += internalBuilder.ExtractWaitCounter();
				}
			} );

			// fullscreen UI processing
			processor.Register( UpdateTickGroup::PreRenderUpdate, *this, "UI/Fullscreen",
				[weakSelf]( const UpdateContext& updateContext, job::Builder& builder )
			{
				if( THandle< RuntimeSystemUI > self = weakSelf.ToHandle() )
				{
					job::Builder internalBuilder;

					// we have to wait for world-related UI before we start updating and drawing
					internalBuilder.DispatchWait(self->m_asyncUpdateCounter);

					self->AsyncUpdate( updateContext.m_tickInfo, internalBuilder );

					// we have to sync independent UI job chains to main frame job chain
					builder.DispatchWait(internalBuilder.ExtractWaitCounter());
				}
			} );
		}

		void RuntimeSystemUI::OnGameInstanceAttached( game::IGameInstance& gameInstance )
		{
			m_blackboardSystem = gameInstance.GetSystemPtr< game::IBlackboardSystem >().Get();

			const auto& cameraGameSystem = gameInstance.GetSystemPtr< game::ICameraSystem >();
			m_cameraDescriptor = red::CreateUniquePtr < world::ui::GameCameraDescriptor >( cameraGameSystem.Get() );

			if( const auto& runtimeSystemEntities = gameInstance.GetScene().GetSystem< world::RuntimeSystemEntity >() )
			{
				m_defaultGameData.SetGameData( runtimeSystemEntities->AcquireGameService(), runtimeSystemEntities->AcquireSceneService(), nullptr );
			}

			m_renderingSystem = gameInstance.GetScene().GetSystem< world::RuntimeSystemRendering >();
			m_sceneId = world::GetSceneID( gameInstance.GetScene() );

			m_enviromentParamsManager = red::CreateUniquePtr<UIEnviromentParamManager>();
			m_enviromentParamsManager->OnGameInstanceAttach( gameInstance );

			ResetContextStack();

			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is attached to the game" );
		}

		void RuntimeSystemUI::OnGameInstanceDetached( game::IGameInstance& gameInstance )
		{
			ReleaseViewportWrapper( true );
			m_renderingSystem = nullptr;
			m_blackboardSystem = nullptr;
			m_enviromentParamsManager->OnGameInstanceDettach();
			m_enviromentParamsManager = nullptr;
			m_cameraDescriptor = red::CreateUniquePtr < ink::NullCameraDescriptor >();

			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is dettached from the game" );
		}

		ink::ICameraDescriptor* RuntimeSystemUI::GetCameraDescriptor() const
		{
			return m_cameraDescriptor.Get();
		}

		void RuntimeSystemUI::SetCameraDescriptor( red::UniquePtr < ink::ICameraDescriptor >&& descriptor )
		{
			m_cameraDescriptor = std::move( descriptor );
		}

		world::ui::ElementManager& RuntimeSystemUI::GetUIElementManager()
		{
			return *m_elementManager;
		}

		void RuntimeSystemUI::AttachWidgetComponent( WorldWidgetComponent* component )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::WorldLayer>& layer = viewportWrapper->GetLayerByType<ink::WorldLayer>( ink::ELayerType::World ) )
				{
					red::SharedPtr< WorldLayerWidget > worldWidget = red::CreateSharedPtr<WorldLayerWidget >( component->WeakHandleFromThis< WorldWidgetComponent>() );
					layer->RegisterWorldComponent( worldWidget );
				}
			}
		}

		void RuntimeSystemUI::DetachWidgetComponent( WorldWidgetComponent* component )
		{
			if( red::SharedPtr< ink::ViewportWrapper > viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle< ink::WorldLayer >& layer = viewportWrapper->GetLayerByType< ink::WorldLayer >( ink::ELayerType::World ) )
				{
					red::SharedPtr< ink::IWidgetComponentWrapper> widget = layer->FindWorldWidget( [ component ]( const red::SharedPtr< ink::IWidgetComponentWrapper >& widget )
					{
						red::SharedPtr< WorldLayerWidget > worldWidget = red::StaticCast< WorldLayerWidget >( widget );
						return worldWidget->GetWorldComponent() == component;
					} );

					if( widget )
					{
						layer->UnregisterWorldComponent( widget );
					}
				}
			}
		}

		void RuntimeSystemUI::AttachWidgetComponent( const THandle<AdvertisementWidgetComponent> &component )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::AdvertisementsLayer>& layer = viewportWrapper->GetLayerByType<ink::AdvertisementsLayer>( ink::ELayerType::Advertisements ) )
				{
					red::SharedPtr< AdvertisementWidgetComponentWrapper > componentWrapper = red::CreateSharedPtr<AdvertisementWidgetComponentWrapper>( component );
					component->SetOwnerWrapper( componentWrapper );
					layer->RegisterWorldFluffComponent( componentWrapper );
				}
			}
		}

		void RuntimeSystemUI::DetachWidgetComponent( const THandle<AdvertisementWidgetComponent> &component )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::AdvertisementsLayer>& layer = viewportWrapper->GetLayerByType<ink::AdvertisementsLayer>( ink::ELayerType::Advertisements ) )
				{
					layer->OnAdvertComponentDetached( component->GetOwnerWrapper() );
				}
			}
		}

		void RuntimeSystemUI::AttachAdvertisementInstance( const THandle<AdvertisementNodeInstance>& instance )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::AdvertisementsLayer>& layer = viewportWrapper->GetLayerByType<ink::AdvertisementsLayer>( ink::ELayerType::Advertisements ) )
				{
					red::SharedPtr< AdvertisementWidgetNodeInstanceWrapper > nodeWrapper = red::CreateSharedPtr<AdvertisementWidgetNodeInstanceWrapper>( instance );
					instance->SetOwnerWrapper( nodeWrapper );
					layer->RegisterWorldFluffComponent( nodeWrapper );
				}
			}
		}

		void RuntimeSystemUI::DetachAdvertisementInstance( const THandle<AdvertisementNodeInstance>& instance )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::AdvertisementsLayer>& layer = viewportWrapper->GetLayerByType<ink::AdvertisementsLayer>( ink::ELayerType::Advertisements ) )
				{					
					layer->OnAdvertComponentDetached( instance->GetOwnerWrapper() );
				}
			}
		}

		void RuntimeSystemUI::AttachWidgetComponent( const THandle<StreetSignWidgetComponent> &component )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::StreetSignsLayer>& layer = viewportWrapper->GetLayerByType<ink::StreetSignsLayer>( ink::ELayerType::StreetSigns ) )
				{
					red::SharedPtr< StreetSignWidgetComponentWrapper > componentWrapper = red::CreateSharedPtr<StreetSignWidgetComponentWrapper>( component );
					component->SetOwnerWrapper( componentWrapper );
					layer->RegisterWorldFluffComponent( componentWrapper );
				}
			}
		}

		void RuntimeSystemUI::DetachWidgetComponent( const THandle<StreetSignWidgetComponent> &component )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::StreetSignsLayer>& layer = viewportWrapper->GetLayerByType<ink::StreetSignsLayer>( ink::ELayerType::StreetSigns ) )
				{
					if( red::SharedPtr<ink::IStreetSignWidgetComponentWrapper> componentWrapper = component->GetOwnerWrapper() )
					{
						layer->OnStreetSignComponentDetached( componentWrapper );
					}
				}
			}
		}

		void RuntimeSystemUI::AttachWidgetComponent( WidgetMenuComponentInterface* component )
		{
			RED_ASSERT( component );
			RED_ASSERT( m_menuComponent == nullptr );

			m_menuComponent = component;
			m_defaultGameData.SetOwner( component->GetGameData()->GetOwner() );
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				viewportWrapper->SetOwner( component->GetGameData()->GetOwner() );
			}
		}

		void RuntimeSystemUI::DetachWidgetComponent( WidgetMenuComponentInterface* component )
		{
			RED_ASSERT( component );
			RED_ASSERT( m_menuComponent == component );

			m_menuComponent = nullptr;
			m_defaultGameData.SetOwner( nullptr );
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				viewportWrapper->SetOwner( nullptr );
			}
		}

		void RuntimeSystemUI::AttachWidgetComponent( WidgetHudComponentInterface* component )
		{
			RED_ASSERT( component );
			//RED_ASSERT( m_hudComponent == nullptr ); It also asserts if we have multipe HUD components in single entity.
			RED_ASSERT( m_hudComponent == nullptr || m_hudComponent->GetEntity() == component->GetEntity() );

			RED_ERROR( m_hudComponent == nullptr, "Entity '%hs' has more than one HUD component!", component->GetEntity() ? component->GetEntity()->GetDebugUniqueName().AsChar() : nullptr );

			if( m_hudComponent == nullptr )
			{
				m_hudComponent = component;
			}
		}

		void RuntimeSystemUI::DetachWidgetComponent( WidgetHudComponentInterface* component )
		{
			RED_ASSERT( component );
			//RED_ASSERT( m_hudComponent == component ); It also asserts if we have multipe HUD components in single entity.

			if( m_hudComponent == component )
			{
				m_hudComponent = nullptr;
			}
		}

		const Vector2& RuntimeSystemUI::GetScreenResolution() const
		{
			return m_screenResolution;
		}

		void RuntimeSystemUI::BroadcastUIState( job::Builder& builder)
		{
			const auto broadcastUIStateFunc = [this](const job::RunContext& context)
			{
				Bool isInMenu = false;

				if (red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock())
				{
					if (const THandle<ink::MenuLayer>& menuLayer = viewportWrapper->GetLayerByType<ink::MenuLayer>(ink::ELayerType::Menu))
					{
						isInMenu = menuLayer->GetActive();
					}
				}

				if (m_isInMenu.Exchange(isInMenu) == !isInMenu)
				{
					FireMenuStateChanged(isInMenu);
				}

				m_hudVideoEventDispatcher.Update(HandleFromPtr(m_blackboardSystem)); // TEMP
			};
			static red::InstrumentationObject instrObj{ "UI/RuntimeSystemUI/BroadcastUIState", PBC_UI };
			builder.DispatchJob(instrObj, broadcastUIStateFunc);
		}

		void RuntimeSystemUI::AsyncUpdate(const tick::Info& tickInfo, job::Builder& builder)
		{
			{
				const bool cp2077Null = !m_controlledViewportWrapper.Lock();
				const unsigned cp2077Count = ++s_cp2077Fullscreen[ cp2077Null ? 1 : 0 ];
				if ( CP2077UiProbeDue( cp2077Count ) )
				{
					std::fprintf( stderr, "CP2077 ui runtime probe: ProcessFullscreenUI(AsyncUpdate) this=%p wrapper=%s count=%u fullscreen=%u/%u world=%u/%u render=%u/%u\n", (void*)this, cp2077Null ? "null" : "ok", cp2077Count, s_cp2077Fullscreen[0].load(), s_cp2077Fullscreen[1].load(), s_cp2077World[0].load(), s_cp2077World[1].load(), s_cp2077Render[0].load(), s_cp2077Render[1].load() );
				}
			}
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				ink::UpdateContext updateContext{ tickInfo, *m_cameraDescriptor, m_screenResolution, m_isInMenu.GetValue() ? ink::UIMode::Menu : ink::UIMode::Normal };

				viewportWrapper->ProcessFullscreenUI( updateContext, builder );

				ElementManagerUpdate( tickInfo, builder );
			}
			
			const auto prepareRenderingStateFunc = [this, tickInfo](const job::RunContext& context)
			{
				PrepareRenderingState( tickInfo );
			};
			static red::InstrumentationObject instrObj{ "UI/RuntimeSystemUI/PrepareRenderingState", PBC_UI };
			builder.DispatchJob(instrObj, prepareRenderingStateFunc);
		}

		void RuntimeSystemUI::EstablishUISystemState( const tick::Info& tickInfo, job::Builder& builder )
		{
			if (red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock())
			{
				viewportWrapper->EstablishUISystemState( tickInfo, builder );
			}
		}

		void RuntimeSystemUI::ProcessWorldUI( const tick::Info& tickInfo, job::Builder& builder )
		{
			{
				const bool cp2077Null = !m_controlledViewportWrapper.Lock();
				const unsigned cp2077Count = ++s_cp2077World[ cp2077Null ? 1 : 0 ];
				if ( CP2077UiProbeDue( cp2077Count ) )
				{
					std::fprintf( stderr, "CP2077 ui runtime probe: ProcessWorldUI this=%p wrapper=%s count=%u fullscreen=%u/%u world=%u/%u render=%u/%u\n", (void*)this, cp2077Null ? "null" : "ok", cp2077Count, s_cp2077Fullscreen[0].load(), s_cp2077Fullscreen[1].load(), s_cp2077World[0].load(), s_cp2077World[1].load(), s_cp2077Render[0].load(), s_cp2077Render[1].load() );
				}
			}
			Bool isInMenu = false;

			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::MenuLayer>& menuLayer = viewportWrapper->GetLayerByType<ink::MenuLayer>( ink::ELayerType::Menu ) )
				{
					if( const THandle< ink::Widget >& widget = menuLayer->GetRootWidget() )
					{
						isInMenu = widget->GetVisible();
					}
				}

				ink::UpdateContext updateContext{ tickInfo, *m_cameraDescriptor, m_screenResolution, isInMenu ? ink::UIMode::Menu : ink::UIMode::Normal };

				viewportWrapper->ProcessWorldUI( updateContext, builder );
			}
		}

		void RuntimeSystemUI::AudioHandlersUpdate( const tick::Info& tickInfo, job::Builder& builder )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				red::DynArray<ink::AudioHandlerRequest> requests{ ink::PoolInk_Layers() };
				viewportWrapper->GetAudioHandlerRequests( requests );

				for( ink::AudioHandlerRequest currentRequest : requests )
				{
					if( auto targetData = currentRequest.m_target.ToHandle() )
					{
						targetData->CreateRootAudioHandler( currentRequest.m_metadata );
					}
				}
			}
		}

		void RuntimeSystemUI::ElementManagerUpdate( const tick::Info& tickInfo, job::Builder& builder )
		{
			const auto tickElementManagerFunc = [ this, tickInfo ]( const job::RunContext& context )
			{
				job::Builder depBuilder{ context };
				m_elementManager->Update( tickInfo, depBuilder );
			};
			static red::InstrumentationObject instrObj{ "UI/TickElementManager/Update", PBC_UI };
			builder.DispatchJob( instrObj, tickElementManagerFunc );
		}
		static Vector2 prev;

		void RuntimeSystemUI::RenderFrame( CRenderFrameInfo& info )
		{
			m_viewportId = info.GetViewport()->GetViewportID();

			m_screenResolution = Vector2( static_cast< Float >( info.GetViewport()->GetWidth() ), static_cast< Float >( info.GetViewport()->GetHeight() ) );
			m_screenResolution = Vector2::Max( m_screenResolution, Vector2( 1, 1 ) );

			red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock();
			const bool cp2077Acquired = !viewportWrapper;
			if( !viewportWrapper )
			{
				AcquireViewportWrapper( m_viewportId );
			}

			viewportWrapper = m_controlledViewportWrapper.Lock();
			{
				const unsigned cp2077Count = ++s_cp2077Render[ viewportWrapper ? 0 : 1 ];
				if ( CP2077UiProbeDue( cp2077Count ) || ( cp2077Acquired && viewportWrapper ) )
				{
					std::fprintf( stderr, "CP2077 ui runtime probe: RenderFrame this=%p info=%p wrapper=%p acquireAttempted=%u viewportId=%llu size=%.0fx%.0f count=%u fullscreen=%u/%u world=%u/%u render=%u/%u\n", (void*)this, (void*)&info, (void*)viewportWrapper.Get(), unsigned( cp2077Acquired ), (unsigned long long)( m_viewportId ), double( m_screenResolution.X ), double( m_screenResolution.Y ), cp2077Count, s_cp2077Fullscreen[0].load(), s_cp2077Fullscreen[1].load(), s_cp2077World[0].load(), s_cp2077World[1].load(), s_cp2077Render[0].load(), s_cp2077Render[1].load() );
				}
			}
			if( viewportWrapper )
			{
				viewportWrapper->RenderFrame( info );
			}

			if( m_calibrationScreenActive )
			{
				if( const THandle<CBitmapTexture> texture = m_calibrationTexture )
				{
					info.m_calibrationScreenInfo.texture = texture->GetRenderResource();
					info.m_calibrationScreenInfo.toRect = m_calibrationScreenToRect;
					info.m_calibrationScreenInfo.fromTextureRect = m_calibrationScreenFromRect;
				}
			}
		}

		void RuntimeSystemUI::QueueEvent( const THandle< red::Event >& eventData )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				viewportWrapper->QueueEvent( eventData );
			}
			else
			{
				RED_SCOPE_LOCK( m_pendingEventsLock );
				m_pendingEvents.PushBack( eventData );
			}
		}

		void RuntimeSystemUI::ChangeHudTierContext( world::ui::ContextVisibility context, red::DynArray<ink::HudWidgetSpawnEntry>& outAffectedEntries )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::HUDLayer>& hudLayer = viewportWrapper->GetLayerByType<ink::HUDLayer>( ink::ELayerType::HUD ) )
				{
					hudLayer->SetTierContext( context, outAffectedEntries );
				}
			}
		}

		void RuntimeSystemUI::ChangeHudGameContext( game::ui::Context context, red::DynArray<ink::HudWidgetSpawnEntry>& outAffectedEntries )
		{
			// #PM: From lack of better place. Hud context would be triggered every time we actually have context change.
			ResolveWorldHitTestState();

			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::HUDLayer>& hudLayer = viewportWrapper->GetLayerByType<ink::HUDLayer>( ink::ELayerType::HUD ) )
				{
					hudLayer->SetGameContext( context, outAffectedEntries );
				}
			}
		}

		Bool RuntimeSystemUI::SetHudEntryForcedVisibility( const CName & entryName, EntryVisibility forcedVisibility, ink::HudWidgetSpawnEntry & outEntry )
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::HUDLayer>& hudLayer = viewportWrapper->GetLayerByType<ink::HUDLayer>( ink::ELayerType::HUD ) )
				{
					return hudLayer->SetEntryForcedVisibility( entryName, forcedVisibility, outEntry );
				}
			}
			return false;
		}

		Bool RuntimeSystemUI::IsHudSpawned()
		{
			if (red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock())
			{
				if (const THandle<ink::HUDLayer>& hudLayer = viewportWrapper->GetLayerByType<ink::HUDLayer>( ink::ELayerType::HUD ))
				{
					return hudLayer->IsHudSpawned();
				}
			}
			return false;
		}

		void RuntimeSystemUI::UpdateHudGlobalParameters( HudGlobalParameters && hudGlobalParams )
		{
			Bool updates = true;

			const Float playerHealthValue = hudGlobalParams.PlayerHealthPercentage / 100.f;
			const Float hudEffectStrength = 1.0f - playerHealthValue;

			GetInkSystem().GetFullscreenCompositionManager().OverrideParamFloatValue( RED_NAME_CONSTEXPR( "playerHealth" ), playerHealthValue, m_compositionEffectsStates );
		}

		void RuntimeSystemUI::UpdateHudEffectGlitchParam( const Vector4& glitchParam, const Vector4& glitchParam1 )
		{
			if( m_compositionEffectsStates.Exist( GetInkSystem().GetFullscreenCompositionManager().GetActiveStateName() ) )
			{
				if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
				{
					if( const THandle<ink::HUDLayer> hudLayer = viewportWrapper->GetLayerByType< ink::HUDLayer>( ink::ELayerType::HUD ) )
					{
						hudLayer->UpdateHUDGlitchParam( glitchParam );
					}
					else
					{
						hudLayer->UpdateHUDGlitchParam( Vector4::ZEROS() );
					}
				}
			}
			GetInkSystem().GetFullscreenCompositionManager().OverrideParamFloatValue( RED_NAME_CONSTEXPR( "glitchParam1" ), glitchParam1.X, m_compositionEffectsStates );
		}		

		void RuntimeSystemUI::AcquireViewportWrapper( ViewportId viewportId )
		{
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is trying to acquire viewport wrapper for id: %llu", viewportId );

			ReleaseViewportWrapper();

			m_controlledViewportWrapper = GetInkSystem().TakeControlOnViewport( viewportId, m_runningMode );
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI acquired viewport wrapper for id: %llu", viewportId );
				viewportWrapper->UpdateGameData( &m_defaultGameData );
				viewportWrapper->SpawnLayers( WeakHandleFromThis(), &m_defaultGameData, GetGameContext() );

				if( const THandle<ink::WorldLayer>& layer = viewportWrapper->GetLayerByType<ink::WorldLayer>( ink::ELayerType::World ) )
				{
					m_hitTester = red::CreateUniquePtr<WorldWidgetHitTester>( layer, this, m_sceneId );
					m_hitTester->SetPhysicalTestFunc( m_physicalHitTestFunc );
					layer->SetHitTester( m_hitTester.Get() );

					m_hitTester->OnWorldInteractionChanged += [ this ]( Bool isInteracting, const WorldWidgetComponent* component )
					{
						// Forward to listeners
						OnWorldInteractionChanged( isInteracting, component );
						GetInkSystem().SetIsInWorldInteraction( isInteracting );
					};
				}

				RED_SCOPE_LOCK( m_pendingEventsLock );
				for( THandle< red::Event > eventData : m_pendingEvents )
				{
					viewportWrapper->QueueEvent( eventData );
				}

				ApplyHiddenLayersMask();

			}
		}

		bool RuntimeSystemUI::IsInMenuMode() const
		{
			return m_isInMenu.GetValue();
		}

		// DEBUG

		void RuntimeSystemUI::OnRenderDebug( const RenderDebugContext& context )
		{
			#ifndef RED_CONFIGURATION_FINAL
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				viewportWrapper->OnRenderDebug( context );
			}
			#endif
		}

		red::WeakPtr<ink::ViewportWrapper> RuntimeSystemUI::GetViewportWrapper() const
		{
			return m_controlledViewportWrapper;
		}

		void RuntimeSystemUI::SetLocalPlayerInteractingWithDevice( Bool isInteractingWithDevice )
		{
			m_hitTester->SetLocalPlayerInteractingWithDevice( isInteractingWithDevice );
		}

		void RuntimeSystemUI::SetPhysicalHitTestFunc( red::Function<Bool( const red::SharedPtr< physics::AsyncQueryToken >&token )> checkFunc )
		{
			m_physicalHitTestFunc = checkFunc;
		}

		void RuntimeSystemUI::RegisterNativeFunctions()
		{
#if !defined( NO_DEBUG_CONSOLE )
			CNativeFunctionsRegistry& reg = GetNativeFunRegistry();

			reg.RegisterNativeFunction( "Ink.SetDebugEnabled", "Toggles various debug helper elements (True or False)", &RuntimeSystemUI::Debug_SetDebugEnabledCommand, this );
			reg.RegisterNativeFunction( "Ink.SetDrawEnabled", "Toggles drawing elements (True or False)", &RuntimeSystemUI::Debug_SetDrawEnabledCommand, this );
			reg.RegisterNativeFunction( "Ink.SubtitleBackground", "Show dark background for subtitles text", &RuntimeSystemUI::Debug_EnableSubtitlesBackgroundCommand, this );
			reg.RegisterNativeFunction( "Ink.SetLayerVisibility", "Show or hide specific UI layer", &RuntimeSystemUI::Debug_SetLayerVisibilityCommand, this );
			reg.RegisterNativeFunction( "Ink.SetOnScreenLanguage", "Change language for on screen texts", &RuntimeSystemUI::Debug_SetOnScreenLanguage, this );

			#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
			reg.RegisterNativeFunction( "Ink.Tests.Play", "Play recorded functional test", &RuntimeSystemUI::PlayFunctionalTest );
			#endif
#endif
		}

		void RuntimeSystemUI::UnregisterNativeFunctions()
		{
#if !defined( NO_DEBUG_CONSOLE )
			CNativeFunctionsRegistry& reg = GetNativeFunRegistry();

			reg.UnregisterNativeFunction( "Ink.SetDebugEnabled" );
			reg.UnregisterNativeFunction( "Ink.SetDrawEnabled" );
			reg.UnregisterNativeFunction( "Ink.SubtitleBackground" );
			reg.UnregisterNativeFunction( "Ink.SetLayerVisibility" );
			reg.UnregisterNativeFunction( "Ink.SetOnScreenLanguage" );

			#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
			reg.UnregisterNativeFunction( "Ink.Tests.Play" );
			#endif
#endif
		}

#if !defined( NO_DEBUG_CONSOLE )
		/* static */ Bool RuntimeSystemUI::Debug_SetDrawEnabledCommand( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if( parameters.Size() < 1 )
			{
				return false;
			}

			Bool enabled = false;
			if( parameters[ 0 ].Length() )
			{
				FromString( parameters[ 0 ].AsChar(), enabled );
			}

			ink::SetGlobalDrawEnabled( enabled );

			return true;
		}

		/* static */ Bool RuntimeSystemUI::Debug_SetDebugEnabledCommand( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if( parameters.Size() < 1 )
			{
				return false;
			}

			Bool enabled = false;
			if( parameters[ 0 ].Length() )
			{
				FromString( parameters[ 0 ].AsChar(), enabled );
			}

			#ifndef RED_CONFIGURATION_FINAL
			ink::SetDebugDrawEnabled( enabled );
			#else
			invokeResult.LogLine( "Unavailable in final game" );
			#endif

			return true;
		}

		/* static */ Bool RuntimeSystemUI::Debug_EnableSubtitlesBackgroundCommand( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if( parameters.Size() < 1 )
			{
				return false;
			}

			Bool enabled = false;
			if( parameters[ 0 ].Length() )
			{
				FromString( parameters[ 0 ].AsChar(), enabled );
			}

			const RuntimeSystemUI* runtimeSystemUI = static_cast< RuntimeSystemUI* >( userData );
			const game::IBlackboardSystem* blackboardSystem = runtimeSystemUI->GetBlackboardSystem();

			if( blackboardSystem )
			{
				THandle<game::IBlackboard> blackboard = blackboardSystem->Get( BLACKBOARD_INDEX( UIGameData ) );
				blackboard->Write( BLACKBOARD_ID( UIGameData, ShowSubtitlesBackground ), enabled, true );
			}

			return true;
		}

		/* static */ Bool RuntimeSystemUI::Debug_SetLayerVisibilityCommand( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if( parameters.Size() < 2 )
			{
				return false;
			}

			RuntimeSystemUI* runtimeSystemUI = reinterpret_cast< RuntimeSystemUI* >( userData );

			Bool shouldBeVisible = false;
			if( parameters[ 0 ] == "Show" || parameters[ 0 ] == "show" )
			{
				shouldBeVisible = true;
			}
			else if( parameters[ 0 ] == "Hide" || parameters[ 0 ] == "hide" )
			{
				shouldBeVisible = false;
			}
			else
			{
				return false;
			}

			red::DynArray<String> tokens{ ink::PoolInk_Debug() };
			tokens.Reserve( static_cast< Uint32 >( ink::ELayerType::MAX ) );
			parameters[ 1 ].GetTokens( '|', true, tokens );

			const CName enumName = RED_NAME_CONSTEXPR( "inkELayerType" );
			if( const rtti::EnumType* enumType = GetRttiSystem().FindEnum( enumName ) )
			{
				for( const String& token : tokens )
				{
					ink::ELayerType enumValue;
					if( enumType->FromString( &enumValue, token ) )
					{
						if( shouldBeVisible )
						{
							runtimeSystemUI->m_hiddenLayers.Remove( enumValue );
						}
						else
						{
							red::alg::PushBackUnique( runtimeSystemUI->m_hiddenLayers, enumValue );
						}
					}
				}

				runtimeSystemUI->ApplyHiddenLayersMask();

				return true;
			}
			return false;
		}

		/* static */ Bool RuntimeSystemUI::Debug_SetOnScreenLanguage( IDebugConsole* callingConsole, const red::DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
		{
			if( parameters.Size() != 1 )
			{
				return false;
			}

			const audio::Language& languageConfig = GLocalizationManager->ResolveClosestLanguage( parameters.Front() );
			GLocalizationManager->SetTextLanguage( languageConfig );
			GetInkSystem().OnLocalizationChanged();

			invokeResult.LogLine( "Language changed to %hs", languageConfig.GetCodeName().AsChar() );
			return true;
		}

		#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		Bool RuntimeSystemUI::PlayFunctionalTest( IDebugConsole * callingConsole, const red::DynArray<String>& parameters, SCommandInvokeResult & invokeResult, void * userData )
		{
			return GetInkSystem().GetFunctionalTests().PlayFromFile();
		}
		#endif
#endif

		const red::DynArray<ink::HudWidgetSpawnEntry>& RuntimeSystemUI::Debug_GetHUDEntries()
		{
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				if( const THandle<ink::HUDLayer>& hudLayer = viewportWrapper->GetLayerByType<ink::HUDLayer>( ink::ELayerType::HUD ) )
				{
					return hudLayer->GetEntries();
				}
			}
			return m_emptyEntries;
		}

		void RuntimeSystemUI::Editor_InitializeViewport( ViewportId viewportId )
		{
			AcquireViewportWrapper( viewportId );
		}

		void RuntimeSystemUI::ReleaseViewportWrapper( Bool gameInstanceDetached )
		{
			RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI is trying to release viewport wrapper for id: %llu", m_viewportId );
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				RED_LOG_CATEGORY_INFO( red::LoggerCategory_UserInterface, "RuntimeSystemUI released viewport wrapper for id: %llu", m_viewportId );
				viewportWrapper->ClearUpdateFlag();
				viewportWrapper->DespawnLayers( gameInstanceDetached );
				viewportWrapper->ReleaseControl();
				viewportWrapper->ResetGameData();
				m_controlledViewportWrapper.Reset();

				m_viewportId = std::numeric_limits<Uint64>::max();
			}
		}

		void RuntimeSystemUI::SetCorrectRunningMode( const world::RuntimeMode& mode )
		{
			if( mode == world::RuntimeMode::GamePreview )
			{
				m_runningMode = RunningMode::GamePreview;
			}
			else
			{
				if( mode == world::RuntimeMode::Preview )
				{
					m_runningMode = RunningMode::Backend;
				}
				else
				{
					const Bool shouldForceMultiplayerWidgets = red::CommandLine::Get().HasOption( "forceCPOControlScheme" );
					if( mode == world::RuntimeMode::Singleplayer && !shouldForceMultiplayerWidgets )
					{
						m_runningMode = RunningMode::SinglePlayer;
					}
					else
					{
						m_runningMode = RunningMode::Multiplayer;
					}
				}
			}
		}

		void RuntimeSystemUI::OnPlayerGenderChanged( ent::EntityID entityID, Bool isMale )
		{
			QueueEvent( CreateHandle< ink::MenuLayer_SetGender >( isMale ) );
		}

		void RuntimeSystemUI::FireMenuStateChanged( Bool isInMenu )
		{
			OnMenuStateChanged( isInMenu );

			if( const auto uiSystemBB = m_blackboardSystem->Get( BLACKBOARD_INDEX( UI_System ) ) )
			{
				uiSystemBB->Write( BLACKBOARD_ID( UI_System, IsInMenu ), isInMenu );
			}
		}

		void RuntimeSystemUI::ResolveWorldHitTestState()
		{
			if( m_hitTester )
			{
				RED_SCOPE_SHARED_LOCK(m_gameContextLock);
				m_hitTester->SetHitTestEnabled( !( m_gameContextStack.Exist( game::ui::Context::BraindanceEditor ) || m_gameContextStack.Exist( game::ui::Context::BraindancePlayback ) ) );
			}
		}

		void RuntimeSystemUI::ResetContextStack()
		{
			RED_SCOPE_LOCK(m_gameContextLock);

			ResetContextStack_NoLock();
		}

		void RuntimeSystemUI::ResetContextStack_NoLock()
		{
			m_gameContextStack.Clear();
			m_gameContextStack.PushBack(game::ui::Context::Default);
		}

		void RuntimeSystemUI::OnPushContextRequest( const game::ui::RequestPushContextEvent& evt )
		{
			RED_SCOPE_LOCK( m_gameContextLock );

			/*
				Conditions for failure:
					1. Pushing Default context
					2. Pushing an existing context
			*/

			const game::ui::Context newContext = game::ui::MapContextEnum( evt.m_context );
			if( newContext == game::ui::Default )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Pushing Default context is forbidden!" );
				return;
			}

			if( m_gameContextStack.Exist( newContext ) )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Cannot push %hs context because it already exists in the stack"
					, game::ui::ContextToString( evt.m_context ).AsChar() );
				return;
			}

			const game::ui::Context oldContext = m_gameContextStack.Back();
			m_gameContextStack.PushBack( newContext );

			QueueEvent( CreateHandle< game::ui::ContextChangedEvent >( oldContext, newContext ) );
		}

		void RuntimeSystemUI::OnPopContextRequest( const game::ui::RequestPopContextEvent& evt )
		{
			RED_SCOPE_LOCK( m_gameContextLock );

			/*
				Conditions for failure:
					1. Popping Default context
					2. Popping a context that is not currently on top of the stack
						NOTE: Getting this error probably means the stack has been corrupted. Important to fix it right away!
			*/

			const game::ui::Context toPopContext = game::ui::MapContextEnum( evt.m_context );
			if( toPopContext == game::ui::Default )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Popping Default context is forbidden!" );
				return;
			}

			const game::ui::Context oldContext = m_gameContextStack.Back();
			if( toPopContext != oldContext )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Cannot pop %hs context because we are currently in %hs context",
					game::ui::ContextToString( toPopContext ).AsChar(),
					game::ui::ContextToString( oldContext ).AsChar() );
				return;
			}

			if( m_gameContextStack.Size() == 1 )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Cannot pop %hs context because we are currently in the Default context",
					game::ui::ContextToString( toPopContext ).AsChar() );
				return;
			}

			m_gameContextStack.PopBack();

			const game::ui::Context newContext = m_gameContextStack.Back();
			QueueEvent( CreateHandle< game::ui::ContextChangedEvent >( oldContext, newContext ) );
		}

		void RuntimeSystemUI::OnSwapContextRequest( const game::ui::RequestSwapContextEvent& evt )
		{
			RED_SCOPE_LOCK( m_gameContextLock );

			/*
				Conditions for failure:
					1. Swapping the two same contexts
					2. Swapping out the Default context
					2. Swapping out a context that is not currently on top of the stack
						NOTE: Getting this error probably means the stack has been corrupted. Important to fix it right away!
			*/

			const game::ui::Context toPopContext = game::ui::MapContextEnum( evt.m_oldContext );
			const game::ui::Context toPushContext = game::ui::MapContextEnum( evt.m_newContext );

			if( toPopContext == toPushContext )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Swapping must be between two different contexts" );
				return;
			}

			if( toPopContext == game::ui::Default )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Swapping Default context is forbidden!" );
				return;
			}

			const game::ui::Context oldContext = m_gameContextStack.Back();
			if( toPopContext != oldContext )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Cannot swap out %hs context because we are currently in %hs context",
					game::ui::ContextToString( toPopContext ).AsChar(),
					game::ui::ContextToString( oldContext ).AsChar() );
				return;
			}

			m_gameContextStack.PopBack();

			const game::ui::Context previousContext = m_gameContextStack.Back();
			if (previousContext == toPushContext)
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "UI Game Context: Context swap removed top context only \"%hs\", as previous one was already swap target \"%hs\"",
					game::ui::ContextToString( toPopContext ).AsChar(),
					game::ui::ContextToString( oldContext ).AsChar() );
				return;
			}

			m_gameContextStack.PushBack( toPushContext );

			const game::ui::Context newContext = m_gameContextStack.Back();
			QueueEvent( CreateHandle< game::ui::ContextChangedEvent >( oldContext, newContext ) );
		}

		void RuntimeSystemUI::OnResetContextRequest( const game::ui::RequestResetContextEvent& evt )
		{
			RED_SCOPE_LOCK( m_gameContextLock );

			if( m_gameContextStack.Size() == 1 )
				return;

			const game::ui::Context oldContext = m_gameContextStack.Back();
			const game::ui::Context newContext = game::ui::Context::Default;
			ResetContextStack_NoLock();

			QueueEvent( CreateHandle< game::ui::ContextChangedEvent >( oldContext, newContext ) );
		}

		game::ui::Context RuntimeSystemUI::GetGameContext() const
		{
			RED_SCOPE_SHARED_LOCK( m_gameContextLock );
			return m_gameContextStack.Back();
		}

		red::DynArray< game::ui::Context > RuntimeSystemUI::GetGameContextStack() const
		{
			RED_SCOPE_SHARED_LOCK( m_gameContextLock );
			return m_gameContextStack;
		}

		void RuntimeSystemUI::PrepareRenderingState( const tick::Info& tickInfo )
		{
			// Get loading screen state
			Bool isLoadingScreenShown = false;
			if (red::SharedPtr< ink::ILoadingScreen > loadingScreen = GetInkSystem().GetLoadingScreen())
			{
				if (loadingScreen->GetLoadingState() != ink::ILoadingScreen::State::Hidden)
				{
					isLoadingScreenShown = true;
				}
			}
			const Bool exclusiveUIRenderingMode = m_isInMenu.GetValue() || isLoadingScreenShown;

			// Get pause state.
			Bool isInMenuWithBackground = false;
			Bool shouldBlendInZeroTime = false;
			if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
			{
				ink::State state = ink::State::Invalid;
				if( const THandle<ink::StateMachine> stateMachine = viewportWrapper->GetStateMachine() )
				{
					state = stateMachine->GetActiveUIState();
				}
				isInMenuWithBackground = ( state == ink::State::PauseMenu ) || ( state == ink::State::InGameMenu );
				shouldBlendInZeroTime = ( state == ink::State::InGameMenu );
			}

			// Set blur effect for pause state.
			// Disable blur effect in save menu after the final background screen capture was performed.
			// We do that to make sure that there is no blur effect for save thumbnail screenshots.
			const Bool isBlurEffectEnabled = !m_backgroundGameRenderMode && isInMenuWithBackground && ( !m_renderingModePrepared || !exclusiveUIRenderingMode || !m_exclusiveUIRenderingMode.GetValue() || !m_isInSaveMenu );
			if( m_enviromentParamsManager != nullptr )
			{
				if( m_isBlurEffectEnabled.Exchange( isBlurEffectEnabled ) != isBlurEffectEnabled )
				{
					const Float overrideBlendTime = shouldBlendInZeroTime ? 0.0f : -1.0f/* use default blend time */;
					if( isBlurEffectEnabled )
					{
						m_enviromentParamsManager->SetBlurEnabled( true, overrideBlendTime );
					}
					else
					{
						m_enviromentParamsManager->SetBlurEnabled( false );
					}
				}
			}

			// Checks if we can enable exclusive render mode. For that the background screenshot must be prepared.
			Bool enableExclusiveRenderMode = false;
			// We are in pause menu - perform background screen capture before enabling exclusive mode.
			if( isInMenuWithBackground && exclusiveUIRenderingMode )
			{
				// Finial screenshot was performed.
				if( m_renderingModePrepared )
				{
					// Enabled next frame after taking screenshot.
					enableExclusiveRenderMode = true;
				}
				else
				{
					// Take a screenshot
					if( red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock() )
					{
						viewportWrapper->PerformBackgroundFrameCapture();
					}

					const Float blendTime = m_enviromentParamsManager ? m_enviromentParamsManager->GetBlendTime() : -1.0f/* No blur, perform capture at the first frame. */;
					if( m_renderingModeSetDelay > blendTime )
					{
						// After we fully blended - mark that the final background screenshot is ready.
						m_renderingModePrepared = true;
					}
					else
					{
						// Update after the check.
						m_renderingModeSetDelay += tickInfo.m_realTimeDelta;
					}
				}
			}
			else
			{
				enableExclusiveRenderMode = exclusiveUIRenderingMode;
				m_renderingModePrepared = false;
				m_renderingModeSetDelay = 0.0f;
			}

			enableExclusiveRenderMode &= !m_backgroundGameRenderMode;

			// Set current rendering mode.
			if( m_exclusiveUIRenderingMode.Exchange( enableExclusiveRenderMode ) == !enableExclusiveRenderMode )
			{
				SetCorrectRenderingState( enableExclusiveRenderMode );
			}
		}

		void RuntimeSystemUI::SetCorrectRenderingState( Bool exclusiveUIRenderingMode )
		{
			if( m_renderingSystem )
			{
				RED_LOG( "UI: Rendering mode set to: %hs", exclusiveUIRenderingMode ? "FULLSCREEN" : "WORLD" );
				m_renderingSystem->EnableNonInteractiveRendering( exclusiveUIRenderingMode );
			}
		}

		void RuntimeSystemUI::ApplyHiddenLayersMask() const
		{
			if (red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_controlledViewportWrapper.Lock())
			{
				viewportWrapper->ApplyDebugLayerVisibility(m_hiddenLayers);
			}
		}

		void RuntimeSystemUI::SetIsInSaveMenu( Bool isInSaveMenu )
		{
			m_isInSaveMenu = isInSaveMenu;
		}

		void RuntimeSystemUI::SetHDRCalibrationScreen( Bool enabled, const THandle<CBitmapTexture> texture, const Rect &fromRect, const Rect &toRect )
		{
			m_calibrationScreenActive = enabled;
			m_calibrationScreenFromRect = fromRect;
			m_calibrationScreenToRect = toRect;
			m_calibrationTexture = texture;
		}

		void RuntimeSystemUI::SetRenderGameInBackground( Bool enabled )
		{
			m_backgroundGameRenderMode = enabled;
		}

	}
}
