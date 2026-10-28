#include <cstdio>
#include "build.h"
#include "voPathResolver.h"
#include "localizationConsts.h"

#include "../include/localizationManager.h"
#include "../include/localizationDBPipe.h"
#include "../../../common/redCore/include/languageRegion.h"

#include "../../../common/redFileSystem/include/fileSys.h"

#include "../../../common/renderData/include/debugFilter.h"
#include "../../../common/renderData/include/debugFilterMask.h"

#include "../../../common/redReflection/include/resourcePath.h"
#include "../../../common/redJobs2/include/jobBuilder.h"

#include "../../../common/redSystem/include/log.h"
#include "../../soundMetadata/include/localizationLanguage.h"
#include "../../soundMetadata/include/audioMetadataManager.h"

#include "../../../common/redCore/include/absolutePath.h"
#include "../../../common/redCore/include/commandline.h"
#include "../../../common/redReflection/include/gatheredResource.h"

#include "../../../common/redSystem/include/crt.h"
#include "../../../common/redConfig/include/inGameConfigSystem.h"
#include "../../../common/redConfig/include/inGameConfigGroup.h"
#include "../../../common/redConfig/include/inGameConfigVar.h"
#include "../../../common/redReflection/include/resourceDepot.h"
#include "../../../common/resource/include/archiveLanguageMap.h"

LOCALIZATION_API loc::LocalizationManager *GLocalizationManager = nullptr;

static CName s_configLanguageGroup( RED_NAME_CONSTEXPR( "/language" ) );
static CName s_configLanguageVoiceOver( RED_NAME_CONSTEXPR( "VoiceOver" ) );
static CName s_configLanguageSubtitles( RED_NAME_CONSTEXPR( "Subtitles" ) );
static CName s_configLanguageOnScreen( RED_NAME_CONSTEXPR( "OnScreen" ) );

namespace loc
{
	void UpdateSettingsFor(
		InGameConfig::VarListName &var,
		const audio::Language &lang,
		red::DynArray< Uint32 > &langs,
		const red::DynArray< audio::LanguageMapItem > &allLangs,
		const InGameConfig::Source source )
	{
		const Int32 varRequestedIndex = var.GetRequestedIndex();
		const Int32 varCurrentIndex = var.GetIndex();
		const Bool cameFromSettings = varRequestedIndex != varCurrentIndex;

		if ( cameFromSettings )
		{
			return;
		}

		const Int32 langsSize = allLangs.Size();
		for ( Int32 i = 0; i < langsSize; ++i )
		{
			if ( allLangs[ i ].m_language == lang )
			{
				const Int32 index = langs.GetIndex( i );

				if ( index != varCurrentIndex )
				{
					var.SetIndex( index, source, false );
				}

				return;
			}
		}
	}

	LocalizationManager::LocalizationManager()
		: m_resourceLoader( GResourceLoader )
		, m_config( nullptr )
		, m_fallbackLanguage{ "English", "en-us", true }
		, m_logQueries( false )
		, m_globalVariantIsFemale( true )
		, m_globalBodyVariantIsFemale( true )
		, m_langsWithVoiceOver( audio::PoolAudioSystems() )
		, m_langsWithSubtitles( audio::PoolAudioSystems() )
		, m_settingsListenerID( 0 )
		, m_reportMissingVoLines( false )
		, m_onscreensByLanguage{ audio::PoolAudioSystems() }
		, m_subtitlesByLanguage{ audio::PoolAudioSystems() }
#if defined( RED_PLATFORM_ORBIS )
		, m_systemVoMissingIndex( -1 )
		, m_hasNoVoicePacksInstalled( false )
		, m_forcePSSystemLanguageOnStartup( true )
#endif
		, m_shouldReloadOnScreens( false )
		, m_needsVoLanguageUpdate( false )
		, m_forceVoLanguageUpdate( false )
		, m_onScreensGenerationInProgress( false )
	{
		GLocalizationManager = this;
	}

	void LocalizationManager::Init()
	{
#ifndef NO_EDITOR
		m_locDBQuerier = red::CreateUniquePtr < LocalizationDBQuery >();
#endif

		LoadLanguageConfiguration();
		LoadStandardVoLanguageDataMap();

		localizationPersistence::SyncLoadLocMap( m_subtitlesByLanguage, m_onscreensByLanguage );

		m_logQueries = false;

		// Passing the default language as the latter will be set at the end of this Init function and we don't want to load a storage that will be unloaded immediately.
		// TODO: in the final version instead of passing the default language we should pass the language the game is set to.
		job::Builder builder{ job::Priority::Latent };
		m_voDatabase.Initialize( builder, GetDefaultLanguage(), m_config->m_commonVoLanguage );

#ifndef NO_EDITOR
		auto commandLine = red::CommandLine::Get();
		m_reportMissingVoLines = commandLine.HasOption( "logMissingVo" );

		if ( m_reportMissingVoLines )
		{
			constexpr Bool appendToExistingFile = true;
			m_missingVoReporter.Initialize( appendToExistingFile );
		}

		LoadCLNumberDateContainer();

		LoadFallbackOnscreens();
		LoadPLOnscreens();
#else
		m_reportMissingVoLines = false;
#endif

		RegisterSettingsHandler();
		InitializeLanguagesInSettings();
	}

	void LocalizationManager::Shutdown()
	{
		UnregisterSettingsHandler();

#ifndef NO_EDITOR
		if ( m_reportMissingVoLines )
		{
			m_missingVoReporter.OnShutdown();
		}
#endif
	}

	void LocalizationManager::Tick( job::Builder& builder )
	{
#ifndef NO_EDITOR
		if (m_shouldReloadOnScreens)
		{
			job::Counter loadingCounter{ job::Priority::CriticalPath };
			ReloadOnScreensAsync(loadingCounter);
			builder.DispatchWait(loadingCounter);
			m_shouldReloadOnScreens = false;
		}
#endif
		if ( m_needsVoLanguageUpdate || m_forceVoLanguageUpdate )
		{
			if ( m_voDatabase.GetState() == VoDatabaseState::ReadyForQueries )
			{
				m_needsVoLanguageUpdate = false;
				m_voDatabase.SetLanguage( builder, m_voiceLanguage, m_forceVoLanguageUpdate );
				m_forceVoLanguageUpdate = false;
			}
		}
	}

