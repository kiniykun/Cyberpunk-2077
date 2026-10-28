#include <cstdio>
/**
* Copyright (C)2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "initialLoadingScreenLogicController.h"
#include "../gameTweakDB/include/records/tweakDBInitLoadingScreen.h"
#include "../inkCore/include/animationDataStructs.h"
#include "../inkCore/include/animationProxy.h"
#include "../inkCore/include/canvasWidget.h"
#include "../inkCore/include/pointerEvent.h"
#include "../inkCore/include/widgetControllerUtils.h"
#include "../sound/include/scriptAudioInterface.h"
#include "../sound/include/soundSystem.h"
#include "../inkCore/include/animationResource.h"
#include "../inkCore/include/widgetUtils.h"
#include "../inkCore/include/videoWidget.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( InitialLoadingScreenLogicController, ink );
RTTI_PARENT_TYPE( ink::ILoadingLogicController );
RTTI_SCRIPT_ALIAS( "InitialLoadingScreenLogicController" );
RTTI_PROPERTY( m_skipButtonPanel ).editable();
RTTI_PROPERTY( m_loadingPartsContainer ).editable();
RTTI_PROPERTY( m_afterSkipAnimation ).editable();
RTTI_PROPERTY( m_loadingFinishedAudioStopEvent ).editable();
RTTI_END_TYPE();

namespace ink
{

	InitialLoadingScreenLogicController::InitialLoadingScreenLogicController()
		: m_proxy( red::CreateUniquePtr<InitialLoadingScreenProxy>( *this ) )
		, m_currentState( ILoadingScreen::State::Hidden )
		, m_partToShow( ELoadingScreenParts::LS1 )
		, m_allAnimationsPlayed( false )
		, m_currentLoadingScreenWidgetLibrary( nullptr )
		, m_spawnedWidgets( (Uint32)ELoadingScreenParts::MAX )
		, m_engineFinishedLoading( false )
		, m_buttonPressed( false )
	{
	}

	InitialLoadingScreenLogicController::~InitialLoadingScreenLogicController()
	{
	}

	void InitialLoadingScreenLogicController::Initialize( const InitializeContext& initContext )
	{
		TBaseClass::Initialize( initContext );
		m_skipButtonPanel.Get()->SetVisible( false );
	}

	void InitialLoadingScreenLogicController::SetLoadingState( const ILoadingScreen::State newState, const Bool calledFromStreaming )
	{
		if (m_currentState == newState)
		{
			return;
		}
		m_currentState = newState;

		switch(newState)
		{
		case ILoadingScreen::State::WaitForKey:
			if( !m_allAnimationsPlayed ) 
			{
				EnableSkipButton();
			}
			break;
		case ILoadingScreen::State::Hidden:
			HideLoadingScreen();
			break;
		case ILoadingScreen::State::Loading:
			CheckFactsAndPickResources();
			break;
		}
	}

	ILoadingScreen::State InitialLoadingScreenLogicController::GetLoadingState()
	{
		return ILoadingScreen::State();
	}

	ILoadingScreen* InitialLoadingScreenLogicController::GetProxy() const
	{
		return m_proxy.Get();
	}

	void InitialLoadingScreenLogicController::SetProgress( const Float value )
	{
		if (m_loadingProgress != value)
		{
			rtti::CallFunction( this, RED_NAME_CONSTEXPR( "SetLoadProgress" ), value );
			m_loadingProgress = value;
		}
	}

	void InitialLoadingScreenLogicController::SetProgressIndicatorVisiblity( Bool enabled )
	{
		rtti::CallFunction( this, RED_NAME_CONSTEXPR( "SetProgressIndicatorVisibility" ), enabled );
	}

	void InitialLoadingScreenLogicController::SetSpinnerVisibility( Bool enabled )
	{
		rtti::CallFunction( this, RED_NAME_CONSTEXPR( "SetSpinnerVisiblility" ), enabled );
	}

	void InitialLoadingScreenLogicController::InitializeLoadingScreenParts( )
	{
		m_spawnedWidgets[(Uint32)ELoadingScreenParts::LS1] = ::ink::SpawnFromLocal( m_loadingPartsContainer.Get(), m_currentLoadingScreenWidgetLibrary, m_currentTDBRecord->firstAnimLibraryName() );
		m_spawnedWidgets[(Uint32)ELoadingScreenParts::LS2] = ::ink::SpawnFromLocal( m_loadingPartsContainer.Get(), m_currentLoadingScreenWidgetLibrary, m_currentTDBRecord->secondAnimLibraryName() );
		m_spawnedWidgets[(Uint32)ELoadingScreenParts::LS3] = ::ink::SpawnFromLocal( m_loadingPartsContainer.Get(), m_currentLoadingScreenWidgetLibrary, m_currentTDBRecord->thirdAnimLibraryName() );
		
		for( THandle<WidgetLibraryItemInstance> instance : m_spawnedWidgets ) 
		{
			if (instance)
			{
				instance->GetRootWidget()->SetVisible( false );
			}
		}

		if ( GEngine->GetSoundSystem() )
		{
			GEngine->GetSoundSystem()->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "ui_main_menu_loop_stop" ) );
			GEngine->GetSoundSystem()->GetScriptAudio()->Play( RED_NAME_CONSTEXPR("mus_game_menus_03_STOP") );
		}

		PlayAnimation();
	}

	void InitialLoadingScreenLogicController::PlayAnimation()
	{
		ShowLoadingScreenPart( m_partToShow );

		switch (m_partToShow)
		{
		case ELoadingScreenParts::LS1:
			PlaySingleAnim( m_currentTDBRecord->firstAnimName );
			break;
		case ELoadingScreenParts::LS2:
			PlaySingleAnim( m_currentTDBRecord->secondAnimName );
			break;
		case ELoadingScreenParts::LS3:
			anim::PlaybackOptions playbackOption;
			playbackOption.m_loopType = anim::LoopType::None;
			playbackOption.m_loopInfinite = false;

			m_currentAnimation = ::ink::PlayLibraryAnimation
			(
				m_spawnedWidgets[(Uint32)m_partToShow]->GetRootWidget(),
				m_currentLoadingScreenWidgetLibrary,
				m_currentTDBRecord->thirdAnimName,
				playbackOption
			);

			m_currentAnimation->RegisterToCallback( ink::anim::EventType::OnFinish, [weakController = WeakHandleFromThis<InitialLoadingScreenLogicController>()]( const THandle< ink::anim::Proxy >& proxy )
			{
				if (THandle< InitialLoadingScreenLogicController > controller = weakController.ToHandle())
				{
					controller->PlayLoopingAnimation();
				}
			} );

			break;
		}
	}

	void InitialLoadingScreenLogicController::PlayLoopingAnimation()
	{
		anim::PlaybackOptions playbackOptionLoop;
		playbackOptionLoop.m_loopType = anim::LoopType::Cycle;
		playbackOptionLoop.m_loopInfinite = true;

		m_currentLoopedAnimation = ::ink::PlayLibraryAnimation
		(
			m_spawnedWidgets[(Uint32)m_partToShow]->GetRootWidget(),
			m_currentLoadingScreenWidgetLibrary,
			m_currentTDBRecord->loopAnimName,
			playbackOptionLoop
		);
	}

	void InitialLoadingScreenLogicController::PlaySingleAnim( const CName& singleAnimName )
	{
		anim::PlaybackOptions playbackOption;
		playbackOption.m_loopType = anim::LoopType::None;
		playbackOption.m_loopInfinite = false;

		if (m_spawnedWidgets[(Uint32)m_partToShow])
		{
			m_currentAnimation = ::ink::PlayLibraryAnimation( m_spawnedWidgets[(Uint32)m_partToShow]->GetRootWidget(), m_currentLoadingScreenWidgetLibrary, singleAnimName, playbackOption );

			m_currentAnimation->RegisterToCallback( ink::anim::EventType::OnFinish, [weakController = WeakHandleFromThis< InitialLoadingScreenLogicController >()]( const THandle< ink::anim::Proxy > )
			{
				if (THandle< InitialLoadingScreenLogicController > controller = weakController.ToHandle())
				{
					controller->SetNextAnimation();
				}
			} );
		}
	}

	void InitialLoadingScreenLogicController::EnableSkipButton()
	{
		SetProgressIndicatorVisiblity( false );
		WeakHandle<InitialLoadingScreenLogicController> loadingController = WeakHandleFromThis<InitialLoadingScreenLogicController>();
		m_loadingHiddenToken = GetInkSystem().OnPreOnPress += [loadingController]( const THandle<PointerEvent>& evt )
		{
			if (THandle< InitialLoadingScreenLogicController > loadingControllerHandle = loadingController.ToHandle())
			{
				if (evt->GetActionName()->IsAction( "UI_Skip" ))
				{
					loadingControllerHandle->m_buttonPressed = true;
					loadingControllerHandle->PlayLibraryAnimation( loadingControllerHandle->m_afterSkipAnimation, anim::PlaybackOptions() );
				}
			}
		};

		m_skipButtonPanel.Get()->SetVisible( true );
	}

	void InitialLoadingScreenLogicController::HideLoadingScreen()
	{
		if ( auto widget = m_skipButtonPanel.GetSafe() )
		{
			widget->SetVisible( false );
		}

		if ( auto widget = m_loadingPartsContainer.GetSafe< ink::CompoundWidget >() )
		{
			widget->RemoveAllChildren();
		}

		m_partToShow = ELoadingScreenParts::LS1;
		GetInkSystem().OnPreOnPress -= m_loadingHiddenToken;
		GetInkSystem().PlaySound( m_loadingFinishedAudioStopEvent );

		if (m_currentAnimation)
		{
			m_currentAnimation->Stop();
		}

		if (m_currentLoopedAnimation)
		{
			m_currentLoopedAnimation->Stop();
		}

		OnPostLoadingFinishedCallback();
	}

	void InitialLoadingScreenLogicController::SpawnLoadingScreenWidget( THandle<WidgetLibraryResource> library )
	{
		m_currentLoadingScreenWidgetLibrary = library;
		
		if( THandle<CanvasWidget> canvas = Cast<CanvasWidget>( GetWidget() ) )
		{
			InitializeLoadingScreenParts( );
		}

		WeakHandle< InitialLoadingScreenLogicController> weakThis = WeakHandleFromThis< InitialLoadingScreenLogicController>();
		res::ResourceTokenHandle token = library->GetAnimationLibraryResRef().IssueLoadingRequest( io::eAsyncPriority_UI );
		token->OnLoaded( [weakThis]( THandle<CResource> resource )
		{
			if (THandle< InitialLoadingScreenLogicController> handle = weakThis)
			{
				THandle< anim::AnimationLibraryResource > animLibrary = Cast< anim::AnimationLibraryResource >( resource );

				THandle<const anim::Sequence> sequence;
				animLibrary->GetAnimation( handle->m_currentTDBRecord->voTrackAnimName(), sequence );

				THandle< ink::Widget > rootWidget = handle->GetWidget();
				red::DynArray< WeakHandle< ink::Widget > > targets { ink::PoolInk_Controllers() };
				targets.PushBack( rootWidget );

				if (sequence && rootWidget)
				{
					PlayAnimationSequence( rootWidget, sequence, targets, anim::PlaybackOptions() );
				}
			}
		} );
	}

	void InitialLoadingScreenLogicController::SetQuestSystem( quest::IQuestsSystem* questSystem )
	{
		m_questSystem = questSystem;
	}

	void InitialLoadingScreenLogicController::CheckFactsAndPickResources()
	{
		THandle< game::data::InitLoadingScreen_Record> record = game::data::GetRecord<game::data::InitLoadingScreen_Record>( m_initialLoadingTweak ).ToHandle();

		if (!record)
		{
			record = game::data::GetRecord<game::data::InitLoadingScreen_Record>( "InitLoadingScreen.DefaultInitialLoadingScreen" ).ToHandle();
		}

		if (!record)
		{
			RED_ASSERT( record, "No default initial loading screen record found" );
		}

		SpawnLoadingScreen( record );
	}

	void InitialLoadingScreenLogicController::SpawnLoadingScreen( THandle<game::data::InitLoadingScreen_Record>& loadingScreenRecord )
	{
		m_currentTDBRecord = loadingScreenRecord;
		m_loadingScreenResource = loadingScreenRecord->loadingScreenResource();
		THandle< CompoundWidget > root = Cast< CompoundWidget >( GetWidget() );

		if ( auto widget = m_loadingPartsContainer.GetSafe< ink::CompoundWidget >() )
		{
			widget->RemoveAllChildren();
		}

		res::ResourceTokenHandle token = m_loadingScreenResource.IssueLoadingRequest(io::eAsyncPriority_UI);
		WeakHandle< InitialLoadingScreenLogicController> weakThis = WeakHandleFromThis< InitialLoadingScreenLogicController>();
		token->OnLoaded( [weakThis]( THandle<CResource> resource )
		{
			if( resource )
			{
				if( THandle<WidgetLibraryResource> widgetLibraryResource = Cast<WidgetLibraryResource>( resource ) )
				{
					if( THandle< InitialLoadingScreenLogicController> handle = weakThis )
					{
						handle->SpawnLoadingScreenWidget( widgetLibraryResource );
					}
					else
					{
						RED_LOG_WARNING( "InitialLoadingScreenLogicController: NO CONTROLLER" );
					}
				}
				else
				{
					RED_LOG_WARNING( "InitialLoadingScreenLogicController: WRONG RESOURCE TYPE" );
				}
			}
			else
			{
				RED_LOG_WARNING( "InitialLoadingScreenLogicController: NO RESOURCE" );
			}
		} );
	}

	void InitialLoadingScreenLogicController::SetNextAnimation()
	{
		StopLoadingScreenVideos( m_partToShow );

		switch( m_partToShow )
		{
		case ELoadingScreenParts::LS1:
			m_partToShow = ELoadingScreenParts::LS2;
			break;
		case ELoadingScreenParts::LS2:
			m_partToShow = ELoadingScreenParts::LS3;
			break;
		case ELoadingScreenParts::LS3:
			m_allAnimationsPlayed = true;
			break;
		default:
			break;
		}

		PlayAnimation();
	}

	void InitialLoadingScreenLogicController::StopLoadingScreenVideos( ELoadingScreenParts part )
	{
		ink::WidgetUtils::ForeachChild_RootToLeaf( m_spawnedWidgets[ ( Uint32 )part ]->GetRootWidget(), [] ( const THandle< ink::Widget >& widget )
		{
			if ( auto videoWidget = Cast< ink::VideoWidget >( widget ) )
			{
				videoWidget->Stop();
			}
		} );
	}

	void InitialLoadingScreenLogicController::ShowLoadingScreenPart( ELoadingScreenParts part )
	{
		for( Uint32 i = 0; i < m_spawnedWidgets.Size(); i++ )
		{
			if (m_spawnedWidgets[i])
			{
				if (m_spawnedWidgets[i] == m_spawnedWidgets[(Uint32)part])
				{
					m_spawnedWidgets[i]->GetRootWidget()->SetVisible( true );
				}
				else
				{
					m_spawnedWidgets[i]->GetRootWidget()->SetVisible( false );
				}
			}
		}

	}

	void InitialLoadingScreenLogicController::SetGameSessionLoadingFinished( Bool finished )
	{
		m_gameSessionLoaded = finished;

		if ( finished )
		{
			HideLoadingScreen();
		}
	}

	Bool InitialLoadingScreenLogicController::IsGameSessionLoadingFinished() const
	{
		return m_gameSessionLoaded;
	}

	void InitialLoadingScreenLogicController::SetInitialLoadingTweak( TweakDBID initialLoading )
	{
		THandle< game::data::InitLoadingScreen_Record> record = game::data::GetRecord<game::data::InitLoadingScreen_Record>( initialLoading ).ToHandle();

		if (record)
		{
			m_initialLoadingTweak = initialLoading;
		}
		else
		{
			m_initialLoadingTweak = TweakDBID( "InitLoadingScreen.DefaultInitialLoadingScreen" );
		}
	}

	void InitialLoadingScreenLogicController::LoadingScreenSetupFinished()
	{

	}

	void InitialLoadingScreenLogicController::LoadingAssetsComplete()
	{
		m_engineFinishedLoading = true;
		EnableSkipButton();
	}

	Bool InitialLoadingScreenLogicController::ShouldFinishLoadingState() const
	{
		static unsigned cpWebUiChecks=0; const unsigned cpWebUiCheck=++cpWebUiChecks;
		if(cpWebUiCheck<=4 || ((cpWebUiCheck & (cpWebUiCheck-1))==0))
			std::fprintf(stderr,"CP2077 loading UI probe: controller=initialLoadingScreenLogicController.cpp check=%u animationsFinished=%u buttonPressed=%u engineLoaded=%u sessionLoaded=%u\n",cpWebUiCheck,unsigned(m_allAnimationsPlayed),unsigned(m_buttonPressed),unsigned(m_engineFinishedLoading),unsigned(m_gameSessionLoaded));
		return m_allAnimationsPlayed || m_buttonPressed;
	}

	InitialLoadingScreenProxy::InitialLoadingScreenProxy( InitialLoadingScreenLogicController& controller )
		: m_controller( controller )
	{

	}

	InitialLoadingScreenProxy::~InitialLoadingScreenProxy()
	{

	}

	void InitialLoadingScreenProxy::SetLoadingState( const State newState )
	{
		m_controller.SetLoadingState( newState, true );
	}

	ILoadingScreen::State InitialLoadingScreenProxy::GetLoadingState()
	{
		return m_controller.GetLoadingState();
	}

	void InitialLoadingScreenProxy::SetProgress( const Float value )
	{
		m_controller.SetProgress( value );
	}

	void InitialLoadingScreenProxy::OnLoadingScreenFinished( const red::FixedSizeFunction<void()>& callback )
	{

	}

	void InitialLoadingScreenProxy::SetGameSessionLoadingFinished( Bool finished )
	{
		m_controller.SetGameSessionLoadingFinished( finished );
	}

	Bool InitialLoadingScreenProxy::IsGameSessionLoadingFinished() const
	{
		return m_controller.IsGameSessionLoadingFinished();
	}

	CName InitialLoadingScreenProxy::GetCompositionName()
	{
		return RED_NAME_CONSTEXPR( "inkInitialLoadingState" );
	}

	void InitialLoadingScreenProxy::SetLoadingScreenData( red::SharedPtr< ILoadingScreenData > data )
	{
		if (red::SharedPtr< InitialLoadingScreenData > initalLoadingData = red::StaticCast< InitialLoadingScreenData >( data ))
		{
			m_controller.SetInitialLoadingTweak( initalLoadingData->m_initialLoadingTweakID );
		}
	}

	void InitialLoadingScreenProxy::LoadingAssetsComplete()
	{
		m_controller.LoadingAssetsComplete();
	}

	Bool InitialLoadingScreenProxy::ShouldFinishLoadingState() const
	{
		return m_controller.ShouldFinishLoadingState();
	}

}