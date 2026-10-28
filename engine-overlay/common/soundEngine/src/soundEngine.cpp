/*
* Copyright (c) CD Projekt Red. All Rights Reserved.
*/
//owner: colin.walder

#include "build.h"
#include <cstdio>
#include <atomic>
#include "soundEngine.h"
#include "soundEngineDebug.h"
#include "wwise.h"
#include "wwiseCallbacks.h"
#include "soundReflectionSystem.h"
#include "soundReverbSystem.h"
#include "soundBankManager.h"
#include "musicEngine.h"
#include "sound.h"
#include "soundListener.h"
#include "sourceReverb.h"
#include "positionDataStream.h"
#include "spatialSoundLimitCounter.h"

#include "../../soundMetadata/include/audioMemory.h"
#include "../../soundMetadata/include/audioMetadataManager.h"
#include "../../soundMetadata/include/audioEventMetadata.h"
#include "../../soundMetadata/include/weaponAudioMetadata.h"
#include "../../soundMetadata/include/audioDebug.h"

#include "../../redCore/include/commandline.h"
#include "../../redReflection/include/gatheredResource.h"

#define WWISE_COMMAND_QUEUE_SIZE RED_MEGA_BYTE( 1 )

// Needed for XAudio.
#if defined( RED_PLATFORM_WINPC )
#include <combaseapi.h>
#endif

// Plug-in includes.
#include <AK/Plugin/CDPVoiceBroadcastSendFXFactory.h>
#include <AK/Plugin/CDPVoiceBroadcastReceiveFXFactory.h>
#include <AK/Plugin/CDPSimpleDelayFXFactory.h>
#include <AK/Plugin/AkCompressorFXFactory.h>		// Compressor
#include <AK/Plugin/AkConvolutionReverbFXFactory.h> // Convolution reverb
#include <AK/Plugin/AkMatrixReverbFXFactory.h>		// Matrix reverb
#include <AK/Plugin/AkMeterFXFactory.h>				// Meter
#include <AK/Plugin/AkParametricEQFXFactory.h>		// Parametric equalizer
#include <AK/Plugin/AkPeakLimiterFXFactory.h>		// Peak limiter
#include <AK/Plugin/AkRoomVerbFXFactory.h>			// RoomVerb
#include <AK/Plugin/AkVorbisDecoderFactory.h>
#include <AK/Plugin/AkTimeStretchFXFactory.h>
#include <AK/Plugin/AkDelayFXFactory.h>
#include <AK/Plugin/AkFlangerFXFactory.h>
#include <AK/Plugin/AkTremoloFXFactory.h>
#include <AK/Plugin/AkGuitarDistortionFXFactory.h>
#include <AK/Plugin/AkHarmonizerFXFactory.h>
#include <AK/Plugin/AkPitchShifterFXFactory.h>
#include <AK/Plugin/AkOpusDecoderFactory.h>
//#include <AK/Plugin/MasteringSuiteFXFactory.h>

// Source plug-ins
#include <AK/Plugin/AkSilenceSourceFactory.h> // Silence generator
#include <AK/Plugin/AkToneSourceFactory.h>	// Tone generator
#include <AK/Plugin/AkSineSourceFactory.h>

#ifndef RED_PLATFORM_LINUX
#include <AK/Plugin/AkMotionSinkFactory.h>
#include <AK/Plugin/AkMotionGeneratorSourceFactory.h>
#include <AK/Plugin/AkMotionSourceSourceFactory.h>
#include <AK/Plugin/CrankcaseAudioREVModelPlayerSourceFactory.h>
#endif

#if defined( RED_PLATFORM_DURANGO )
#include <apu.h>
#include <mmdeviceapi.h>

#if defined(USE_PROFILER)
#define USE_PIX
#include <pix.h>
#endif

#endif // RED_PLATFORM_DURANGO

#if defined( RED_PLATFORM_ORBIS )
#include <AK/Plugin/SceAudio3dEngineFactory.h>
#pragma comment(lib, "libSceMove_stub_weak.a")
#pragma comment(lib, "libSceAudio3d_stub_weak.a")
#endif

#ifdef RED_PLATFORM_DURANGO
#define USE_MS_SPATIAL_AUDIO
#endif // RED_PLATFORM_DURANGO
#ifdef RED_PLATFORM_WINPC
#define USE_MS_SPATIAL_AUDIO
#include "../../redSystem/include/platformUtilsWin32.h"
#endif

#ifdef USE_MS_SPATIAL_AUDIO
#include <AK/Plugin/MSSpatialSinkFactory.h>
#endif // RED_PLATFORM_WINPC || RED_PLATFORM_DURANGO

#ifdef RED_PLATFORM_ORBIS	
//#include <audio3d.h>
//#include <libsysmodule.h>
#endif


#include "wwiseContainerStreaming.h"
#include "soundObjects.h"
#include "sourceReverb.h"
#include "audioResourceUtils.h"

#include "../../redCore/include/profilerManager.h"
#include "../../redSystem/include/redThreadsThread.h"
#include "../../redSystem/include/hex.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "oneShotSound.h"

namespace Config
{
	TConfigVar< Bool > cvAudioNoContainer( "Editor/Audio", "NoContainer", false );
	TConfigVar< Bool > cvAudioNoSpatial( "Editor/Audio", "NoSpatial", false );
	TConfigVar< Bool > cvAudioConnectWwiseInFinal("Editor/Audio", "ConnectWwiseInFinal", false );
	TConfigVar< Bool > cvAudioUseXaudio( "Editor/Audio", "UseXaudio", false );
	TConfigVar< Bool > cvAudioPlayDebugEventNotFoundSound( "Editor/Audio", "PlayEventNotFoundSound", true );
	constexpr Uint64 howManyTimesDebugEventNotFoundSoundCanPlayForGivenMissingEventName = 3U;
}

namespace
{
	AudioHash DebugPostMetadataEventNotFoundError( CName eventName, audio::SoundEngine* soundEngine )
	{
#ifndef RED_CONFIGURATION_FINAL
		Bool canPlayEventNotFoundSound = false;
		const String eventNotFoundErrorMessage = String::Printf( "%hs Event not found in Metadata", eventName.AsChar() );
		{
			RED_SCOPE_LOCK( soundEngine->GetMetadataManager()->m_loggedMissingEventNamesLock );
			auto& occurrencesCount = soundEngine->GetMetadataManager()->m_loggedMissingEventNames.GetRef( eventName, 0U );
			if( occurrencesCount == 0U )
			{
				soundEngine->PostErrorToWwiseProfilingLog( eventNotFoundErrorMessage );
			}
			occurrencesCount++;
			canPlayEventNotFoundSound = occurrencesCount <= Config::howManyTimesDebugEventNotFoundSoundCanPlayForGivenMissingEventName;
		}
		AUDIO_DEBUG_DATA_ERROR( ::audio::DebugLogCategory::EventMetadata, eventNotFoundErrorMessage.AsChar() );

		// `test_sound` event
		if( canPlayEventNotFoundSound && Config::cvAudioPlayDebugEventNotFoundSound.Get() )
		{
			constexpr AudioHash testSoundWWiseId = 1936185617U;
			return testSoundWWiseId;
		}
#endif

		return AK_INVALID_PLAYING_ID;
	}

	AudioHash DebugPostMetadataNotFoundError( CName wwiseName, audio::SoundEngine* soundEngine )
	{
#ifndef RED_CONFIGURATION_FINAL
		RED_SCOPE_LOCK( soundEngine->GetMetadataManager()->m_loggedMissingNamesLock );
		if( ! soundEngine->GetMetadataManager()->m_loggedMissingNames.Exist( wwiseName ) )
		{
			soundEngine->GetMetadataManager()->m_loggedMissingNames.Insert( wwiseName );
			if( !wwiseName.AsStringView().StartsWith( "debug" ) )
			{
				const String nameNotFoundErrorMessage = String::Printf( "%hs Name not found in Metadata", wwiseName.AsChar() );
				soundEngine->PostErrorToWwiseProfilingLog( nameNotFoundErrorMessage );
			}
		}
#endif
		return AK_INVALID_PLAYING_ID;
	}
}

static Uint32 wwiseThreadRegisteredProfile;
static red::InstrumentationObject wwiseFrameProfile( "Wwise Update" );
static red::InstrumentationObject wwiseRenderFrameProfile( "Wwise Render Frame" );
static red::InstrumentationObject wwiseProfileMarker( "Wwise Markers" );

namespace audio
{
	SOUND_ENGINE_API SoundEngine* GSoundEngine;

	Bool wwiseIsProcessing = false;
	void wwiseEndProcessingCallback( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
		wwiseContainerStreamingPostCallback( context, location, cookie );
		wwiseIsProcessing = false;
	}

	void wwiseBeginProcessingCallback( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
		wwiseIsProcessing = true;
		wwiseContainerStreamingPreCallback( context, location, cookie );
	}