	LocalizationManager::~LocalizationManager()
	{
		GLocalizationManager = nullptr;
	}

	const audio::LanguageGameConfiguration* LocalizationManager::GetConfig() const
	{
		return m_config;
	}

	bool LocalizationManager::GlobalVariantIsFemale() const
	{
		return m_globalVariantIsFemale;
	}

	const audio::Language& LocalizationManager::ResolveClosestLanguage( const String& languageCountry ) const
	{
		const String languageCountryStr = red::SanitiseLocale( languageCountry );

		// Find and return an exact match if possible
		// This should be the default place we find the language as we should be getting a well formatted matching language and country string
		for ( const audio::LanguageMapItem& languageInConfig : m_config->m_langsInProject )
		{
			if ( languageInConfig.m_language.GetCodeNameString() == languageCountryStr )
			{
				return languageInConfig.m_language;
			}
		}

		// If we haven't found a matching locale then just extract the language and attempt to find the closest match for it
		String languageCodeStr = red::GetLanguageFromLocale( languageCountryStr );
		if ( languageCountryStr.Empty() || languageCodeStr.Empty() )
		{
			// If the language country string is empty it means it wasn't a correctly formatted locale string to begin with
			// If the language code string is empty it means there wasn't a valid language in the locale string (and/or the above case as well)
			// Therefore in either case we'll just lower case the entire input string and use that to try and find a language match
			languageCodeStr = languageCountry;
			languageCodeStr.ToLower();
		}

		// quick hack, cannot change the entire language pipeline to fix the names of the language and regions right now
		// to be fixed soon
		if ( languageCodeStr == "ja" )
		{
			languageCodeStr = "jp";
		}
		else if ( languageCodeStr == "ko" )
		{
			languageCodeStr = "kr";
		}
		else if ( languageCodeStr == "cs" )
		{
			languageCodeStr = "cz";
		}

		const CName languageCode = RED_NAME( languageCodeStr );

		// Find the first entry with the same language
		for ( const audio::LanguageMapItem& languageInConfig : m_config->m_langsInProject )
		{
			if ( languageInConfig.m_language.GetLanguageCode() == languageCode )
			{
				return languageInConfig.m_language;
			}
		}

		RED_LOG_ERROR( "LocalizationManager: Couldn't find a language to match '%hs', using fallback language '%hs'",
			languageCountry.AsChar(), m_fallbackLanguage.GetCodeNameString().AsChar() );
		return m_fallbackLanguage;
	}

	const audio::Language& LocalizationManager::GetDefaultLanguage() const
	{
		return ResolveClosestLanguage( red::GetDefaultLanguageCountryCode() );
	}

	const audio::Language& LocalizationManager::GetVoiceLanguage() const
	{
		return m_voiceLanguage;
	}

	CName LocalizationManager::GetVoiceLanguageCode() const
	{
		return m_voiceLanguage.GetLanguageCode();
	}

	void LocalizationManager::SetVoiceLanguage( const audio::Language &voiceLang, const InGameConfig::Source source )
	{
#if defined( RED_PLATFORM_CONSOLE )
		if ( !IsAudioLocalizationIncluded( voiceLang.GetCodeName() ) )
		{
			return;
		}
#endif
		if ( m_voiceLanguage != voiceLang )
		{
			m_voiceLanguage = voiceLang;
			// The VoDatabase language cannot be set immediately because its data loading is async
			// and might not be ready yet (when language is set during Init phase)
			m_needsVoLanguageUpdate = true;

			auto &system = InGameConfig::System::GetInstance();

			if ( system.GetMode() == InGameConfig::Mode::Game && source != InGameConfig::Source::LoadSettings )
			{
				auto &languageGroup = system.GetGroup( s_configLanguageGroup );
				auto &varVoiceOvers = languageGroup.GetVarListName( s_configLanguageVoiceOver );

				UpdateSettingsFor( varVoiceOvers, voiceLang, m_langsWithVoiceOver, m_config->m_langsInProject, source );
			}

		}
	}

	void LocalizationManager::SetVoiceLanguageFromSettings( const Bool requested, const InGameConfig::Source source )
	{
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		auto &group = system.GetGroup( s_configLanguageGroup );
		auto &var = group.GetVarListName( s_configLanguageVoiceOver );
		const Int32 index = requested ? var.GetRequestedIndex() : var.GetIndex();

		if ( !m_langsWithVoiceOver.Empty() )
		{
			const Int32 realIndex = m_langsWithVoiceOver[ index ];
			const auto &language = m_config->m_langsInProject[ realIndex ];
			const Bool isInstalled = IsLanguageVoicePackInstalled( language.m_language.GetCodeName() );
			if ( isInstalled )
			{
				SetVoiceLanguage( language.m_language, source );
			}
			else
			{
				var.RestoreDefault( InGameConfig::Source::Code );
			}
		}
		else
		{
			SetVoiceLanguage( m_fallbackLanguage, source );
		}
	}

	const audio::Language &LocalizationManager::GetSubtitleLanguage() const
	{
		return m_subtitleLanguage;
	}

	void LocalizationManager::SetSubtitleLanguage( const audio::Language &subtitleLang, const InGameConfig::Source source )
	{
#if defined( RED_PLATFORM_CONSOLE )
		if ( !IsSubtitleLocalizationIncluded( subtitleLang.GetCodeName() ) )
		{
			return;
		}
#endif
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() == InGameConfig::Mode::Game && source != InGameConfig::Source::LoadSettings )
		{
			auto &languageGroup = system.GetGroup( s_configLanguageGroup );
			auto &varSubtitles = languageGroup.GetVarListName( s_configLanguageSubtitles );

			UpdateSettingsFor( varSubtitles, subtitleLang, m_langsWithSubtitles, m_config->m_langsInProject, source );
		}

