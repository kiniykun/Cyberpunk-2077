//
// Copyright( C ) CD Projekt Red.All Rights Reserved.
//

#include "build.h"
#include "soundApi.h"
#include "soundPublic.h"
#include "audioSceneUtils.h"
#include "audioInterface.h"
#include "audioEmitter.h"
#include "audioHelpers.h"
#include "audioEventFactory.h"
#include "soundSystem.h"
#include "audioDirectorSystem.h"
#include "entityAudioSystem.h"
#include "../../localization/include/localizationPublic.h"
#include "../../localization/include/localizationManager.h"
#include "../../soundMetadata/include/audioStateMetadata.h"
#include "../../soundEngine/include/wwiseContainerStreaming.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <cstring>
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( DialogLineEventData, audio );
RTTI_PROPERTY( m_stringId );
RTTI_PROPERTY( m_context );
RTTI_PROPERTY( m_expression );
RTTI_PROPERTY( m_isPlayer );
RTTI_PROPERTY( m_isRewind );
RTTI_PROPERTY( m_isHolocall );
RTTI_PROPERTY( m_customVoEvent );
RTTI_PROPERTY( m_seekTime );
RTTI_PROPERTY( m_playbackSpeedParameter );
RTTI_END_TYPE();

namespace audio
{
	void LoadAudioScenesMap( CName audioScenesMapMetadataName, std::function< void( THandle< AudioSceneData > ) > onDefaultSceneLoaded, std::function<void( CName, THandle< AudioSceneData > )> onAudioSceneDataLoaded )
	{
		auto audioScenesMap = GSoundEngine->GetMetadataManager()->GetMetadataObject< AudioScenesMap >( audioScenesMapMetadataName );
		RED_DATA_ERROR( red::LoggerCategory_Audio, audioScenesMap, "AudioScenesMap metadata with name '%s' is missing!", audioScenesMapMetadataName.AsChar() );

		if ( audioScenesMap )
		{
			if ( const Bool hasDefaultScene = audioScenesMap->m_defaultScene && audioScenesMap->m_defaultScene != CName::NONE() )
			{
				auto defaultAudioScene = HandleFromPtr( GSoundEngine->GetMetadataManager()->GetMetadataObject< AudioSceneData >( audioScenesMap->m_defaultScene ) );
				RED_DATA_ERROR( red::LoggerCategory_Audio, defaultAudioScene, "Default audio scene metadata with name '%s' is missing", audioScenesMap->m_defaultScene.AsChar() );

				if ( defaultAudioScene )
				{
					onDefaultSceneLoaded( defaultAudioScene );
				}
			}

			for ( CName questEvent : audioScenesMap->m_scenesToActivateByQuestEvent->GetMap()->Keys() )
			{
				red::Optional< CName > sceneMetadataName = audioScenesMap->m_scenesToActivateByQuestEvent->GetValueForKey( questEvent );
				RED_DATA_ERROR( red::LoggerCategory_Audio, sceneMetadataName.HasValue(), "Failed to load audio scene for event %s", questEvent.AsChar() );

				if ( sceneMetadataName.HasValue() )
				{
					auto audioScene = HandleFromPtr( GSoundEngine->GetMetadataManager()->GetMetadataObject< AudioSceneData >( sceneMetadataName.Value() ) );
					RED_DATA_ERROR( red::LoggerCategory_Audio, audioScene, "Failed to load audio scene for event %s. Metadata file '%s' is missing", questEvent.AsChar(), sceneMetadataName.Value().AsChar() );

					if ( audioScene )
					{
						onAudioSceneDataLoaded( questEvent, audioScene );
					}
				}
			}
		}
	}