	void wwiseBeginFrameProfile( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
#if defined(USE_PROFILER)
		RED_ASSERT( location == AkGlobalCallbackLocation_Begin );

		wwiseFrameProfile.m_prevChannel = red::SwapThreadLocalPerfChannels( EProfilerBlockChannel::PBC_AUDIO );
		gProfilers.StartBlock( &wwiseFrameProfile );
#endif // !RED_PLATFORM_ORBIS
	}

	void wwiseEndFrameProfile( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
#if defined(USE_PROFILER)
		gProfilers.StopBlock( &wwiseFrameProfile );
		red::SwapThreadLocalPerfChannels( wwiseFrameProfile.m_prevChannel );
		RED_ASSERT( location == AkGlobalCallbackLocation_End );
#endif
	}

	void wwiseBeginRenderFrameProfile( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
#if defined(USE_PROFILER)
		RED_ASSERT( location == AkGlobalCallbackLocation_BeginRender );
		gProfilers.StartBlock( &wwiseRenderFrameProfile );
#endif
	}

	void wwiseEndRenderFrameProfile( AK::IAkGlobalPluginContext* context, AkGlobalCallbackLocation location, void* cookie )
	{
#if defined(USE_PROFILER)
		gProfilers.StopBlock( &wwiseRenderFrameProfile );
		RED_ASSERT( location == AkGlobalCallbackLocation_EndRender );
#endif
	}

	void wwiseProfileMarkerPush( char const* name )
	{
#if defined(USE_PROFILER)
		gProfilers.StartBlock( &wwiseProfileMarker, name );
#endif
	}

	void wwiseProfileMarkerPop()
	{
#if defined(USE_PROFILER)
		gProfilers.StopBlock( &wwiseProfileMarker );
#endif
	}

	void wwiseEventPerform( void( *callbackResult )() )
	{
		static bool registerThread = false;
		if ( !registerThread )
		{
			red::memory::RegisterCurrentThread( "WWise" );
			PROFILER_InitThread( "Wwise", 1024 );
			registerThread = true;
		}

#if defined(RED_PLATFORM_DURANGO)
#if defined(USE_PROFILER)
		PIXScopedEvent( PIX_COLOR(255,128,255), "wwiseEventPerformCallback" );
#endif
		// Switch Xbox back to let Wwise use a seperate thread to run these callbacks
		callbackResult();
#else
		// Leave other platforms as they are for now
		job::Builder builder( job::Priority::Immediate );
		ScopedProfilerChannel scopedChannel( PBC_AUDIO );
		builder.DispatchJobWithHint< job::JobHint::AudioEvent >( "wwiseEventPerform", [ callbackResult ]( const job::RunContext &context )
		{
			callbackResult();
		} );
#endif
	}


	void wwiseMemInitForThread( const char * threadName )
	{
	}

	void wwiseMemTermForThread()
	{
	}

	void* wwiseMemMalloc( AkMemPoolId poolId, size_t uSize )
	{
		return RED_ALLOCATE( PoolWwise, uSize );
	}

	void* wwiseMemMalign( AkMemPoolId poolId, size_t uSize, AkUInt32 uAlignment )
	{
		return RED_ALLOCATE_ALIGNED( PoolWwise, uSize, uAlignment );
	}

	void* wwiseMemRealloc( AkMemPoolId poolId, void* pAddress, size_t uSize )
	{
		return RED_REALLOCATE( PoolWwise, pAddress, uSize );
	}

	void wwiseMemFree( AkMemPoolId poolId, void* pAddress )
	{
		RED_FREE( PoolWwise, pAddress );
	}

	void wwiseMemFalign( AkMemPoolId poolId, void* pAddress )
	{
		RED_FREE( PoolWwise, pAddress );
	}

	size_t wwiseMemTotalReservedMemorySize()
	{
		return red::memory::GetPoolTotalBytesAllocated< PoolWwise >();
	}

	size_t wwiseMemSizeOfMemory( AkMemPoolId poolId, void* pAddress )
	{
		return PoolWwise::GetInstance().GetBlockSize( red::memory::AddressOf( pAddress ) );
	}

	AKRESULT wwiseMemDebugCheckForOverwrite()
	{
		//It's not really useful for us so we ignore this functionality
		return AK_Success;
	}

	// Gathered resources for tracking
	res::GatheredResourceReference g_soundbankInit( RED_CONST_RESOURCEPATH( "base\\sound\\soundbanks\\init.bnk" ), GRF_Default );
	res::GatheredResourceReference g_soundbankWindowsNoSpatialInit( RED_CONST_RESOURCEPATH( "base\\sound\\no_spatial\\init.bnk" ), GRF_Default );
	res::GatheredResourceReference g_soundbankContainer( RED_CONST_RESOURCEPATH( "base\\sound\\soundbanks\\sfx_container.bnk" ), GRF_Default );
	res::GatheredResourceReference g_wwiseContainer( RED_CONST_RESOURCEPATH( "base\\sound\\soundbanks\\sfx_container.opusinfo" ), GRF_Default );
#ifdef RED_PLATFORM_ORBIS
	//SceAudio3dPortId s_3dAudioPort;
	//AkOutputDeviceID s_spatialDeviceId; 
#endif