		m_subtitleLanguage = subtitleLang;
		LoadSubtitles();
	}

	void LocalizationManager::SetSubtitleLanguageFromSettings( const Bool requested, const InGameConfig::Source source )
	{
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		auto &group = system.GetGroup( s_configLanguageGroup );
		auto &var = group.GetVarListName( s_configLanguageSubtitles );
		const Int32 index = requested ? var.GetRequestedIndex() : var.GetIndex();

		if ( !m_langsWithSubtitles.Empty() )
		{
			const Int32 realIndex = m_langsWithSubtitles[index];
			const auto &language = m_config->m_langsInProject[realIndex];
			SetSubtitleLanguage( language.m_language, source );
		}
		else
		{
			SetSubtitleLanguage( m_fallbackLanguage, source );
		}
	}

	red::StringView LocalizationManager::GetSubtitle( red::RUID key, loc::Variant variant ) const
	{
		if( m_subtitleRepository.HasData() )
		{
			return m_subtitleRepository.AcquireSubtitle( key, variant == Variant::female );
		}

		return red::StringView();
	}

	const audio::Language & LocalizationManager::GetTextLanguage() const
	{
		return m_onScreenLanguage;
	}

	void LocalizationManager::SetTextLanguage( const audio::Language &textLang, const InGameConfig::Source source )
	{
#if defined( RED_PLATFORM_CONSOLE )
		if ( !IsOnScreenLocalizationIncluded( textLang.GetCodeName() ) )
		{
			return;
		}
#endif
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() == InGameConfig::Mode::Game && source != InGameConfig::Source::LoadSettings )
		{
			auto &languageGroup = system.GetGroup( s_configLanguageGroup );
			auto &varOnScreen = languageGroup.GetVarListName( s_configLanguageOnScreen );

			UpdateSettingsFor( varOnScreen, textLang, m_langsWithSubtitles, m_config->m_langsInProject, source );
		}

		m_onScreenLanguage = textLang;
		LoadOnScreenStrings();
	}

	void LocalizationManager::SetTextLanguageFromSettings( const Bool requested, const InGameConfig::Source source )
	{
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

#if defined( RED_PLATFORM_ORBIS )
		if ( m_forcePSSystemLanguageOnStartup && source == InGameConfig::Source::Settings )
		{
			return;
		}
#endif
		auto &group = system.GetGroup( s_configLanguageGroup );
		auto &var = group.GetVarListName( s_configLanguageOnScreen );
		const Int32 index = requested ? var.GetRequestedIndex() : var.GetIndex();

		if ( !m_langsWithSubtitles.Empty() )
		{
			const Int32 realIndex = m_langsWithSubtitles[index];
			const auto &language = m_config->m_langsInProject[realIndex];
			SetTextLanguage( language.m_language, source );
		}
		else
		{
			SetTextLanguage( m_fallbackLanguage, source );
		}
	}

	const audio::Language & LocalizationManager::GetFallbackLanguage() const
	{
		return m_fallbackLanguage;
	}

	void LocalizationManager::SetLanguageGlobal(const audio::Language &lang)
	{
		SetTextLanguage( lang );
		SetSubtitleLanguage( lang );
		SetVoiceLanguage( lang );
	}

	void LocalizationManager::SetVariantGlobal( const Bool isFemale )
	{
		m_globalVariantIsFemale = isFemale;
	}

	void LocalizationManager::SetBodyVariantGlobal( const Bool isFemale )
	{
		m_globalBodyVariantIsFemale = isFemale;
	}

	Bool LocalizationManager::IsBodyVariantFemale() const
	{
		return m_globalBodyVariantIsFemale;
	}

	Variant LocalizationManager::GetGlobalVariant()
	{
		return m_globalVariantIsFemale ? Variant::female : Variant::male;
	}

#ifndef NO_EDITOR

	void LocalizationManager::LoadFallbackOnscreens()
	{
		m_onScreenFallbackRepository.Load( m_onscreensByLanguage[ RED_NAME_CONSTEXPR("db-db" )] );
	}

	void LocalizationManager::LoadPLOnscreens()
	{
		m_onScreenPLRepository.Load( m_onscreensByLanguage[ RED_NAME_CONSTEXPR( "pl-pl" ) ] );
	}

	void LocalizationManager::RefreshVoiceoverDatabase()
	{
		m_voDatabase.Uninitialize();

		job::Builder builder{ job::Priority::Latent };
		m_voDatabase.Initialize( builder, m_config->m_langsInProject[ 0 ].m_language, m_config->m_commonVoLanguage );
	}

	res::GatheredResourceReference g_clnumberJsonResource( RED_CONST_RESOURCEPATH( "base\\localization\\clnumber.json" ), GRF_Debug );

	void LocalizationManager::LoadCLNumberDateContainer()
	{
		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( g_clnumberJsonResource.GetPath() );
		token->WaitUntilLoaded();
		bool isLoaded = token->IsLoaded();

		if (!isLoaded)
		{
			return;
		}

		const THandle< JsonResource > resJson = SafeCast< JsonResource >(token->GetResource());
		m_clContainer = resJson->GetRoot< localizationPersistence::CLNumberDateContainer >();
	}

	CName LocalizationManager::GetCLName() const
	{
		if (m_clContainer)
		{
			return m_clContainer->m_clNumber;
		}
		return CName::NONE();
	}

	CName LocalizationManager::GetCLDate() const
	{
		if (m_clContainer)
		{
			return m_clContainer->m_clTimestamp;
		}
		return CName::NONE();
	}

