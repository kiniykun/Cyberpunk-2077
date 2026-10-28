#include <cstdio>
/**
 * Copyright (C) 2017-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include "input-web.h"

#include "../../input/include/inputContext.h"
#include "../../input/include/inputManagerView.h"
#include "../../sound/include/audioInterfaceBuilder.h"
#include "../../sound/include/audioCustomEntityIdGenerator.h"
#include "../../sound/include/audioInterface.h"
#include "../../sound/include/audioEventFactory.h"
#include "../../sound/include/emitterInterfaceBuilder.h"

#include "systemRequestsHandlerInterface.h"
#include "../../textSystem/include/typographyManager.h"
#include "styleThemeManager.h"
#include "virtualWindow.h"
#include "inputManager.h"
#include "inputKeyIconManager.h"
#include "focusEvent.h"
#include "pointerEvent.h"
#include "characterEvent.h"
#include "globalWindowActionListener.h"
#include "hittestGrid.h"
#include "userSettings.h"
#include "inkSystem.h"
#include "arrangedChildren.h"
#include "../../inkRendering/include/rectangle.h"

#ifdef INK_ENABLE_PROFILING
#include "../../inkCore/include/dynamicMetricsManager.h"
#endif

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
#include "inkFunctionalTesting.h"
#endif

#include "layersResource.h"
#include "inkGameSettingsResource.h"
#include "inkResourceManager.h"
#include "iconAtlasManagerNull.h"
#include "localizationUtils.h"

#include "../../renderData/include/viewport.h"

#ifndef RED_CONFIGURATION_FINAL
#include "inkSpawnDebugger.h"
#endif
#include "../../redReflection/include/gatheredResource.h"
#include "../../redConfig/include/inGameConfigVar.h"
#include "../../redConfig/include/inGameConfigSystem.h"
#include "../../redConfig/include/inGameConfigGroup.h"
#include "../../textSystem/include/shapedTextCache.h"
#include "../../textSystem/include/typographyUtils.h"
#include "../../inkCore/include/fullscreenCompositionManager.h"
#include "loadingLayer.h"
#include "inkMaskDynamicTextureManager.h"
#include "nullCameraDescriptor.h"
#include "e3DemoHelpers.h"
#include "gameNotificationsUtilities.h"
#include "stateMachine_States.h"
#include "stateMachine.h"
#include "../../sound/include/soundSystem.h"
#include "../../textSystem/include/textParameterSet.h"
#include "../../inkRendering/include/drawUtils.h"
#include "advertisementsUtils.h"
#include "onscreenVOHandler.h"
#include "inputKeyData.h"


#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif
namespace ink
{
	extern INK_CORE_API void LoadDefaultShapeCollectionResources();
	extern INK_CORE_API void ClearDefaultShapeCollectionResource();
}

namespace
{
	// #TODO_UI : Should not be static, but rather as a parameter for game project settings
	static res::TStaticResource< ink::GameSettingsResource > s_defaultGameSettings( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\cp_2077.inkgamesettings" ), GRF_SkipOnHeadless | GRF_Startup );

	static ink::InkSystem* s_inkSystem = nullptr;
	static CName s_contextExplorationSimple( RED_NAME_CONSTEXPR( "ExplorationSimple" ) );
	static CName s_configGameplayUI( RED_NAME_CONSTEXPR( "/ui_to_remove" ) );
	static CName s_configBrightness( RED_NAME_CONSTEXPR( "/brightness" ) );
	static CName s_configLanguage( RED_NAME_CONSTEXPR( "/language" ) );
	static CName s_configOnScreen( RED_NAME_CONSTEXPR( "OnScreen" ) );

	template<class EventType>
	void RouteEvent( const THandle<ink::Widget>& widget, const THandle<EventType>& event, red::FixedSizeFunction<void( const THandle<ink::Widget>&, const THandle<EventType>& )> func )
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		if( widget )
		{
			event->SetTarget( widget );

			THandle<ink::Widget> widgetHandle = widget;
			while( widgetHandle )
			{
				if( event->IsCanceled() )
					break;

				event->SetCurrentTarget( widgetHandle );
				func( widgetHandle, event );

				widgetHandle = widgetHandle->GetParentWidget();
			}
		}
	}

	Int8 GetSign( float x )
	{
		return Int8( x > 0 ) - Int8( x < 0 );
	}

	CName UIBase = RED_NAME_CONSTEXPR( "UIBase" );
	CName UIAiming = RED_NAME_CONSTEXPR( "Aiming" );
	CName RELEASE_ALL = RED_NAME_CONSTEXPR( "RELEASE_ALL" );
}

namespace ink
{
	InkSystem::InkSystem()
		: m_pointerScreenSpacePosition( Vector2::ZEROS() )
		, m_pointerWindowSpacePosition( Vector2::ZEROS() )
		, m_viewportCreator( nullptr )
		, m_mode( InkSystemMode::Game )
		, m_pointedWidgetsPath( ink::PoolInk_System() )
		, m_inkCurrentGameState( InkGameState::Game )
		, m_layersResource( nullptr )
		, m_inputManager( nullptr )
		, m_uiInputManager( red::CreateUniquePtr<InputManager>() )
		, m_activeDeviceID( ~0 )
#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		, m_isInVerifyMode{ false }
#endif
		, m_uiListenerID{}
		, m_brightnessListenerID{}
		, m_languageListenerID{}
		, m_keyBindingSettingsListeners( ink::PoolInk_System() )
	{
	}

	InkSystem::~InkSystem()
	{
		s_inkSystem = nullptr;
	}

	Bool InkSystem::Initialize( rend::IViewportCreator* viewportCreator )
	{
#ifndef __SHIPPING__
		CheckNoInputIconsModeFlag();
#endif

		RED_ASSERT( !s_inkSystem, "Ink: Multiple Ink framework initialization" );
		s_inkSystem = this;
		m_resourceManager = red::CreateUniquePtr<ResourceManager>();
		m_resourceManager->Initialize();

		m_maskDTManager = red::CreateUniquePtr< MaskDTManager>();
		m_onscreenVOHandler = red::CreateUniquePtr< OnscreenVOHandler >();

		m_typographyManager = red::CreateUniquePtr<TypographyManager>();

		m_fullscreenCompositionManager = CreateHandle< FullscreenCompositionManager >();
		m_fullscreenCompositionManager->Initialize();

		text::InitializeTypographyManager( m_typographyManager.Get() );

#ifndef RED_CONFIGURATION_FINAL
		m_spawnDebugger = red::CreateUniquePtr< SpawnDebugger >();
#endif

		m_styleThemeManager = red::CreateUniquePtr< StyleThemeManager >();
		m_styleThemeManager->Initialize();

		m_uiInputManager->LoadXML( "config\\uiInputActions.xml" );
		m_inputKeyIconManager = red::CreateUniquePtr<InputKeyIconManager>();

		m_globalWindowActionListener = red::CreateUniquePtr<GlobalWindowActionListener>();

		m_internationalizationManager = red::CreateUniquePtr<InternationalizationManager>();
		m_internationalizationManager->Initialize();

		m_iconAtlasManager = red::CreateUniquePtr< NullIconAtlasManager >();
		m_iconAtlasManager->Initialize();
		m_efectTexturePools = RED_NEW( ink::EfectRenderTexturePools, inkRend::PoolInkRendering_Composition() );

		RED_ASSERT( viewportCreator, "Viewport creator cannot be empty in this place. Probably initialization order is incorrect." );
		m_viewportCreator = viewportCreator;
		m_viewportCreator->RegisterListener( this );

		LoadDefaultShapeCollectionResources();

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests = red::CreateUniquePtr<tests::FunctionalTestsImpl>();
		m_functionalTestPointerEventCallbackId = m_functionalTests->m_events.OnPointerEvent += [ this ]( tests::EInputEventType type, const THandle<PointerEvent>& evt )
		{
			switch ( type )
			{
			case tests::EInputEventType::Press:
				OnPressEvent(evt);
				break;

			case tests::EInputEventType::Hold:
				OnHoldEvent(evt);
				break;

			case tests::EInputEventType::Repeat:
				OnRepeatEvent(evt);
				break;

			case tests::EInputEventType::Release:
				OnReleaseEvent(evt);
				break;

			case tests::EInputEventType::Axis:
				OnAxisEvent(evt);
				break;

			case tests::EInputEventType::Relative:
				OnRelativeEvent(evt);
				break;
			}
		};
#endif

		LoadAllOptions();

		RegisterSettingsListener();

		text::shapedTextCacheUtil::CreateDefaultTextShaper();

		return LoadLayersDefinition();
	}

	void InkSystem::Shutdown()
	{
		m_iconAtlasManager->Shutdown();
		m_fullscreenCompositionManager->Shutdown();

		UnregisterSettingsListener();

		DestroyAudioInterface();

		m_viewportCreator->UnregisterListener( this );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->m_events.OnPointerEvent -= m_functionalTestPointerEventCallbackId;
#endif
		ClearDefaultShapeCollectionResource();

		m_layersResource = nullptr;

		text::shapedTextCacheUtil::ClearTextShaper();
		text::UnInitializeTypographyManager();

		m_pointedWidgetsPath.Clear();
		m_viewportWrappers.Clear();

		// We have to destroy after all widgets.
		// The method calls u_cleanup() inside which clears custom allocators in icu.
		// We can call this when we are sure that all icu objects was already destroyed.
		m_internationalizationManager->Shutdown();

		RED_DELETE( m_efectTexturePools, inkRend::PoolInkRendering_Composition() );
	}

	void InkSystem::BeginFrame()
	{
		inkRend::ResetOneFrameAllocator();
	}

	void InkSystem::Tick( const tick::Info& tickInfo, input::IInputContext& input )
	{
		ScopedProfilerChannel scopedChannel( PBC_UI );
		PC_SCOPE_FUNC();

		m_efectTexturePools->ResetAvailablePoolIndices();
		ProcessInput( tickInfo, input );

		m_internationalizationManager->Update();	// to check language change

		m_fullscreenCompositionManager->Tick( tickInfo );

		m_iconAtlasManager->OnDraw();

		m_onscreenVOHandler->Update();

#ifdef INK_ENABLE_PROFILING
		ink::DynamicMetricsManager::Get().Update( tickInfo );
#endif

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->Update( tickInfo.m_realTimeDelta );
#endif

#ifndef RED_CONFIGURATION_FINAL
		GetSpawnDebugger().Tick();
#endif

		FreeRemovedWidgetsWithResources();
		if( m_requestReleaseRenderTargets )
		{
			m_efectTexturePools->ReleaseWidgetCompositorSharedTexturesPool();
			m_requestReleaseRenderTargets = false;
		}

		if( m_requestReleaseFullscreenRenderTargets )
		{
			m_efectTexturePools->ReleaseFullscreenWidgetCompositorSharedTexturesPool();
			m_requestReleaseFullscreenRenderTargets = false;
		}

		if (!IsEditorMode() && !::audio::IsAudioRuntimeReady())
		{
			job::Builder builder;
			::audio::ForceSystemUpdate( tickInfo, builder );
			job::FlushCounter(builder.ExtractWaitCounter());
		}
	}

	// SimpleTick is called only when the world is not loaded yet
	// In that situation RuntimeSystemUI does not exist yet
	// Because of that InkSystem has to take responsibility for updating and drawing for the viewport
	// It is always called from main thread
	void InkSystem::SimpleTick( const tick::Info& tickInfo, input::IInputContext& input )
	{
		ScopedProfilerChannel scopedChannel( PBC_UI );
		PC_SCOPE_FUNC();

		{
			// CP2077 ui simpletick probe: log on transitions of (gameMode, wrappers, controlled) and at counts 1,2,4,...,every 4096.
			static unsigned s_cp2077Ticks = 0, s_cp2077Last = ~0u;
			const unsigned cp2077Wrappers = unsigned( m_viewportWrappers.Size() );
			const unsigned cp2077Controlled = ( cp2077Wrappers == 1 && m_viewportWrappers[0] ) ? ( m_viewportWrappers[0]->IsControlled() ? 1u : 0u ) : 2u;
			const unsigned cp2077Key = ( IsGameMode() ? 1u : 0u ) | ( cp2077Controlled << 1 ) | ( cp2077Wrappers << 3 );
			++s_cp2077Ticks;
			if ( cp2077Key != s_cp2077Last || ( s_cp2077Ticks & ( s_cp2077Ticks - 1 ) ) == 0 || ( s_cp2077Ticks % 4096 ) == 0 )
			{
				std::fprintf( stderr, "CP2077 ui state probe: SimpleTick tick=%u gameMode=%u mode=%d wrappers=%u controlled=%u wrapper0=%p willRender=%u\n", s_cp2077Ticks, unsigned( IsGameMode() ), int( m_mode ), cp2077Wrappers, cp2077Controlled, cp2077Wrappers ? (void*)m_viewportWrappers[0].Get() : nullptr, unsigned( IsGameMode() && cp2077Wrappers == 1 && cp2077Controlled == 0 ) );
				s_cp2077Last = cp2077Key;
			}
		}
		if ( IsGameMode() && m_viewportWrappers.Size() == 1 )
		{
			const red::SharedPtr< ViewportWrapper > viewport = m_viewportWrappers[0];
			if ( viewport->IsControlled() )
			{
				return;
			}

			Tick( tickInfo, input );
			
			job::Builder builder;
			{
				viewport->EstablishUISystemState(tickInfo, builder);
				PerformNoSceneRendering(viewport, tickInfo, builder);
			}
			job::FlushCounter(builder.ExtractWaitCounter());
		}
	}

	void InkSystem::PerformNoSceneRendering( const red::SharedPtr< ViewportWrapper >& viewport, const tick::Info& tickInfo, job::Builder& builder )
	{
		NullCameraDescriptor cameraDescriptor;
		Vector2 viewportSize( 1980.0f, 1024.0f );

		if (auto nativeViewport = viewport->GetNativeViewport().Lock())
		{
			viewportSize.X = static_cast<Float>( nativeViewport->GetWidth() );
			viewportSize.Y = static_cast<Float>( nativeViewport->GetHeight() );
		}

		ink::UpdateContext updateContext { tickInfo, cameraDescriptor, viewportSize, ink::UIMode::Normal };

		viewport->ProcessFullscreenUI( updateContext, builder );
	}

	void InkSystem::SimpleRender( CRenderFrameInfo& frame )
	{
		ScopedProfilerChannel scopedChannel( PBC_UI );
		PC_SCOPE_FUNC();

		static unsigned s_cp2077SimpleRender = 0;
		++s_cp2077SimpleRender;
		if ( ( s_cp2077SimpleRender & ( s_cp2077SimpleRender - 1 ) ) == 0 || ( s_cp2077SimpleRender % 4096 ) == 0 )
		{
			std::fprintf( stderr, "CP2077 ui state probe: SimpleRender count=%u frame=%p mode=%d wrappers=%u callsWrapperRenderFrame=%u\n", s_cp2077SimpleRender, (void*)&frame, int( m_mode ), unsigned( m_viewportWrappers.Size() ), unsigned( ( m_mode == InkSystemMode::Game ) && ( m_viewportWrappers.Size() == 1 ) ) );
		}
		if ( ( m_mode == InkSystemMode::Game ) && ( m_viewportWrappers.Size() == 1 ) )
		{
			const auto &wrapper = m_viewportWrappers[ 0 ];
			wrapper->RenderFrame( frame );
		}
	}

	const ink::GameSettingsResource* InkSystem::GetGameSettings() const
	{
		return s_defaultGameSettings.Get().Get();
	}

	red::SharedPtr<ViewportWrapper> InkSystem::GetGameViewportWrapper() const
	{
		if ( ( m_mode == InkSystemMode::Game ) && ( m_viewportWrappers.Size() == 1 ) )
		{
			return m_viewportWrappers[ 0 ];
		}

		return nullptr;
	}

	void InkSystem::SetInputManager( game::input::ManagerView & manager )
	{
		m_inputManager = &manager;
	}

	void InkSystem::ResetInputManager( game::input::ManagerView & manager )
	{
		m_inputManager = nullptr;
	}

	game::input::ManagerView* InkSystem::GetInputManager()
	{
		return m_inputManager;
	}

	void InkSystem::ResetToActiveLayerContext()
	{
		if ( m_inputManager )
		{
			m_inputManager->UnregisterListener( this );
			m_inputManager->ResetUIContext( UIBase );

			m_uiInputManager->DisableAllFilters();

			const auto& activeLayer = m_activeLayer.ToHandle();
			const auto& contextName = activeLayer ? activeLayer->GetInputContext() : CName::NONE();

			if ( !contextName.Empty() )
			{
				if ( activeLayer->IsUsingGameInput() )
				{
					m_inputManager->RegisterListener( this );
					m_inputManager->StoreUIContext( contextName );
				}
				else
				{
					m_uiInputManager->EnableFilter( contextName );
				}

				m_inputManager->StoreContext( RELEASE_ALL );
				m_inputManager->PopContext();

				m_inputManager->TogglePlayerStackActive( false );
			}
			else
			{
				if ( activeLayer && activeLayer->IsUsingGameInput() )
				{
					m_inputManager->RegisterListener( this );
				}

				m_inputManager->TogglePlayerStackActive( true );
			}
		}
		else
		{
			m_uiInputManager->DisableAllFilters();

			const auto& activeLayer = m_activeLayer.ToHandle();
			const auto& contextName = activeLayer ? activeLayer->GetInputContext() : CName::NONE();

			if ( !contextName.Empty() )
			{
				if ( !activeLayer->IsUsingGameInput() )
				{
					m_uiInputManager->EnableFilter( contextName );
				}
			}
		}

		if ( m_storedUiInputContext.IsValid() )
		{
			const auto& activeLayer = m_activeLayer.ToHandle();
			if ( activeLayer && activeLayer->GetType() == m_storedUiInputContext.m_layerType )
			{
				StoreUIContext( m_storedUiInputContext.m_contextName, m_storedUiInputContext.m_exclusively );
			}
		}
	}

	void InkSystem::StoreUIContext( CName contextName, Bool exclusively )
	{
		if ( m_inputManager )
		{
			if ( contextName != m_inputManager->GetCurrentUIContext() )
			{
				m_inputManager->StoreUIContext( contextName );

				if ( const auto& activeLayer = m_activeLayer.ToHandle() )
				{
					m_storedUiInputContext.Reset( activeLayer->GetType(), contextName, exclusively );
				}
				else
				{
					m_storedUiInputContext.Reset();
				}
			}

			if ( exclusively )
			{
				m_inputManager->StoreContext( RELEASE_ALL );
				m_inputManager->PopContext();

				m_inputManager->TogglePlayerStackActive( false );
			}
		}
	}

	void InkSystem::ResetStoredUIContext()
	{
		if ( m_storedUiInputContext.IsValid() )
		{
			m_storedUiInputContext.Reset();
			ResetToActiveLayerContext();
		}
	}

	EInputKey InkSystem::GetUIActionToCurrentRawInput( CName action ) const
	{
		if( m_inputManager )
		{
			return m_inputManager->GetUIActionToCurrentRawInput( action, GetActiveInputDevice() );
		}

		return IK_None;
	}

	EInputKey InkSystem::GetUIActionToCurrentRawInput( CName contextName, CName action ) const
	{
		if ( m_inputManager )
		{
			if ( contextName.Empty() || contextName == CName::NONE() )
			{
				return m_inputManager->GetUIActionToCurrentRawInput( action, GetActiveInputDevice() );
			}
			else
			{
				return m_inputManager->GetUIActionToCurrentRawInput( contextName, action, GetActiveInputDevice() );
			}
		}

		return IK_None;
	}

	InputKeyData InkSystem::GetUIActionToInputKeyData( CName action ) const
	{
		if( m_inputManager )
		{
			InputKeyData keyData = m_inputManager->GetUIActionToCurrentRawInput( action, GetActiveInputDevice() );
			keyData.SetIsHoldInput( m_inputManager->IsHoldAction( action ) );

			return keyData;
		}

		return InputKeyData( IK_None );
	}

	String InkSystem::GetUIActionToCurrentInputName( CName action ) const
	{
		if ( m_inputManager )
		{
			return m_inputManager->GetUIActionToCurrentInputName( action, GetActiveInputDevice() );
		}

		return String::EMPTY();
	}

	String InkSystem::GetUIActionToCurrentInputName( CName contextName, CName action ) const
	{
		if ( m_inputManager )
		{
			return m_inputManager->GetUIActionToCurrentInputName( contextName, action, GetActiveInputDevice() );
		}

		return String::EMPTY();
	}

	void InkSystem::GetActionToInputKeyDataList( CName action, red::DynArray< InputKeyData >& keys, input::EInputDevice deviceId /*= input::EInputDevice::INVALID */ )
	{
		if( m_inputManager )
		{
			return m_inputManager->GetActionToInputKeyDataList( action, keys, deviceId );
		}
	}

	void InkSystem::GetActionToInputKeyDataList( CName contextName, CName action, red::DynArray< InputKeyData >& keys, input::EInputDevice deviceId /*= input::EInputDevice::INVALID */ )
	{
		if ( m_inputManager )
		{
			if ( contextName.Empty() || contextName == CName::NONE() )
			{
				return m_inputManager->GetActionToInputKeyDataList( action, keys, deviceId );
			}
			return m_inputManager->GetActionToInputKeyDataList( contextName, action, keys, deviceId );
		}
	}

	THandle<Widget> InkSystem::GetKeyboardFocusedWidget()
	{
		if( auto widget = m_focusedWidget.ToHandle() )
		{
			return widget;
		}

		return nullptr;
	}

	void InkSystem::SetFocusedWidget( const FocusEventHandle& event )
	{
		const auto& focusedWidget = event->GetCurrentTarget().ToHandle();

		if( auto widget = m_focusedWidget.ToHandle() )
		{
			if( widget == focusedWidget )
			{
				return;
			}

			widget->OnFocusLostEvent( event );
		}

		m_focusedWidget = nullptr;

		if (focusedWidget && focusedWidget->SupportsKeyboardFocus())
		{
			focusedWidget->OnFocusReceivedEvent(event);
			m_focusedWidget = focusedWidget;
		}
	}

	InputKeyIconManager& InkSystem::GetInputKeyIconManager()
	{
		return *m_inputKeyIconManager.Get();
	}

	TypographyManager& InkSystem::GetTypographyManager() const
	{
		return *m_typographyManager.Get();
	}

	FullscreenCompositionManager & InkSystem::GetFullscreenCompositionManager() const
	{
		return *m_fullscreenCompositionManager.Get();
	}

	const THandle< ink::FullscreenCompositionManager >& InkSystem::GetFullscreenCompositionManagerHandle() const
	{
		return m_fullscreenCompositionManager;
	}

	void InkSystem::SetMode(InkSystemMode mode)
	{
		m_mode = mode;

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->Init();
#endif
	}

	ink::InkSystemMode InkSystem::GetMode() const
	{
		return m_mode;
	}

	void InkSystem::InitializeUISoundSystem()
	{
		CreateAudioInterface();
	}

	void InkSystem::PlaySound( CName soundEventName )
	{
		if( m_audioInterface )
		{
			m_audioInterface->PostEvent( ::audio::CreatePlayEvent( soundEventName ) );
		}
	}

	void InkSystem::PlaySound( audio::AudioEvent* audioEvent, audio::AudioEventId* eventId /*= nullptr */)
	{
		if (m_audioInterface)
		{
			m_audioInterface->PostEvent( audioEvent, eventId );
		}
	}

	const Vector2& InkSystem::GetPointerScreenSpacePosition() const
	{
		return m_pointerScreenSpacePosition;
	}

	void InkSystem::SetPointerScreenSpacePosition( const Vector2 & newPosition )
	{
		m_pointerScreenSpacePosition = newPosition;
	}

	void InkSystem::OnViewportCreate( ViewportHandle viewport, rend::ViewportType viewportType )
	{
		viewport->RegisterActionListener( m_globalWindowActionListener.Get() );

		SpawnViewportWrapper( viewport, viewportType );

		Vector2 texturesSize = Vector2( 4096, 2160 );
		
		if ( viewportType == rend::ViewportType::GameViewport )
		{
			texturesSize = Vector2( (Float)viewport->GetWidth(), (Float)viewport->GetHeight() );
		}

		// Create shared pool for widget compositors.
		{
			RED_LOG_WARNING( "[INKSYSTEM][VIEWPORTSIZE] : ( %d , %d )", viewport->GetWidth(), viewport->GetHeight() );
			m_efectTexturePools->InitializePools( texturesSize );
		}
	}

	void InkSystem::OnViewportClose( ViewportId viewportId )
	{
		red::WeakPtr<ViewportWrapper> viewportWrapperWeak = GetViewportWrapper( viewportId );

		if (red::SharedPtr<ink::ViewportWrapper> viewportWrapper = viewportWrapperWeak.Lock())
		{
			if (ViewportHandle viewport = viewportWrapper->GetNativeViewport().Lock())
			{
				viewport->UnregisterActionListener( m_globalWindowActionListener.Get() );
			}
		}

		DestroyViewportWrapper(viewportId);
	}

	void InkSystem::RequestReleaseSharedTexturesPool()
	{
		m_requestReleaseRenderTargets = true;
	}

	void InkSystem::RequestReleaseFullscreenSharedTexturesPool()
	{
		m_requestReleaseFullscreenRenderTargets = true;
	}
	
	res::GatheredResourceReference g_layersDefinition( RED_CONST_RESOURCEPATH( "base\\gameplay\\gui\\ui_game.inklayers" ), GRF_Default );

	Bool InkSystem::LoadLayersDefinition()
	{
		if (!m_layersResource)
		{
			res::ResourceTokenHandle loadingToken = GResourceLoader->IssueLoadingRequest( g_layersDefinition.GetPath() );
			m_layersResource = Cast<LayersResource>(loadingToken->WaitUntilLoaded());

			const char* errorText = res::GetResourceTokenErrorTypeText( loadingToken->GetError() );
			RED_LOG_CATEGORY_ERROR(red::LoggerCategory_UserInterface, "Loading error info about layers definition: %hs", errorText);
		}

		return m_layersResource != nullptr;
	}

	void InkSystem::ProcessInput(const tick::Info& tickInfo, input::IInputContext& inputContext)
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		m_uiInputManager->Update( tickInfo );

		if ( m_viewportWrappers.Empty() )
			return;

		// retrieve rect of last widget under cursor
		Rectangle lastWidgetUnderCursorRect;

		if ( const THandle<const Widget>& widget = m_lastWidgetUnderCursor.ToHandle() )
		{
			if ( const THandle<const Widget>& parent = widget->GetParentWidget() )
			{
				const ArrangedChildren& arrangedChildren = parent->GetArrangedChildren();
				if ( const ArrangedWidget* arrangedWidget = arrangedChildren.GetChildByWidget( widget ) )
				{
					lastWidgetUnderCursorRect = arrangedWidget->GetDrawArea().GetBoundaryRect();
				}
			}
		}

		auto& inputEvents = inputContext.GetBufferedInputData();

		Vector2 mousePos{ m_pointerScreenSpacePosition };
        bool cpWebAbsoluteMouseMotion=false;
		Vector2 mouseOffset;
		static Vector2 gamepadOffset;
		ViewportID activeViewportID;

		// calc mouse and gamepad offsets
		for ( const auto& inputEvent : inputEvents )
		{
			activeViewportID = inputEvent.m_viewportID;

			const input::BufferedInputEvent::Raw& rawData = inputEvent.m_raw;
			if ( rawData.m_action == IACT_Axis )
			{
                if(rawData.m_key==IK_MouseX || rawData.m_key==IK_MouseY)cpWebAbsoluteMouseMotion=true;
				if ( rawData.m_key == IK_MouseX )
					mouseOffset.X += rawData.m_data;
				else if ( rawData.m_key == IK_MouseY )
					mouseOffset.Y += rawData.m_data;
				else if ( rawData.m_key == IK_Pad_LeftAxisX )
					gamepadOffset.X += rawData.m_data - gamepadOffset.X;
				else if ( rawData.m_key == IK_Pad_LeftAxisY )
					gamepadOffset.Y += -rawData.m_data - gamepadOffset.Y;

				mousePos.Set( static_cast< Float >( rawData.m_x ), static_cast< Float >( rawData.m_y ) );
			}
		}

		CheckDeadZones( gamepadOffset );

		// get active viewport ID
		if ( inputEvents.Empty() )
		{
			auto activeViewport = m_activeViewport.Lock();
			if ( !activeViewport )
			{
				activeViewport = m_viewportWrappers.Front();
			}

			if ( const auto& nativeViewport = activeViewport->GetNativeViewport().Lock() )
			{
				activeViewportID = nativeViewport->GetViewportID();
			}
		}

		// get active layer
		LayerHandle activeLayer;
		for ( const red::SharedPtr<ViewportWrapper>& viewportWrapperPtr : m_viewportWrappers )
		{
			const auto& nativeViewport = viewportWrapperPtr->GetNativeViewport().Lock();

			if ( nativeViewport && nativeViewport->GetViewportID() == activeViewportID )
			{
				m_activeViewport = viewportWrapperPtr;

				const red::DynArray<LayerHandle>& layers = viewportWrapperPtr->GetLayers();
				for ( const LayerHandle& layer : layers )
				{
					if ( layer->GetActive() && layer->GetInteractable() )
					{
						activeLayer = layer;
						break;
					}
				}
			}
		}

		const auto& currentLayer = m_activeLayer.ToHandle();
		if ( activeLayer != currentLayer )
		{
			ProcessPointerMotionEvent( currentLayer );

			m_activeLayer = activeLayer;
			ResetToActiveLayerContext();

			if ( currentLayer )
			{
				currentLayer->OnActiveLayer( false );
			}

			if ( activeLayer )
			{
				activeLayer->OnActiveLayer( true );
			}
		}

		// [cursorpin] Tell the browser input layer whether a menu ink cursor is shown (sessions hide the OS cursor even in menus).
		{
			const THandle<ink::VirtualWindow> cpWebRoot = activeLayer ? activeLayer->GetRootWindow() : nullptr;
			// [touch] Not the World layer: it is always interactable and keeps its root cursor visible (worldLayer.cpp
			// SetCursorVisibility(true)), so it is the active layer in plain gameplay. Its "cursor" is the crosshair on in-world
			// device screens, driven by the camera, not by the pointer; reporting it made gameplay look like a menu (no
			// click-to-lock pointer, unlocked stray moves not dropped, touch overlay in menu mode).
			const ELayerType cpWebType = activeLayer ? activeLayer->GetType() : ELayerType::Editor;
			input::SetBrowserInkCursorVisible( cpWebRoot && cpWebType != ELayerType::Editor && cpWebType != ELayerType::World && cpWebRoot->IsCursorVisibile() );
		}

		if ( THandle<ink::VirtualWindow> currentWindow = activeLayer ? activeLayer->GetRootWindow() : nullptr )
		{
			if ( !currentWindow->IsCursorVisibile() )
			{
				mouseOffset = gamepadOffset = Vector2::ZEROS();
			}
		}

		// process pointer
		m_cursorSpeedProcessor.Update( tickInfo, activeLayer, gamepadOffset );

		if ( activeLayer )
		{
			if ( activeLayer->GetType() == ELayerType::Editor )
			{
				m_pointerScreenSpacePosition = mousePos;
			}
			else
			{
				const auto& viewportSize = activeLayer->GetRootWindow()->GetViewportSize();
				// Browser menus follow the physical pointer. Pointer-locked play and
                // gamepad processing retain the original relative path.
                if(cpWebAbsoluteMouseMotion && input::BrowserMouseUsesAbsoluteCoordinates() && activeLayer->GetRootWindow()->IsCursorVisibile()){
                    m_pointerScreenSpacePosition=mousePos;
                    mouseOffset=Vector2::ZEROS();
                }
                UpdateGameWindowPointer( tickInfo, mouseOffset, gamepadOffset, viewportSize, lastWidgetUnderCursorRect );
			}

			ProcessPointerMotionEvent( activeLayer );
		}

		auto it = inputEvents.Begin();
		while ( it != inputEvents.End() )
		{
			const auto& inputEvent = *it;
			const input::BufferedInputEvent::Raw& rawData = inputEvent.m_raw;

			if ( inputContext.IsDeviceConnected( inputEvent.m_deviceID ) || inputContext.GetDeviceType( inputEvent.m_deviceID ) == input::EInputDevice::KBD_MOUSE )
			{
				// TODO: The 'active' input device should be handled separately for each viewport window
				m_inputKeyIconManager->SetActiveInputDevice( inputContext.GetDeviceType( inputEvent.m_deviceID ) );
			}

			if ( m_activeDeviceID != inputEvent.m_deviceID )
			{
				m_activeDeviceID = inputEvent.m_deviceID;
				OnInputDeviceIdChanged( m_activeDeviceID );
			}

			if( ProcessVisibleCharacterEvent( rawData.m_key, rawData.m_action ) )
			{
				const auto& result = inputEvents.Remove( it );
				it = result.IsSuccessful() ? result.Iterator() : ++it;
			}
			else
			{
				if ( rawData.m_action == IACT_Press )
				{
					ProcessPressEvent( rawData.m_key );
				}
				else if ( rawData.m_action == IACT_Release )
				{
					ProcessReleaseEvent( rawData.m_key );
				}
				else if ( rawData.m_action == IACT_DoubleClick )
				{
					ProcessDoubleClickEvent( rawData.m_key );
				}

				++it;
			}
		}
	}

	void InkSystem::FreeRemovedWidgetsWithResources()
	{
		auto it = std::remove_if(m_pointedWidgetsPath.Begin(), m_pointedWidgetsPath.End(),
			[](const THandle<Widget>& w)
		{
			return w.GetRefCount() <= 1;
		});

		m_pointedWidgetsPath.Remove(it, m_pointedWidgetsPath.End());
	}

	void InkSystem::ProcessPointerMotionEvent(const ink::LayerHandle& layer)
	{
		if (layer == nullptr)
		{
			return;
		}

		ink::ICustomHitTester* hittestGrid = layer->GetHitTester();

		if (hittestGrid == nullptr)
		{
			return;
		}

		THandle<ink::VirtualWindow> currentWindow = layer->GetRootWindow();

		if (currentWindow == nullptr)
		{
			return;
		}

		red::DynArray<THandle<Widget>> prevPointedWidgetsPath{ PoolInk_System() };
		prevPointedWidgetsPath = std::move( m_pointedWidgetsPath );

		Vector2 cursorSize;
		currentWindow->GetCursorSize( cursorSize );
		hittestGrid->GetBubblePath( m_pointedWidgetsPath, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition, cursorSize );

		THandle<ink::Widget> newHoveredWidget;
		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition );

		red::DynArray<THandle<Widget>> newPointedWidgetsPath{ PoolInk_System() };
		for (const THandle<ink::Widget> pointedWidget : m_pointedWidgetsPath)
		{
			if (!pointedWidget)
			{
				continue;
			}

			if ( pointedWidget->IsInteractive() )
			{
				if ( !newHoveredWidget )
					newHoveredWidget = pointedWidget;

				if ( !prevPointedWidgetsPath.Remove( pointedWidget ).IsSuccessful() )
				{
					newPointedWidgetsPath.PushBack( pointedWidget );
				}
			}
		}

		currentWindow->SetCursorPosition( m_pointerScreenSpacePosition );

		auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle();