	Bool SoundEngine::Initialize()
	{
#ifdef RED_PLATFORM_DURANGO
		if (!Config::cvAudioNoSpatial.Get())
		{
			EnableSpatialAudio();
		}
#endif

#ifndef RED_CONFIGURATION_FINAL
		// Enable console error output.
		AK::Monitor::SetLocalOutput(AK::Monitor::ErrorLevel_Error, debug::SoundEngineOutputFunc);
#endif
		AkMemSettings memSettings;

		AK::MemoryMgr::GetDefaultSettings(memSettings);

		memSettings.pfInitForThread = nullptr;  //wwiseMemInitForThread;
		memSettings.pfTermForThread = wwiseMemTermForThread;
		memSettings.pfMalloc = wwiseMemMalloc;
		memSettings.pfMalign = wwiseMemMalign;
		memSettings.pfRealloc = wwiseMemRealloc;
		memSettings.pfFree = wwiseMemFree;
		memSettings.pfFalign = wwiseMemFalign;
		memSettings.pfTotalReservedMemorySize = wwiseMemTotalReservedMemorySize;
		memSettings.pfSizeOfMemory = wwiseMemSizeOfMemory;

		memSettings.pfDebugCheckForOverwrite = wwiseMemDebugCheckForOverwrite;

		// Initialize memory manager.
		AKRESULT res = AK::MemoryMgr::Init(&memSettings);
		if (res != AK_Success)
		{
			return false;
		}

		AkStreamMgrSettings stmSettings;
		AK::StreamMgr::GetDefaultSettings(stmSettings);

		// Initialize streaming manager.
		if (!AK::StreamMgr::Create(stmSettings))
		{
			return false;
		}

		auto commandLine = red::CommandLine::Get();

		AK::StreamMgr::SetFileLocationResolver(&g_wwiseContainerStreamingResolver);

		AkDeviceSettings defaultDeviceSettings;
		AK::StreamMgr::GetDefaultDeviceSettings(defaultDeviceSettings);
		defaultDeviceSettings.uSchedulerTypeFlags = AK_SCHEDULER_DEFERRED_LINED_UP;
		defaultDeviceSettings.bUseStreamCache = true;
		defaultDeviceSettings.uMaxConcurrentIO = 1024;
		defaultDeviceSettings.uGranularity = 1024 * 256;

#ifdef RED_PLATFORM_DURANGO
		defaultDeviceSettings.threadProperties.dwAffinityMask = 1 << 6;
		defaultDeviceSettings.threadProperties.nPriority = THREAD_PRIORITY_HIGHEST;
#elif defined( RED_PLATFORM_ORBIS )
		defaultDeviceSettings.threadProperties.dwAffinityMask = 1 << 6;
		defaultDeviceSettings.threadProperties.nPriority = SCE_KERNEL_PRIO_FIFO_HIGHEST + 5;
#endif

		Uint32 defaultStreamingDeviceMemorySize = RED_MEGA_BYTE(8);

#ifndef RED_CONFIGURATION_FINAL
		if (commandLine.HasOption("soundDesigner") || Config::cvAudioNoContainer.Get())
		{
			defaultStreamingDeviceMemorySize += RED_GIGA_BYTE(1);
		}
#endif

		defaultDeviceSettings.uIOMemorySize = defaultStreamingDeviceMemorySize;
		AkDeviceID defaultDeviceID = AK::StreamMgr::CreateDevice(defaultDeviceSettings, &g_wwiseContainerStreamingDefaultDevice);
		RED_ASSERT(defaultDeviceID == 0);

		AkDeviceSettings containerDeviceSettings;
		wwiseContainerStreamingDefaultDeviceSettings(&containerDeviceSettings);
		AkDeviceID containerDeviceID = AK::StreamMgr::CreateDevice(containerDeviceSettings, &g_wwiseContainerStreamingContainerDevice);
		RED_ASSERT(containerDeviceID == 1);

		AkInitSettings initSettings;
		AK::SoundEngine::GetDefaultInitSettings(initSettings);
		initSettings.uCommandQueueSize = WWISE_COMMAND_QUEUE_SIZE;
		initSettings.pfnAssertHook = debug::SoundEngineAssertHook;
		initSettings.pfnEventPerformHook = wwiseEventPerform;
#ifndef RED_CONFIGURATION_FINAL
		initSettings.uMonitorQueuePoolSize = RED_MEGA_BYTE(5);
#endif // ! RED_CONFIGURATION_FINAL

#ifdef RED_PLATFORM_WINPC
		initSettings.uNumSamplesPerFrame = 512;
#else
		initSettings.uNumSamplesPerFrame = 1024;
#endif

		AkPlatformInitSettings platformInitSettings;
		AK::SoundEngine::GetDefaultPlatformInitSettings(platformInitSettings);
		platformInitSettings.uNumRefillsInVoice = 2;

#ifdef RED_PLATFORM_WINPC
		platformInitSettings.bEnableAvxSupport = false;

		if (Config::cvAudioUseXaudio.Get())
		{
			platformInitSettings.eAudioAPI = AkAPI_XAudio2;
		}
		else
		{
			platformInitSettings.eAudioAPI = AkAPI_Wasapi;
		}
#endif

#ifdef RED_PLATFORM_DURANGO
		// #tbd: set to the same affinity as tasks. May be overkill.
		DWORD audioAffinityMask = 1 << 3 | 1 << 4 | 1 << 5;

		// The bank manager is causing synch CSoundFileLoaderHook::Open to be called
		// #tbd: set it to I/O cores #5 and #6 too or expand it to there as well?
		DWORD ioAffinityMask = 1 << 4 | 1 << 5;

		platformInitSettings.threadLEngine.dwAffinityMask = audioAffinityMask;
		platformInitSettings.threadLEngine.nPriority = THREAD_PRIORITY_HIGHEST;
		platformInitSettings.threadBankManager.dwAffinityMask = ioAffinityMask;
		platformInitSettings.threadBankManager.nPriority = THREAD_PRIORITY_HIGHEST;
		platformInitSettings.threadMonitor.dwAffinityMask = audioAffinityMask;
		platformInitSettings.threadOutputMgr.dwAffinityMask = audioAffinityMask;
		//Need to implement this if the memory size of io + soundbanks with xma > 64MB
		//HRESULT result = ApuCreateHeap( APU_DEFAULT_POOL_SIZE_CACHED, APU_DEFAULT_POOL_SIZE_NONCACHED );
		platformInitSettings.uMaxXMAVoices = 128;

#elif defined( RED_PLATFORM_ORBIS )
		SceKernelCpumask  audioAffinityMask = 1 << 3 | 1 << 4 | 1 << 5;
		SceKernelCpumask  ioAffinityMask = 1 << 4 | 1 << 5;
		platformInitSettings.threadLEngine.dwAffinityMask = audioAffinityMask;
		platformInitSettings.threadLEngine.nPriority = SCE_KERNEL_PRIO_FIFO_HIGHEST + 5;
		platformInitSettings.threadBankManager.dwAffinityMask = audioAffinityMask;
		platformInitSettings.threadBankManager.nPriority = SCE_KERNEL_PRIO_FIFO_HIGHEST + 20;
#endif

#if !defined( RED_CONFIGURATION_FINAL ) || defined( LOG_IN_FINAL )
		/*if( noSound )
		{
			initSettings.uDefaultPoolSize = 1024 * 128;
			platformInitSettings.uLEngineDefaultPoolSize = 1024 * 128;
		}*/
#endif
		initSettings.pfnProfileMarkerPush = wwiseProfileMarkerPush;
		initSettings.pfnProfileMarkerPop = wwiseProfileMarkerPop;

		// Initialize sound engine.
		res = AK::SoundEngine::Init(&initSettings, &platformInitSettings);
		if (res != AK_Success)
		{
			return false;
		}

		AkThreadProperties containerDecodeThreadProps;
#if defined( RED_PLATFORM_WINPC )
		containerDecodeThreadProps.nPriority = THREAD_PRIORITY_BELOW_NORMAL;
		containerDecodeThreadProps.dwAffinityMask = 0;
		containerDecodeThreadProps.uStackSize = 2 * 1024 * 1024;
#elif defined( RED_PLATFORM_DURANGO )
		containerDecodeThreadProps.nPriority = THREAD_PRIORITY_NORMAL;
		containerDecodeThreadProps.dwAffinityMask = RED_FLAG(6);
		containerDecodeThreadProps.uStackSize = 2 * 1024 * 1024;
#elif defined( RED_PLATFORM_ORBIS )
		containerDecodeThreadProps.nPriority = SCE_KERNEL_PRIO_FIFO_DEFAULT;
		containerDecodeThreadProps.dwAffinityMask = RED_FLAG(6);
		containerDecodeThreadProps.uStackSize = 2 * 1024 * 1024;
		containerDecodeThreadProps.uSchedPolicy = SCHED_FIFO;
#endif

		bool streaming_initialized = wwiseContainerStreamingInit(&containerDecodeThreadProps);
		RED_ASSERT(streaming_initialized);

		AkMusicSettings musicInit;
		AK::MusicEngine::GetDefaultInitSettings(musicInit);

		// Initialize music engine (used for interactive music).
		res = AK::MusicEngine::Init(&musicInit);
		if (res != AK_Success)
		{
			return false;
		}

		auto loadBankSynchronous = [](const res::ResourcePath& soundbankPath) -> AKRESULT
		{
			audio::ResourcePathHexStringPtr bankPathHexString;
			audio::ResourcePathHashToHexString(soundbankPath, bankPathHexString);

			AkBankID bankID;
			return AK::SoundEngine::LoadBank(bankPathHexString, bankID);
		};
#if defined( RED_PLATFORM_WEB )
		// The cooked data only ships the no-spatial init bank (audio_1_general.archive); the browser has no
		// spatial audio endpoint either, so use it like the original Windows 7 path does.
		{
			const AKRESULT cpInitBank = loadBankSynchronous(g_soundbankWindowsNoSpatialInit.GetPath());
			std::fprintf(stderr, "CP2077 audio bank probe: bank=no_spatial_init result=%d\n", int(cpInitBank));
		}
#else
#ifdef RED_PLATFORM_WINPC
		if ( IsWindows7())
		{
			loadBankSynchronous(g_soundbankWindowsNoSpatialInit.GetPath());
		}
		else
#endif
		{
			const AKRESULT cpInitBank = loadBankSynchronous(g_soundbankInit.GetPath());
			std::fprintf(stderr, "CP2077 audio bank probe: bank=init result=%d\n", int(cpInitBank));
		}
#endif
		const AKRESULT cpContainerBank = loadBankSynchronous( g_soundbankContainer.GetPath() );
		std::fprintf(stderr, "CP2077 audio bank probe: bank=container result=%d\n", int(cpContainerBank));

		AkOutputSettings outputSettings( "System" );
		res = AK::SoundEngine::AddOutput( outputSettings );
		std::fprintf(stderr, "CP2077 audio output probe: device=System result=%d\n", int(res));

		// Handling game recording on consoles, Wwise will exclude the bus with 'DVR_Bypass' as Audio Device. The same bus should use 'System' on the other platforms.
#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
		AkOutputSettings dvrOutputSettings( "DVR_Bypass" );
		AK::SoundEngine::AddOutput( dvrOutputSettings );
#endif

#ifndef RED_PLATFORM_ORBIS
		if ( !Config::cvAudioNoSpatial.Get() )
		{
			AkOutputSettings spatialOutputSettings( "Spatial" );
			AK::SoundEngine::AddOutput( spatialOutputSettings );
		}
#else
#if 0 //TODO:[CW] Use this to enable spatial e.g. on platinum headphones and ps5 back compat. NB spatial from headphones will never be active at this point, need to check in an update. Currently it doesn't work. 
		int result = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIO_3D);
		result = sceAudio3dInitialize(0);

		SceAudio3dOpenParameters sParameters;
		sceAudio3dGetDefaultOpenParameters(&sParameters);
		sParameters.uiGranularity = 512;         // Must be a multiple of 256
		sParameters.uiMaxObjects = 2048;         // Maximum number of objects this port can use
		sParameters.uiQueueDepth = 2;
		sParameters.uiNumBeds = 3;               // SDK 4.5 supports 3 beds

		SceAudio3dPortId p3dAudioPort;
		result = sceAudio3dPortOpen(SCE_USER_SERVICE_USER_ID_SYSTEM, &sParameters, &p3dAudioPort);
		
		SceAudio3dAttributeId pCapabilities[128] = { 0 };
		unsigned int pNumCapabilities = 0;
		sceAudio3dPortGetAttributesSupported(s_3dAudioPort, nullptr, &pNumCapabilities); //The System output will always be port ID 1
		sceAudio3dPortGetAttributesSupported(s_3dAudioPort, pCapabilities, &pNumCapabilities); //The System output will always be port ID 1

		Bool is3dAvailable = false;