#endif

	Float LocalizationManager::GetVoFileDuration( red::RUID stringId, Variant variant )
	{
		return m_voDatabase.GetVoDuration( stringId, variant );
	}

	ResolveVoFileResult LocalizationManager::ResolveVoFileName( const red::RUID stringId, Variant variant, res::ResourcePath &outResPath )
	{
		const ResolveVoFileResult result = m_voDatabase.GetVoFile( VoType::Standard, stringId, variant, outResPath );
#ifndef NO_EDITOR
		if ( result == ResolveVoFileResult::NoFileFound && m_reportMissingVoLines )
		{
			red::StringView temp;
			const red::StringView sub = GetSubtitle( stringId, variant );
			m_missingVoReporter.RegisterMissingVoLine( stringId, std::move( sub.ToString() ) );
		}
#endif
		return result;
	}

	ResolveVoFileResult LocalizationManager::ResolveRewindVoFileName( const red::RUID stringId, Variant variant, res::ResourcePath &outResPath )
	{
		return m_voDatabase.GetVoFile( VoType::Rewind, stringId, variant, outResPath );
	}

	ResolveVoFileResult LocalizationManager::ResolveHelmetVoFileName( const red::RUID stringId, Variant variant, res::ResourcePath &outResPath )
	{
		return m_voDatabase.GetVoFile( VoType::Helmet, stringId, variant, outResPath );
	}

	ResolveVoFileResult LocalizationManager::ResolveHolocallVoFileName( const red::RUID stringId, Variant variant, res::ResourcePath &outResPath )
	{
		return m_voDatabase.GetVoFile( VoType::Holocall, stringId, variant, outResPath );
	}

	void LocalizationManager::RegenerateOnScreensStatic(void* jobData, const job::RunContext& runContext)
	{
		auto* thisPtr = static_cast<loc::LocalizationManager*>(jobData);
		thisPtr->RegenerateOnScreens(runContext);
	}
	void LocalizationManager::RegenerateOnScreens(const job::RunContext& runContext)
	{
		SetOnScreensGenerationInProgress(true);

#ifndef NO_EDITOR
		red::String queryContent = "refreshjson;";
		queryContent += GetTextLanguage().GetCodeNameString();
		queryContent += ";";
		auto cachePath = GFileManager->GetGameRoot().AddDirPath("depot\\base\\localization");
		queryContent += cachePath.AsString();
		m_locDBQuerier->DBQuery(queryContent);
#endif

		SetOnScreensGenerationInProgress(false);
	}

	res::GatheredResourceReference g_volanguageDataMapResource( RED_CONST_RESOURCEPATH( "base\\localization\\volanguagedatamap.json" ), GRF_Default );

	void LocalizationManager::LoadStandardVoLanguageDataMap()
	{
		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( g_volanguageDataMapResource.GetPath() );
		token->WaitUntilLoaded();

		if ( token->IsLoaded() )
		{
			const THandle< JsonResource > resJson = SafeCast< JsonResource >( token->GetResource() );
			m_standardVoLanguageDataMap = resJson->GetRoot< VoLanguageDataMap >();
		}
	}

	void LocalizationManager::ReimportOnScreens()
	{
#ifndef NO_EDITOR
		static red::InstrumentationObject instrumentedObject{ "ReimportOnScreens" };
		job::JobDecl jobDecl;
		jobDecl.jobFunc = RegenerateOnScreensStatic;
		jobDecl.jobData = this;
		jobDecl.instrumentationObject = &instrumentedObject;
		job::Counter noWait{ job::Priority::CriticalPath };
		job::Counter noAccum{ job::Priority::CriticalPath };
		job::RunJob(jobDecl, noWait, noAccum);
#endif
	}

	bool LocalizationManager::GetOnScreensRegenerationInProgress()
	{
		return m_onScreensGenerationInProgress;
	}

	void LocalizationManager::ReloadOnScreensAsync(job::Counter& counter)
	{
		LoadOnScreenStringsAsync(counter);
	}

	void LocalizationManager::ReloadOnScreens()
	{
		m_shouldReloadOnScreens = true;
	}

#ifndef NO_EDITOR
	localizationPersistence::OnScreenString LocalizationManager::CreateOnScreenString( const String & content )
	{
		localizationPersistence::OnScreenString result;

		LocalizationDBQuery dbQuery;
		const red::String resultString = dbQuery.DBQuery( "createonscreen;" + content );
		if ( result.ParseDBInput( resultString ) )
		{
			m_onScreenFallbackRepository.AddNewItem( result );
		}

		return result;
	}


	void LocalizationManager::UpdateOnScreenString( const localizationPersistence::OnScreenString& result )
	{
		m_onScreenFallbackRepository.UpdateStringValue( result.m_key, result.m_female, result.m_male );

		m_onScreenFallbackRepository.UpdateStringValue( result.m_key, result.m_femalePL, result.m_malePL );

	}

	void LocalizationManager::AddOnScreenString( const localizationPersistence::OnScreenString& result )
	{
		m_onScreenFallbackRepository.AddNewItem( result );
	}
#endif

	void LocalizationManager::LoadOnScreenStrings()
	{
		if( m_onscreensByLanguage.KeyExist( m_onScreenLanguage.GetCodeName() ) )
		{
			m_onScreenRepository.Load( m_onscreensByLanguage[ m_onScreenLanguage.GetCodeName() ] );
		}
	}

	void LocalizationManager::LoadOnScreenStringsAsync(job::Counter& counter)
	{
		if (m_onscreensByLanguage.KeyExist(m_onScreenLanguage.GetCodeName()))
		{
			m_onScreenRepository.LoadAsync(m_onscreensByLanguage[m_onScreenLanguage.GetCodeName()], counter);
		}
	}

	void LocalizationManager::LoadSubtitles()
	{
		if ( m_subtitlesByLanguage.KeyExist( m_subtitleLanguage.GetCodeName() ) )
		{
			m_subtitleRepository.Load( m_subtitlesByLanguage[ m_subtitleLanguage.GetCodeName() ] );
		}
	}

	const CName LocalizationManager::GetOnScreenStringKey( const String& humanReadableKey ) const
	{
		return CName( m_onScreenRepository.GenerateHashedKey( humanReadableKey ) );
	}

	const String LocalizationManager::LocKeyToString_TEMP(const CName locKey) const
	{
		if (!locKey)
		{
			return String::EMPTY();
		}
		// uint64 is 20 digits max + 7 from prefix + null terminator
		AnsiChar buffer[28]{ 0 };
		red::CNameHash hash = locKey.GetHash();
		red::SNPrintFSafe(buffer, sizeof(buffer), "%s%llu", "LocKey#", hash);
		return String(buffer);
	}

	const red::String LocalizationManager::GetOnScreenString( const String& humanReadableKey ) const
	{
		return GetOnScreenString( humanReadableKey, m_globalVariantIsFemale );
	}
	
	const red::String LocalizationManager::GetItemNameOnScreenString( const String& humanReadableKey ) const
	{
		return GetOnScreenString( humanReadableKey, m_globalBodyVariantIsFemale );
	}

	const red::String LocalizationManager::GetOnScreenString( const String& humanReadableKey, Bool variantIsFemale ) const
	{
		red::String value = m_onScreenRepository.AcquireStringValue( humanReadableKey, variantIsFemale );
        if(humanReadableKey=="UI-ScriptExports-NewGame0" || humanReadableKey=="UI-ScriptExports-LoadGame0" || humanReadableKey=="UI-Labels-Settings" || humanReadableKey=="UI-Labels-Credits"){
            static unsigned reports=0;if(reports++<32)std::fprintf(stderr,"CP2077 localization probe: key=%s resultBytes=%u\n",humanReadableKey.AsChar(),unsigned(value.Length()));
        }

#ifndef NO_EDITOR
		if ( m_logQueries )
		{
			Bool inDB = false;
			red::String keyString = m_onScreenFallbackRepository.GetDebugString( m_onScreenRepository.GenerateHashedKey(humanReadableKey), variantIsFemale, inDB );
			if ( inDB )
			{
				value = keyString + value;
			}
		}
#endif
		return value;
	}

	const red::String LocalizationManager::GetOnScreenString( const CName& humanReadableKey ) const
	{
		return GetOnScreenString( humanReadableKey, m_globalVariantIsFemale );
	}

	const red::String LocalizationManager::GetItemNameOnScreenString( const CName& humanReadableKey ) const
	{
		return GetOnScreenString( humanReadableKey, m_globalBodyVariantIsFemale );
	}

	const red::String LocalizationManager::GetOnScreenString( const CName& humanReadableKey, Bool variantIsFemale ) const
	{
		red::String value = m_onScreenRepository.AcquireStringValue( humanReadableKey, variantIsFemale );
#ifndef NO_EDITOR
		if ( m_logQueries )
		{
			Uint64 key = humanReadableKey.GetHash();

			Bool inDB = false;
			red::String keyString = m_onScreenFallbackRepository.GetDebugString( key, variantIsFemale, inDB );
			if ( inDB )
			{
				value = keyString + value;
			}
		}
#endif
		return value;
	}

#ifndef NO_EDITOR
	const Bool LocalizationManager::GetOnScreenString( const String& humanReadableKey, localizationPersistence::OnScreenString& onScreenData ) const
	{
		return m_onScreenFallbackRepository.AcquireStringValue( humanReadableKey, m_globalVariantIsFemale, onScreenData );
	}

	const Bool LocalizationManager::GetPLOnScreenString( const String& humanReadableKey, localizationPersistence::OnScreenString& onScreenData ) const
	{
		return m_onScreenPLRepository.AcquireStringValue( humanReadableKey, m_globalVariantIsFemale, onScreenData );

		return false;
	}

