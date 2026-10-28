#include <cstdio>
#include <atomic>
/**
 * Copyright (c) 2018-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include "viewportWrapper.h"
#include "advertisementsLayer.h"
#include "editorLayer.h"
#include "hudLayer.h"
#include "layersResource.h"
#include "loadingLayer.h"
#include "menuLayer.h"
#include "videoLayer.h"
#include "offscreenLayer.h"
#include "styleThemeManager.h"
#include "inkSystem.h"
#include "../../renderData/include/renderFrameInfo.h"
#include "../../renderData/include/viewport.h"
#include "inkResourceManager.h"
#include "systemNotificationsLayer.h"
#include "gameNotificationsLayer.h"
#include "photoModeLayer.h"
#include "streetSignsLayer.h"
#include "../../redCore/include/commandline.h"
#include "../../../../engine/shaders/include/include_hud.fx"
#include "fullscreenCompositionManager.h"
#include "stateMachine.h"
#include "stateMachine_State.h"
#include "widgetVirtualDrawManager.h"
#include "watermarksLayer.h"
// CP2077 ui viewport probe counters (index 0 = InParallel/world, 1 = InOrder/fullscreen).
namespace { inline bool CP2077UiProbeDue( unsigned n ) { return ( n & ( n - 1 ) ) == 0 || ( n % 4096 ) == 0; } }
namespace { std::atomic<unsigned> s_cp2077DrawDispatched[2]; std::atomic<unsigned> s_cp2077DrawExecuted[2]; std::atomic<unsigned> s_cp2077PresentBlank{0}; std::atomic<unsigned> s_cp2077PresentReal{0};
	std::atomic<const void*> s_cp2077LastReal{ nullptr }; std::atomic<unsigned> s_cp2077BlankRun{ 0 }, s_cp2077Reused{ 0 }, s_cp2077InFlight{ 0 }; }

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
#include "debugLayer.h"
#endif

namespace ink
{
	ViewportWrapper::ViewportWrapper()
		: m_layers( ink::PoolInk_Layers_ViewportWrapper() )
		, m_permanentLayersSpawned( false )
		, m_runningMode( RunningMode::Unknown )
		, m_uiShouldBeGloballyHidden( false )
	{
	}

	ViewportWrapper::~ViewportWrapper()
	{
		Uninitialize();
	}

	void ViewportWrapper::Initialize( const ViewportHandle& viewport, rend::ViewportType viewportType )
	{
		m_nativeViewport = viewport;

		m_composition = red::CreateUniquePtr< rend::ui::Drawer >();
		m_blankFrameComposition = red::CreateUniquePtr< rend::ui::Drawer >();

		m_stateMachine = CreateHandle<StateMachine>();
		m_stateMachine->Initialize(this, viewportType);
		m_eventBroker.RegisterListener(m_stateMachine);
		m_eventBroker.RegisterListener( GetInkSystem().GetFullscreenCompositionManagerHandle() );

		DynamicTexture::InitInfo texInitInfo;
		texInitInfo.width = viewport->GetWidth();
		texInitInfo.height = viewport->GetHeight();
		m_previousFrameTexture = CreateHandle<DynamicTexture>( texInitInfo );

		m_worldDynamicTextureManager = red::CreateUniquePtr<WorldDynamicTextureManager>();
		m_worldDynamicTextureManager->Initialize();

		m_widgetVirtualDrawManager = red::CreateUniquePtr< WidgetVirtualDrawManager>();
		m_widgetVirtualDrawManager->Initialize();
		SpawnPermanentLayers(nullptr, nullptr);

		// Set default values for forced activity and visibility for every layer
		for (Uint32 i = 0; i < (Uint32)ELayerType::MAX; ++i)
		{
			m_forcedActivityAndVisibility.PushBack(true);
			m_debugLayerVisibility.PushBack(true);
		}

		OnGlobalVisibilityChanged( false );

		GetInkSystem().GetStyleThemeManager().OnStyleThemeChanged += [this]()
		{
			RefreshAllLayerStyles();
		};
	}

	void ViewportWrapper::Uninitialize()
	{
		m_stateMachine->Uninitialize();

		for (const LayerHandle& layer : m_layers)
		{
			layer->Uninitialize();
		}
		m_layers.Clear();
		m_permanentLayersSpawned = false;
		m_worldDynamicTextureManager->Uninitialize();
		m_widgetVirtualDrawManager->Uninitialize();
	}

	void ViewportWrapper::DespawnLayers( Bool gameInstanceDetached )
	{
		Int32 count = m_layers.Size();
		for (Int32 i = count - 1; i >= 0; i--)
		{
			const LayerHandle& layer = m_layers[i];
			if (!layer->GetIsPermanent())
			{
				layer->Uninitialize();
				m_layers.RemoveAt( i );
			}
			else if ( gameInstanceDetached )
			{
				layer->OnGameInstanceDetached();
			}
		}
	}

	void ViewportWrapper::TakeControl( RunningMode mode )
	{
		RED_FATAL_ASSERT( m_runningMode == RunningMode::Unknown, "Viewport Wrapper object can be controlled only by one runtime system!" );
		m_runningMode = mode;

		if (m_runningMode == RunningMode::Backend || m_runningMode == RunningMode::Unknown || m_runningMode == RunningMode::GamePreview )
		{
			const ELayerType loadingType = ELayerType::Watermarks;
			SetLayerActive( loadingType, false);
		}
	}

	void ViewportWrapper::ReleaseControl()
	{
		RED_FATAL_ASSERT( m_runningMode != RunningMode::Unknown, "Viewport Wrapper object must be controlled by any runtime system before control released!" );
		m_runningMode = RunningMode::Unknown;
	}

	Bool ViewportWrapper::IsControlled() const
	{
		return m_runningMode != RunningMode::Unknown;
	}

	Bool ViewportWrapper::IsCompatibleWith( ViewportId viewportId ) const
	{
		if( const red::SharedPtr< IViewport > viewport = m_nativeViewport.Lock() )
		{
			return viewport->GetViewportID() == viewportId;
		}

		return false;
	}

	void ViewportWrapper::EstablishUISystemState( const tick::Info& tickInfo, job::Builder& builder )
	{
		ProcessResourceManager( builder );
		ServiceEvents( builder );
		ProcessStateMachine( tickInfo, builder );
	}

	void ViewportWrapper::ProcessFullscreenUI( const ink::UpdateContext& updateContext, job::Builder& builder )
	{		
		m_videoManager.Tick( builder );

		ProcessLayersUpdate( updateContext, builder, LayerDrawingPolicy::InOrder );
		UpdateCompositionIfNeeded( updateContext, builder );
		ProcessLayersDraw( updateContext, builder, LayerDrawingPolicy::InOrder);
		ProcessVirtualManagerDraw( builder );
		FinishComposition(builder);
	}

	void ViewportWrapper::ProcessWorldUI(const ink::UpdateContext& updateContext, job::Builder& builder)
	{
		ProcessWorldDynamicTextureManagerTick(updateContext, builder);
		ProcessLayersUpdate(updateContext, builder, LayerDrawingPolicy::InParallel);
		ProcessLayersDraw(updateContext, builder, LayerDrawingPolicy::InParallel);
	}

	void ViewportWrapper::ClearUpdateFlag()
	{
		m_hasUpdated.SetValue( false );
	}

	void ViewportWrapper::RenderFrame( CRenderFrameInfo& info )
	{
		if ( !m_hasUpdated.GetValue() )
		{
			const unsigned cp2077Blank = ++s_cp2077PresentBlank;
			if ( CP2077UiProbeDue( cp2077Blank ) )
			{
				std::fprintf( stderr, "CP2077 ui viewport probe: present=blank this=%p info=%p blank=%u real=%u layers=%u runningMode=%d activeUI=%d drawDispatched=%u/%u drawExecuted=%u/%u\n", (void*)this, (void*)&info, cp2077Blank, s_cp2077PresentReal.load(), unsigned( m_layers.Size() ), int( m_runningMode ), m_stateMachine ? int( m_stateMachine->GetActiveUIState() ) : -2, s_cp2077DrawDispatched[0].load(), s_cp2077DrawDispatched[1].load(), s_cp2077DrawExecuted[0].load(), s_cp2077DrawExecuted[1].load() );
			}
			// [gamerender] HUD flicker: a render frame that arrives with no UI update since the previous one
			// (browser frame pacing can render without a fresh UI tick) presented the BLANK composition, so the
			// whole HUD vanished for one frame. When this viewport presented a real composition recently and no
			// UI draw job is in flight (dispatched == executed, so m_composition is finished and untouched),
			// present that last good composition again instead. Long blank runs (loading/UI teardown) stay blank.
			const unsigned cp2077Run = ++s_cp2077BlankRun;
			const bool cp2077Idle = s_cp2077DrawDispatched[0].load() == s_cp2077DrawExecuted[0].load() &&
				s_cp2077DrawDispatched[1].load() == s_cp2077DrawExecuted[1].load();
			if ( m_composition && s_cp2077LastReal.load() == this && cp2077Run <= 8 )
			{
				if ( cp2077Idle )
				{
					const unsigned cp2077N = ++s_cp2077Reused;
					if ( CP2077UiProbeDue( cp2077N ) )
						std::fprintf( stderr, "CP2077 ui viewport: reused last composition instead of blank (count=%u run=%u)\n", cp2077N, cp2077Run );
					info.m_composition = m_composition->GetDrawBuffer();
					return;
				}
				const unsigned cp2077N = ++s_cp2077InFlight;
				if ( CP2077UiProbeDue( cp2077N ) )
					std::fprintf( stderr, "CP2077 ui viewport: blank present while UI draw in flight (count=%u dispatched=%u/%u executed=%u/%u)\n", cp2077N,
						s_cp2077DrawDispatched[0].load(), s_cp2077DrawDispatched[1].load(), s_cp2077DrawExecuted[0].load(), s_cp2077DrawExecuted[1].load() );
			}
			info.m_composition = m_blankFrameComposition->GetDrawBuffer();
			return;
		}
		{
			s_cp2077LastReal.store( this ); s_cp2077BlankRun.store( 0 ); // [gamerender] see the blank path above
			const unsigned cp2077Real = ++s_cp2077PresentReal;
			if ( CP2077UiProbeDue( cp2077Real ) )
			{
				std::fprintf( stderr, "CP2077 ui viewport probe: present=real this=%p info=%p real=%u blank=%u layers=%u runningMode=%d activeUI=%d drawDispatched=%u/%u drawExecuted=%u/%u\n", (void*)this, (void*)&info, cp2077Real, s_cp2077PresentBlank.load(), unsigned( m_layers.Size() ), int( m_runningMode ), m_stateMachine ? int( m_stateMachine->GetActiveUIState() ) : -2, s_cp2077DrawDispatched[0].load(), s_cp2077DrawDispatched[1].load(), s_cp2077DrawExecuted[0].load(), s_cp2077DrawExecuted[1].load() );
			}
		}

		Int32 screenHeight = info.GetViewport()->GetHeight();
		Int32 screenWidth = info.GetViewport()->GetWidth();

		if( m_screenHeight != screenHeight || m_screenWidth != screenWidth )
		{
			m_screenWidth = screenWidth;
			m_screenHeight = screenHeight;
			UpdateWindowSize( ( Uint32 )m_screenWidth, ( Uint32 )m_screenHeight );
		}
		ClearUpdateFlag();

		info.m_composition = m_composition->GetDrawBuffer();

		// #TODO: Also account for black/white screens, minigames, anything else that should prevent a thumbnail update?
		info.m_allowGameSaveThumbnail = (m_stateMachine->GetActiveUIState() == State::Game);

		//if (m_previousFrameTexture)
		if( m_shouldPerformScreenCapture  || m_continiousScreenCapture )
		{
			// TODO OPTIMIZATION
			// this forces unnecessary full-screen copy every frame
			// should be done only just before switching to menu screen
			info.m_dynamicTextureTargetFinal = m_previousFrameTexture->GetRenderResource();
			m_shouldPerformScreenCapture = false;
		}
	}

	void ViewportWrapper::OnRenderDebug( const RenderDebugContext& context )
	{
		for( const LayerHandle& layer : m_layers )
		{
			layer->OnRenderDebug( context );
		}
	}

	void ViewportWrapper::SetOwner( const WeakHandle<ent::Entity>& owner )
	{
		if (m_runningMode == RunningMode::Backend || m_runningMode == RunningMode::Unknown)
		{
			return;
		}

		for (const LayerHandle& layer : m_layers)
		{
			layer->SetOwner(owner);
		}
	}

	void ViewportWrapper::SpawnPermanentLayers( const WeakHandle< ISerializable >& runtimeSystemUI, const IControllerGameData* gameData )
	{
		if (m_permanentLayersSpawned)
		{
			return;
		}

		PermanentLayerDefinitionCollection* definitions = &GetInkSystem().GetLayersDefinition()->GetPermanentDefinitions();

		const auto& viewport = m_nativeViewport.Lock();

		Bool noFullscreenUI = red::CommandLine::Get().HasOption("noFullscreenUI");
		if (!noFullscreenUI)
		{
			if (definitions->m_watermarksLayer.m_enabled)
			{
				THandle<WatermarksLayer> watermarksLayer = CreateHandle<WatermarksLayer>();
				m_layers.PushBack(watermarksLayer);

				watermarksLayer->Initialize(this, runtimeSystemUI, gameData);

				watermarksLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				watermarksLayer->SpawnFromDefinition(definitions->m_watermarksLayer);

				if (m_runningMode == RunningMode::Backend || m_runningMode == RunningMode::Unknown || m_runningMode == RunningMode::GamePreview)
				{
					watermarksLayer->SetActive(false);
				}
			}

			if (definitions->m_loadingLayer.m_enabled)
			{
				THandle<LoadingLayer> loadingLayer = CreateHandle<LoadingLayer>();
				m_layers.PushBack(loadingLayer);

				loadingLayer->Initialize(this, runtimeSystemUI, gameData);

				loadingLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				loadingLayer->SpawnFromDefinition(definitions->m_loadingLayer);
			}

			if (definitions->m_sysNotificationsLayer.m_enabled)
			{
				THandle<SystemNotificationsLayer> sysNotificationsLayer = CreateHandle<SystemNotificationsLayer>();
				m_layers.PushBack(sysNotificationsLayer);

				sysNotificationsLayer->Initialize(this, runtimeSystemUI, gameData);

				sysNotificationsLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				sysNotificationsLayer->SpawnFromDefinition(definitions->m_sysNotificationsLayer);
			}
		}

		// #TODO_UI @CYB-252603
		// All SetInteractable should be turned to true to block inputs when we have a way for the controllers to change
		// the SetActive of the layer

		SortLayers();

		m_permanentLayersSpawned = true;

		DeactivateAllLayers();
	}

	void ViewportWrapper::SpawnLayers(const WeakHandle< ISerializable >& runtimeSystemUI, const IControllerGameData* gameData, game::ui::Context gameContext)
	{
		InkGameState gameState = GetInkSystem().GetInkGameState();

		LayerDefinitionCollection* definitions;

		if (gameState == InkGameState::PreGame)
		{
			definitions = &GetInkSystem().GetLayersDefinition()->GetPreGameDefinitions();
		}
		else
		{
			definitions = &GetInkSystem().GetLayersDefinition()->GetGameDefinitions();
		}

		const auto& viewport = m_nativeViewport.Lock();

		if (GetInkSystem().GetMode() != InkSystemMode::Backend)
		{
			ResourceManager &resourceManager = GetInkSystem().GetResourceManager();
			resourceManager.UpdateWindowSize((Uint32)viewport->GetWidth(), (Uint32)viewport->GetHeight());

			m_worldDynamicTextureManager->SetScreenSize( ( Uint32 )viewport->GetWidth(), ( Uint32 )viewport->GetHeight() );
			m_widgetVirtualDrawManager->SetScreenSize( ( Uint32 )viewport->GetWidth(), ( Uint32 )viewport->GetHeight() );
		}

		MenuLayerDefinition* menuLayerDef = nullptr;
		Uint32 mpMenu;
		if (red::CommandLine::Get().GetFirstParam<Uint32>("mpMenu", mpMenu) && mpMenu > 0)
		{
			menuLayerDef = &definitions->m_menuLayerMP;
		}
		else
		{
			menuLayerDef = &definitions->m_menuLayer;
		}

		Bool noFullscreenUI = red::CommandLine::Get().HasOption("noFullscreenUI");
		if (!noFullscreenUI)
		{
			if (menuLayerDef && menuLayerDef->m_enabled)
			{
				THandle<MenuLayer> menuLayer = CreateHandle<MenuLayer>();
				m_layers.PushBack(menuLayer);

				menuLayer->Initialize(this, runtimeSystemUI, gameData);

				menuLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				menuLayer->SpawnFromDefinition(*menuLayerDef);
			}

			const Bool noHUD = red::CommandLine::Get().HasOption("noHUD");
			if (!noHUD)
			{
				if (definitions->m_hudLayer.m_enabled)
				{
					THandle<HUDLayer> hudLayer = CreateHandle<HUDLayer>();
					m_layers.PushBack(hudLayer);

					hudLayer->Initialize(this, runtimeSystemUI, gameData);

					hudLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

					hudLayer->SpawnFromDefinition(definitions->m_hudLayer);
				}
			}

			if (definitions->m_videoLayer.m_enabled)
			{
				THandle<VideoLayer> videoLayer = CreateHandle<VideoLayer>();
				m_layers.PushBack(videoLayer);

				videoLayer->Initialize(this, runtimeSystemUI, gameData);

				videoLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				videoLayer->SpawnFromDefinition(definitions->m_videoLayer);
			}

			if (definitions->m_gameNotificationsLayer.m_enabled)
			{
				THandle< GameNotificationsLayer > gameNotificationsLayer(CreateHandle< GameNotificationsLayer >());
				m_layers.PushBack(gameNotificationsLayer);

				gameNotificationsLayer->Initialize(this, runtimeSystemUI, gameData);

				gameNotificationsLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				gameNotificationsLayer->SpawnFromDefinition(definitions->m_gameNotificationsLayer);
			}

			if (definitions->m_photoModeLayer.m_enabled)
			{
				THandle<PhotoModeLayer> photoModeLayer = CreateHandle<PhotoModeLayer>();
				m_layers.PushBack(photoModeLayer);

				photoModeLayer->Initialize(this, runtimeSystemUI, gameData);

				photoModeLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				photoModeLayer->SpawnFromDefinition(definitions->m_photoModeLayer);
			}

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
			if (definitions->m_debugLayer.m_enabled)
			{
				THandle<DebugLayer> debugLayer = CreateHandle<DebugLayer>();
				m_layers.PushBack(debugLayer);

				debugLayer->Initialize(this, runtimeSystemUI, gameData);

				debugLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				debugLayer->SpawnFromDefinition(definitions->m_debugLayer);
			}
#endif
		}

		Bool noWorldUI = red::CommandLine::Get().HasOption("noWorldUI");
		if (!noWorldUI)
		{
			if (definitions->m_worldLayer.m_enabled)
			{
				THandle<WorldLayer> worldLayer = CreateHandle<WorldLayer>();
				m_layers.PushBack(worldLayer);

				worldLayer->Initialize(this, runtimeSystemUI, gameData);
				worldLayer->SetGameContext( gameContext );

				worldLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());
			}

			if (definitions->m_advertisementsLayer.m_enabled)
			{
				THandle<AdvertisementsLayer> advertisementsLayer = CreateHandle<AdvertisementsLayer>();
				m_layers.PushBack(advertisementsLayer);

				advertisementsLayer->Initialize(this, runtimeSystemUI, gameData);

				advertisementsLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());
			}

			if (definitions->m_streetSignsLayer.m_enabled)
			{
				THandle<StreetSignsLayer> streetSignsLayer = CreateHandle<StreetSignsLayer>();
				m_layers.PushBack(streetSignsLayer);

				streetSignsLayer->Initialize(this, runtimeSystemUI, gameData);

				streetSignsLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());
			}

			if (definitions->m_offscreenLayer.m_enabled)
			{
				THandle< OffscreenLayer > offscreenLayer(CreateHandle< OffscreenLayer >());
				m_layers.PushBack(offscreenLayer);

				offscreenLayer->Initialize(this, runtimeSystemUI, gameData);

				offscreenLayer->SetupRendering(viewport->GetWidth(), viewport->GetHeight(), m_composition.Get());

				offscreenLayer->SpawnFromDefinition(definitions->m_offscreenLayer);
			}
		}

		SortLayers();

		// reapply previous state of global visibility
		OnGlobalVisibilityChanged(m_uiShouldBeGloballyHidden);
	}

	void ViewportWrapper::UpdateWindowSize( Uint32 width, Uint32 height )
	{
		if( GetInkSystem().GetMode() != InkSystemMode::Backend )
		{
			ResourceManager &resourceManager = GetInkSystem().GetResourceManager();
			resourceManager.UpdateWindowSize( width, height );

			m_worldDynamicTextureManager->SetScreenSize( width, height );
			m_widgetVirtualDrawManager->SetScreenSize( width, height );
		}

		for (const LayerHandle& layer : m_layers)
		{
			layer->UpdateWindowSize( width, height );
		}
	}

	void ViewportWrapper::SpawnLayers_Editor( const CName& editorStateRequest )
	{
		const auto& viewport = m_nativeViewport.Lock();

		THandle<EditorLayer> editorLayer = CreateHandle<EditorLayer>();
		m_layers.PushBack( editorLayer );

		editorLayer->Initialize( this, nullptr, nullptr );

		editorLayer->SetupRendering( viewport->GetWidth(), viewport->GetHeight(), m_composition.Get() );

		QueueEvent(CreateHandle<StateChangeRequestEvent>( editorStateRequest ));
	}

	THandle<Layer> ViewportWrapper::GetLayerByType( ELayerType layerType ) const
	{
		if( layerType != ELayerType::MAX )
		{
			for (const LayerHandle layer : m_layers)
			{
				if (layer->GetType() == layerType)
				{
					return layer;
				}
			}
		}
		return nullptr;
	}

	const red::DynArray< LayerHandle >& ViewportWrapper::GetLayers() const
	{
		return m_layers;
	}

	void ViewportWrapper::SetLayerActive( ELayerType layerType, Bool active)
	{
		const LayerHandle& layer = GetLayerByType( layerType );
		if (layer != nullptr)
		{
			layer->SetActive( active );
		}
	}

	Uint32 ViewportWrapper::GetLayerCount()
	{
		return m_layers.Size();
	}

	void ViewportWrapper::ServiceEvents( job::Builder& builder )
	{
		const auto serviceEventsFunc = [this](const job::RunContext& context)
		{
			m_eventBroker.ServiceEvents();
		};
		static red::InstrumentationObject instrObj{ "UI/ServiceEvents", PBC_UI };
		builder.DispatchJob( instrObj, serviceEventsFunc );
	}

	void ViewportWrapper::ProcessResourceManager( job::Builder& builder )
	{
		const auto tickResourceManager = [ this ]( const job::RunContext& context )
		{
			GetInkSystem().GetResourceManager().Tick();
		};
		static red::InstrumentationObject instrObj{ "UI/ProcessResourceManager", PBC_UI };
		builder.DispatchJob( instrObj, tickResourceManager );
	}

	void ViewportWrapper::ProcessStateMachine( const tick::Info& tickInfo, job::Builder& builder )
	{
		const auto func = [ this, tickInfo]( const job::RunContext& context )
		{
			m_stateMachine->Tick( tickInfo );
		};
		static red::InstrumentationObject instrObj{ "UI/StateMachine", PBC_UI };
		builder.DispatchJob( instrObj, func );
	}

	void ViewportWrapper::RefreshAllLayerStyles()
	{
		for( const LayerHandle layer : m_layers )
		{
			if( layer->GetUsesGlobalStyleTheme() )
			{
				layer->RefreshStyles();
			}
		}
	}

	void ViewportWrapper::GetScreenSize( Int32 &width, Int32 &height )
	{
		width = m_screenWidth;
		height = m_screenHeight;
	}

	red::WeakPtr<IViewport> ViewportWrapper::GetNativeViewport() const
	{
		return m_nativeViewport;
	}

	void ViewportWrapper::ProcessLayersUpdate( const ink::UpdateContext& updateContext, job::Builder& builder, ink::LayerDrawingPolicy policy)
	{
		// Layers can be updated in parallel, but drawing needs to be in layer order from lowest(world) to highest(loading)
		const auto tickSingleLayerFunc = [this, updateContext, policy]( Uint32 index, const job::RunContext& context )
		{
			job::Builder depBuilder{context};
			if( const LayerHandle& layer = m_layers[index] )
			{
				if( layer->GetDrawingPolicy() != policy )
				{
					return;
				}

				if( layer->GetActive() )
				{
					layer->ActiveTick( updateContext, depBuilder );
				}
				else
				{
					layer->PassiveTick( updateContext, depBuilder );
				}
			}
		};
		static red::InstrumentationObject instrObj{"UI/TickLayers", PBC_UI};
		builder.DispatchParallelForJob( instrObj, {m_layers.Size()}, tickSingleLayerFunc );
	}

	void ViewportWrapper::ProcessLayersDraw(const ink::UpdateContext& updateContext, job::Builder& builder, ink::LayerDrawingPolicy policy)
	{
		static red::InstrumentationObject instrObj{ "UI/DrawLayers", PBC_UI };
		{
			const unsigned cp2077Policy = ( policy == LayerDrawingPolicy::InOrder ) ? 1u : 0u;
			const unsigned cp2077Dispatched = ++s_cp2077DrawDispatched[ cp2077Policy ];
			if ( CP2077UiProbeDue( cp2077Dispatched ) )
			{
				std::fprintf( stderr, "CP2077 ui viewport probe: drawLayers dispatched this=%p policy=%s count=%u layers=%u\n", (void*)this, cp2077Policy ? "inOrder" : "inParallel", cp2077Dispatched, unsigned( m_layers.Size() ) );
			}
		}

		builder.DispatchJob( instrObj, [this, updateContext, policy]( const job::RunContext& runContext )
		{
			job::Builder depBuilder( runContext );

			if( policy == LayerDrawingPolicy::InParallel )
			{
				for( Uint32 i : m_layers.Indices() )
				{
					if (const LayerHandle layer = m_layers[i])
					{
						const ELayerType layerType = layer->GetType();
						if (layer->GetDrawingPolicy() == policy && layer->GetActive() && IsActiveGlobally(layer->GetType()) && IsVisibleForDebug(layerType))
						{
							m_layers[i]->Draw(updateContext, depBuilder);
						}
					}
				}
				depBuilder.DispatchFenceExplicitly();
			}

			if( policy == LayerDrawingPolicy::InOrder )
			{
				for( Uint32 i : m_layers.ReverseIndices() )
				{
					if (const LayerHandle layer = m_layers[i])
					{
						const ELayerType layerType = layer->GetType();
						if (layer->GetDrawingPolicy() == policy && layer->GetActive() && IsActiveGlobally(layer->GetType()) && IsVisibleForDebug(layerType))
						{
							layer->Draw(updateContext, depBuilder);
						}
					}
				}
			}	
			m_hasUpdated.SetValue( true );
			{
				const unsigned cp2077Policy = ( policy == LayerDrawingPolicy::InOrder ) ? 1u : 0u;
				const unsigned cp2077Executed = ++s_cp2077DrawExecuted[ cp2077Policy ];
				if ( CP2077UiProbeDue( cp2077Executed ) )
				{
					std::fprintf( stderr, "CP2077 ui viewport probe: drawLayers executed setHasUpdated this=%p policy=%s count=%u layers=%u blank=%u real=%u\n", (void*)this, cp2077Policy ? "inOrder" : "inParallel", cp2077Executed, unsigned( m_layers.Size() ), s_cp2077PresentBlank.load(), s_cp2077PresentReal.load() );
				}
			}
		} );
	}

	void ViewportWrapper::ProcessVirtualManagerDraw( job::Builder & builder )
	{
		red::DynArray<ELayerType> activeLayers{ ink::PoolInk_Layers_ViewportWrapper() };
		for( auto &layer : m_layers )
		{
			const ELayerType layerType = layer->GetType();
			if( layer->GetActive() && IsActiveGlobally( layer->GetType() ) && IsVisibleForDebug( layerType ) )
			{
				activeLayers.PushBack( layerType );
			}
		}

		m_widgetVirtualDrawManager->Draw( builder, activeLayers );
	}

	RunningMode ViewportWrapper::GetRunningMode() const
	{
		return m_runningMode;
	}

	void ViewportWrapper::GetAudioHandlerRequests( red::DynArray< AudioHandlerRequest >& requests )
	{
		for( const LayerHandle& layer : m_layers )
		{
			layer->GetAudioHandlerRequests( requests );
		}
	}


	static red::UniquePtr< rend::ui::Drawer > CreateBlankFrameComposition( Uint32 x, Uint32 y )
	{
		auto blankFrameComposition = red::CreateUniquePtr< rend::ui::Drawer >();

		rend::CompositionParamsPtr blankFrameCompositionParams = GetInkSystem().GetFullscreenCompositionManager().AssembleCompositionParams( nullptr );
		blankFrameCompositionParams->previousFrameTexture = nullptr;
		blankFrameCompositionParams->shaderParams->m_uiFactor = 0.0f;
		blankFrameCompositionParams->shaderParams->m_uiPassthroughFactor = 1.0f;
		blankFrameCompositionParams->shaderParams->m_backgroundTextureFactor = 0.0f;

		blankFrameComposition->InitializeForFullscreen( x, y, std::move( blankFrameCompositionParams ) );
		rend::ui::DrawCommand* command = blankFrameComposition->RequestDrawCommand( rend::PrimitiveType::TriangleList, 6 );
		{
			// draw black rectangle
			const auto colour = math::Color::BLACK(); // Note(D.G): Yes, I am a monster.
			rend::ui::DrawVertex* vertices = command->LockVertices();
			vertices[ 0 ] = rend::ui::DrawVertex( Vector4( 0.0f, 0.0f, 0.0f ), colour );
			vertices[ 1 ] = rend::ui::DrawVertex( Vector4( (Float)x, 0.0f, 0.0f ), colour );
			vertices[ 2 ] = rend::ui::DrawVertex( Vector4( 0.0f, (Float)y, 0.0f ), colour );
			vertices[ 3 ] = rend::ui::DrawVertex( Vector4( (Float)x, 0.0f, 0.0f ), colour );
			vertices[ 4 ] = rend::ui::DrawVertex( Vector4( (Float)x, (Float)y, 0.0f ), colour );
			vertices[ 5 ] = rend::ui::DrawVertex( Vector4( 0.0f, (Float)y, 0.0f ), colour );
			command->UnlockVertices( 6 );
		}
		command->Finish();

		blankFrameComposition->Finish();

		return blankFrameComposition;
	}

	void ViewportWrapper::UpdateCompositionIfNeeded( const ink::UpdateContext &updateContext, job::Builder &builder )
	{
		const auto rebuildCompFunc = [ this, updateContext ]( const job::RunContext& context )
		{
			// Editor will always redraw since there are so many hacks/workaround that this is not currently worth fixing
			if ( IsInkSystemReady() && GetInkSystem().GetMode() == InkSystemMode::Game )
			{
				m_needRedraw.SetValue( false );

				for ( const LayerHandle& layer : m_layers )
				{
					if ( layer->GetDrawingPolicy() == ink::LayerDrawingPolicy::InOrder && layer->GetNeedsRedraw() )
					{
						m_needRedraw.SetValue(true);
						break;
					}
				}

				if( GetInkSystem().GetFullscreenCompositionManager().GetNeedsRedraw() )
				{
					m_needRedraw.SetValue( true );
				}
			}
			else
			{
				m_needRedraw.SetValue(true);
			}


			if ( m_needRedraw.GetValue() )
			{
				auto x = static_cast< Uint32 >( updateContext.screenResolution.X );
				auto y = static_cast< Uint32 >( updateContext.screenResolution.Y );

				rend::CompositionParamsPtr compositionParams = GetInkSystem().GetFullscreenCompositionManager().AssembleCompositionParams( m_previousFrameTexture->GetRenderResource() );
				m_composition->InitializeForFullscreen( x, y, std::move( compositionParams ) );

				// Create a blank frame composition so we can put this in when we're rendering a frame without having an update called first
				m_blankFrameComposition = CreateBlankFrameComposition( x, y );
			}
		};
		static red::InstrumentationObject instrObj{ "UI/RebuildComp", PBC_UI };
		builder.DispatchJob( instrObj, rebuildCompFunc );
	}

	void ViewportWrapper::FinishComposition( job::Builder &builder )
	{
		const auto finishCompFunc = [this](const job::RunContext& context)
		{
			if (m_needRedraw.GetValue())
			{
				m_composition->Finish();
			}
		};
		static red::InstrumentationObject instrObj{ "UI/FinishComp", PBC_UI };
		builder.DispatchJob(instrObj, finishCompFunc);
	}

	void ViewportWrapper::ProcessWorldDynamicTextureManagerTick( const ink::UpdateContext & updateContext, job::Builder & builder )
	{
		const auto tickWorldDTManagerFunc = [ this, updateContext ]( const job::RunContext& context )
		{
			m_worldDynamicTextureManager->Tick();
		};
		static red::InstrumentationObject instrObj{ "UI/WorldDTManagerTick", PBC_UI };
		builder.DispatchJob( instrObj, tickWorldDTManagerFunc );
	}

	WorldDynamicTextureManager & ViewportWrapper::GetWorldDynamicTextureManager()
	{
		return *m_worldDynamicTextureManager.Get();
	}

	WidgetVirtualDrawManager & ViewportWrapper::GetWidgetVirtualDrawManager()
	{
		return *m_widgetVirtualDrawManager.Get();
	}

	void ViewportWrapper::QueueEvent( const THandle< red::Event >& evt )
	{
		QueueViewportEvent( evt );

		for ( const LayerHandle& layer : m_layers )
		{
			layer->QueueEvent( evt );
		}
	}

	void ViewportWrapper::QueueViewportEvent( const THandle<red::Event>& evt )
	{
		m_eventBroker.QueueEvent( evt );
	}

	void ViewportWrapper::ResetGameData()
	{
		for( const LayerHandle& layer : m_layers )
		{
			layer->ResetGameData();
		}
	}

	void ViewportWrapper::UpdateGameData( const IControllerGameData* gameData )
	{
		for (const LayerHandle& layer : m_layers)
		{
			//we update only permanent layers as those can exist without game services
			//regular layers should never need that
			if (layer->GetIsPermanent())
			{
				layer->UpdateGameData( gameData );
			}
		}
	}

	red::SharedPtr< VideoInstance >  ViewportWrapper::AcquireVideoPlayer(const WeakHandle<ink::VideoWidget>& videoWidget, const VideoWidgetData& videoData, Bool isFullscreen)
	{
		return m_videoManager.AcquireVideoPlayer(videoWidget, videoData, isFullscreen);
	}

	ink::VideoManager& ViewportWrapper::GetVideoManager()
	{
		return m_videoManager;
	}

	void ViewportWrapper::ApplyLayerActivationMask( const LayerActivationMask& mask )
	{
		for ( Uint32 i = 0; i < mask.Size(); ++i )
		{
			SetLayerActive( (ELayerType)i, mask[ i ] );
		}
	}

#if defined( INK_ENABLE_FUNCTIONAL_TESTING )
	void ViewportWrapper::FT_ChangeHUDVisibility( const Bool visibility )
	{
		SetLayerActive( ELayerType::HUD, visibility );
		SetLayerActive( ELayerType::Watermarks, visibility );
		SetLayerActive( ELayerType::SystemNotifications, visibility );
	}
#endif

	void ViewportWrapper::SortLayers()
	{
		// Ensure layers are in the correct order
		static red::FixedSizeFunction<Bool( LayerHandle, LayerHandle )> sortLayersFunc = []( const LayerHandle& layer1, const LayerHandle layer2 )
		{
			return (Uint8)layer1->GetType() < (Uint8)layer2->GetType();
		};
		std::sort( m_layers.Begin(), m_layers.End(), sortLayersFunc );
	}

	void ViewportWrapper::DeactivateAllLayers()
	{
		for ( const auto &layer : m_layers )
		{
			layer->SetActive( false );
		}
	}

	const THandle<StateMachine>& ViewportWrapper::GetStateMachine() const
	{
		return m_stateMachine;
	}

	void ViewportWrapper::PerformBackgroundFrameCapture(Bool continiousCapture)
	{
		m_shouldPerformScreenCapture = true;
		m_continiousScreenCapture = continiousCapture;
	}

	Bool ViewportWrapper::IsPreparingForBackgroundBlur()
	{
		return m_shouldPerformScreenCapture;
	}

	void ViewportWrapper::ForceLayersActivityAndVisibility(Bool isActiveAndVisible)
	{
		red::DynArray< LayerHandle > layers = GetLayers();
		for ( THandle<Layer> layer : layers )
		{
			if (layer)
			{
				layer->RequestGlobalHide( !isActiveAndVisible );
			}
		}
	}

	Bool ViewportWrapper::IsActiveGlobally(ELayerType layerType) const
	{
		return !GetLayerByType( layerType )->GetIsGloballyHidden();
	}

	void ViewportWrapper::OnGlobalVisibilityChanged(Bool shouldBeHidden)
	{
		m_uiShouldBeGloballyHidden = shouldBeHidden;

		const Bool isActiveAndVisible = !shouldBeHidden;
		ForceLayersActivityAndVisibility(isActiveAndVisible);

		// special actions related to fade in / fade out mechanism
		BlockUnblockMenuBasedOnGlobalHiddenFlag();
	}

	void ViewportWrapper::OnGlobalFullscreenVisibilityChanged(Bool shouldBeHidden)
	{
		if (!shouldBeHidden)
		{
			if (THandle< ink::MenuLayer > menuLayer = GetLayerByType< ink::MenuLayer >( ink::ELayerType::Menu ))
			{
				menuLayer->ShowFullscreenUI();
			}
		}
	}

	RegistrationToken ViewportWrapper::RegisterLoadingScreenShownCallback( red::FixedSizeFunction<void()>&& callbackFunction )
	{
		THandle<LoadingLayer> loadingLayer = GetLayerByType<LoadingLayer>( ELayerType::Loading );
		//loadingLayer will not exist if the game was started with '-noFullscreenUI' cmd line arg
		if (loadingLayer)
		{
			RegistrationToken token = loadingLayer->OnLoadingScreenShown += callbackFunction;
			return token;
		}

		return RegistrationToken::INVALID();
	}

	RegistrationToken ViewportWrapper::RegisterLoadingScreenHiddenCallback( red::FixedSizeFunction<void()>&& callbackFunction )
	{
		THandle<LoadingLayer> loadingLayer = GetLayerByType<LoadingLayer>( ELayerType::Loading );
		//loadingLayer will not exist if the game was started with '-noFullscreenUI' cmd line arg
		if (loadingLayer)
		{
			RegistrationToken token = loadingLayer->OnLoadingScreenHidden += callbackFunction;
			return token;
		}

		return RegistrationToken::INVALID();
	}

	void ViewportWrapper::UnregisterLoadingScreenShownCallback( const RegistrationToken& callbackToken )
	{
		THandle<LoadingLayer> loadingLayer = GetLayerByType<LoadingLayer>( ELayerType::Loading );
		//loadingLayer will not exist if the game was started with '-noFullscreenUI' cmd line arg
		if (loadingLayer)
		{
			loadingLayer->OnLoadingScreenShown -= callbackToken;
		}
	}

	void ViewportWrapper::UnregisterLoadingScreenHiddenCallback( const RegistrationToken& callbackToken )
	{
		THandle<LoadingLayer> loadingLayer = GetLayerByType<LoadingLayer>( ELayerType::Loading );
		//loadingLayer will not exist if the game was started with '-noFullscreenUI' cmd line arg
		if (loadingLayer)
		{
			loadingLayer->OnLoadingScreenHidden -= callbackToken;
		}
	}

	void ViewportWrapper::BlockUnblockMenuBasedOnGlobalHiddenFlag()
	{
		if (THandle< ink::MenuLayer > menuLayer = GetLayerByType< ink::MenuLayer >(ink::ELayerType::Menu))
		{
			if (m_uiShouldBeGloballyHidden)
			{
				menuLayer->BlockMenu(LockAllMenus);
			}
			else
			{
				menuLayer->UnblockMenu(LockAllMenus);
			}
		}
	}

	void ViewportWrapper::ApplyDebugLayerVisibility(const red::DynArray<ink::ELayerType>& hiddenLayers)
	{
		for (Uint32 i = 0; i < (Uint32)ELayerType::MAX; ++i)
		{
			m_debugLayerVisibility[i] = true;
		}

		for (Uint32 i = 0; i < hiddenLayers.Size(); ++i)
		{
			Uint32 index = static_cast<Uint32>(hiddenLayers[i]);
			if (index < static_cast<Uint32>(ELayerType::MAX))
			{
				m_debugLayerVisibility[index] = false;
			}
		}
	}

	Bool ViewportWrapper::IsVisibleForDebug(ELayerType layerType) const
	{
		Uint32 index = static_cast<Uint32>(layerType);
		if (index < static_cast<Uint32>(ELayerType::MAX))
		{
			return m_debugLayerVisibility[index];
		}
		return false;
	}

} // namespace ink