		for (Uint32 i = 0; i < pNumCapabilities; i++)
		{
			if (pCapabilities[i] == SCE_AUDIO3D_OBJECT_ATTRIBUTE_AMBISONICS)
			{
				is3dAvailable = true;
			}
		}
		AkOutputSettings spatialOutputSettings( "Spatial_Sony" );
		AK::SoundEngine::AddOutput( spatialOutputSettings, &s_spatialDeviceId );
#endif
#endif // !RED_PLATFORM_ORBIS

#ifndef RED_PLATFORM_LINUX
		AkOutputSettings motionSettings( "Wwise_Motion" );
		res = AK::SoundEngine::AddOutput( motionSettings );
#endif		

#ifndef RED_CONFIGURATION_FINAL
		if( !commandLine.HasOption( "soundDesigner" ) && !Config::cvAudioNoContainer.Get() )
#endif
		{
			wwiseContainerStreamingLoadContainer( g_wwiseContainer.GetPath() );
		}

		AK::SoundEngine::RegisterGlobalCallback( wwiseBeginProcessingCallback, AkGlobalCallbackLocation_Begin );
		AK::SoundEngine::RegisterGlobalCallback( wwiseEndProcessingCallback, AkGlobalCallbackLocation_End );

		sourceReverbInitialize();
		AK::SoundEngine::RegisterGlobalCallback( wwiseBeginRenderFrameProfile, AkGlobalCallbackLocation_BeginRender );
		AK::SoundEngine::RegisterGlobalCallback( wwiseEndRenderFrameProfile, AkGlobalCallbackLocation_EndRender );


		m_reflectionSystem->Initialize();
		m_reverbSystem->Init();

		m_defaultListener = red::CreateUniquePtr< SoundListener >();

		// s.ortiz: this may blow up, needs proper testing
#if !defined( RED_PLATFORM_LINUX )
		// CDPVoiceBroadcastSwapBuffers ( forward declared in the factory headers )
		// must be registered as a AkGlobalCallbackLocation_BeginRender global
		// callback. this function takes care of swapping the double buffer.
		AK::SoundEngine::RegisterGlobalCallback( CDPVoiceBroadcastSwapBuffers, AkGlobalCallbackLocation_BeginRender );
#endif
		AkGameObjectID defaultListenerId = (AkGameObjectID)m_defaultListener->GetListenerId();

		AK::SoundEngine::SetDefaultListeners( &defaultListenerId, 1 );

		return true;
	}

	void SoundEngine::FinalizeInit( CName initContext, Bool loadResidentBanks )
	{
		//We want to have wwise tool connection possible alwayas in release and above, in profiling only if we have connect in final toggles on  
#if defined( USE_PROFILER ) 
#if defined( RED_CONFIGURATION_FINAL ) && !defined( LOG_IN_FINAL )
		if( Config::cvAudioConnectWwiseInFinal.Get() )
		{
#endif
			AkCommSettings commSettings;
			AK::Comm::GetDefaultInitSettings( commSettings );

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )
			sprintf_s( commSettings.szAppNetworkName, 64, initContext.AsChar() );
#endif
			// Initialize debug communications.
			AK::Comm::Init( commSettings );
#if defined( RED_CONFIGURATION_FINAL ) && !defined( LOG_IN_FINAL )
		}
#endif
#endif

		m_soundBankManager->FinalizeInit( initContext, loadResidentBanks );
	}


	SoundEngine::SoundEngine()
		: m_defaultListener( nullptr )
		, m_cachedListenerForwards( math::Vector4::ZEROS() )
		, m_cachedListenerUp( math::Vector4::ZEROS() )
		, m_cachedListenrPos( math::Vector4::ZEROS() )
	{
		m_reverbSystem = red::CreateUniquePtr< ReverbSystem >();

		m_reflectionSystem = red::CreateUniquePtr< ReflectionSystem >();
		m_soundBankManager = red::CreateUniquePtr< SoundBankManager >();
		m_musicEngine = red::CreateUniquePtr< MusicEngine >();
		m_metadataManager = red::CreateUniquePtr< MetadataManager >();
		m_soundObjectManager = red::CreateUniquePtr< SoundObjectManager >();
		m_propertiesDataStream = red::CreateUniquePtr< PropertiesDataStream >();
		m_positionDataStream = red::CreateUniquePtr< PositionDataStream >();
		m_spatialSoundLimitCounter = red::CreateUniquePtr< SpatialSoundLimitCounter >();
	}

	SoundEngine::~SoundEngine()
	{
	}

	void ForceStopAllSounds()
	{
		AK::SoundEngine::Query::AkGameObjectsList activeObjects;
		AK::SoundEngine::Query::GetActiveGameObjects( activeObjects );
		for( auto gameObjectItr = activeObjects.Begin(); gameObjectItr != activeObjects.End(); ++gameObjectItr )
		{
			AK::SoundEngine::StopAll( *gameObjectItr.pItem );
		}
	}

	void SoundEngine::BeginShutdown()
	{
		SetSoundState( RED_NAME_CONSTEXPR( "st_pause" ), RED_NAME_CONSTEXPR( "st_pause_game_running" ) );

		g_wwiseContainerStreamingResolver.ShutDown();
		AK::Monitor::SetLocalOutput();
		m_isShuttingDown = true;

		ForceStopAllSounds();

		AK::SoundEngine::RenderAudio();

#if !defined( RED_PLATFORM_ORBIS )
		//Give wwise a chance to clean up its sounds.
		red::SleepOnCurrentThread( 100 );
#endif
	}

	void SoundEngine::Shutdown()
	{
		RED_SCOPE_LOCK(m_shutdownLock);

#ifdef RED_PLATFORM_ORBIS
		//sceAudio3dPortClose(s_3dAudioPort);
		//sceSysmoduleUnloadModule(SCE_SYSMODULE_AUDIO_3D);
#endif

		sourceReverbShutdown();

		AK::SoundEngine::UnregisterGlobalCallback( wwiseContainerStreamingPreCallback, AkGlobalCallbackLocation_Begin );
		AK::SoundEngine::UnregisterGlobalCallback( wwiseContainerStreamingPostCallback, AkGlobalCallbackLocation_End );

#if defined( USE_PROFILER ) 
#if defined( RED_CONFIGURATION_FINAL ) && !defined( LOG_IN_FINAL )
		if (Config::cvAudioConnectWwiseInFinal.Get())
		{
#endif
			AK::Comm::Term();
#if defined( RED_CONFIGURATION_FINAL ) && !defined( LOG_IN_FINAL )
		}
#endif
#endif

		m_defaultListener = nullptr;

		m_soundBankManager->Shutdown();

		AK::MusicEngine::Term();

		wwiseContainerStreamingTerm();

		if( AK::SoundEngine::IsInitialized() )
		{
			AK::SoundEngine::Term();
		}

		if( AK::IAkStreamMgr::Get() )
		{
			AK::IAkStreamMgr::Get()->Destroy();
		}

		if( AK::MemoryMgr::IsInitialized() )
		{
			AK::MemoryMgr::Term();
		}

		m_reflectionSystem->Terminate();

		m_musicEngine = nullptr;

	}

	void SoundEngine::SetListenerPosition( const math::Vector4& position, const math::Vector4& front, const math::Vector4& up )
	{
		RED_ASSERT( position.IsOk() && front.IsOk() && up.IsOk(), "Transform provided to SoundEngine is invalid!" );

		AkListenerPosition listenerPositionAk;
		listenerPositionAk.SetOrientation( wwise::VectorToAk( front ), wwise::VectorToAk( up ) );
		listenerPositionAk.SetPosition( wwise::VectorToAk( position ) );

		//For now we have just the one listener but this should change in the future
		m_defaultListener->SetPosition( AudioPosition( position, front, up ) );

		m_cachedListenerForwards = front;
		m_cachedListenerUp = up;
		m_cachedListenrPos = position;

		m_reverbSystem->SetPositionForReverb( position, front );
	}

	void SoundEngine::GetListernerVectors( math::Vector4& position, math::Vector4& front, math::Vector4& up )
	{
		position = m_cachedListenrPos;
		front = m_cachedListenerForwards;
		up = m_cachedListenerUp;
	}

	void SoundEngine::RegisterVolumeIds( SoundObjectId gameObjectId, const ReverbVolumeIds& volumeIds )
	{
		RED_SCOPE_LOCK( m_reverbVolumeIdsLock );
		m_registeredVolumeIds[ gameObjectId ] = volumeIds;
	}

	Bool SoundEngine::GetVolumeIds( SoundObjectId gameObjectId, ReverbVolumeIds& outVolumeIds )
	{
		RED_SCOPE_SHARED_LOCK( m_reverbVolumeIdsLock );
		if( m_registeredVolumeIds.KeyExist( gameObjectId ) )
		{
			outVolumeIds = m_registeredVolumeIds[ gameObjectId ];
			return true;
		}
		return false;
	}

	Bool SoundEngine::HasVolumeIds( SoundObjectId gameObjectId ) const
	{
		RED_SCOPE_SHARED_LOCK( m_reverbVolumeIdsLock );
		return m_registeredVolumeIds.KeyExist( gameObjectId );
	}

	void SoundEngine::GatherActiveReverbVolumeIds( red::BitSet64< SOURCE_REVERB_VOLUME_MAX >& activeReverbVolumes ) const
	{
		RED_SCOPE_SHARED_LOCK( m_reverbVolumeIdsLock );
		activeReverbVolumes.ClearAll();
		for( auto volumeIter : m_registeredVolumeIds )
		{
			for( Uint32 i = 0; i < volumeIter.Value().m_count; i++ )
			{
				activeReverbVolumes.Set( volumeIter.Value().m_ids[ i ] );
			}
		}
	}

	ReverbSystem* SoundEngine::GetReverbSystem()
	{
		return m_reverbSystem.Get();
	}

	SoundBankManager* SoundEngine::GetSoundBankManager()
	{
		return m_soundBankManager.Get();
	}

	MusicEngine* SoundEngine::GetMusicEngine()
	{
		return m_musicEngine.Get();
	}

	MetadataManager* SoundEngine::GetMetadataManager()
	{
		return m_metadataManager.Get();
	}

	SoundObjectManager* SoundEngine::GetSoundObjectManager()
	{
		return m_soundObjectManager.Get();
	}

	PropertiesDataStream* SoundEngine::GetPropertiesDataStream()
	{
		return m_propertiesDataStream.Get();
	}

	audio::PositionDataStream* SoundEngine::GetPositionDataStream()
	{
		return m_positionDataStream.Get();
	}

	SpatialSoundLimitCounter* SoundEngine::GetSpatialLimitCounter()
	{
		return m_spatialSoundLimitCounter.Get();
	}

	math::Vector4 SoundEngine::GetListenerPosition()
	{
		return m_defaultListener->GetPosition();
	}