	Bool PlayDialogLine( const DialogLineEventData& data, const red::UniquePtr< AudioInterface >& interface, Uint64 entityId, CName voEventOverride, Bool hasHelmet, Uint32& playedSpeechLine, CName& playedVoEvent, Float playbackSpeed, CName tagEvent )
	{
		static const CName sc_voEmitter = RED_NAME_CONSTEXPR( "vo" );
		static const CName sc_globalTvVoEmitter = RED_NAME_CONSTEXPR( "global_tv_vo" );

		res::ResourcePath voResPath;
		loc::ResolveVoFileResult voFilenameResolveResult = loc::ResolveVoFileResult::NoFileFound;
		if( data.m_isRewind )
		{
			voFilenameResolveResult = GLocalizationManager->ResolveRewindVoFileName( data.m_stringId, data.m_variant, voResPath );
		}
		else if( data.m_isHolocall )
		{
			voFilenameResolveResult = GLocalizationManager->ResolveHolocallVoFileName( data.m_stringId, data.m_variant, voResPath );
		}
		else if( hasHelmet )
		{
			voFilenameResolveResult = GLocalizationManager->ResolveHelmetVoFileName( data.m_stringId, data.m_variant, voResPath );
		}

		//If we're not using a special VO type or if we didn't find the file for it, use the normal one
		if( voFilenameResolveResult == loc::ResolveVoFileResult::NoFileFound )
		{
			voFilenameResolveResult = GLocalizationManager->ResolveVoFileName( data.m_stringId, data.m_variant, voResPath );
		}

#if ENABLE_AUDIO_DEBUG
		GSoundSystem->GetEntityAudioSystem()->m_debug->DebugRegisterDialogLine( voResPath, data.m_sceneFilePath, data.m_stringId, voFilenameResolveResult );
#endif

#ifdef __EMSCRIPTEN__
		// CP2077 web [vo]: the port ships no Wwise VO banks, so voice lines are played by the page from converted
		// .ogg files named <locstring RUID hex>_<f|m>. Tell the page which line starts (fire-and-forget, any thread).
		// Engine behaviour is unchanged (the NoFileFound return below still applies).
		{
			Uint64 cpRuid = 0;
			static_assert( sizeof( data.m_stringId ) == sizeof( Uint64 ), "RUID must be 64-bit" );
			std::memcpy( &cpRuid, &data.m_stringId, sizeof( cpRuid ) );
			const int cpVariant = data.m_variant == loc::Variant::male ? 2 : 1;
			const int cpFlags = ( data.m_isPlayer ? 1 : 0 ) | ( data.m_isRewind ? 2 : 0 ) | ( data.m_isHolocall ? 4 : 0 ) | ( hasHelmet ? 8 : 0 );
			MAIN_THREAD_ASYNC_EM_ASM( {
				try { if ( globalThis.cp2077VO ) globalThis.cp2077VO( $0 >>> 0, $1 >>> 0, $2, $3, $4 ); } catch ( e ) {}
			}, ( Uint32 )( cpRuid >> 32 ), ( Uint32 )( cpRuid & 0xffffffffu ), cpVariant, cpFlags, ( double )data.m_seekTime );
		}
#endif

		if( voFilenameResolveResult == loc::ResolveVoFileResult::NoFileFound )
		{
			return false;
		}

		loc::VoiceEventParams voParams;
		voParams.m_context = data.m_context;
		voParams.m_expression = data.m_expression;
		voParams.m_isPlayer = data.m_isPlayer;

		CName voEvent = RED_NAME_CONSTEXPR( "vo_2d_V" );

		if( !voParams.m_isPlayer )
		{
			static const CName vo_context = RED_NAME_CONSTEXPR( "VO_context" );
			static const CName vo_expression = RED_NAME_CONSTEXPR( "VO_expression" );

			::audio::SetSoundSwitch( entityId, vo_context, voParams.GetContextSwitchName() );
			::audio::SetSoundSwitch( entityId, vo_expression, voParams.GetExpressionSwitchName() );
			voEvent = RED_NAME_CONSTEXPR( "vo_matrix" );
		}

		if( voEventOverride )
		{
			voEvent = voEventOverride;
		}
		else if( data.m_expression == loc::Vo_Expression_Cyberspace )
		{
			voEvent = RED_NAME_CONSTEXPR( "vo_2d_cyberspace" );
		}
		else if( data.m_expression == loc::Vo_Expression_GlobalTV )
		{
			voEvent = RED_NAME_CONSTEXPR( "vo_global_tv" );
		}

		const CName voEmitter = data.m_expression == loc::Vo_Expression_GlobalTV ? sc_globalTvVoEmitter : sc_voEmitter;
		interface->PostEvent( ::audio::CreateEvent( voEmitter,
							  ::audio::EventDescriptor( ::audio::EventActionType::PlayExternal, voEvent, std::move( voResPath ),
							  data.m_seekTime, ::audio::EnablePlayPosition ) ),
							  &playedSpeechLine );

		if( tagEvent )
		{
			interface->PostEvent( ::audio::ChainTagEvent( playedSpeechLine, tagEvent ) );
		}

		// Registering the VO id as the latter could trigger a transition in the active audio scene
		GSoundSystem->GetDirectorSystem()->HandleVOLine( data.m_stringId );
		playedVoEvent = voEvent;
		return true;
	}

	VoFilesPair GetPossibleVoFiles( const red::RUID lineStringId, const ::loc::Variant playerGenderVariant, const Bool canBeRewind, const Bool isHolocall, const Bool hasHelmet )
	{
		VoFilesPair result;
		auto addVoFile = [ &result ] ( const res::ResourcePath& path )
		{
			if( !path.IsEmpty() )
			{
				result.PushBack( path );
			}
		};

		if( canBeRewind )
		{
			res::ResourcePath rewindVoPath;
			GLocalizationManager->ResolveRewindVoFileName( lineStringId, playerGenderVariant, rewindVoPath );
			addVoFile( rewindVoPath );
		}

		res::ResourcePath forwardVoPath;
		if( isHolocall )
		{
			GLocalizationManager->ResolveHolocallVoFileName( lineStringId, playerGenderVariant, forwardVoPath );
		}
		else if( hasHelmet )
		{
			GLocalizationManager->ResolveHelmetVoFileName( lineStringId, playerGenderVariant, forwardVoPath );
		}
		if( forwardVoPath.IsEmpty() )
		{
			GLocalizationManager->ResolveVoFileName( lineStringId, playerGenderVariant, forwardVoPath );
		}
		addVoFile( forwardVoPath );

		return result;
	}

	void AddAudioEventPrefetch( const CName audioEventName )
	{
		if( audioEventName )
		{
			GSoundSystem->AddContainerStreamingPrefetch( audioEventName );
		}
	}

	void RemoveAudioEventPrefetch( const CName audioEventName )
	{
		if( audioEventName )
		{
			GSoundSystem->RemoveContainerStreamingPrefetch( audioEventName );
		}
	}

	void PinVoFileToCache( const res::ResourcePath& voFile )
	{
		if( !voFile.IsEmpty() )
		{
			GSoundEngine->PinFileToCache( voFile );
		}
	}

	void UnpinVoFileFromCache( const res::ResourcePath& voFile )
	{
		if( !voFile.IsEmpty() )
		{
			GSoundEngine->UnpinFileFromCache( voFile );
		}
	}

}