#ifndef RED_CONFIGURATION_FINAL
		if ( m_cursorProcessingInfo.m_showHoweredWidget && !( lastWidgetUnderCursor == newHoveredWidget ) )
		{
			currentWindow->SetWidgetUnderCursor( newHoveredWidget ? newHoveredWidget->WeakHandleFromThis<Widget>() : nullptr );
		}
#endif

		// Propagate HoverOut
		Bool setCursorContext = false;
		for (const THandle<ink::Widget> pointedWidget : prevPointedWidgetsPath)
		{
			if (!pointedWidget)
			{
				continue;
			}

			if ( pointedWidget->IsInteractive() )
			{
				// hover path
				RouteEvent<PointerEvent>( pointedWidget, evt, []( const THandle<ink::Widget>& widget, const PointerEventHandle& event )
				{
					widget->OnHoverOutEvent( event );
				} );

				setCursorContext = true;
			}
		}

		if ( setCursorContext )
		{
			layer->SetCursorContext( RED_NAME_CONSTEXPR( "Default" ), nullptr );
		}

		// Propagate HoverOver
		setCursorContext = false;
		for( const THandle<ink::Widget> pointedWidget : newPointedWidgetsPath )
		{
			if (!pointedWidget)
			{
				continue;
			}

			// hover path
			RouteEvent<PointerEvent>( pointedWidget, evt, [] ( const THandle<ink::Widget>& widget, const PointerEventHandle& event )
			{
				widget->OnHoverOverEvent( event );
			} );

			setCursorContext = true;
		}

		if ( setCursorContext && newHoveredWidget->GetUserData<IgnoresCursorState>() == nullptr )
		{
			layer->SetCursorContext( RED_NAME_CONSTEXPR( "Hover" ), nullptr );
		}

		THandle<ink::Widget> lastHoveredWidget = lastWidgetUnderCursor;
		if( lastHoveredWidget != newHoveredWidget )
		{
			// leave path
			if ( lastHoveredWidget )
			{
				OnWidgetMouseLeave( m_lastWidgetUnderCursor );

				RouteEvent<PointerEvent>( lastHoveredWidget, evt, []( const THandle<ink::Widget>& widget, const PointerEventHandle& event )
				{
					widget->OnLeaveEvent( event );
				} );

				m_lastWidgetUnderCursor = nullptr;
			}

			// enter path
			if( newHoveredWidget )
			{
				m_lastWidgetUnderCursor = newHoveredWidget;

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
				m_functionalTests->RegisterWidgetUnderCursor( m_lastWidgetUnderCursor, m_isInVerifyMode );
#endif

				OnWidgetMouseEnter( m_lastWidgetUnderCursor );

				RouteEvent<PointerEvent>( newHoveredWidget, evt, []( const THandle<ink::Widget>& widget, const PointerEventHandle& event )
				{
					widget->OnEnterEvent( event );
				} );
			}
		}

		m_cursorSpeedProcessor.SetIsInteracting( newHoveredWidget != nullptr );
	}

	void InkSystem::ProcessPressEvent( EInputKey key )
	{
		m_inputKeyModifier.UpdateInputKeyModifiers( key, true );

		if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
		{
			if ( key == IK_LeftMouse )
			{
				OnWidgetMouseButtonDown( m_lastWidgetUnderCursor );
			}
		}

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		if ( m_functionalTests->IsVerificationKey( key ) )
			m_isInVerifyMode = true;

		if ( m_inputKeyModifier.IsLeftShiftDown() && m_inputKeyModifier.IsRightShiftDown() )
		{
			if ( m_functionalTests->IsInState( tests::EPlayerMode::Idle ) )
				m_functionalTests->StartRecording();
			else if ( m_functionalTests->IsInState( tests::EPlayerMode::Recording ) )
				m_functionalTests->StopRecording();
		}
#endif

		auto onPressCallback = [ this ]( const IInputAction& action, EInputKey key )
		{
			OnPressAction( action.GetName() );
		};

		auto target = m_lastWidgetUnderCursor;
		auto onHoldCallback = [ this, target ]( const IInputAction& action, EInputKey key )
		{
			OnHoldAction( action.GetName(), action.GetHoldProgress() );
		};

		m_uiInputManager->OnPress( key, onPressCallback, onHoldCallback );
	}

	void InkSystem::ProcessReleaseEvent( EInputKey key )
	{
		m_inputKeyModifier.UpdateInputKeyModifiers( key, false );

		if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
		{
			if ( key == IK_LeftMouse )
			{
				OnWidgetMouseButtonUp( m_lastWidgetUnderCursor );

				THandle<FocusEvent> evt = CreateHandle<FocusEvent>( FocusCause::Mouse );
				RouteEvent<FocusEvent>( lastWidgetUnderCursor, evt, [ this ]( const THandle<ink::Widget>& widget, const FocusEventHandle& event )
				{
					if ( !widget->SupportsKeyboardFocus() )
					{
						return;
					}

					SetFocusedWidget( event );
					event->Cancel();
				} );
			}
		}

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		if ( m_functionalTests->IsVerificationKey( key ) )
			m_isInVerifyMode = false;
#endif

		auto onReleaseCallback = [ this ]( const IInputAction& action, EInputKey key )
		{
			OnReleaseAction( action.GetName() );
		};

		m_uiInputManager->OnRelease( key, onReleaseCallback );

#if !defined(RED_CONFIGURATION_FINAL) && defined(RED_PLATFORM_WINPC)
// Decide what shortcut is best for fake sign out
//		if ( key == IK_F11 )
//		{
//			if ( auto handler = m_systemRequestsHandler.ToHandle() )
//			{
//				handler->FakeSignout();
//			}
//		}
#endif
	}

	void InkSystem::ProcessDoubleClickEvent( EInputKey key )
	{
		if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
		{
			if ( key == IK_LeftMouse )
			{
				OnWidgetMouseButtonDoubleClick( m_lastWidgetUnderCursor );
			}
		}
	}

	Bool InkSystem::ProcessVisibleCharacterEvent( EInputKey key, EInputAction action )
	{
		// #TODO_UI CYB-158302

		if( auto focusedWidget = m_focusedWidget.ToHandle() )
		{
			const InputKeyModifiers& modifiers = GetModifierKeys();

			// Process control keys

			CharacterEventType eventType = CharacterEventType::CharInput;
			if( key == IK_Left )
			{
				eventType = CharacterEventType::MoveCaretBackward;
			}
			else if( key == IK_Right )
			{
				eventType = CharacterEventType::MoveCaretForward;
			}
			else if( key == IK_Backspace )
			{
				eventType = CharacterEventType::Backspace;
			}
			else if( key == IK_Delete )
			{
				eventType = CharacterEventType::Delete;
			}

			if( eventType != CharacterEventType::CharInput )
			{
				CharacterEventHandle evt = CreateHandle<CharacterEvent>( modifiers, action, eventType );
				focusedWidget->OnCharacterKeyEvent( evt );
				return true;
			}

			// Process visible char

			char visibleCharacter = (char) key;
			Bool isShiftDown = modifiers.IsShiftDown();

			if( key >= IK_A && key <= IK_Z )	// letters
			{
				if( !isShiftDown )
				{
					visibleCharacter += 0x20;
				}
			}
			else if( key >= IK_0 && key <= IK_9 )	// digits
			{
				if( isShiftDown )
				{
					switch( key )
					{
					case IK_0: visibleCharacter = ')'; break;
					case IK_1: visibleCharacter = '!'; break;
					case IK_2: visibleCharacter = '@'; break;
					case IK_3: visibleCharacter = '#'; break;
					case IK_4: visibleCharacter = '$'; break;
					case IK_5: visibleCharacter = '%'; break;
					case IK_6: visibleCharacter = '^'; break;
					case IK_7: visibleCharacter = '&'; break;
					case IK_8: visibleCharacter = '*'; break;
					case IK_9: visibleCharacter = '('; break;
					default: return false;
					}
				}
				else
				{
					visibleCharacter = (char) ( key - IK_0 ) + 0x30;
				}
			}
			else if( key >= IK_NumPad0 && key <= IK_NumPad9 )
			{
				switch( key )
				{
				case IK_NumPad0: visibleCharacter = '0'; break;
				case IK_NumPad1: visibleCharacter = '1'; break;
				case IK_NumPad2: visibleCharacter = '2'; break;
				case IK_NumPad3: visibleCharacter = '3'; break;
				case IK_NumPad4: visibleCharacter = '4'; break;
				case IK_NumPad5: visibleCharacter = '5'; break;
				case IK_NumPad6: visibleCharacter = '6'; break;
				case IK_NumPad7: visibleCharacter = '7'; break;
				case IK_NumPad8: visibleCharacter = '8'; break;
				case IK_NumPad9: visibleCharacter = '9'; break;
				default: return false;
				}
			}
			else if( key == IK_Space )
				visibleCharacter = ' ';
			else if( key == IK_Period )
				visibleCharacter = isShiftDown ? '>' : '.';
			else if( key == IK_Semicolon )
				visibleCharacter = isShiftDown ? ':' : ';';
			else if( key == IK_Equals )
				visibleCharacter = isShiftDown ? '+' : '=';
			else if( key == IK_Comma )
				visibleCharacter = isShiftDown ? '<' : ',';
			else if( key == IK_Minus )
				visibleCharacter = isShiftDown ? '_' : '-';
			else if( key == IK_Backslash )
				visibleCharacter = isShiftDown ? '|' : '\\';
			else if( key == IK_Tilde )
				visibleCharacter = isShiftDown ? '~' : '`';
			else if( key == IK_LeftBracket )
				visibleCharacter = isShiftDown ? '{' : '[';
			else if( key == IK_Slash )
				visibleCharacter = isShiftDown ? '?' : '/';
			else if( key == IK_RightBracket )
				visibleCharacter = isShiftDown ? '}' : ']';
			else if( key == IK_SingleQuote )
				visibleCharacter = isShiftDown ? '"' : '\'';
			else {
				// Unknown character
				return false;
			}

			CharacterEventHandle evt = CreateHandle<CharacterEvent>( modifiers, action, visibleCharacter );
			focusedWidget->OnCharacterKeyEvent( evt );
			return true;
		}

		return false;
	}

	void InkSystem::OnAction( const game::input::ListenerAction& action, game::input::ListenerActionConsumer& consumer )
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		if ( ( m_inputManager && m_inputManager->IsUIStackActive() && m_inputManager->GetCurrentUIContext() != UIBase ) ||
			(IsInWorldInteraction() && m_inputManager->GetCurrentContext() != UIAiming)) 
		{
			Bool consume = false;

			const auto& type = action.GetType();
			switch ( type )
			{
			case game::input::ActionType::BUTTON_PRESSED:
				consume = OnPressAction( action.GetName() );
				break;

			case game::input::ActionType::BUTTON_RELEASED:
				consume = OnReleaseAction( action.GetName() );
				break;

			case game::input::ActionType::BUTTON_HOLD_PROGRESS:
				OnHoldAction( action.GetName(), action.GetValue() );
				break;

			case game::input::ActionType::BUTTON_HOLD_COMPLETE:
				consume = OnHoldAction( action.GetName(), action.GetValue() );
				break;

			case game::input::ActionType::REPEAT:
				OnRepeatAction( action.GetName() );
				break;

			case game::input::ActionType::AXIS_CHANGE:
				consume = OnAxisAction( action.GetName(), action.GetValue() );
				break;

			case game::input::ActionType::RELATIVE_CHANGE:
				consume = OnRelativeAction( action.GetName(), action.GetValue() );
				break;
			}

			if ( IsInWorldInteraction() && consume )
			{
				consumer.Consume();
			}
		}
	}

	Bool InkSystem::OnPressAction( CName actionName )
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterWidgetUnderCursor( m_lastWidgetUnderCursor, true );
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Press, actionName, evt );
#endif

		OnPressEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	Bool InkSystem::OnHoldAction( CName actionName, Float actionHoldProgress )
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, actionHoldProgress, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterWidgetUnderCursor( m_lastWidgetUnderCursor, true );
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Hold, actionName, evt );
#endif

		OnHoldEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	Bool InkSystem::OnRepeatAction(CName actionName)
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterWidgetUnderCursor( m_lastWidgetUnderCursor, true );
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Repeat, actionName, evt );
#endif

		OnRepeatEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	Bool InkSystem::OnReleaseAction(CName actionName)
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterWidgetUnderCursor( m_lastWidgetUnderCursor, true );
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Release, actionName, evt );
#endif

		OnReleaseEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	Bool InkSystem::OnAxisAction( CName actionName, Float actionAxisData )
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition, actionAxisData );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Axis, actionName, evt );
#endif

		OnAxisEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	Bool InkSystem::OnRelativeAction(CName actionName, Float actionAxisData)
	{
		ScopedProfilerChannel scopedChannel(PBC_UI);
		PC_SCOPE_FUNC();

		THandle<PointerEvent> evt =
			CreateHandle<PointerEvent>( m_inputKeyModifier, actionName, m_pointerScreenSpacePosition, m_pointerWindowSpacePosition, actionAxisData );

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
		m_functionalTests->RegisterInputEvent( tests::EInputEventType::Relative, actionName, evt );
#endif

		OnRelativeEvent( evt );
		return evt->IsActionNotified() && ( evt->IsActionMatched() || !evt->GetChecksCount() );
	}

	void InkSystem::OnPressEvent( const THandle<PointerEvent>& evt )
	{
		OnPreOnPress( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [ this ] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnPressEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnPress( evt );
		}
	}

	void InkSystem::OnHoldEvent( const THandle<PointerEvent>& evt )
	{
		OnPreOnHold( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnHoldEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnHold( evt );
		}
	}

	void InkSystem::OnRepeatEvent(const THandle<PointerEvent>& evt)
	{
		OnPreOnRepeat( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [ this ] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnRepeatEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnRepeat( evt );
		}
	}

	void InkSystem::OnReleaseEvent(const THandle<PointerEvent>& evt)
	{
		OnPreOnRelease( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnReleaseEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnRelease( evt );
		}
	}

	void InkSystem::OnAxisEvent( const THandle<PointerEvent>& evt )
	{
		OnPreOnAxis( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnAxisEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnAxis( evt );
		}
	}

	void InkSystem::OnRelativeEvent( const THandle<PointerEvent>& evt )
	{
		OnPreOnRelative( evt );

		if ( !evt->IsCanceled() )
		{
			if ( auto lastWidgetUnderCursor = m_lastWidgetUnderCursor.ToHandle() )
			{
				RouteEvent<PointerEvent>( lastWidgetUnderCursor, evt, [] ( const THandle<ink::Widget>& widget, const PointerEventHandle& e )
				{
					e->SetActionNotified( widget->OnRelativeEvent( e ) );
				} );
			}
		}

		if ( !evt->IsCanceled() )
		{
			OnPostOnRelative( evt );
		}
	}

	void InkSystem::CheckDeadZones(Vector2& gamepadOffset)
	{
		if( m_inputManager )
		{
			// its a quick solution for now, maybe needs rethinking in the future, not sure.
			// at least we have to remove the data dependency from here
			m_inputManager->TryApplyDeadzone( gamepadOffset, RED_NAME_CONSTEXPR( "Preset_2" ) );
		}
	}

	void InkSystem::UpdateGameWindowPointer(const tick::Info& tickInfo,const Vector2& mouseOffset,const Vector2& gamepadOffset,const Vector2& viewportSize,const Rectangle& lastWidgetUnderCursorRect)
	{
		m_pointerScreenSpacePosition += mouseOffset;

		auto newGamepadOffset = gamepadOffset;

		Float absX = Abs( newGamepadOffset.X );
		Float absY = Abs( newGamepadOffset.Y );

		if ( absX - absY > m_cursorProcessingInfo.m_pointerAxisesDiff )
		{
			absY = Min( 1 - absX, absY );
			newGamepadOffset.Y = absY * GetSign( newGamepadOffset.Y );
		}
		else if ( absY - absX > m_cursorProcessingInfo.m_pointerAxisesDiff )
		{
			absX = Min( 1 - absY, absX );
			newGamepadOffset.X = absX * GetSign( newGamepadOffset.X );
		}

		if ( const auto customCursorProcessor = m_customCursorProcessor.Lock() )
		{
			m_pointerScreenSpacePosition = customCursorProcessor->UpdateCursorPosition( tickInfo, mouseOffset, newGamepadOffset, viewportSize, m_pointerScreenSpacePosition );
		}
		else
		{
			switch ( m_cursorProcessingInfo.m_processingType )
			{
			case 0: Version0( tickInfo, newGamepadOffset, lastWidgetUnderCursorRect ); break;
			case 1: Version1( tickInfo, newGamepadOffset, lastWidgetUnderCursorRect ); break;
			case 2: Version2( tickInfo, newGamepadOffset, lastWidgetUnderCursorRect ); break;
			case 3: Version3( tickInfo, newGamepadOffset, lastWidgetUnderCursorRect ); break;
			}
		}

		// Offset from black bars applied on widescreens
		Vector2 uiSize = GetUISize( viewportSize );
		Vector2 uiOffset( ( viewportSize.X - uiSize.X ) / 2.0f , ( viewportSize.Y - uiSize.Y ) / 2.0f );
		Vector2 clampX( uiOffset.X, viewportSize.X - uiOffset.X );
		Vector2 clampY( uiOffset.Y, viewportSize.Y - uiOffset.Y );
		
		m_pointerScreenSpacePosition.X = Clamp( m_pointerScreenSpacePosition.X, clampX.X, clampX.Y );
		m_pointerScreenSpacePosition.Y = Clamp( m_pointerScreenSpacePosition.Y, clampY.X, clampY.Y );
	}

	void InkSystem::Version0( const tick::Info& tickInfo, Vector2& gamepadOffset, const Rectangle& lastWidgetUnderCursorRect )
	{
		if ( gamepadOffset.IsAlmostZero() )
			return;

		const auto& velocity = gamepadOffset * m_cursorSpeedProcessor.GetSpeed();
		m_pointerScreenSpacePosition += velocity * tickInfo.m_realTimeDelta;
	}

	void InkSystem::Version1( const tick::Info& tickInfo, Vector2& gamepadOffset, const Rectangle& lastWidgetUnderCursorRect )
	{
		if ( gamepadOffset.IsAlmostZero() )
			return;

		const auto& size = lastWidgetUnderCursorRect.GetSize().AsVector();
		if ( !size.IsAlmostZero() )
		{
			const auto& pos = lastWidgetUnderCursorRect.GetPosition();
			const auto& halfSize = size * 0.5f;
			const auto& center = pos + halfSize;
			const auto& offset = center - m_pointerScreenSpacePosition;

			gamepadOffset.X *= Clamp( Abs( offset.X ) / halfSize.X, m_cursorProcessingInfo.m_pointerLeaveMinMult, m_cursorProcessingInfo.m_pointerLeaveMaxMult );
			gamepadOffset.Y *= Clamp( Abs( offset.Y ) / halfSize.Y, m_cursorProcessingInfo.m_pointerLeaveMinMult, m_cursorProcessingInfo.m_pointerLeaveMaxMult );
		}

		const auto& velocity = gamepadOffset * m_cursorSpeedProcessor.GetSpeed();
		m_pointerScreenSpacePosition += velocity * tickInfo.m_realTimeDelta;
	}

	void InkSystem::Version2( const tick::Info& tickInfo, Vector2& gamepadOffset, const Rectangle& lastWidgetUnderCursorRect )
	{
		if ( gamepadOffset.IsAlmostZero() )
			return;

		const auto prevPointerScreenSpacePosition = m_pointerScreenSpacePosition;

		const auto& velocity = m_cursorSpeedProcessor.GetSpeed();
		m_pointerScreenSpacePosition += ( gamepadOffset * velocity ) * tickInfo.m_realTimeDelta;

		const auto& size = lastWidgetUnderCursorRect.GetSize().AsVector();
		if ( !size.IsAlmostZero() )
		{
			const auto& pos = lastWidgetUnderCursorRect.GetPosition();
			const auto& halfSize = size * 0.5f;
			const auto& center = pos + halfSize;
			const auto& offset = center - m_pointerScreenSpacePosition;
			const auto& offsetSquareMag = offset.SquareMag();
			if ( offsetSquareMag < ( center - prevPointerScreenSpacePosition ).SquareMag() )
			{
				gamepadOffset.X = Clamp( offset.X / halfSize.X, -m_cursorProcessingInfo.m_pointerSnapSpeed, m_cursorProcessingInfo.m_pointerSnapSpeed );
				gamepadOffset.Y = Clamp( offset.Y / halfSize.Y, -m_cursorProcessingInfo.m_pointerSnapSpeed, m_cursorProcessingInfo.m_pointerSnapSpeed );

				const auto& dist = ( gamepadOffset * velocity ) * tickInfo.m_realTimeDelta;
				if ( offsetSquareMag > dist.SquareMag() )
					m_pointerScreenSpacePosition += dist;
				else
					m_pointerScreenSpacePosition = center;
			}
			else
			{
				gamepadOffset.X *= Clamp( Abs( offset.X ) / halfSize.X, m_cursorProcessingInfo.m_pointerLeaveMinMult, m_cursorProcessingInfo.m_pointerLeaveMaxMult );
				gamepadOffset.Y *= Clamp( Abs( offset.Y ) / halfSize.Y, m_cursorProcessingInfo.m_pointerLeaveMinMult, m_cursorProcessingInfo.m_pointerLeaveMaxMult );

				m_pointerScreenSpacePosition = prevPointerScreenSpacePosition + ( ( gamepadOffset * velocity ) * tickInfo.m_realTimeDelta );
			}
		}
	}

	void InkSystem::Version3( const tick::Info& tickInfo, Vector2& gamepadOffset, const Rectangle& lastWidgetUnderCursorRect )
	{
		static Float trigger = 0.f;

		const auto& velocity = m_cursorSpeedProcessor.GetSpeed();
		const auto& initialDest = ( gamepadOffset * velocity ) * tickInfo.m_realTimeDelta;

		const auto& size = lastWidgetUnderCursorRect.GetSize().AsVector();
		if ( !size.IsAlmostZero() )
		{
			const auto& pos = lastWidgetUnderCursorRect.GetPosition();
			const auto& halfSize = size * 0.5f;
			const auto& center = pos + halfSize;
			const auto& offset = center - m_pointerScreenSpacePosition;
			const auto& offsetSquareMag = offset.SquareMag();

			gamepadOffset.X += Clamp( offset.X / halfSize.X, -m_cursorProcessingInfo.m_pointerSnapSpeed, m_cursorProcessingInfo.m_pointerSnapSpeed );
			gamepadOffset.Y += Clamp( offset.Y / halfSize.Y, -m_cursorProcessingInfo.m_pointerSnapSpeed, m_cursorProcessingInfo.m_pointerSnapSpeed );

			const auto& modifiedDest = ( gamepadOffset * velocity ) * tickInfo.m_realTimeDelta;

			if ( offsetSquareMag < ( center - ( m_pointerScreenSpacePosition + modifiedDest ) ).SquareMag() )
			{
				trigger += tickInfo.m_realTimeDelta;
				m_pointerScreenSpacePosition += trigger < m_cursorProcessingInfo.m_pointerMagnetismTime ? modifiedDest : initialDest;
			}
			else
			{
				trigger = 0.f;
				m_pointerScreenSpacePosition += modifiedDest;
			}
		}
		else
		{
			trigger = 0.f;
			m_pointerScreenSpacePosition += initialDest;
		}
	}

	void InkSystem::CreateAudioInterface()
	{
		::audio::AudioInterfaceContext context( ::audio::CustomEntityType::Ui, RED_NAME_CONSTEXPR( "inkSystem" ) );
		m_audioInterface = ::audio::AudioInterfaceBuilder( &context ).AcquireAudioInterface( );
		::audio::EntityEmitterContext emitterContext( RED_NAME_CONSTEXPR( "InkSystem" ), context.GetId( ) );
		emitterContext.m_isDefault = true;
		m_emitter = ::audio::EmitterInterfaceBuilder( &emitterContext ).AquireEmitterInterface( );
	}

	void InkSystem::DestroyAudioInterface()
	{
		m_audioInterface = nullptr;
		m_emitter = nullptr;
	}

	const InputKeyModifiers& InkSystem::GetModifierKeys() const
	{
		return m_inputKeyModifier;
	}

	red::SharedPtr< ILoadingScreen > InkSystem::GetLoadingScreen() const
	{
		if (red::SharedPtr<ViewportWrapper> wrapper = GetGameViewportWrapper())
		{
			if (THandle<LoadingLayer> loadingLayer = wrapper->GetLayerByType< LoadingLayer >( ELayerType::Loading ))
			{
				return loadingLayer->GetLoadingScreenProxy();
			}
		}

		return nullptr;
	}

	input::EInputDevice InkSystem::GetActiveInputDevice() const
	{
		return m_inputKeyIconManager ? m_inputKeyIconManager->GetActiveInputDevice() : input::EInputDevice::INVALID;
	}

	void InkSystem::SetCustomCursorProcessor( const red::SharedPtr< ICustomCursorProcessor >& processor )
	{
		m_customCursorProcessor = processor;
	}

	ResourceManager& InkSystem::GetResourceManager()
	{
		return *m_resourceManager.Get();
	}

	MaskDTManager& InkSystem::GetMaskDTManager()
	{
		return *m_maskDTManager.Get();
	}

	ink::InternationalizationManager& InkSystem::GetInternationalizationManager()
	{
		return *m_internationalizationManager.Get();
	}

	void InkSystem::SetIconAtlasManager( red::UniquePtr< IIconAtlasManager >&& newIconAtlasManager )
	{
		m_iconAtlasManager = std::move( newIconAtlasManager );
	}

	ink::IIconAtlasManager& InkSystem::GetIconAtlasManager()
	{
		return *m_iconAtlasManager.Get();
	}

	void InkSystem::SetAdvertisementsTWDBConnector( red::UniquePtr< IAdvertisementsTWDBWorldUIConnector> && connector )
	{
		m_adsTWDBConnector = std::move( connector );
	}

	IAdvertisementsTWDBWorldUIConnector& InkSystem::GetAdvertisementsTWDBConnector()
	{
		return *m_adsTWDBConnector.Get();
	}

	Bool InkSystem::IsReadyAdvertisementsTWDBWorldUIConnector()
	{
		return m_adsTWDBConnector != nullptr;
	}

	ink::StyleThemeManager& InkSystem::GetStyleThemeManager()
	{
		return *m_styleThemeManager.Get();
	}