#if 0 //TODO[CW] IN theory this is the way to detect platinum headphones being connected. But why does it crash if I don't load the module each time? And although this code seems to work in init, in this function, pCapabilities is always 0 def RED_PLATFORM_ORBIS
	void UpdatePs4SpatialOutput()
	{
		sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIO_3D);
		SceAudio3dAttributeId pCapabilities[128] = { 0 };
		unsigned int pNumCapabilities = 0;
		sceAudio3dPortGetAttributesSupported(s_3dAudioPort, nullptr, &pNumCapabilities); //The System output will always be port ID 1
		sceAudio3dPortGetAttributesSupported(s_3dAudioPort, pCapabilities, &pNumCapabilities); //The System output will always be port ID 1

		Bool is3dAvailable = false;

		for ( Uint32  i = 0; i < pNumCapabilities; i++)
		{
			if (pCapabilities[i] == SCE_AUDIO3D_OBJECT_ATTRIBUTE_AMBISONICS)
			{
				is3dAvailable = true;
			}
		}

		static const AkUniqueID spatialBusId = SoundEngine::GetWwiseIdFromString("Spatial Bus");
		static Bool isUsingSpatial = false;

		if( is3dAvailable && !isUsingSpatial )
		{
			AK::SoundEngine::SetBusDevice(spatialBusId, s_spatialDeviceId);
			isUsingSpatial = true;
		}
		else if ( !is3dAvailable && isUsingSpatial )
		{
			AK::SoundEngine::SetBusDevice(spatialBusId, 0);
			isUsingSpatial = false;
		}

		sceSysmoduleUnloadModule(SCE_SYSMODULE_AUDIO_3D);
	}
#endif // RED_PLATFORM_ORBIS

	void SoundEngine::Tick( Float dt, const tick::Info& tickInfo )
	{
		AUDIO_PC_SCOPE_FUNC();

		GSoundEngine->GetSoundBankManager()->Update( dt );

		{
			PC_SCOPE( Update Wwise );
			AK::SoundEngine::RenderAudio();
		}
		m_reverbSystem->Tick( dt );
		{
			RED_SCOPE_LOCK( m_soundObjectAsyncUnregisterLock );
			auto temp = m_soundObjectAsyncUnregisterReadQueue;
			m_soundObjectAsyncUnregisterReadQueue = m_soundObjectAsyncUnregisterAddQueue;
			m_soundObjectAsyncUnregisterAddQueue = temp;
		}

		while( ! m_soundObjectAsyncUnregisterReadQueue->Empty() )
		{
			UnregisterSoundObject( m_soundObjectAsyncUnregisterReadQueue->PopBack() );
		}
	}

	void SoundEngine::SingleRenderAudio()
	{
		AK::SoundEngine::RenderAudio();
	}

	void SoundEngine::ClearBanks()
	{
		AK::SoundEngine::ClearBanks();
	}

	void SoundEngine::LoadMetadata()
	{
		m_metadataManager->LoadMetadata();
	}

	void SoundEngine::SetMultiplePositions( SoundObjectId objectId, const WorldTransform* transforms, Uint32 numPositions )
	{
		AkSoundPosition akPositions[ k_maxMultiplePositions ];
		for( Uint32 i = 0; i < numPositions; i++ )
		{
			const auto& currentTransform = transforms[ i ];
			RED_ASSERT( currentTransform.IsOk(), "Transform provided to SoundEngine is invalid!" );

			akPositions[ i ].SetPosition( wwise::VectorToAk( currentTransform.GetPosition().AsVector3() ) );
			akPositions[ i ].SetOrientation( wwise::VectorToAk( currentTransform.GetForward() ), wwise::VectorToAk( currentTransform.GetUp() ) );
		}

		AK::SoundEngine::SetMultiplePositions( static_cast< AkGameObjectID >( objectId ), akPositions, numPositions, AK::SoundEngine::MultiPositionType_MultiSources );
	}

	void SoundEngine::SetMultipleChannelPositions( SoundObjectId objectId, const Vector3* positions, SpeakerChannel* channels, Uint32 numPositions )
	{
		RED_ASSERT( numPositions <= k_maxMultiplePositions );
		
		//Keep this in sync with SpeakerChannel enum
		AkChannelMask speakersToChannels[ SpeakerChannel::NumSpeakers ] =
		{
			AK_SPEAKER_FRONT_LEFT,
			AK_SPEAKER_FRONT_RIGHT,
			AK_SPEAKER_SIDE_LEFT,
			AK_SPEAKER_SIDE_RIGHT
		};
		static const AkVector front = { 0.0f, 0.0f, 1.0f };
		static const AkVector top = { 0.0f, 1.0f, 0.0f };
		AkChannelEmitter akPositions[ k_maxMultiplePositions ];
		for( Uint32 i = 0; i < numPositions; i++ )
		{
			const auto& currentPosition = positions[ i ];

			akPositions[ i ].position.SetPosition( wwise::VectorToAk( currentPosition ) );
			akPositions[ i ].position.SetOrientation( front, top );

			RED_ASSERT( channels[ i ] < SpeakerChannel::NumSpeakers, "Invalid speaker channel used in SetMultipleChannelPositions" );
			akPositions[ i ].uInputChannels = speakersToChannels[ channels[ i ] ];
		}

		AK::SoundEngine::SetMultiplePositions( static_cast< AkGameObjectID >( objectId ), akPositions, numPositions, AK::SoundEngine::MultiPositionType_MultiSources );
	}

	SoundObject* SoundEngine::RegisterSoundObject( CName name )
	{
		return m_soundObjectManager->RegisterSoundObject( name );
	}

	void SoundEngine::UnregisterSoundObject( SoundObjectId soundObjectId )
	{

		m_soundObjectManager->UnregisterSoundObject( soundObjectId );

		RED_SCOPE_LOCK( m_reverbVolumeIdsLock );

		auto findResult = m_registeredVolumeIds.Find( soundObjectId );
		if( findResult != m_registeredVolumeIds.End() )
		{
			m_registeredVolumeIds.Remove( findResult );
		}
	}

	void SoundEngine::UnregisterSoundObjectAsync( SoundObjectId soundObjectId )
	{
		RED_SCOPE_LOCK( m_soundObjectAsyncUnregisterLock );
		m_soundObjectAsyncUnregisterAddQueue->PushBack( soundObjectId );
	}

	void SoundEngine::SetPosition( SoundObjectId objectId, const math::Vector4& pos, const math::Vector4& forwards, const math::Vector4& up )
	{
		RED_ASSERT( pos.IsOk() && forwards.IsOk() && up.IsOk(), "Transform provided to SoundEngine is invalid!" );

		AkSoundPosition soundPosition;

		soundPosition.SetPosition( wwise::VectorToAk( pos ) );
		soundPosition.SetOrientation( wwise::VectorToAk( forwards ), wwise::VectorToAk( up ) );

		AK::SoundEngine::SetPosition( objectId, soundPosition );
	}

	void SoundEngine::PostInfoToWwiseProfilingLog( const String& message )
	{
		if( !m_isShuttingDown )
		{
			AK::Monitor::PostString( message.AsChar(), AK::Monitor::ErrorLevel_Message );
		}
	}

	void SoundEngine::PostErrorToWwiseProfilingLog( const String& message )
	{
		if( !m_isShuttingDown )
		{
			AK::Monitor::PostString( message.AsChar(), AK::Monitor::ErrorLevel_Error );
		}
	}

	void SoundEngine::StopPlayingId( PlayingSoundId id, Int32 fadeDuration, ESoundCurveType curve /*= ESoundCurveType::Linear*/ )
	{
		AK::SoundEngine::StopPlayingID( id, static_cast< AkInt32 >( fadeDuration ), wwise::CurveTypeToAk( curve ) );
	}