#endif

	red::Utf16String LocalizationManager::ConvertToUnicode( const String& text ) const
	{
		red::Utf16String result;
		const red::Utf8String utf8Content = text.Empty() ? red::Utf8String() : red::Utf8String( reinterpret_cast< const red::CodeUnit8* >( text.AsChar() ), text.Length() );

		result.FromUtf8( utf8Content );

		return result;
	}
	const red::Utf16String LocalizationManager::GetOnScreenStringUtf16( const String& humanReadableKey ) const
	{
		return GetOnScreenStringUtf16( humanReadableKey, m_globalVariantIsFemale );
	}

	const red::Utf16String LocalizationManager::GetOnScreenStringUtf16( const CName& humanReadableKey ) const
	{
		return GetOnScreenStringUtf16( humanReadableKey, m_globalVariantIsFemale );
	}

	const red::Utf16String LocalizationManager::GetOnScreenStringUtf16( const String& humanReadableKey, Bool variantIsFemale ) const
	{
		const red::String resultString = m_onScreenRepository.AcquireStringValue( humanReadableKey, variantIsFemale );

		if ( !resultString.Empty() )
		{
			return ConvertToUnicode( resultString );
		}

		return red::Utf16String();
	}

	const red::Utf16String LocalizationManager::GetOnScreenStringUtf16( const CName& humanReadableKey, Bool variantIsFemale ) const
	{
		const red::String resultString = m_onScreenRepository.AcquireStringValue( humanReadableKey, variantIsFemale );

		if ( !resultString.Empty() )
		{
			return ConvertToUnicode( resultString );
		}

		return red::Utf16String();
	}

	Bool LocalizationManager::RetrieveLocstringVariant(const red::RUID& locstringId, const CName& localeIdentifier, const loc::Variant& singleGenderMask, localizationPersistence::LocstringVariant& output) const
	{
		return localizationPersistence::RetrieveLocstringVariantFromRepository( m_timeStampRepository, locstringId, singleGenderMask, localeIdentifier, output );
	}

	res::ResourcePath LocalizationManager::GetOnScreensResourcePathForLanguage( CName lang )
	{
		if ( m_onscreensByLanguage.KeyExist( lang ) )
		{
			return m_onscreensByLanguage[ lang ];
		}

		return res::ResourcePath();
	}

	void LocalizationManager::ReloadDebugOnScreens()
	{
#ifndef NO_EDITOR
		m_onScreenFallbackRepository.LoadDebugOnScreens();
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	void LocalizationManager::LoadLanguageConfiguration()
	{
		static audio::LanguageGameConfiguration fallbackLanguageConfig;
		m_config = audio::GMetadataManager->GetMetadataObject<audio::LanguageGameConfiguration>( RED_NAME_CONSTEXPR( "LanguageGameConfiguration" ) );
		if ( !m_config || m_config->m_langsInProject.Size() == 0 )
		{
			RED_DATA_ERROR( red::LoggerCategory_Localization, m_config, "No LanguageGameConfiguration loaded: localized content won't work as intended" );

			// Initializing the fallback configuration to avoid crashes in the vo database
			audio::LanguageMapItem languageMapItem;
			languageMapItem.m_language = m_fallbackLanguage;
			fallbackLanguageConfig.m_langsInProject.Clear();
			fallbackLanguageConfig.m_langsInProject.PushBack( languageMapItem );

			m_config = &fallbackLanguageConfig;
		}
	}

	void LocalizationManager::SelectDefaultLanguage()
	{
		SetLanguageGlobal( GetDefaultLanguage() );
	}

	void LocalizationManager::HandleVarChange( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason reason )
	{
		if ( reason != InGameConfig::ChangeReason::Accepted )
		{
			return;
		}

		if ( groupPath == s_configLanguageGroup )
		{
			if ( varName == s_configLanguageVoiceOver )
			{
				SetVoiceLanguageFromSettings( true );
			}
			else if ( varName == s_configLanguageSubtitles )
			{
				SetSubtitleLanguageFromSettings( true );
			}
			else if ( varName == s_configLanguageOnScreen )
			{
				SetTextLanguageFromSettings( true );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////

	red::AbsolutePath LocalizationManager::DefaultTimeStampWorkingPath()
	{
		red::AbsolutePath soundMetadataDir = GFileManager->GetGameRoot();
		soundMetadataDir.AppendDirPath( "soundbanks\\localization\\stamps\\pl-pl" ); // currently pl_pl only
		return soundMetadataDir;
	}
#ifndef NO_EDITOR
	localizationPersistence::OnScreenString LocalizationManager::UpdateOnScreenString( const red::String& locKey, const red::String& content )
	{
		localizationPersistence::OnScreenString result;
		LocalizationDBQuery dbQuery;
		if ( locKey.BeginsWith( LOC_KEY_PREFIX ) )
		{
			const red::String keyString = locKey.StringAfterFromRight( "#" );
			const red::String resultString = dbQuery.DBQuery( "updateonscreen;" + keyString + "###" + content );
			if ( result.ParseDBInput( resultString ) )
			{
				m_onScreenFallbackRepository.UpdateStringValue( result.m_key, result.m_female, result.m_male );

				m_onScreenPLRepository.UpdateStringValue( result.m_key, result.m_femalePL, result.m_malePL );

			}
		}
		return result;
	}

	localizationPersistence::OnScreenString LocalizationManager::QueryOnScreenString( const String & content )
	{
		localizationPersistence::OnScreenString result;
		if ( GetOnScreenString( content, result ) )
		{
			result.m_female.Replace( "[db]", "" );
			result.m_female.Replace( "[pl]", "" );
			result.m_female.Replace( "[en]", "" );
			result.m_male.Replace( "[db]", "" );
			result.m_male.Replace( "[pl]", "" );
			result.m_male.Replace( "[en]", "" );
			return result;
		}

		return result;
	}

	localizationPersistence::OnScreenString LocalizationManager::QueryPLOnScreenString( const String & content )
	{
		localizationPersistence::OnScreenString result;
		result.m_female = "|";
		result.m_male = "|";
		if ( GetPLOnScreenString( content, result ) )
		{
			//dont show fallback
			if ( result.m_female.BeginsWith( "[db]" ) || result.m_female.BeginsWith( "[en]" ) )
				result.m_female = "|";
			if ( result.m_male.BeginsWith( "[db]" ) || result.m_male.BeginsWith( "[en]" ) )
				result.m_male = "|";

			return result;
		}
		return result;
	}
#endif

	void LocalizationManager::LogQueries( bool condition )
	{
		m_logQueries = condition;
	}

	void LocalizationManager::SetOnScreensGenerationInProgress(const Bool value)
	{
		if (!value && m_onScreensGenerationInProgress)
		{
			m_shouldReloadOnScreens = true;
		}
		m_onScreensGenerationInProgress = value;
	}

	void LocalizationManager::InitializeLanguagesInSettings()
	{
		auto &system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		red::DynArray< CName > voiceOvers{ audio::PoolAudioSystems() };
		red::DynArray< CName > voiceOversTexts{ audio::PoolAudioSystems() };
		red::DynArray< CName > subtitles{ audio::PoolAudioSystems() };
		red::DynArray< CName > subtitlesTexts{ audio::PoolAudioSystems() };
		red::DynArray< CName > onScreens{ audio::PoolAudioSystems() };
		red::DynArray< CName > onScreensTexts{ audio::PoolAudioSystems() };

		m_resolvedDefaultLanguage = GetDefaultLanguage().GetCodeName();
		const CName fallbackLanguage = m_fallbackLanguage.GetCodeName();

		Int32 defaultVOIndex{ -1 };
		Int32 fallbackVOIndex{ -1 };
		Int32 defaultSubtitlesIndex{ -1 };
		Int32 fallbackSubtitlesIndex{ -1 };
		Int32 firstInstalledVoicePackInSystemIndex{ -1 };
		Int32 realDefaultVOIndex{ -1 };

#if defined( RED_PLATFORM_ORBIS )
		res::LanguageID systemLang{ res::LanguageID::EN };
		res::LanguageID defaultVO{ res::LanguageID::EN };

		m_systemVoMissingIndex = -1;
		m_hasNoVoicePacksInstalled = false;
		res::prv::ArchiveLanguageMap *languageMap = GetResourceDepot()->GetLanguageMap();

		systemLang = GetResourceDepot()->GetSystemLanguageID();

		if ( languageMap )
		{
			systemLang = languageMap->GetSystemLanguageID();
			defaultVO = languageMap->GetDefaultVO();
		}
#endif

		for ( Uint32 i = 0, size = m_config->m_langsInProject.Size(); i < size; ++i )
		{
			const auto &language = m_config->m_langsInProject[ i ].m_language;
			const CName codeName = language.GetCodeName();
			const String languageName = "UI-Settings-Language-" + language.GetLongNameString();
			const CName languageDisplayName = RED_NAME( languageName );
#if defined( RED_PLATFORM_ORBIS )
			res::LanguageID langID{ res::LanguageID::Invalid };
			if ( !IsDebugLanguage( language.GetCodeName() ) )
			{
				langID = res::ToLanguageID( language.GetCodeNameString().AsChar() );
				if ( langID == systemLang )
				{
					m_psSystemLanguage = language;
				}
			}
#endif

#ifdef RED_CONFIGURATION_FINAL
			if ( IsDebugLanguage( language.GetCodeName() ) )
			{
				continue;
			}
#endif

			if ( IsSubtitleLocalizationIncluded( codeName ) )
			{
				const Int32 currentIndex = m_langsWithSubtitles.Size();
				m_langsWithSubtitles.PushBack( i );
				subtitles.PushBack( codeName );
				subtitlesTexts.PushBack( languageDisplayName );

				if ( codeName == m_resolvedDefaultLanguage )
				{
					defaultSubtitlesIndex = currentIndex;
				}

				if ( codeName == fallbackLanguage )
				{
					fallbackSubtitlesIndex = currentIndex;
				}
			}

			if ( IsOnScreenLocalizationIncluded( codeName ) )
			{
				onScreens.PushBack( codeName );
				onScreensTexts.PushBack( languageDisplayName );
			}

			if ( IsAudioLocalizationIncluded( codeName ) )
			{
				if ( language.HasVO() )
				{
					const Int32 currentIndex = m_langsWithVoiceOver.Size();
					m_langsWithVoiceOver.PushBack( i );
					voiceOvers.PushBack( codeName );
					voiceOversTexts.PushBack( languageDisplayName );

					if ( firstInstalledVoicePackInSystemIndex == -1 && IsLanguageVoicePackInstalled( codeName ) )
					{
						firstInstalledVoicePackInSystemIndex = currentIndex;
					}

#if defined( RED_PLATFORM_ORBIS )
					if ( languageMap && languageMap->IsLoaded() )
					{
						if ( langID == defaultVO )
						{
							realDefaultVOIndex = currentIndex;

							if ( IsLanguageVoicePackInstalled( codeName ) )
							{
								defaultVOIndex = currentIndex;
							}
							else
							{
								m_systemVoMissingIndex = currentIndex;
							}
						}
					}
					else
#endif
					if ( codeName == m_resolvedDefaultLanguage )
					{
						realDefaultVOIndex = currentIndex;

						if ( IsLanguageVoicePackInstalled( codeName ) )
						{
							defaultVOIndex = currentIndex;
						}
#if defined( RED_PLATFORM_ORBIS )
						else
						{
							m_systemVoMissingIndex = currentIndex;
						}
#endif
					}

					if ( codeName == fallbackLanguage && IsLanguageVoicePackInstalled( codeName ) )
					{
						fallbackVOIndex = currentIndex;
					}
				}
			}
		}

		if ( firstInstalledVoicePackInSystemIndex == -1 )
		{
			// special case where no voice packs are installed and first one is installing..
			firstInstalledVoicePackInSystemIndex = realDefaultVOIndex;
#if defined( RED_PLATFORM_ORBIS )
			m_hasNoVoicePacksInstalled = true;
#endif
		}

		if ( defaultVOIndex == -1 )
		{
			defaultVOIndex = firstInstalledVoicePackInSystemIndex;
		}
		if ( fallbackVOIndex == -1 )
		{
			fallbackVOIndex = defaultVOIndex != -1 ? defaultVOIndex : firstInstalledVoicePackInSystemIndex;
		}

		auto &languageGroup = system.GetGroup( s_configLanguageGroup );
		auto &varVoiceOvers = languageGroup.GetVarListName( s_configLanguageVoiceOver );
		auto &varSubtitles = languageGroup.GetVarListName( s_configLanguageSubtitles );
		auto &varOnScreen = languageGroup.GetVarListName( s_configLanguageOnScreen );

		defaultVOIndex = defaultVOIndex != -1 ? defaultVOIndex : fallbackVOIndex;
		defaultSubtitlesIndex = defaultSubtitlesIndex != -1 ? defaultSubtitlesIndex : fallbackSubtitlesIndex;

		varVoiceOvers.InitializeDynamic( voiceOvers, defaultVOIndex );
		varVoiceOvers.SetDisplayValues( voiceOversTexts );
		varSubtitles.InitializeDynamic( subtitles, defaultSubtitlesIndex );
		varSubtitles.SetDisplayValues( subtitlesTexts );
		varOnScreen.InitializeDynamic( subtitles, defaultSubtitlesIndex );
		varOnScreen.SetDisplayValues( subtitlesTexts );

		SetTextLanguageFromSettings( false, InGameConfig::Source::Code );
		SetSubtitleLanguageFromSettings( false, InGameConfig::Source::Code );
		SetVoiceLanguageFromSettings( false, InGameConfig::Source::Code );
	}

	const Bool LocalizationManager::IsDebugLanguage( const CName languageCodeName )
	{
		return languageCodeName == RED_NAME_CONSTEXPR_NOREG( "db-db" ) ||
			languageCodeName == RED_NAME_CONSTEXPR_NOREG( "pl-db" ) ||
			languageCodeName == RED_NAME_CONSTEXPR_NOREG( "en-db" );
	}

	void LocalizationManager::RegisterSettingsHandler()
	{
		auto& system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		const auto listener = [ this ]( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason reason )
		{
			HandleVarChange( groupPath, varName, varType, reason );
		};

		m_settingsListenerID = system.RegisterListener( s_configLanguageGroup, listener, 255 );
	}

	void LocalizationManager::UnregisterSettingsHandler()
	{
		auto& system = InGameConfig::System::GetInstance();

		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		system.UnregisterListener( m_settingsListenerID );
	}

	bool LocalizationManager::IsOnScreenLocalizationIncluded( const CName languageCodeName )
	{
		return GetResourceDepot()->ResourceExists( m_onscreensByLanguage[languageCodeName] );
	}

	bool LocalizationManager::IsSubtitleLocalizationIncluded( const CName languageCodeName )
	{
		return GetResourceDepot()->ResourceExists( m_subtitlesByLanguage[languageCodeName] );
	}

	bool LocalizationManager::IsAudioLocalizationIncluded( const CName languageCodeName )
	{
		if ( m_standardVoLanguageDataMap )
		{
			auto languageEntries = m_standardVoLanguageDataMap->m_entries;
			auto it = std::find_if( languageEntries.Begin(), languageEntries.End(), [languageCodeName]( const VoLanguageDataEntry& entry )
			{
				return entry.m_languageCode == languageCodeName;
			} );
			if ( it != languageEntries.End() )
			{
				const VoLanguageDataEntry* entry = &(*it);

#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
				if ( IsDebugLanguage( languageCodeName ) )
				{
					return false;
				}

				res::IResourceDepot* depot = GetResourceDepot();
				const Bool c_haveArchiveMap = depot->HaveArchiveMap();
				if ( c_haveArchiveMap )
				{
					return IsLanguageVoicePackAvailable( entry->m_languageCode );
				}
				else
				{
					return GetResourceDepot()->ResourceExists( entry->m_lenghtMapReport.GetPath() );
				}
#elif defined( RED_PLATFORM_WINPC )
				return GetResourceDepot()->ResourceExists( entry->m_lenghtMapReport.GetPath() );
#endif
			}
		}

		return false;
	}

	bool LocalizationManager::IsLanguageVoicePackAvailable( const CName &languageCodeName )
	{
#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
		res::IResourceDepot* depot = GetResourceDepot();
		const Bool c_haveArchiveMap = depot->HaveArchiveMap();
		return c_haveArchiveMap ? depot->IsLanguageVoicePackAvailable( languageCodeName ) : true;
#else
		return true;
#endif
	}

	bool LocalizationManager::IsLanguageVoicePackInstalled( const CName &languageCodeName )
	{
#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
		res::IResourceDepot* depot = GetResourceDepot();
		const Bool c_haveArchiveMap = depot->HaveArchiveMap();
		return c_haveArchiveMap ? depot->IsLanguageVoicePackInstalled( languageCodeName ) : true;
#else
		return true;
#endif
	}

	void LocalizationManager::ForceVoLanguageUpdate()
	{
		m_forceVoLanguageUpdate = true;
	}

	bool LocalizationManager::IsLanguageLocalizationIncluded( const CName languageCodeName )
	{
		return IsOnScreenLocalizationIncluded( languageCodeName ) && IsSubtitleLocalizationIncluded( languageCodeName ) && IsAudioLocalizationIncluded( languageCodeName );
	}

#if defined( RED_PLATFORM_ORBIS )
	const audio::Language& LocalizationManager::GetPSSystemLanguage() const
	{
		return m_psSystemLanguage;
	}

	void LocalizationManager::LoadPSSystemOnscreens( const CName languageCodeName )
	{
		job::Counter jobCounter;
		if ( IsOnScreenLocalizationIncluded( languageCodeName ) )
		{
			m_psSystemOnscreenRepository.LoadAsync( m_onscreensByLanguage[ languageCodeName ], jobCounter );
		}
		else
		{
			m_psSystemOnscreenRepository.LoadAsync( m_onscreensByLanguage[ m_fallbackLanguage.GetCodeName() ], jobCounter );
		}
	}

	const red::String LocalizationManager::GetPSSystemOnScreenString( const String& humanReadableKey ) const
	{
		return m_psSystemOnscreenRepository.AcquireStringValue( humanReadableKey, m_globalVariantIsFemale );
	}

	const red::String LocalizationManager::GetPSSystemOnScreenString( const CName& humanReadableKey ) const
	{
		return m_psSystemOnscreenRepository.AcquireStringValue( humanReadableKey, m_globalVariantIsFemale );
	}

	const red::String LocalizationManager::GetPSSystemOnScreenString( const CName& humanReadableKey, Bool variantIsFemale ) const
	{
		return m_psSystemOnscreenRepository.AcquireStringValue( humanReadableKey, variantIsFemale );
	}

	Bool LocalizationManager::HasSystemVoMissing() const
	{
		return m_systemVoMissingIndex != -1;
	}

	void LocalizationManager::SetSystemVoMissing()
	{
		auto &system = InGameConfig::System::GetInstance();
		if ( system.GetMode() != InGameConfig::Mode::Game )
		{
			return;
		}

		auto &group = system.GetGroup( s_configLanguageGroup );
		auto &var = group.GetVarListName( s_configLanguageVoiceOver );

		var.SetIndex( m_systemVoMissingIndex, InGameConfig::Source::Code );

		system.QueueNotifyStatus( InGameConfig::NotificationType::Refresh );

		m_systemVoMissingIndex = -1;
	}

	void LocalizationManager::ResetSystemVoMissing()
	{
		m_systemVoMissingIndex = -1;
	}

	Bool LocalizationManager::HasNoVoicePacksInstalled() const
	{
		return m_hasNoVoicePacksInstalled;
	}
#endif
}
