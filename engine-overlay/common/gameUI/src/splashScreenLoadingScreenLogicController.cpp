#include <cstdio>
/**
* Copyright (C)2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "splashScreenLoadingScreenLogicController.h"
#include "../inkCore/include/inkSystem.h"
#include "../inkCore/include/pointerEvent.h"
#include "../inkCore/include/videoWidget.h"
#include "../engine/include/fadeOutSupervisorInterface.h"
#include "../redCore/include/commandline.h"
#include "../inkCore/include/animationDataStructs.h"
#include "../inkCore/include/animationProxy.h"
#include "../localization/include/localizationManager.h"
#include "../redCore/include/languageRegion.h"
#include "../redConfig/include/inGameConfigSystem.h"
#include "../redConfig/include/inGameConfigGroup.h"
#include "../redConfig/include/inGameConfigVar.h"
#include "../redFileSystem/include/fileSys.h"
#include "../../../common/resource/include/jsonSerializer.h"
#include "../../../common/gameServices/include/gameServices.h"
#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
#include "../gameServices/include/gameServices.h"
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( BinkLanguageDescriptor, ink );
	RTTI_PROPERTY( m_bink ).editable();
	RTTI_PROPERTY( m_languageId ).editable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( LocalizedBink, ink );
	RTTI_PROPERTY( m_binks ).editable().resizable();
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( SplashScreenLoadingScreenLogicController, ink );
	RTTI_PARENT_TYPE( ink::ILoadingLogicController );
	RTTI_SCRIPT_ALIAS( "SplashScreenLoadingScreenLogicController" );
	RTTI_PROPERTY( m_logoTrainWBBink ).editable();
	RTTI_PROPERTY( m_logoTrainNamcoBink ).editable();
	RTTI_PROPERTY( m_logoTrainStadiaBink ).editable();
	RTTI_PROPERTY( m_logoTrainNoRTXBink ).editable();
	RTTI_PROPERTY( m_logoTrainRTXBink ).editable();
	RTTI_PROPERTY( m_introMessageBink ).editable();
	RTTI_PROPERTY( m_trailerBink ).editable();
	RTTI_PROPERTY( m_logosTrainAnimation ).editable();
	RTTI_PROPERTY( m_localizedMessageAnimation ).editable();
	RTTI_PROPERTY( m_gameIntroAnimation ).editable();
	RTTI_PROPERTY( m_longLogosTrainAnimation ).editable();
	RTTI_PROPERTY( m_stopIntroAudioEventName ).editable();
	RTTI_PROPERTY( m_afterSkipAnimation ).editable();
	RTTI_PROPERTY( m_videoPlayer ).editable();
	RTTI_PROPERTY( m_skipButtonPanel ).editable();
RTTI_END_TYPE();

namespace ink
{
	TResAsyncRef< Bink > m_logoTrainWBBink;
	TResAsyncRef< Bink > m_logoTrainNamcoBink;
	TResAsyncRef< Bink > m_logoTrainStadiaBink;
	TResAsyncRef< Bink > m_logoTrainNoRTXBink;
	TResAsyncRef< Bink > m_logoTrainRTXBink;

	SplashScreenLoadingScreenLogicController::SplashScreenLoadingScreenLogicController()
		: m_proxy( red::CreateUniquePtr<SplashScreenLoadingScreenProxy>( *this ) )
		, m_splashScreenFinished( false )
		, m_gameSessionLoaded( false )
		, m_bumpersFinished( false )
		, m_skipLongLoadingScreens( false )
		, m_engineFinishedLoading( false )
		, m_selectedPublisher( Publisher::NoRTX )
	{
	}

	SplashScreenLoadingScreenLogicController::~SplashScreenLoadingScreenLogicController()
	{
	}

	void SplashScreenLoadingScreenLogicController::Initialize( const InitializeContext& initContext )
	{
		TBaseClass::Initialize( initContext );

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
		const red::CommandLine& commandLine = red::CommandLine::Get();

		if (commandLine.HasOption( "keepLongLoadingScreens" ))
		{
			m_skipLongLoadingScreens = false;
		}
		else
		{
			m_skipLongLoadingScreens = true;
		}
#endif

#ifdef __EMSCRIPTEN__
		// CP2077 web [sandbox-floor]: ?sandbox sessions take the dev (non-FINAL) default and skip the long
		// loading screens, so WaitingForUIAcceptance finishes without a "PRESS UI_Skip TO CONTINUE" key press.
		// CP2077 web [boot]: the direct-start links (?start=..., ?jump=..., ?sandbox*) also skip the intro logo videos
		// (CDPR / REDengine / legal), like the PC game's skip-intro option: this is the engine's own skip path
		// (non-FINAL default), so the splash loading state ends as soon as the session is loaded.
		// ?skipIntro=1 forces it on any link, ?skipIntro=0 keeps the logos. The plain URL keeps the logos.
		{
			const int cpSkip = MAIN_THREAD_EM_ASM_INT( {
				try {
					const q = new URLSearchParams( location.search );
					const on = v => v !== null && v !== '0' && v !== 'false';
					const f = q.get( 'skipIntro' );
					if ( f !== null ) return on( f ) ? 3 : 0;
					if ( on( q.get( 'sandbox' ) ) || q.has( 'sandboxWorld' ) || q.has( 'sandboxGamedef' ) ) return 1;
					if ( on( q.get( 'start' ) ) || ( q.get( 'jump' ) || '' ) !== '' ) return 2;
					return 0;
				} catch ( e ) { return 0; }
			} );
			if ( cpSkip )
			{
				m_skipLongLoadingScreens = true;
				std::fprintf( stderr, "[boot] splash loading screen skipLongLoadingScreens=1 (intro logo videos skipped, %s)\n",
					cpSkip == 1 ? "?sandbox" : cpSkip == 2 ? "?start/?jump" : "?skipIntro=1" );
			}
		}
#endif

		m_skipButtonPanel.Get()->SetVisible( false );
		LoadSettings();
	}

	void SplashScreenLoadingScreenLogicController::SetLoadingState( const ILoadingScreen::State newState, const Bool calledFromStreaming )
	{
		if (m_currentState != newState)
		{
			OnLoadingStateChangeCallback( newState );
		}

		m_currentState = newState;

		switch (newState)
		{
		case ILoadingScreen::State::Loading:
			DisplayLogoTrain();
			break;
		case ILoadingScreen::State::WaitForKey :
			TryEnableSkipButton();
			break;
		case ILoadingScreen::State::Hidden :
			HideLoadingScreen();
			break;
		}
	}

	ILoadingScreen::State SplashScreenLoadingScreenLogicController::GetLoadingState()
	{
		return m_currentState;
	}

	ILoadingScreen* SplashScreenLoadingScreenLogicController::GetProxy() const
	{
		return m_proxy.Get();
	}

	void SplashScreenLoadingScreenLogicController::SetProgress( const Float value )
	{
	}

	void SplashScreenLoadingScreenLogicController::TryEnableSkipButton()
	{
		const Bool finished =
#if defined( RED_PLATFORM_ORBIS )
			!GGameServices->IsGameStillInstalling() &&
#endif
			m_bumpersFinished && m_engineFinishedLoading;

		if ( finished )
		{
			EnableSkipButton();
		}
	}

	void SplashScreenLoadingScreenLogicController::EnableSkipButton()
	{
		WeakHandle<SplashScreenLoadingScreenLogicController> loadingController = WeakHandleFromThis<SplashScreenLoadingScreenLogicController>();

		if (auto callback = GetInkSystem().GetGlobalInputCallback( RED_NAME_CONSTEXPR( "OnPostOnRelease" ) ))
		{
			m_loadingHiddenToken = *callback += [loadingController]( const THandle<PointerEvent>& evt )
			{
				if (THandle< SplashScreenLoadingScreenLogicController > loadingControllerHandle = loadingController.ToHandle())
				{
					if (evt->GetActionName()->IsAction( "UI_Skip" ))
					{
						loadingControllerHandle->m_splashScreenFinished = true;
						loadingControllerHandle->TryHideLoadingScreen();
						loadingControllerHandle->PlayLibraryAnimation( loadingControllerHandle->m_afterSkipAnimation, anim::PlaybackOptions() );
					}
				}
			};
		}

		m_skipButtonPanel.Get()->SetVisible( true );
		OnMainLoadingFinishedCallback();
	}

	void SplashScreenLoadingScreenLogicController::TryHideLoadingScreen()
	{
		if (m_splashScreenFinished && m_gameSessionLoaded)
		{
			SetLoadingState( ILoadingScreen::State::Hidden );
		}
	}

	void SplashScreenLoadingScreenLogicController::HideLoadingScreen()
	{
		m_skipButtonPanel.Get()->SetVisible( false );

		if( m_loadingHiddenToken.IsValid() )
		{
			if( auto callback = GetInkSystem().GetGlobalInputCallback( RED_NAME_CONSTEXPR( "OnPostOnRelease" ) ) )
			{
				*callback -= m_loadingHiddenToken;
			}
		}

		if ( auto widget = m_videoPlayer.GetSafe< ink::VideoWidget >() )
		{
			widget->SetIsLoadingScreenVideo( false );
#ifdef __EMSCRIPTEN__
			// CP2077 web [boot]: skipped intro - stop the logo video now; its animation callbacks early-out below,
			// so no further logo / message / trailer Bink is opened (CPU-decoded on web) behind the hidden screen.
			if ( m_skipLongLoadingScreens )
			{
				widget->Stop();
			}
#endif
		}

		GetInkSystem().PlaySound( m_stopIntroAudioEventName );
		GetWidget()->SetVisible( false );
#if defined( RED_PLATFORM_ORBIS )
		GLocalizationManager->m_forcePSSystemLanguageOnStartup = false;
#endif
#if defined( RED_PLATFORM_CONSOLE )
		GLocalizationManager->SetTextLanguageFromSettings( false, InGameConfig::Source::Code );
#endif
		OnPostLoadingFinishedCallback();
	}

	void SplashScreenLoadingScreenLogicController::DisplayLogoTrain()
	{
		TResAsyncRef< Bink > selectedBink;

		CName logoAnimName = m_logosTrainAnimation;
		switch (m_selectedPublisher)
		{
		case Publisher::WB:
			selectedBink = m_logoTrainWBBink;
			logoAnimName = m_longLogosTrainAnimation;
			break;
		case Publisher::Namco:
			selectedBink = m_logoTrainNamcoBink;
			logoAnimName = m_longLogosTrainAnimation;
			break;
		case Publisher::Stadia:
			selectedBink = m_logoTrainStadiaBink;
			logoAnimName = m_longLogosTrainAnimation;
			break;
		case Publisher::RTX:
			selectedBink = m_logoTrainRTXBink;
			break;
		default: selectedBink = m_logoTrainNoRTXBink;
		}

		THandle< ink::VideoWidget > videoWidget = m_videoPlayer.GetSafe< VideoWidget >();
		if ( videoWidget )
		{
			videoWidget->SetVideoPath( selectedBink.GetPath() );
			videoWidget->PreloadNextVideo( m_introMessageBink.GetLocalizedBink().GetPath() );
		}

		m_logosAnimProxy = PlayLibraryAnimation( logoAnimName, anim::PlaybackOptions() );
		m_logosAnimProxy->RegisterToCallback( anim::EventType::OnFinish,
		[weakController = WeakHandleFromThis<SplashScreenLoadingScreenLogicController>()]( const THandle< anim::Proxy >& proxy ) {
			if (THandle< SplashScreenLoadingScreenLogicController > controller = weakController.ToHandle())
			{
				controller->DisplayLocalizedMessage();
			}
		} );

		if ( videoWidget )
		{
			videoWidget->SetIsLoadingScreenVideo( true );
		}
	}

	void SplashScreenLoadingScreenLogicController::DisplayLocalizedMessage()
	{
#ifdef __EMSCRIPTEN__
		if ( m_skipLongLoadingScreens && m_currentState == ILoadingScreen::State::Hidden )
		{
			m_bumpersFinished = true; // [boot] intro skipped and screen already hidden: no more videos
			return;
		}
#endif
		THandle< ink::VideoWidget > videoWidget = m_videoPlayer.GetSafe< VideoWidget >();
		TResAsyncRef< Bink > binkRes = m_introMessageBink.GetLocalizedBink();
		
		if ( videoWidget )
		{
			videoWidget->Stop();
			if ( binkRes.IsValid() )
			{
				videoWidget->SetVideoPath( binkRes.GetPath() );
				videoWidget->PreloadNextVideo( m_trailerBink.GetPath() );
			}
		}

		if (binkRes.IsValid())
		{
			m_messageAnimProxy = PlayLibraryAnimation( m_localizedMessageAnimation, anim::PlaybackOptions() );
			m_messageAnimProxy->RegisterToCallback( anim::EventType::OnFinish,
				[weakController = WeakHandleFromThis<SplashScreenLoadingScreenLogicController>()]( const THandle< anim::Proxy >& proxy ) {
				if (THandle< SplashScreenLoadingScreenLogicController > controller = weakController.ToHandle())
				{
					controller->m_bumpersFinished = true;
					controller->TryEnableSkipButton();
					controller->DisplayTrailer();
				}
			} );
		}
		else
		{
			m_bumpersFinished = true;
			TryEnableSkipButton();
			DisplayTrailer();
		}
	}

	void SplashScreenLoadingScreenLogicController::DisplayTrailer()
	{
#ifdef __EMSCRIPTEN__
		if ( m_skipLongLoadingScreens && m_currentState == ILoadingScreen::State::Hidden )
		{
			return; // [boot] intro skipped and screen already hidden: no trailer video
		}
#endif
		THandle< ink::VideoWidget > videoWidget = m_videoPlayer.GetSafe< VideoWidget >();
		{
			videoWidget->Stop();
			videoWidget->SetVideoPath( m_trailerBink.GetPath() );
		}

		m_introAnimProxy = PlayLibraryAnimation( m_gameIntroAnimation, anim::PlaybackOptions() );
		m_introAnimProxy->RegisterToCallback( anim::EventType::OnFinish,
		[weakController = WeakHandleFromThis<SplashScreenLoadingScreenLogicController>()]( const THandle< anim::Proxy >& proxy ) {
			if (THandle< SplashScreenLoadingScreenLogicController > controller = weakController.ToHandle())
			{
				controller->m_splashScreenFinished = true;
				controller->TryHideLoadingScreen();
			}
		} );
	}

	void SplashScreenLoadingScreenLogicController::SetGameSessionLoadingFinished( Bool finished )
	{
		if (finished)
		{
			m_gameSessionLoaded = finished;

			if (m_skipLongLoadingScreens || ( !m_logosAnimProxy && !m_introAnimProxy && !m_logosAnimProxy ))
			{
				m_splashScreenFinished = true;
			}

			TryHideLoadingScreen();
		}
	}

	Bool SplashScreenLoadingScreenLogicController::IsGameSessionLoadingFinished() const
	{
		return m_gameSessionLoaded;
	}

	void SplashScreenLoadingScreenLogicController::LoadingScreenSetupFinished()
	{

	}

	void SplashScreenLoadingScreenLogicController::LoadingAssetsComplete()
	{
		m_engineFinishedLoading = true;
		if( !m_skipLongLoadingScreens )
		{
			TryEnableSkipButton();
		}
	}

	Bool SplashScreenLoadingScreenLogicController::ShouldFinishLoadingState() const
	{
		static unsigned cpWebUiChecks=0; const unsigned cpWebUiCheck=++cpWebUiChecks;
		if(cpWebUiCheck<=4 || ((cpWebUiCheck & (cpWebUiCheck-1))==0))
			std::fprintf(stderr,"CP2077 loading UI probe: controller=splashScreenLoadingScreenLogicController.cpp check=%u skip=%u splashFinished=%u bumpersFinished=%u engineLoaded=%u sessionLoaded=%u logos=%u message=%u intro=%u\n",cpWebUiCheck,unsigned(m_skipLongLoadingScreens),unsigned(m_splashScreenFinished),unsigned(m_bumpersFinished),unsigned(m_engineFinishedLoading),unsigned(m_gameSessionLoaded),unsigned(bool(m_logosAnimProxy)),unsigned(bool(m_messageAnimProxy)),unsigned(bool(m_introAnimProxy)));
		return m_skipLongLoadingScreens || m_splashScreenFinished;
	}

	void SplashScreenLoadingScreenLogicController::LoadSettings()
	{
		const red::AbsolutePath c_path = GFileManager->GetGameRoot().AddFilePath( "config\\bumpersSettings.json" );
		red::UniquePtr< IFile > file = GFileManager->CreateFileReader( c_path, FOF_AbsolutePath );

		if (!file)
		{
			return;
		}

		red::String string;
		if (!red::LoadFileToString( *file.Get(), string ))
		{
			return;
		}

		rapidjson::JSONDocument doc;
		rapidjson::ParseResult result = doc.Parse( string.AsChar(), string.Length() );
		if (!result)
		{
			return;
		}

		const rapidjson::JSONValue& json( doc );
		if (!json.IsObject())
		{
			return;
		}

		if (!json.HasMember( "WB" ) || !json["WB"].IsBool() ||
			!json.HasMember( "Namco" ) || !json["Namco"].IsBool() ||
			!json.HasMember( "Stadia" ) || !json["Stadia"].IsBool() ||
			!json.HasMember( "RTX" ) || !json["RTX"].IsBool())
		{
			return;
		}

		m_selectedPublisher = Publisher::Invalid;
		if (json["WB"].GetBool())
		{
			m_selectedPublisher = Publisher::WB;
		}
		else if (json["Namco"].GetBool())
		{
			m_selectedPublisher = Publisher::Namco;
		}
		else if (json["Stadia"].GetBool())
		{
			m_selectedPublisher = Publisher::Stadia;
		}
		else if (json["RTX"].GetBool())
		{
			m_selectedPublisher = Publisher::RTX;
		}
		else
		{
			m_selectedPublisher = Publisher::NoRTX;
		}

#if defined( RED_PLATFORM_ORBIS )
		//hack to not display publisher for korean system language
		red::StringView languageCodeName = GGameServices->GetLanguageCodeName();
		const ink::LanguageId activeLanguageId = GetInkSystem().GetInternationalizationManager().ConvertToLanguageId( RED_NAME( languageCodeName ) );

		if ( activeLanguageId == LanguageId::KR )
		{
			m_selectedPublisher = Publisher::NoRTX;
		}
#elif defined( RED_PLATFORM_DURANGO )
		auto countryCode = services::GetConsoleRegion();

		if ( countryCode == "kr" )
		{
			m_selectedPublisher = Publisher::NoRTX;
		}
#endif
	}

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	SplashScreenLoadingScreenProxy::SplashScreenLoadingScreenProxy( SplashScreenLoadingScreenLogicController& controller )
		: m_controller( controller )
	{

	}

	SplashScreenLoadingScreenProxy::~SplashScreenLoadingScreenProxy()
	{

	}

	void SplashScreenLoadingScreenProxy::SetLoadingState( const State newState )
	{
		m_controller.SetLoadingState( newState, true );
	}

	ILoadingScreen::State SplashScreenLoadingScreenProxy::GetLoadingState()
	{
		return m_controller.GetLoadingState();
	}

	void SplashScreenLoadingScreenProxy::SetProgress( const Float value )
	{

	}

	void SplashScreenLoadingScreenProxy::OnLoadingScreenFinished( const red::FixedSizeFunction<void()>& callback )
	{
		m_controller.OnPostLoadingFinishedCallback += callback;
	}

	void SplashScreenLoadingScreenProxy::SetGameSessionLoadingFinished( Bool finished )
	{
		m_controller.SetGameSessionLoadingFinished( finished );
	}

	Bool SplashScreenLoadingScreenProxy::IsGameSessionLoadingFinished() const
	{
		return m_controller.IsGameSessionLoadingFinished();
	}

	void SplashScreenLoadingScreenProxy::SetLoadingScreenData( red::SharedPtr< ILoadingScreenData > data )
	{

	}

	void SplashScreenLoadingScreenProxy::LoadingAssetsComplete()
	{
		m_controller.LoadingAssetsComplete();
	}

	Bool SplashScreenLoadingScreenProxy::ShouldFinishLoadingState() const
	{
		return m_controller.ShouldFinishLoadingState();
	}

	const TResAsyncRef<Bink>& LocalizedBink::GetLocalizedBink()
	{
		audio::Language activeLanguage = GLocalizationManager->GetTextLanguage();
#if defined( RED_PLATFORM_ORBIS )
		activeLanguage = GLocalizationManager->GetPSSystemLanguage();
#endif
		if ( !GLocalizationManager->IsOnScreenLocalizationIncluded( activeLanguage.GetCodeName() ) )
		{
			activeLanguage = GLocalizationManager->GetFallbackLanguage();
		}
		
		const CName activeLanguageCodeName = activeLanguage.GetCodeName();
		const ink::LanguageId activeLanguageId = GetInkSystem().GetInternationalizationManager().ConvertToLanguageId( activeLanguageCodeName );

		for (const BinkLanguageDescriptor& descriptor : m_binks)
		{
			if (descriptor.m_languageId == activeLanguageId && descriptor.m_bink.IsValid())
			{
				return descriptor.m_bink;
			}
		}

		if (m_binks.Size())
		{
			return m_binks[0].m_bink;
		}

		return m_fallbackBink;
	}

}