#if defined( RED_PLATFORM_WEB )
	// [audio] Event-post probe: shows which game events are posted and whether Wwise knows them
	// (without the cooked sfx_container bank every gameplay event returns AK_INVALID_PLAYING_ID).
	static void CpAudioEventProbe( const char* kind, CName eventName, Uint32 wwiseId, AkPlayingID result )
	{
		static std::atomic< Uint32 > s_posted{ 0 }, s_noMetadata{ 0 }, s_invalid{ 0 }, s_ok{ 0 };
		const Uint32 n = ++s_posted;
		if( wwiseId == 0 ) ++s_noMetadata; else if( result == AK_INVALID_PLAYING_ID ) ++s_invalid; else ++s_ok;
		if( n <= 24 || ( n & ( n - 1 ) ) == 0 || ( n % 2048 ) == 0 )
		{
			std::fprintf( stderr, "CP2077 audio event probe: n=%u kind=%s event=%s wwiseId=%u playing=%u ok=%u invalid=%u noMetadata=%u\n",
				n, kind, eventName.AsChar(), wwiseId, unsigned( result ), s_ok.load(), s_invalid.load(), s_noMetadata.load() );
		}
	}
#define CP_AUDIO_EVENT_PROBE( kind, name, id, result ) CpAudioEventProbe( kind, name, Uint32( id ), result )
#else
#define CP_AUDIO_EVENT_PROBE( kind, name, id, result ) ( (void)0 )
#endif

	PlayingSoundId SoundEngine::PostEvent( CName eventName, Sound* sound )
	{
		RED_ASSERT( sound );

		if( sound->GetSoundObject() == nullptr )
		{
			AUDIO_DEBUG_ERROR( ::audio::DebugLogCategory::General, "Dead sound object for sound %s", eventName.AsChar() );
			return AK_INVALID_PLAYING_ID;
		}

		AudioHash wwiseId = m_metadataManager->GetEventWwiseId( eventName );

		if( wwiseId == AudioId::null_hash )
		{
#ifndef RED_CONFIGURATION_FINAL
			wwiseId = DebugPostMetadataEventNotFoundError( eventName, this );
			if( wwiseId == AK_INVALID_PLAYING_ID )
			{
				return AK_INVALID_PLAYING_ID;
			}
#else
			CP_AUDIO_EVENT_PROBE( "sound", eventName, 0, AK_INVALID_PLAYING_ID );
			return AK_INVALID_PLAYING_ID;
#endif
		}

		AkUInt32 callbackType = GetWwiseCallbackType( sound->GetCallbackFlag() );
		callbackType |= AK_EndOfEvent;
		callbackType |= AK_Duration;
		callbackType |= AK_SpeakerVolumeMatrix;
		AkPlayingID result = AK::SoundEngine::PostEvent( wwiseId, sound->GetSoundObject()->m_id, callbackType, WwiseCallbacks::SoundCallback, (void*)sound );
		CP_AUDIO_EVENT_PROBE( "sound", eventName, wwiseId, result );

		sound->m_playingId = result;
		return result;
	}

	PlayingSoundId SoundEngine::PostEvent( OneShotSound* sound, SoundObject* soundObject )
	{
		RED_ASSERT( sound );
		RED_ASSERT( soundObject );

		AudioHash wwiseId = m_metadataManager->GetEventWwiseId( sound->m_eventName );

		if( wwiseId == AudioId::null_hash )
		{
#ifndef RED_CONFIGURATION_FINAL
			wwiseId = DebugPostMetadataEventNotFoundError( sound->m_eventName, this );
			if( wwiseId == AK_INVALID_PLAYING_ID )
			{
				return AK_INVALID_PLAYING_ID;
			}
#else
			CP_AUDIO_EVENT_PROBE( "oneshot", sound->m_eventName, 0, AK_INVALID_PLAYING_ID );
			return AK_INVALID_PLAYING_ID;
#endif
		}

		AkUInt32 callbackType = 0;
		callbackType |= AK_EndOfEvent;
		callbackType |= AK_SpeakerVolumeMatrix;
		AkPlayingID result = AK::SoundEngine::PostEvent( wwiseId, soundObject->m_id, callbackType, WwiseCallbacks::OneShotSoundCallback, (void*)soundObject );
		CP_AUDIO_EVENT_PROBE( "oneshot", sound->m_eventName, wwiseId, result );
		return result;
	}

	void SoundEngine::ExternalEvent( CName eventName, SoundObjectId objectId, void* data, Uint32 compression, Uint32 size, float percent )
	{
		AudioHash wwiseId = m_metadataManager->GetEventWwiseId( eventName );
		if( wwiseId == AudioId::null_hash )
		{
			DebugPostMetadataEventNotFoundError( eventName, this );
			return;
		}

		AkExternalSourceInfo source;
		source.iExternalSrcCookie = wwiseId;
		source.idCodec = compression;
		source.pInMemory = data;
		source.uiMemorySize = size;

		AK::SoundEngine::PostEvent( wwiseId, (AkGameObjectID)objectId, 0, nullptr, nullptr, 1, &source );
	}

	void SoundEngine::PinFileToCache(const res::ResourcePath& resPath)
	{
		// ResourcePath's hash disguised as external source path (Wwise requires a char*). We'll retrieve the ResourcePath later (in wwiseLowLeveIO).
		audio::ResourcePathHexStringPtr encodedHash;
		if ( !audio::ResourcePathHashToHexString( resPath, encodedHash ) )
		{
			return;
		}
		
		AkFileSystemFlags flags;
		flags.uCompanyID = AKCOMPANYID_AUDIOKINETIC_EXTERNAL;
		flags.uCodecID = AKCODECID_VORBIS;
		flags.uNumBytesPrefetch = RED_KILO_BYTE(256);
		flags.pCustomParam = encodedHash;
		flags.uCustomParamSize = red::HexEncodedSize( sizeof( Uint64 ) );
		AK::IAkStreamMgr::Get()->PinFileInCache( red::CalculateHash32( encodedHash ), &flags, AK_DEFAULT_PRIORITY );
	}

	void SoundEngine::UnpinFileFromCache(const res::ResourcePath& resPath)
	{
		// ResourcePath's hash disguised as external source path (Wwise requires a char*). We'll retrieve the ResourcePath later (in wwiseLowLeveIO).
		audio::ResourcePathHexStringPtr encodedHash;
		if ( !audio::ResourcePathHashToHexString( resPath, encodedHash ) )
		{
			return;
		}	
		
		AK::IAkStreamMgr::Get()->UnpinFileInCache( red::CalculateHash32(encodedHash), AK_DEFAULT_PRIORITY );
	}

	PlayingSoundId SoundEngine::ExternalEvent( CName eventName, const res::ResourcePath& resPath, Sound* sound )
	{
		if( sound->GetSoundObject() == nullptr )
		{
			AUDIO_DEBUG_ERROR( ::audio::DebugLogCategory::General, "Dead sound object for sound %s : %s", eventName.AsChar(), resPath.ToString().AsChar() );
			return AK_INVALID_PLAYING_ID;
		}

		if( resPath.IsEmpty() )
		{
			return AK_INVALID_PLAYING_ID;
		}

		AudioHash wwiseId = m_metadataManager->GetEventWwiseId( eventName );
		if( wwiseId == AudioId::null_hash )
		{
			DebugPostMetadataEventNotFoundError( eventName, this );
			CP_AUDIO_EVENT_PROBE( "external", eventName, 0, AK_INVALID_PLAYING_ID );
			return AK_INVALID_PLAYING_ID;
		}

		// ResourcePath's hash disguised as external source path (Wwise requires a char*). We'll retrieve the ResourcePath later (in wwiseLowLeveIO).
		audio::ResourcePathHexStringPtr encodedHash;
		if ( !audio::ResourcePathHashToHexString( resPath, encodedHash ) )
		{
			return AK_INVALID_PLAYING_ID;
		}

		AkExternalSourceInfo source;
		source.iExternalSrcCookie = wwiseId;

#if !defined( RED_PLATFORM_ORBIS ) && !defined( RED_PLATFORM_LINUX ) && !defined( RED_PLATFORM_WEB ) //TODO: PS4 wants char * unlike other platforms but also can't take fileName.AsChar() ...because it's const char *?
		source.szFile = ANSI_TO_UNICODE( encodedHash );
#else
		source.szFile = encodedHash;
#endif //!RED_PLATFORM_ORBIS

		source.idCodec = AKCODECID_VORBIS;
		//source.idCodec = AKCODECID_AKOPUS;

		AkUInt32 flags = 0;

		RED_ASSERT( sound );
		AkUInt32 callbackType = GetWwiseCallbackType( sound->GetCallbackFlag() );
		callbackType |= AK_EndOfEvent;
		callbackType |= AK_Duration;
		callbackType |= AK_SpeakerVolumeMatrix;

		RED_ASSERT( source.szFile );

		AkPlayingID result = AK::SoundEngine::PostEvent( wwiseId, sound->GetSoundObject()->m_id, callbackType, WwiseCallbacks::SoundCallback, (void*)sound, 1, &source );
		CP_AUDIO_EVENT_PROBE( "external", eventName, wwiseId, result );
		sound->m_playingId = result;
		return result;
	}

	void SoundEngine::ExternalDynamicEvent( const char* dynamicEvent, const red::DynArray< CName >& dynamicArgs, SoundObjectId objectId, const String& fileName )
	{
		// Function not being used
#if 0
		if( fileName.Length() == 0 || dynamicArgs.Size() == 0 )
		{
			return;
		}

		AkPlayingID sequenceId = AK::SoundEngine::DynamicSequence::Open( objectId );

		{
			red::DynArray< const char* > charArgs = {red::PoolFrame()};
			for( CName arg : dynamicArgs )
			{
				charArgs.PushBack( arg.AsChar() );
			}
			AkUniqueID nodeId = AK::SoundEngine::DynamicDialogue::ResolveDialogueEvent( dynamicEvent, charArgs.TypedData(), charArgs.Size() );

			AK::SoundEngine::DynamicSequence::Playlist* playlist = AK::SoundEngine::DynamicSequence::LockPlaylist( sequenceId );
			AkExternalSourceInfo source;
			source.iExternalSrcCookie = nodeId;
#if !defined( RED_PLATFORM_ORBIS ) && !defined( RED_PLATFORM_LINUX ) && !defined( RED_PLATFORM_WEB ) //TODO: PS4 wants char * unlike other platforms but also can't take fileName.AsChar() ...because it's const char *?
			source.szFile = ANSI_TO_UNICODE( fileName.AsChar() );
#else
			String copyFile( fileName );
			source.szFile = copyFile.AsChar();
#endif
			source.idCodec = AKCODECID_AKOPUS;

			playlist->Enqueue( nodeId, 0, nullptr, 1, &source );

			AK::SoundEngine::DynamicSequence::LockPlaylist( sequenceId );
		}

		AK::SoundEngine::DynamicSequence::Play( sequenceId );
		AK::SoundEngine::DynamicSequence::Close( sequenceId );
#endif
	}

	void SoundEngine::SetParameter( CName parameterName, Float parameterValue, SoundObjectId objectId )
	{
		AudioHash wwiseId = m_metadataManager->GetGameParameterId( parameterName );
		if( wwiseId == AudioId::null_hash )
		{
			DebugPostMetadataNotFoundError( parameterName, this );
			return;
		}
		AK::SoundEngine::SetRTPCValue( wwiseId, parameterValue, (AkGameObjectID)objectId );
	}

	void SoundEngine::SetGlobalParameter( CName parameterName, Float parameterValue, Float duration, ESoundCurveType curveType )
	{
		AudioHash wwiseId = GMetadataManager->GetGameParameterId( parameterName );
		if( wwiseId == AudioId::null_hash )
		{
			//DebugPostMetadataNotFoundError( parameterName, this );
			return;
		}
		AK::SoundEngine::SetRTPCValue( wwiseId, parameterValue, AK_INVALID_GAME_OBJECT, static_cast< AkTimeMs >( duration * 1000 ), wwise::CurveTypeToAk( curveType ) );
	}

	void SoundEngine::SetSwitch( CName switchName, CName switchValue, SoundObjectId objectId )
	{
		AudioHash wwiseIdName = m_metadataManager->GetSwitchGroupId( switchName );
		AudioHash wwiseIdValue = m_metadataManager->GetSwitchId( switchValue );
		if( wwiseIdName == AudioId::null_hash )
		{
			DebugPostMetadataNotFoundError( switchName, this );
			return;
		}
		if( wwiseIdValue == AudioId::null_hash )
		{
			DebugPostMetadataNotFoundError( switchValue, this );
			return;
		}
		//AK::SoundEngine::SetSwitch( switchName.AsChar(), switchValue.AsChar(), static_cast< AkGameObjectID >( objectId ) );
		
		AK::SoundEngine::SetSwitch( wwiseIdName, wwiseIdValue, static_cast< AkGameObjectID >( objectId ) );
	}

	void SoundEngine::SeekOnEvent( CName eventName, SoundObjectId objectId, Float seek, PlayingSoundId playingSoundId )
	{
		AudioHash wwiseId = m_metadataManager->GetEventWwiseId( eventName );
		if( wwiseId == AudioId::null_hash )
		{
			DebugPostMetadataEventNotFoundError( eventName, this );
			return;
		}

		AK::SoundEngine::SeekOnEvent( wwiseId, static_cast< AkGameObjectID >( objectId ), static_cast< AkTimeMs >( seek * 1000 ), false, playingSoundId );
	}

	void SoundEngine::SetSoundState( const CName stateGroup, const CName stateName )
	{
		//Events metedata currently doesn't support states
		AudioHash wwiseIdName = m_metadataManager->GetStateGroupId( stateGroup );
		AudioHash wwiseIdValue = m_metadataManager->GetStateId( stateName );
		if( wwiseIdName == AudioId::null_hash )
		{
			DebugPostMetadataNotFoundError( stateGroup, this );
			return;
		}
		if( wwiseIdValue == AudioId::null_hash )
		{
			DebugPostMetadataNotFoundError( stateName, this );
			return;
		}
		AK::SoundEngine::SetState( wwiseIdName, wwiseIdValue );
	}

	Int32 SoundEngine::GetCurrentPlayTime( PlayingSoundId playingId )
	{
		Int32 resultTime = -1;

		AkUInt32 numSources = 0;
		AK::SoundEngine::GetSourcePlayPositions( playingId, nullptr, &numSources );
		if( numSources > 0 )
		{
			red::DynArray< AkSourcePosition > sourcePositions = {red::PoolFrame()};
			sourcePositions.Resize( numSources );
			AK::SoundEngine::GetSourcePlayPositions( playingId, sourcePositions.TypedData(), &numSources );
			for( AkSourcePosition pos : sourcePositions )
			{
				if( pos.msTime > resultTime )
				{
					resultTime = pos.msTime;
				}
			}
		}

		return resultTime;
	}

	Int32 GetCurrentMusicTime( PlayingSoundId playingId )
	{
		AkSegmentInfo musicInfo;
		AK::MusicEngine::GetPlayingSegmentInfo( playingId, musicInfo );
		return musicInfo.iCurrentPosition;
	}

	Bool SoundEngine::IsSoundbankLoaded( CName name )
	{
		return GetSoundBankManager()->IsSoundbankLoaded( name );
	}

	namespace wwise
	{
		math::Vector4 AkToVector( const AkVector& akVector )
		{
			return math::Vector4( akVector.X, akVector.Z, akVector.Y );
		}

		AkVector VectorToAk( const math::Vector4& vector )
		{
			AkVector akVector = {vector.X, vector.Z, vector.Y};
			return akVector;
		}

		void LoadBankCallback( AkUInt32 in_bankID, const void* in_plnMemoryBankPtr, AKRESULT in_eLoadResult, void* in_pCookie )
		{
			CName* name = static_cast< CName* >( in_pCookie );
			if( name != nullptr )
			{
				GSoundEngine->GetSoundBankManager()->OnSoundbankLoaded( *name );
			}
			else
			{
				AUDIO_DEBUG_ERROR( ::audio::DebugLogCategory::WWise, "Could not parse name as cookie" );
			}
		}

		void UnloadBankCallback( AkUInt32 in_bankID, const void* in_pInMemoryBankPtr, AKRESULT in_eLoadResult, void* in_pCookie )
		{
			CName* name = static_cast< CName* >( in_pCookie );
			if( name != nullptr )
			{
				GSoundEngine->GetSoundBankManager()->OnSoundbankUnloaded( *name );
			}
			else
			{
				AUDIO_DEBUG_ERROR( ::audio::DebugLogCategory::WWise, "Could not parse name as cookie." );
			}
		}

		Bool LoadBank( const res::ResourcePath& soundbankPath, CName& bankName, AkBankID& outId )
		{
			audio::ResourcePathHexStringPtr bankPathHexString;
			audio::ResourcePathHashToHexString( soundbankPath, bankPathHexString );

			AKRESULT result = AK::SoundEngine::LoadBank( bankPathHexString, LoadBankCallback, &bankName, outId );
			return result == AK_Success;
		}

		Bool UnloadBank( const res::ResourcePath& soundbankPath, CName& bankName )
		{
			audio::ResourcePathHexStringPtr bankPathHexString;
			audio::ResourcePathHashToHexString( soundbankPath, bankPathHexString );

			AKRESULT result = AK::SoundEngine::UnloadBank( bankPathHexString, nullptr, UnloadBankCallback, &bankName );
			return result == AK_Success;
		}

		// Adapted from AkInterpolation::InterpolateNoCheck in wwise/SDK/source/SoundEngine/AkAudioLib/Common/AkInterpolation.h
		// NB: this is still WWISE code, do not include in non-wwise projects
		// Copyright remains with AudioKinetic
		Float Interpolate( Float timeRatio, Float initialVal, Float targetVal, ESoundCurveType curveType )
		{
			switch( curveType )
			{
			case ESoundCurveType::Linear:
				return timeRatio * ( targetVal - initialVal ) + initialVal;
			case ESoundCurveType::Exp1:
				//return pow( timeRatio, 1.41f ) * ( targetVal - initialVal ) + initialVal;
				return ( timeRatio + 1 ) * timeRatio * 0.5f * ( targetVal - initialVal ) + initialVal;
			case ESoundCurveType::ReciprocalOfSineCurve:
				//return MCos( RED_PI_HALF * timeRatio ) * ( initialVal - targetVal ) + targetVal;
				// Using a 7th order MacLaurin series of cos with minimax economization over range [0,PI/2]. Error <= 6.7e-6
				{
					Float x = RED_PI_HALF * timeRatio;
					Float x2 = x * x;
					return ( 0.999993295282167f + x2 * ( -4.99912439712246e-1f + x2 * ( 4.14877480454292e-2f + x2 * -1.27120948569655e-3f ) ) )
							   * ( initialVal - targetVal )
						   + targetVal;
				}
			case ESoundCurveType::Exp3:
				return ( timeRatio * timeRatio * timeRatio * ( targetVal - initialVal ) ) + initialVal;
			case ESoundCurveType::Log1:
				//return pow( 1 - timeRatio, 1.41f ) * ( initialVal - targetVal ) + targetVal;
				return ( ( timeRatio - 3 ) * timeRatio ) * 0.5f * ( initialVal - targetVal ) + initialVal;
			case ESoundCurveType::Sine:
				//return MSin( RED_PI_HALF * timeRatio ) * ( targetVal - initialVal ) + initialVal;
				// Using a 7th order MacLaurin series of sin with minimax economization over range [0,PI/2]. Error <= 5.9e-7
				/// REVIEW: Could reduce order and obtain precision ~6e-5.
				{
					Float x = RED_PI_HALF * timeRatio;
					Float x2 = x * x;
					return ( x * ( 0.999996615908002f + x2 * ( -1.66648283818951e-1f + x2 * ( 8.30632522715989e-3f + x2 * -1.83636539769468e-4f ) ) ) )
							   * ( targetVal - initialVal )
						   + initialVal;
				}
			case ESoundCurveType::Log3:
			{
				Float fFlippedTimeRatio = 1 - timeRatio;
				return fFlippedTimeRatio * fFlippedTimeRatio * fFlippedTimeRatio * ( initialVal - targetVal ) + targetVal;
			}
			case ESoundCurveType::SCurve:
			{
				// return ( targetVal - initialVal ) * ( 1 - MCos( RED_PI * timeRatio ) ) * 0.5f + initialVal;
				// Using a 7th order MacLaurin series of (1-cos)/2 with minimax economization over range [0,PI]. Error <= 7e-4
				AkReal32 x = RED_PI * timeRatio;
				AkReal32 x2 = x * x;
				return ( 6.967021361369841e-4f + x2 * ( 2.476747878488185e-1f + x2 * ( -1.961384016191180e-2f + x2 * 4.848339972837020e-4f ) ) )
						   * ( targetVal - initialVal )
					   + initialVal;
			}
			case ESoundCurveType::InversedSCurve:
			{
				/*
				if(timeRatio <= 0.5f)
				return ( ( targetVal - initialVal ) * MSin( RED_PI * timeRatio ) * 0.5f ) + initialVal;
				else
				return ( targetVal - initialVal ) * ( 1 - MSin( RED_PI * timeRatio ) * 0.5f ) + initialVal;
				*/
				// Using a 7th order MacLaurin series of MSin(x)/2 with minimax economization over range [0,PI/2]. Error <= 5.9e-7
				if( timeRatio <= 0.5f )
				{
					AkReal32 x = RED_PI * timeRatio;
					AkReal32 x2 = x * x;
					return ( x * ( 0.499998307954001f + x2 * ( -8.33241419094755e-2f + x2 * ( 4.153162613579945e-3f + x2 * -9.181826988473399e-5f ) ) ) )
							   * ( targetVal - initialVal )
						   + initialVal;
				}
				else
				{
					AkReal32 x = RED_PI - RED_PI * timeRatio;
					AkReal32 x2 = x * x;
					return ( 1.f - x * ( 0.499998307954001f + x2 * ( -8.33241419094755e-2f + x2 * ( 4.153162613579945e-3f + x2 * -9.181826988473399e-5f ) ) ) )
							   * ( targetVal - initialVal )
						   + initialVal;
				}
			}
			default:
				ASSERT( !"Fade curve not supported" );
			}
			return 0;
		}

		AkCurveInterpolation CurveTypeToAk( ESoundCurveType curve )
		{
			switch( curve )
			{
			case audio::ESoundCurveType::Log3:
				return AkCurveInterpolation_Log3;
				break;
			case audio::ESoundCurveType::Sine:
				return AkCurveInterpolation_Sine;
				break;
			case audio::ESoundCurveType::Log1:
				return AkCurveInterpolation_Log1;
				break;
			case audio::ESoundCurveType::InversedSCurve:
				return AkCurveInterpolation_InvSCurve;
				break;
			case audio::ESoundCurveType::Linear:
				return AkCurveInterpolation_Linear;
				break;
			case audio::ESoundCurveType::SCurve:
				return AkCurveInterpolation_SCurve;
				break;
			case audio::ESoundCurveType::Exp1:
				return AkCurveInterpolation_Exp1;
				break;
			case audio::ESoundCurveType::ReciprocalOfSineCurve:
				return AkCurveInterpolation_SineRecip;
				break;
			case audio::ESoundCurveType::Exp3:
				return AkCurveInterpolation_Exp3;
				break;
			default:
				return AkCurveInterpolation_Linear;
				break;
			}

			return AkCurveInterpolation_Linear;
		}

		void SetDefaultListeners( AkGameObjectID objectId )
		{
			//NB: takes an array in case we want to support multible default listeners
			AK::SoundEngine::SetDefaultListeners( &objectId, 1 );
		}

		Uint32 GetAkCallbackType( ESoundCallbackFlag flags )
		{
			Uint32 callbackType = 0;

			if( flags & ESoundCallbackFlag::MusicSync )
			{
				callbackType |= AK_MusicSyncBeat | AK_MusicSyncBar | AK_MusicSyncGrid | AK_MusicSyncUserCue | AK_EnableGetMusicPlayPosition | AK_EnableGetSourcePlayPosition;
			}
			if( flags & ESoundCallbackFlag::MusicEvent )
			{
				callbackType |= AK_MusicSyncUserCue | AK_MusicSyncAll | AK_SpeakerVolumeMatrix | AK_EnableGetMusicPlayPosition | AK_EnableGetSourcePlayPosition;
			}
			if( flags & ( ESoundCallbackFlag::WeaponReport | ESoundCallbackFlag::Echo ) )
			{
				callbackType |= AK_SpeakerVolumeMatrix;
			}
			if( flags & ESoundCallbackFlag::EnablePlayPosition )
			{
				callbackType |= AK_EnableGetSourcePlayPosition;
			}

			return callbackType;
		}

	} //namespace wwise

	Uint32 SoundEngine::GetWwiseCallbackType( ESoundCallbackFlag flags )
	{
		return wwise::GetAkCallbackType( flags );
	}

	Uint32 SoundEngine::GetWwiseIdFromString( String s )
	{
		return AK::SoundEngine::GetIDFromString( s.AsChar() );
	}

	SoundListener* SoundEngine::GetDefaultSoundListener()
	{
		return m_defaultListener.Get();
	}

	void SoundEngine::ReloadMetadata( job::Builder& builder )
	{
#ifndef RED_CONFIGURATION_FINAL
		m_metadataManager->ReloadMetadata( builder );
#endif
	}

	void SoundEngine::ReloadMetadataFromEditorPipe( job::Builder& builder, THandle<AudioMetadata> newMetadataContent )
	{
#ifndef RED_CONFIGURATION_FINAL
		m_metadataManager->ReloadMetadataFromEditorPipe( builder, std::move( newMetadataContent ) );
#endif
	}

	void SoundEngine::CacheEngineTime( const EngineTime& engineTime )
	{
		m_engineTime = engineTime;
	}

	EngineTime SoundEngine::GetCachedEngineTime()
	{
		return m_engineTime;
	}

	Bool SoundEngine::ShouldSoundsBeLimitedDueToDrivingStreaming() const
	{
		return m_shouldSoundsBeLimitedDueToDrivingStreaming;
	}

	void SoundEngine::SetShouldSoundsBeLimitedDueToDrivingStreaming( Bool value )
	{
		m_shouldSoundsBeLimitedDueToDrivingStreaming = value;
	}

}