#ifndef RED_CONFIGURATION_FINAL
	ink::SpawnDebugger& InkSystem::GetSpawnDebugger()
	{
		return *m_spawnDebugger;
	}
#endif

	const THandle< ink::LayersResource >& InkSystem::GetLayersDefinition() const
	{
		return m_layersResource;
	}

	void InkSystem::SetInkGameState( InkGameState gamestate )
	{
		m_inkCurrentGameState = gamestate;
	}

	InkGameState InkSystem::GetInkGameState() const
	{
		return m_inkCurrentGameState;
	}

	void InkSystem::SetSystemRequestsHandler( const THandle<ISystemRequestsHandler>& systemRequestsHandler )
	{
		m_systemRequestsHandler = systemRequestsHandler;
	}

	const WeakHandle<ISystemRequestsHandler>& InkSystem::GetSystemRequestsHandler() const
	{
		return m_systemRequestsHandler;
	}

	void InkSystem::SetSoundSystem( audio::SoundSystem * soundSystem )
	{
		m_soundSystem = soundSystem; 
		InitializeUISoundSystem();
	}

	audio::SoundSystem * InkSystem::GetSoundSystem()
	{
		return m_soundSystem;
	}

	ink::OnscreenVOHandler& InkSystem::GetOnscreenVOHandler()
	{
		return *m_onscreenVOHandler.Get();
	}

	PointerEventCallback * InkSystem::GetGlobalInputCallback( CName name ) const
	{
		static red::DynArray<PointerEventCallback*> callbacks
		{
			{
				&OnPreOnPress,
				&OnPreOnRelease,
				&OnPreOnHold,
				&OnPreOnAxis,
				&OnPreOnRelative,
				&OnPreOnRepeat,
				&OnPostOnPress,
				&OnPostOnRelease,
				&OnPostOnHold,
				&OnPostOnAxis,
				&OnPostOnRelative,
				&OnPostOnRepeat
			},
			ink::PoolInk_Events()
		};

		const auto& it = std::find_if( callbacks.Begin(), callbacks.End(), [ name ] ( PointerEventCallback* callback )
		{
			return callback->GetCallbackName() == name;
		} );

		return it != callbacks.End() ? ( *it ) : nullptr;
	}

	void InkSystem::OnLocalizationChanged()
	{
		if ( auto wrapper = GetGameViewportWrapper() )
		{
			for ( auto& layer : wrapper->GetLayers() )
			{
				layer->OnLocalizationChanged();
			}
		}
	}

	void InkSystem::SetIsInWorldInteraction( Bool value )
	{
		m_isInWorldInteraction = value;
	}

	Bool InkSystem::IsInWorldInteraction() const
	{
		return m_isInWorldInteraction;
	}

	void InkSystem::LoadAllOptions()
	{
		const InGameConfig::System &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		const InGameConfig::Group &group = system.GetGroup( s_configGameplayUI );

		CursorSpeedDefinition regularSpeed{};
		regularSpeed.m_minSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSpeedMin" ) ).GetValue();
		regularSpeed.m_maxSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSpeedMax" ) ).GetValue();
		regularSpeed.m_acceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerAccelereation" ) ).GetValue();
		m_cursorSpeedProcessor.SetRegularSpeed( regularSpeed );

		CursorSpeedDefinition interactionSpeed{};
		interactionSpeed.m_minSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionSpeedMin" ) ).GetValue();
		interactionSpeed.m_maxSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionSpeedMax" ) ).GetValue();
		interactionSpeed.m_acceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionAcceleration" ) ).GetValue();
		m_cursorSpeedProcessor.SetInteractionSpeed( interactionSpeed );

		const Float deceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerDeceleration" ) ).GetValue();
		m_cursorSpeedProcessor.SetDeceleration( deceleration );

		const Bool isOnEnterDecelerationEnabled = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerOnEnterDeceleration" ) ).GetValue();
		m_cursorSpeedProcessor.SetIsOnEnterDecelerationEnabled( isOnEnterDecelerationEnabled );

		const Bool isOnLeaveAccelerationEnabled = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerOnLeaveAcceleration" ) ).GetValue();
		m_cursorSpeedProcessor.SetIsOnLeaveAccelerationEnabled( isOnLeaveAccelerationEnabled );

		m_cursorProcessingInfo.m_processingType = group.GetVarInt( RED_NAME_CONSTEXPR( "PadPointerProcessingType" ) ).GetValue();
		m_cursorProcessingInfo.m_pointerSnapSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSnapSpeed" ) ).GetValue();
		m_cursorProcessingInfo.m_pointerLeaveMinMult = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerLeaveMinMult" ) ).GetValue();
		m_cursorProcessingInfo.m_pointerLeaveMaxMult = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerLeaveMaxMult" ) ).GetValue();
		m_cursorProcessingInfo.m_pointerAxisesDiff = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerAxisesDiff" ) ).GetValue();
		m_cursorProcessingInfo.m_pointerMagnetismTime = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerMagnetismTime" ) ).GetValue();
#ifndef RED_CONFIGURATION_FINAL
		m_cursorProcessingInfo.m_showHoweredWidget = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerShowHoweredWidget" ) ).GetValue();
#endif
	}

	void InkSystem::RegisterSettingsListener()
	{
		InGameConfig::System& system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		const auto listener = [ this ]( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason reason )
		{
			HandleVarChange( groupPath, varName, varType, reason );
		};

		m_uiListenerID = system.RegisterListener( s_configGameplayUI, listener );
		m_brightnessListenerID = system.RegisterListener( s_configBrightness, listener );
		m_languageListenerID = system.RegisterListener( s_configLanguage, listener );

		for ( const CName& settingsGroupName : game::input::ManagerView::GetSettingsGroupsNames() )
		{
			InGameConfig::VarListener::ID listenerID = system.RegisterListener( settingsGroupName, [ this ]( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason reason )
			{
				if ( reason != InGameConfig::ChangeReason::Accepted )
				{
					return;
				}

				m_keyBindingsVarChanged = true;
			} );
			m_keyBindingSettingsListeners.PushBack( listenerID );
		}

		m_keyBindingsNotifierID = system.RegisterNotifier( [ this ]( InGameConfig::NotificationType type )
		{
			if ( type == InGameConfig::NotificationType::ChangesApplied )
			{
				if ( m_keyBindingsVarChanged )
				{
					m_keyBindingsVarChanged = false;
					m_inputKeyIconManager->OnInputDeviceChanged( input::EInputDevice::KBD_MOUSE, input::EInputDevice::KBD_MOUSE );
				}
			}
		} );
	}

	void InkSystem::UnregisterSettingsListener()
	{
		InGameConfig::System &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		system.UnregisterListener( m_uiListenerID );
		system.UnregisterListener( m_brightnessListenerID );
		system.UnregisterListener( m_languageListenerID );

		for ( InGameConfig::VarListener::ID id : m_keyBindingSettingsListeners )
		{
			system.UnregisterListener( id );
		}
		m_keyBindingSettingsListeners.Clear();

		system.UnregisterNotifier( m_keyBindingsNotifierID );
	}

	void InkSystem::HandleVarChange( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason reason )
	{
		if ( reason != InGameConfig::ChangeReason::Accepted )
		{
			return;
		}

		if ( groupPath == s_configGameplayUI )
		{
			HandleGameplayUIVarInput( varName, varType );
		}
		else if ( groupPath == s_configBrightness )
		{
			HandleBrightnessVarInput( varName, varType );
		}
		else if ( groupPath == s_configLanguage && varName == s_configOnScreen )
		{
			OnLocalizationChanged();
		}
	}

	void InkSystem::HandleGameplayUIVarInput( CName varName, const InGameConfig::VarType varType )
	{
		const InGameConfig::System &system = InGameConfig::System::GetInstance();
		const InGameConfig::Group &group = system.GetGroup( s_configGameplayUI );

		// should/would be a switch on constexpr CNames..
		if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerSpeedMin" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetRegularSpeed();
			speed.m_minSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSpeedMin" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetRegularSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerSpeedMax" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetRegularSpeed();
			speed.m_maxSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSpeedMax" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetRegularSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerAccelereation" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetRegularSpeed();
			speed.m_acceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerAccelereation" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetRegularSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerInteractionSpeedMin" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetInteractionSpeed();
			speed.m_minSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionSpeedMin" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetInteractionSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerInteractionSpeedMax" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetInteractionSpeed();
			speed.m_maxSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionSpeedMax" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetInteractionSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerInteractionAcceleration" ) )
		{
			CursorSpeedDefinition speed = m_cursorSpeedProcessor.GetInteractionSpeed();
			speed.m_acceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerInteractionAcceleration" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetInteractionSpeed( speed );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerDeceleration" ) )
		{
			const Float deceleration = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerDeceleration" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetDeceleration( deceleration );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerOnEnterDeceleration" ) )
		{
			const Bool isOnEnterDecelerationEnabled = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerOnEnterDeceleration" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetIsOnEnterDecelerationEnabled( isOnEnterDecelerationEnabled );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerOnLeaveAcceleration" ) )
		{
			const Bool isOnLeaveAccelerationEnabled = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerOnLeaveAcceleration" ) ).GetRequestedValue();
			m_cursorSpeedProcessor.SetIsOnLeaveAccelerationEnabled( isOnLeaveAccelerationEnabled );
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerProcessingType" ) )
		{
			m_cursorProcessingInfo.m_processingType = group.GetVarInt( RED_NAME_CONSTEXPR( "PadPointerProcessingType" ) ).GetRequestedValue();
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerSnapSpeed" ) )
		{
			m_cursorProcessingInfo.m_pointerSnapSpeed = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerSnapSpeed" ) ).GetRequestedValue();
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerLeaveMinMult" ) )
		{
			m_cursorProcessingInfo.m_pointerLeaveMinMult = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerLeaveMinMult" ) ).GetRequestedValue();
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerLeaveMaxMult" ) )
		{
			m_cursorProcessingInfo.m_pointerLeaveMaxMult = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerLeaveMaxMult" ) ).GetRequestedValue();
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerAxisesDiff" ) )
		{
			m_cursorProcessingInfo.m_pointerAxisesDiff = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerAxisesDiff" ) ).GetRequestedValue();
		}
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerMagnetismTime" ) )
		{
			m_cursorProcessingInfo.m_pointerMagnetismTime = group.GetVarFloat( RED_NAME_CONSTEXPR( "PadPointerMagnetismTime" ) ).GetRequestedValue();
		}
#ifndef RED_CONFIGURATION_FINAL
		else if ( varName == RED_NAME_CONSTEXPR_NOREG( "PadPointerShowHoweredWidget" ) )
		{
			m_cursorProcessingInfo.m_showHoweredWidget = group.GetVarBool( RED_NAME_CONSTEXPR( "PadPointerShowHoweredWidget" ) ).GetRequestedValue();
		}
#endif
	}

	void InkSystem::HandleBrightnessVarInput( CName varName, const InGameConfig::VarType varType  )
	{
		const InGameConfig::System &system = InGameConfig::System::GetInstance();
		const InGameConfig::Group &group = system.GetGroup( s_configBrightness );

		if ( varName == RED_NAME_CONSTEXPR_NOREG( "Brightness" ) )
		{
#ifndef RED_CONFIGURATION_FINAL
			DebugDumpVar( "SET", s_configBrightness, varName, varType );
#endif
		}
	}

#ifdef INK_ENABLE_PROFILING
	void InkSystem::SetCurrentFrameNumber( Uint64 currentFrameNumber )
	{
		m_currentFrameNumber = currentFrameNumber;
	}

	Uint64 InkSystem::GetCurrentFrameNumber() const
	{
		return m_currentFrameNumber;
	}
#endif

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
	tests::FunctionalTests & InkSystem::GetFunctionalTests()
	{
		return *m_functionalTests;
	}
#endif

	red::SharedPtr< ink::SystemNotificationToken > InkSystem::ShowSystemNotification( const ESystemNotificationTypes notificationType,
																					  const CName& title/* = ""*/,
																					  const CName& description/* = ""*/,
																					  const THandle< text::TextParameterSet >& textParamSet/* = nullptr*/,
																					  const String& additionalData/* = String::EMPTY()*/ )
	{
		red::SharedPtr< SystemNotificationToken > token = red::CreateSharedPtr< SystemNotificationToken >( notificationType );
		token->SetLocalizationData( title, description, textParamSet );
		token->SetAdditionalData( additionalData );

		for (red::SharedPtr< ViewportWrapper > viewportWrapper : m_viewportWrappers)
		{
			THandle< SystemNotificationEvt > evt = CreateHandle< SystemNotificationEvt >( token );

			viewportWrapper->QueueEvent( evt );
		}

		return token;
	}

	THandle< ink::GameNotificationToken > InkSystem::ShowGameNotification( THandle< ink::GameNotificationData >& data )
	{
		THandle< ink::GameNotificationToken > token = CreateHandle< ink::GameNotificationToken >();
		data->m_token = token;
		token->SetData( data );

		THandle< ink::GameNotificationEvt > evt = CreateHandle< ink::GameNotificationEvt >( token );

		for (red::SharedPtr< ViewportWrapper > viewportWrapper : m_viewportWrappers)
		{
			viewportWrapper->QueueEvent( evt );
		}

		return token;
	}

	void InkSystem::OnHideGameUI( Bool hide )
	{
		// TODO: please implement me!
		// this is called when game logic decides that game UI (hud, panels, etc.) should be hidden (or shown), due to screen fade out or similar event

		// ...
	}

	//////////////////////////////////////////////////////////////////////////

	void InkSystem::SpawnViewportWrapper(const ViewportHandle& viewport, rend::ViewportType viewportType)
	{
		red::SharedPtr< ViewportWrapper > newViewportWrapper = red::CreateSharedPtr< ViewportWrapper >();
		newViewportWrapper->Initialize( viewport, viewportType );

		m_viewportWrappers.PushBack( std::move( newViewportWrapper ) );
	}

	void InkSystem::DestroyViewportWrapper( ViewportId viewportId )
	{
		Int32 elementIndex = -1;

		for ( const red::SharedPtr<ViewportWrapper>& wrapper : m_viewportWrappers )
		{
			++elementIndex;

			if ( wrapper->IsCompatibleWith( viewportId ) )
			{
				break;
			}
		}
		RED_FATAL_ASSERT( elementIndex != -1, "Viewport wrapper object must exist for every viewport in the engine!" );

		m_viewportWrappers[elementIndex]->Uninitialize();

		m_viewportWrappers.RemoveAt( elementIndex );
	}

	red::WeakPtr<ViewportWrapper> InkSystem::TakeControlOnViewport( ViewportId viewportId, RunningMode runningMode )
	{
		red::WeakPtr<ViewportWrapper> wrapper = GetViewportWrapper( viewportId );
		if (red::SharedPtr<ViewportWrapper> viewportWrapper = wrapper.Lock())
		{
			viewportWrapper->TakeControl( runningMode );
		}
		return wrapper;
	}

	red::WeakPtr<ViewportWrapper> InkSystem::GetViewportWrapper( ViewportId viewportId )
	{
		for( const red::SharedPtr<ViewportWrapper>& wrapper : m_viewportWrappers )
		{
			if( wrapper->IsCompatibleWith( viewportId ) )
			{
				return wrapper;
			}
		}
		return red::WeakPtr<ViewportWrapper>();
	}

}	// namespace ink

INK_CORE_API ink::InkSystem& GetInkSystem()
{
	RED_ASSERT( s_inkSystem, "Ink: Ink framework is not initialized!" );
	return *s_inkSystem;
}

INK_CORE_API Bool IsInkSystemReady()
{
	return s_inkSystem != nullptr;
}

INK_CORE_API red::SharedPtr< ink::SystemNotificationToken > RequestSystemNotification( const ESystemNotificationTypes type )
{
	if( type != ESystemNotificationTypes::Generic && type != ESystemNotificationTypes::GenericNotModal && type != ESystemNotificationTypes::GenericYesNo )
	{
		return GetInkSystem().ShowSystemNotification( type );
	}
	else
	{
		return GetInkSystem().ShowSystemNotification (type, RED_NAME_CONSTEXPR( "" ), RED_NAME_CONSTEXPR( "" ));
		RED_LOG( "[UI System Notifications] You are spawning generic notification like a defined one. Please Try RequestSystemNotificationGeneric or RequestSystemNotificationGenericYesNo" );
	}
}

INK_CORE_API red::SharedPtr< ink::SystemNotificationToken >  RequestSystemNotificationGenericYesNo( const CName& title, const CName& description )
{
	return GetInkSystem().ShowSystemNotification( ESystemNotificationTypes::GenericYesNo, title, description );
}

INK_CORE_API red::SharedPtr< ink::SystemNotificationToken > RequestSystemNotificationGeneric( Bool isModal, const CName& title, const CName& description, const THandle< text::TextParameterSet >& textParamSet )
{
	if( isModal )
	{
		return GetInkSystem().ShowSystemNotification( ESystemNotificationTypes::Generic, title, description, textParamSet );
	}
	else
	{
		return GetInkSystem().ShowSystemNotification( ESystemNotificationTypes::GenericNotModal, title, description, textParamSet );
	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif