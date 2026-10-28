/**
 * Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
 */
#include "build.h"
#include "renderBinkVideo.h"
#if defined(__EMSCRIPTEN__)
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <cstdio>
#endif

#if defined( USE_BINK_VIDEO )

#include "renderViewport.h"
#include "renderShader.h"
#include "renderStateManager.h"
#include "renderSurfaceCache.h"
#include "renderSurfaceGenerator_Video.h"
#include "renderVideo.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../videoPlayer/include/videoPlayerBink.h"
#include "../../renderData/include/renderCommandHandler.h"
#include "../../renderData/include/renderer.h"
#include "binkIO.h"
#include "../../redContainers/include/string/stringUtils.h"
#include "../../redReflection/include/resourceDepot.h"
#include "../../redReflection/include/serializationAsyncSource.h"
#include "../../resource/include/serializationAsyncSourceImpl.h"

#include "../../redReflection/include/resourceMetricsBank.h"


namespace Config
{
	extern TConfigVar<Int32>	cvMaxFrameUpload;
}

#if defined(__EMSCRIPTEN__)
extern "C" Int32 CpWebBinkTryGetSummary(HBINK, BINKSUMMARY*, Uint32*);
namespace {
// A snapshot contains only values returned by the original SDK. Holding this
// mutex never runs the decoder or waits for a browser/guest CPU request.
std::mutex cpWebVideoSnapshotsMutex;
std::unordered_map<const RenderBinkVideo*, VideoSummary> cpWebVideoSnapshots;
std::atomic<unsigned> cpWebVideoPendingReports{0};
}
#endif
static constexpr size_t c_texMemAlign = 16;
static constexpr Uint32 c_audioSyncMaxFrameSkip = 3;
static constexpr Uint32 c_ioBufferSize = RED_MEGA_BYTE(2);

const Float RenderBinkVideo::REALTIME_MAX_BUFFER_FILL_PERCENTAGE = .8f;
const Float RenderBinkVideo::REALTIME_MIN_BUFFER_FILL_PERCENTAGE = .2f;

const Bool c_debugProfileVideo = false;

class RenderBinkVideoProfilerCallback final : public bink::profiler::IBinkIOProfilerCallback
{
	RED_USE_MEMORY_POOL(red::PoolDebug);

public:
	RenderBinkVideoProfilerCallback(const res::ResourcePath& videoPath)
		: m_videoPath(videoPath)
	{
	}

	virtual ~RenderBinkVideoProfilerCallback()
	{
	}

	void OnSuspendBinkLoad(const BINKREALTIME& realtime)
	{
		const Uint64 timestamp = red::ProfileTimer::GetTicks();

		GenericEvent evt;
		evt.realtimeEvent = { realtime, RealtimeEventType::SuspendBinkLoad };
		evt.timestamp = timestamp;
		evt.durationTicks = 0;
		evt.type = EventType::RealtimeEvent;

		RED_SCOPE_LOCK(m_bgIOLock);
		m_events.PushBack(evt);
	}

	void OnResumeBinkLoad(const BINKREALTIME& realtime)
	{
		const Uint64 timestamp = red::ProfileTimer::GetTicks();

		GenericEvent evt;
		evt.realtimeEvent = { realtime, RealtimeEventType::ResumeBinkLoad };
		evt.timestamp = timestamp;
		evt.durationTicks = 0;
		evt.type = EventType::RealtimeEvent;

		RED_SCOPE_LOCK(m_bgIOLock);
		m_events.PushBack(evt);
	}

	void FlushEventsToLog()
	{
		RED_SCOPE_LOCK(m_bgIOLock);

		std::sort(m_events.Begin(), m_events.End(), [](const GenericEvent& a, const GenericEvent& b)
		{
			if (a.timestamp != b.timestamp)
			{
				return a.timestamp < b.timestamp;
			}

			const Uint32 weightA = (a.type == EventType::RealtimeEvent ? 1 : 0);
			const Uint32 weightB = (b.type == EventType::RealtimeEvent ? 1 : 0);

			return weightA < weightB;
		});

		Float maxWaitMsec = 0.f;
		Float maxDurationMsec = 0.f;

		RED_LOG_ERROR("Video: %hs (%u events)", m_videoPath.ToDebugString(), m_events.Size());

		Uint32 readEventIndex = 0;
		Uint32 realtimeEventIndex = 0;

		for (Uint32 i : m_events.Indices())
		{
			const auto& evt = m_events[i];

			if (i > 0)
			{
				const auto& prevEvt = m_events[i - 1];

				if (evt.type == EventType::RealtimeEvent && prevEvt.type == EventType::RealtimeEvent)
				{
					if (evt.realtimeEvent.type == prevEvt.realtimeEvent.type)
					{
						const auto& realtime = evt.realtimeEvent.realtime;
						const auto& prevRealtime = prevEvt.realtimeEvent.realtime;

						if (realtime.ReadBufferSize == prevRealtime.ReadBufferSize && realtime.ReadBufferUsed == prevRealtime.ReadBufferUsed)
						{
							// Skip spamming suspends... but should add up the wait times!!
							//continue;
						}
					}
				}

				Uint64 startTick = evt.timestamp;
				Uint64 startWaitTick = prevEvt.timestamp + prevEvt.durationTicks;
				Uint64 endWaitTick = evt.timestamp;
				const Float idleMsec = red::ProfileTimer::GetDeltaUsec(endWaitTick, startWaitTick) / 1000.f;
				RED_LOG_ERROR("\t\t [idle %.2f ms]", idleMsec);

				// 				if (waitMsec > maxWaitMsec)
				// 				{
				// 					maxWaitMsec = waitMsec;
				// 				}
			}

			if (evt.type == EventType::RealtimeEvent)
			{
				const auto& realtimeEvent = evt.realtimeEvent;
				const char* subtypeText = (realtimeEvent.type == RealtimeEventType::SuspendBinkLoad ? "[-]Suspend" : "[+]Resume");
				const Uint64 bufferUsed = realtimeEvent.realtime.ReadBufferUsed;
				const Uint64 bufferSize = realtimeEvent.realtime.ReadBufferSize;
				const Float percentUsed = bufferSize > 0.f ? ( bufferUsed * 100.f / bufferSize ) : -1.f;
				RED_LOG_ERROR("[%u] %hs, fill=%.2f%% (%hs/%hs)", i, subtypeText, percentUsed, red::FormatByteNumber(bufferUsed).AsChar(), red::FormatByteNumber(bufferSize).AsChar());
			}
			else if (evt.type == EventType::ReadEvent)
			{
				// assumably offset growing... could flush ourselves when we see the offset wrapped around = looped or have a Bool or sth

				Uint64 startTick = evt.timestamp;
				const Float durationMsec = red::ProfileTimer::GetUsec(evt.durationTicks) / 1000.f;
				RED_LOG_ERROR("\t[%u]Read %hs in %.2f ms", i, red::FormatByteNumber(evt.readEvent.bytesRead).AsChar(), durationMsec);

				// 			if (durationMsec > maxDurationMsec)
				// 			{
				// 				maxDurationMsec = durationMsec;
				// 			}
			}
			else
			{
				RED_FATAL("UNknown event");
			}

			//RED_LOG_ERROR("Max wait=%.2f ms, max duration=%.2f ms", maxWaitMsec, maxDurationMsec);

			//RED_LOG_ERROR()

			// LOCK in case bg I/O adds stuff
			// if suspending load... shouldn't START new I/O although maybe finish... and maybe not 100% synced to BG thread...
		}

		m_events.Clear();
	}


protected:
	virtual void OnOpenFile() override {}
	
	virtual void OnBeginReadFile() override
	{
		RED_SCOPE_LOCK(m_bgIOLock);

		// Under lock for now since want a consistent view if the events are later reset
		m_beginReadTick = red::ProfileTimer::GetTicks();
	}
	
	virtual void OnFinishReadFile(Uint64 bytesRead) override
	{

		RED_SCOPE_LOCK(m_bgIOLock);

		const Uint64 finishReadTick = red::ProfileTimer::GetTicks();
		const Uint64 durationTicks = finishReadTick - m_beginReadTick;
		GenericEvent evt;
		evt.readEvent = { bytesRead };
		evt.timestamp = m_beginReadTick;
		evt.durationTicks = durationTicks;
		evt.type = EventType::ReadEvent;
		m_events.PushBack(evt);
	}

	virtual void OnSeekFile() override {}

private:
	// The time between read events also helpful... but compare to when balanceing buffer maybe later corellated by timestamp
	struct ReadEvent
	{
		Uint64 bytesRead;
	};

	enum class RealtimeEventType : Uint8
	{
		SuspendBinkLoad,
		ResumeBinkLoad,
	};

 	struct RealtimeEvent
 	{
		BINKREALTIME realtime;
		RealtimeEventType type;
 	};

	enum class EventType : Uint8
	{
		ReadEvent,
		RealtimeEvent,
	};

	struct GenericEvent
	{
		union
		{
			RealtimeEvent realtimeEvent;
			ReadEvent readEvent;
		};
		Uint64 timestamp;
		Uint64 durationTicks;
		EventType type;
	};

	Uint64 m_beginReadTick{ 0 };

	red::DynArray<GenericEvent> m_events{ red::PoolDebug() };

	Uint64 m_previousReadEventTimestamp{ 0 };
	res::ResourcePath m_videoPath;
	//Uint64 m_videoInstanceID;
	mutable red::SpinLock m_bgIOLock; // maybe not really necessary, depends if bink will unexpectedly do I/O when flushing events to log
};


RED_INLINE Uint32 GetFrameUploadSize( const BINKFRAMEBUFFERS& binkBuffers, Uint32 frameIndex )
{
	const BINKFRAMEPLANESET& srcFrame = binkBuffers.Frames[frameIndex];
	const Uint32 uploadSize =
		srcFrame.YPlane.BufferPitch*binkBuffers.YABufferHeight +
		srcFrame.cRPlane.BufferPitch*binkBuffers.cRcBBufferHeight +
		srcFrame.cBPlane.BufferPitch*binkBuffers.cRcBBufferHeight +
		(srcFrame.APlane.Allocate ? srcFrame.APlane.BufferPitch*binkBuffers.YABufferHeight : 0);
	return uploadSize;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RenderBinkVideo::RenderBinkVideo( const VideoParamsBink& videoParams )
	: m_videoPath( videoParams.videoPath )
	, m_threadCount( 0 )
	, m_audioEventId( c_invalidAudioEventId )
	, m_numberOfFramesToSkip( 0 )
	, m_loopVideo( videoParams.loopVideo )
	, m_forceVideoFrameRate( videoParams.forceVideoFrameRate )
	, m_passiveMode( false )
	, m_passiveUpdateEnabled( false )
	, m_bufferVideo( videoParams.bufferVideo )
	, m_paused( false )
	, m_wasPaused( false )
	, m_finishedVideo( false )
	, m_dirtyTextures( false )
	, m_decodingFrame( false )
	, m_binkLoadSuspended( false )
	, m_gameLoadSuspended( false )
	, m_firstDecodedFrame( false )
	, m_needWaitForExternal( false )
	, m_waitsForExternal( false )
	, m_closeRequested( false )
	, m_wasLooped( false )
	, m_asyncOperation( AsyncOperation::None )
	, m_ioPriority(videoParams.ioPriority)
{
}

RenderBinkVideo::~RenderBinkVideo()
{
	PC_SCOPE( RenderBink_Destructor );
#if defined(__EMSCRIPTEN__)
    { std::lock_guard<std::mutex> guard(cpWebVideoSnapshotsMutex);cpWebVideoSnapshots.erase(this); }
#endif

	if ( m_decodingFrame )
	{
		BinkDoFrameAsyncWait( m_bink, -1 );
	}

	for ( Int32 i = 0; i < BINKMAXFRAMEBUFFERS; ++i )
	{
		MappedBinkFrameBuffer& mappedBinkBuffer = m_mappedBinkBuffer[ i ];
		mappedBinkBuffer.m_frameBuffers.Reset();
		red::Memzero( &mappedBinkBuffer, sizeof( mappedBinkBuffer ) );
	}

	if (m_profilerCallback)
	{
		m_profilerCallback->FlushEventsToLog();
	}

	if ( m_bink )
	{
		job::Builder builder{{job::Priority::Latent, job::Affinity::ConsoleCore7}};

		builder.DispatchJob( "RenderBink/Close", [ bink = m_bink, videoPath = m_videoPath ] ( const job::RunContext& )
		{
			RED_LOG_INFO( "Video: Closing bink video file '%s'", videoPath.ToDebugString() );

			BinkClose( bink );
		} );
		m_bink = nullptr;
	}

	GpuApi::SafeRelease( m_drawTexture.m_yTexture );
	GpuApi::SafeRelease( m_drawTexture.m_cRTexture );
	GpuApi::SafeRelease( m_drawTexture.m_cBTexture );
	GpuApi::SafeRelease( m_drawTexture.m_aTexture );
}

static void AsyncReadCompleteCallback( serialization::AsyncSourceReadBuffer readBuffer, red::UniqueBuffer, void* userData, red::EAsyncResult result )
{
	RenderBinkVideo::PrefetchContext* prefetchCtx = ( RenderBinkVideo::PrefetchContext* )userData;
	IRenderVideo::InitContext& ctx = prefetchCtx->initContext;
	red::FixedSizeFunction< void( Bool ) > initCallback = ctx.initCallback;

	Bool initResult = false;
	if ( result == red::EAsyncResult::eAsyncResult_Success )
	{
		initResult = ctx.renderVideo->Init( ctx.canvasName, ctx.passiveMode, ctx.audioEventId, ctx.loopCallback );
	}
	initCallback( initResult );
}

void RenderBinkVideo::Init( const InitContext& ctx )
{
	m_asyncOperation.SetValue( AsyncOperation::Init );
	m_bufferVideo = ctx.bufferVideo;

	if ( m_bufferVideo )
	{
		RenderBinkVideo* renderVideo = static_cast< RenderBinkVideo* >( ctx.renderVideo.Get() );

		auto asyncSource = GetResourceDepot()->CreateResourceAsyncSource( m_videoPath );
		if ( !asyncSource )
		{
			RED_LOG_ERROR( "Video: Failed to open bink video file '%s'", m_videoPath.ToDebugString() );
			return ctx.initCallback( false );
		}

		auto readSizes = asyncSource->GetReadSizes( 0, asyncSource->GetSizeOnDisk() );

		m_prefetchMemory = io::ShareableIOMemory( red::CreateUniqueBuffer< rend::PoolBink >( readSizes.bufferSize, 16 ) );
		m_prefetchContext = red::CreateUniquePtr< PrefetchContext >( );
		m_prefetchContext->initContext = ctx;

		serialization::IAsyncSource::CallbackContext callbackContext;
		callbackContext.userData = m_prefetchContext.Get();
		callbackContext.callback = &AsyncReadCompleteCallback;

		serialization::IAsyncSource::ReadAsyncParams params;
		params.requestSource = io::RequestSource::VideoSystem_PreReadInline;
		params.offset = 0;
		params.size = asyncSource->GetSizeOnDisk();
		params.priority = io::eAsyncPriority_FULLSCREENVIDEO;
		params.allocDeserializationMemoryForDecompressorIfCompressed = false; // not compressed with kraken (?)

		asyncSource->ReadAsync( params, serialization::AsyncSourceReadBuffer( m_prefetchMemory, 0, readSizes ), callbackContext );
	}
	else
	{
		job::Builder builder{ { job::Priority::Latent, job::Affinity::ConsoleCore7 } };
		builder.DispatchJob( "RenderBink/Init", [ ctx ] ( const job::RunContext& jobCtx )
		{
			Bool result = ctx.renderVideo->Init( ctx.canvasName, ctx.passiveMode, ctx.audioEventId, ctx.loopCallback );
			ctx.initCallback( result );
		} );
	}
}

Bool RenderBinkVideo::Init( const CName& canvasName, Bool passiveMode, Int32 audioEventId, const red::FixedSizeFunction< void() >& loopCallback )
{
	PC_SCOPE( RenderBink_Init );

	red::UniquePtr< PrefetchContext > prefetchCtx{ std::move( m_prefetchContext ) };

	m_canvasName = canvasName;

	m_threads[0] = 4;
	m_threads[1] = 5;
	m_threadCount = 2;

	if ( audioEventId != c_invalidAudioEventId )
	{
		m_audioEventId = audioEventId;
	}

	m_loopCallback = loopCallback;

	if (c_debugProfileVideo)
	{
		m_profilerCallback = red::CreateUniquePtr< RenderBinkVideoProfilerCallback >(m_videoPath);
	}

	bink::BinkOpenFileParams params;
	params.videoPath = m_videoPath;
	params.profilerCallback = m_profilerCallback.Get();
	params.ioPriority = m_ioPriority;


	BINK_OPEN_OPTIONS opts;
	red::Memzero(&opts, sizeof(opts));

	U32 extraBinkFlags = 0;
	
	Uint32 ioBufferSize = c_ioBufferSize;
	// #tbd: variable buffer size, better if could be specified in seconds and then calculated like Bink does internally
	red::SharedPtr< serialization::IAsyncSource > asyncSource;
	if ( m_prefetchMemory.GetSize() )
	{
		asyncSource = serialization::CreateMemoryAsyncSource( m_prefetchMemory.ExtractAsNonShared() );

		// We have already loaded entire bink file into memory, so no need for a big buffer
		ioBufferSize = RED_KILO_BYTE( 512u );
	}
	else
	{
		asyncSource = GetResourceDepot()->CreateResourceAsyncSource( m_videoPath );
	}

	if (asyncSource)
	{
		params.optionalSharedSource = asyncSource;

		const Uint32 sizeOnDisk = asyncSource->GetSizeOnDisk();
		opts.IOBufferSize = Min<Uint32>(sizeOnDisk, ioBufferSize);
		extraBinkFlags = BINKIOSIZE;
	}

	// Because still does IO to get the header and we don't want to stall the job/main thread for a long time
	// Need to eventually have the header already loaded.
	serialization::TEMPHACK_SetBinkVideoOpen_ThreadLocal(true);

 	m_bink = BinkOpenWithOptions( static_cast< const char* >( static_cast< const void* >( &params ) ), &opts,
 		BINKNOFILLIOBUF | BINKALPHA | BINKNOFRAMEBUFFERS | BINKDONTCLOSETHREADS | extraBinkFlags );
	
	serialization::TEMPHACK_SetBinkVideoOpen_ThreadLocal(false);

	if ( !m_bink )
	{
		RED_LOG_ERROR( "Video: Failed to open bink video file '%s'", m_videoPath.ToDebugString() );
		return false;
	}

	RED_LOG_INFO( "Video: Opened bink video file '%s' (resolution=%luX%lu, total time=%.2fs, frames=%lu, fps=%.2f)",
		m_videoPath.ToDebugString(),
		m_bink->Width,
		m_bink->Height,
		m_bink->Frames / ( (float)m_bink->FrameRate / m_bink->FrameRateDiv ), 
		m_bink->Frames, (float)m_bink->FrameRate / m_bink->FrameRateDiv );

	// turn off sounds for all videos
	BinkSetSoundOnOff( m_bink, 0 );

	/*
	// todo: check if it is required to do on all platforms
	// 7.1 route the sound tracks to the correct speaker
	Uint32 bins[ 2 ];
	bins[ 0 ] = 0;  // 0 is front left
	bins[ 1 ] = 1;  // 1 is front right
	BinkSetSpeakerVolumes( m_bink, 0, bins, nullptr, 2 );
	bins[ 0 ] = 2; // 2 is center
	BinkSetSpeakerVolumes( m_bink, 1, bins, nullptr, 1 );
	bins[ 0 ] = 3; // 3 is subwoofer
	BinkSetSpeakerVolumes( m_bink, 2, bins, nullptr, 1 );
	bins[ 0 ] = 4;  // 4 is back left
	bins[ 1 ] = 5;  // 5 is back right
	BinkSetSpeakerVolumes( m_bink, 3, bins, nullptr, 2 );
	bins[ 0 ] = 6; // 6 is surround left
	bins[ 1 ] = 7; // 7 is surround right
	BinkSetSpeakerVolumes( m_bink, 4, bins, nullptr, 2 );
	*/

	if ( !CreateBinkCpuResources() )
	{
		RED_LOG_ERROR( "Video: Failed to create cpu resources" );
		return false;
	}
	if ( !CreateBinkGpuResources() )
	{
		RED_LOG_ERROR( "Video: Failed to create gpu resources" );
		return false;
	}


#ifdef RED_LOGGING_ENABLED
	{
		Bool hasBigFrame = false;
		Uint32 bigFrameSize = 0;
		const Uint32 maxFrameUpload = Config::cvMaxFrameUpload.Get();
		for ( Int32 i = 0; i < m_binkBuffers.TotalFrames; ++i )
		{
			const Uint32 uploadSize = GetFrameUploadSize( m_binkBuffers, m_binkBuffers.FrameNum );
			if ( uploadSize > maxFrameUpload )
			{
				bigFrameSize = uploadSize;
				hasBigFrame = true;
				break;
			}
		}
		RED_WARNING( !hasBigFrame, "Video '%s' frame size (%u) is too big for the per-frame upload limit (%u).", m_videoPath.ToDebugString(), bigFrameSize, maxFrameUpload );
		RED_DATA_ERROR_BEGIN( red::LoggerCategory_Rendering, !hasBigFrame, "Video '%s' frame size (%u) is too big for the per-frame upload limit (%u)", m_videoPath.ToDebugString(), bigFrameSize, maxFrameUpload );
		RED_DATA_ERROR_ACTION_SHOW_ASSET( m_videoPath.ToDebugString() );
		RED_DATA_ERROR_END();
	}
#endif
	
#ifdef USE_RESOURCE_METRICS_BANK
	{
		PC_SCOPE( RenderBink_Init_UploadTextures );

		auto* resourceMetricsBank = GResourceLoader->GetResourceMetricsBank();
		res::SingleResourceMetrics videoMemoryMetrics{};
		videoMemoryMetrics.path = m_videoPath;

		// TODO figure out what sizes to use here exactly
		videoMemoryMetrics.cpuBytesUsed = 0;

		Uint32 usedGPUMemory = GpuApi::GetTextureUsedVideoMemory(m_drawTexture.m_yTexture)
			+ GpuApi::GetTextureUsedVideoMemory(m_drawTexture.m_cRTexture)
			+ GpuApi::GetTextureUsedVideoMemory(m_drawTexture.m_cBTexture)
			+ GpuApi::GetTextureUsedVideoMemory(m_drawTexture.m_aTexture);
		videoMemoryMetrics.gpuBytesUsed = usedGPUMemory;

		resourceMetricsBank->OnLoadVideo(videoMemoryMetrics);
	}
#endif

	BinkSetWillLoop( m_bink, m_loopVideo.GetValue() );
	m_wasLooped = m_loopVideo.GetValue();

#if defined(__EMSCRIPTEN__)
    // Obtain genuine metadata before launching the first asynchronous decode.
    VideoSummary initialSummary{};GetVideoSummary(initialSummary);
#endif
	StartNextFrame();

	m_passiveMode = passiveMode;
	if ( m_passiveMode )
	{
		m_passiveUpdateEnabled = true;
	}

	m_asyncOperation.SetValue( AsyncOperation::None );

	return true;
}

void RenderBinkVideo::DestroySelfIfRefCountZero()
{
	RED_FATAL_ASSERT( IsRefCountZero(), "Releasing RenderBinkVideo too many times" );
	m_closeRequested = true;
	QueueForDestroy( this );

#ifdef USE_RESOURCE_METRICS_BANK
	// if the textures haven't been created we haven't added to the metrics either
	if(!(m_drawTexture.m_aTexture.isNull() && m_drawTexture.m_yTexture.isNull() && 
		m_drawTexture.m_cBTexture.isNull() && m_drawTexture.m_cRTexture.isNull()))
	{
		auto* resourceMetricsBank = GResourceLoader->GetResourceMetricsBank();
		resourceMetricsBank->OnUnloadVideo(m_videoPath);
	}
#endif
}

void RenderBinkVideo::ReleaseCanvas()
{
	m_canvasName = CName::NONE();
}

Bool RenderBinkVideo::IsValid() const
{
	return m_bink && !m_finishedVideo;
}

const res::ResourcePath& RenderBinkVideo::GetVideoPath() const
{
	return m_videoPath;
}

Bool RenderBinkVideo::IsLooped() const
{
	return m_loopVideo.GetValue();
}

void RenderBinkVideo::Loop( Bool loop )
{
	m_loopVideo.SetValue( loop );
}

Bool RenderBinkVideo::IsVideoFrameRateForced() const
{
	return m_forceVideoFrameRate.GetValue();
}

void RenderBinkVideo::ForceVideoFrameRate( Bool force )
{
	m_forceVideoFrameRate.SetValue( force );
}

Bool RenderBinkVideo::IsPaused() const
{
	return m_paused.GetValue();
}

void RenderBinkVideo::Pause( Bool pause )
{
	m_paused.SetValue( pause );
	BinkPause( m_bink, pause ? 1 : 0 );
}

void RenderBinkVideo::SkipFrames( Int32 numberOfFrames )
{
	m_numberOfFramesToSkip.ExchangeAdd( numberOfFrames );
}

void RenderBinkVideo::SkipFramePercentage( Int32 framePercentage )
{
	RED_ASSERT( m_bink != nullptr, "Video wasn't initialized" );
	const float percentage = ( framePercentage % 100 ) / 100.f;
	const Int32 numberOfFrames = static_cast< Int32 >( m_bink->Frames * percentage );
	m_numberOfFramesToSkip.ExchangeAdd( numberOfFrames );
}

Int32 RenderBinkVideo::GetAudioEventId() const
{
	return m_audioEventId;
}

void RenderBinkVideo::KeepInSyncWithExternal( Float time, Bool enableSkipBackwards )
{
	if ( !m_bink )
	{
		return;
	}

	if( m_needWaitForExternal )
	{
		m_needWaitForExternal = false;
	}

	const auto videoFrameRate = ( ( Float )m_bink->FrameRate / m_bink->FrameRateDiv );
	const Uint32 expectedFrame = static_cast< Uint32 >( std::round( time * videoFrameRate ) );

	if( expectedFrame > m_bink->FrameNum )
	{
		// audio is ahead of the video

		const Uint32 framesToSkip = Min( ( expectedFrame - m_bink->FrameNum ) / 2, c_audioSyncMaxFrameSkip );
		if( ( m_bink->FrameNum + framesToSkip > m_bink->Frames ) && !m_loopVideo.GetValue() )
		{
			// if video is not looped ignore rest of the logic
			return;
		}
		else if ( ( expectedFrame - m_bink->FrameNum ) > c_audioSyncMaxFrameSkip )
		{
			// Skip to next key frame or make small adjustments
			Uint32 keyFrame = GetKeyFrame( expectedFrame );			
			Uint32 frameToSkipTo = keyFrame == 1 ? expectedFrame : keyFrame;  // Some binks seems to be missing keyframes

			// Don't double jump, wait for the last skip to finish
			if ( m_lastFrameSkippedTo <= m_bink->FrameNum )
			{
				SkipFrames( frameToSkipTo - m_bink->FrameNum );
				m_lastFrameSkippedTo = frameToSkipTo;
			}
		}
	}
	else if( m_bink->FrameNum > expectedFrame )
	{
		const Uint32 difference = m_bink->FrameNum - expectedFrame;
		if( enableSkipBackwards )
		{
			SkipFrames( -( Int32 )difference );
		} 
		else 
		{
			// audio is behind the video
			m_needWaitForExternal = true;
		}
	}
}

void RenderBinkVideo::KeepInSyncWith( Bool & needWaitForToSync, Float time )
{
	if( needWaitForToSync )
	{
		needWaitForToSync = false;
	}

	const auto videoFrameRate = ( ( Float )m_bink->FrameRate / m_bink->FrameRateDiv );
	const Uint32 expectedFrame = static_cast< Uint32 >( std::round( time * videoFrameRate ) );

	if( expectedFrame > m_bink->FrameNum )
	{
		// audio is ahead of the video

		const Uint32 framesToSkip = Min( ( expectedFrame - m_bink->FrameNum ) / 2, c_audioSyncMaxFrameSkip );
		if( ( m_bink->FrameNum + framesToSkip > m_bink->Frames ) && !m_loopVideo.GetValue() )
		{
			// if video is not looped ignore rest of the logic
			return;
		}
		else
		{
			SkipFrames( framesToSkip );
		}
	}
	else if( m_bink->FrameNum > expectedFrame )
	{
		const Uint32 difference = m_bink->FrameNum - expectedFrame;
		if( difference < c_audioSyncMaxFrameSkip )
		{
			// audio is behind the video
			needWaitForToSync = true;
		}
	}
}

void RenderBinkVideo::GetVideoSummary( VideoSummary& summary ) const
{
#if defined(__EMSCRIPTEN__)
    if(m_bink && m_decodingFrame && !BinkDoFrameAsyncWait(m_bink,0)) {
        std::lock_guard<std::mutex> guard(cpWebVideoSnapshotsMutex);
        auto found=cpWebVideoSnapshots.find(this);
        if(found!=cpWebVideoSnapshots.end()) {
            summary=found->second;
            const unsigned count=cpWebVideoPendingReports.fetch_add(1);
            if(count<8)std::fprintf(stderr,"CP2077 Bink renderer probe: phase=published-status-while-pending frame=%u totalFrames=%u\n",summary.currentFrame,summary.totalFrames);
            return;
        }
    }
#endif
	if ( m_bink ) 
	{
		BINKSUMMARY videoSummary = {};
#if defined(__EMSCRIPTEN__)
        Uint32 currentFrame=0;
        const Int32 status=CpWebBinkTryGetSummary(m_bink,&videoSummary,&currentFrame);
        if(status==0) {
            std::lock_guard<std::mutex> guard(cpWebVideoSnapshotsMutex);
            auto found=cpWebVideoSnapshots.find(this);
            if(found!=cpWebVideoSnapshots.end()){summary=found->second;return;}
        }
        // Initialization without a snapshot and genuine request errors retain
        // the original SDK path rather than manufacturing metadata.
        if(status!=1){BinkGetSummary(m_bink,&videoSummary);currentFrame=m_bink->FrameNum;}
#else
		BinkGetSummary( m_bink, &videoSummary );
#endif

		summary.width = m_bink->Width;
		summary.height = m_bink->Height;
#if defined(__EMSCRIPTEN__)
        summary.currentFrame=currentFrame;
#else
		summary.currentFrame = m_bink->FrameNum;
#endif
		summary.totalTimeMs = (Uint32)( (float)videoSummary.TotalFrames * 1000.f / ( videoSummary.FrameRate / videoSummary.FrameRateDiv ) );
		RED_ASSERT( summary.totalTimeMs, "totalTimeMs cannot be 0" );
		summary.currentTimeMs = videoSummary.TotalTime % summary.totalTimeMs;
		summary.frameRate = videoSummary.FrameRate;
		summary.frameRateDiv = videoSummary.FrameRateDiv;
		summary.totalFrames = videoSummary.TotalFrames;
#if defined(__EMSCRIPTEN__)
        { std::lock_guard<std::mutex> guard(cpWebVideoSnapshotsMutex);cpWebVideoSnapshots[this]=summary; }
#endif
	}
}

void RenderBinkVideo::EnablePassiveUpdate( Bool enable )
{
	m_passiveUpdateEnabled = enable;
}

void RenderBinkVideo::Update()
{
	PC_SCOPE( RenderBink_Update );

	if ( m_waitsForExternal && !m_needWaitForExternal )
    {
		m_waitsForExternal = false;
    }

	if ( !m_bink || ( m_passiveMode && !m_passiveUpdateEnabled ) || m_waitsForExternal )
	{
		// Tad: Remove changes to BackgroundIO thread as IO is balanced via queues anyway
		//BalanceRealtimeBufferFill();
		return;
	}

	if ( m_asyncOperation.GetValue() != AsyncOperation::None )
	{
		return;
	}

	if ( m_wasLooped != m_loopVideo.GetValue() )
	{
		BinkSetWillLoop( m_bink, m_loopVideo.GetValue() );
		m_wasLooped = m_loopVideo.GetValue();
	}

	PrepareFrame();
	
	// Tad: Remove changes to BackgroundIO thread as IO is balanced via queues anyway
	//BalanceRealtimeBufferFill();
}

RenderBinkVideo::UploadResult RenderBinkVideo::UploadTextures( Uint32 maxAllowedUpload, Uint32& accumUploadedSize )
{
	PC_SCOPE( RenderBink_UploadTextures );

	if ( !m_bink || !m_dirtyTextures || !m_firstDecodedFrame )
	{
		return UploadResult::NotNeeded;
	}

	if ( m_passiveMode )
	{
		EnablePassiveUpdate( false );
	}

	const Uint32 uploadSize = GetFrameUploadSize( m_binkBuffers, m_binkBuffers.FrameNum );
	if ( uploadSize > maxAllowedUpload )
	{
		return UploadResult::TooBigForBudget;
	}
	else if ( accumUploadedSize + uploadSize > maxAllowedUpload )
	{
		return UploadResult::OutOfBudget;
	}

	const BINKFRAMEPLANESET& srcFrame = m_binkBuffers.Frames[m_binkBuffers.FrameNum];
	GpuApi::LoadTextureData2D( m_drawTexture.m_yTexture, 0, 0, nullptr, srcFrame.YPlane.Buffer, srcFrame.YPlane.BufferPitch );
	GpuApi::LoadTextureData2D( m_drawTexture.m_cRTexture, 0, 0, nullptr, srcFrame.cRPlane.Buffer, srcFrame.cRPlane.BufferPitch );
	GpuApi::LoadTextureData2D( m_drawTexture.m_cBTexture, 0, 0, nullptr, srcFrame.cBPlane.Buffer, srcFrame.cBPlane.BufferPitch );
	if ( m_drawTexture.m_aTexture )
	{
		GpuApi::LoadTextureData2D( m_drawTexture.m_aTexture, 0, 0, nullptr, srcFrame.APlane.Buffer, srcFrame.APlane.BufferPitch );
	}

	m_dirtyTextures = false;
	accumUploadedSize += uploadSize;

	return UploadResult::Uploaded;
}

void RenderBinkVideo::RenderFullscreen( const SRenderNodeImplContext& rctx )
{
	if ( !m_bink || !m_firstDecodedFrame )
	{
		return;
	}

	ShowFrame( rctx );
}

Bool RenderBinkVideo::CreateBinkCpuResources()
{
	PC_SCOPE( RenderBink_CreateCpuRes );
	BinkGetFrameBuffersInfo( m_bink, &m_binkBuffers );

	for ( Int32 i = 0; i < m_binkBuffers.TotalFrames; ++i )
	{
		MappedBinkFrameBuffer& mappedBinkBuffer = m_mappedBinkBuffer[ i ];
		red::Memzero( &mappedBinkBuffer, sizeof( MappedBinkFrameBuffer ) );

		BINKFRAMEPLANESET& binkBuffer = m_binkBuffers.Frames[ i ];

		// Calculate Y plane size
		Uint32 ySize = 0;
		if ( binkBuffer.YPlane.Allocate )
		{
			binkBuffer.YPlane.BufferPitch = red::AlignUp( m_binkBuffers.YABufferWidth, ( Uint32 )c_texMemAlign );
			ySize = binkBuffer.YPlane.BufferPitch * m_binkBuffers.YABufferHeight;
		}

		// Calculate cR plane size
		Uint32 cRSize = 0;
		if ( binkBuffer.cRPlane.Allocate )
		{
			binkBuffer.cRPlane.BufferPitch = red::AlignUp( m_binkBuffers.cRcBBufferWidth, ( Uint32 )c_texMemAlign );
			cRSize = binkBuffer.cRPlane.BufferPitch * m_binkBuffers.cRcBBufferHeight;
		}

		// Calculate cB plane size
		Uint32 cBSize = 0;
		if ( binkBuffer.cBPlane.Allocate )
		{
			binkBuffer.cBPlane.BufferPitch = red::AlignUp( m_binkBuffers.cRcBBufferWidth, ( Uint32 )c_texMemAlign );
			cBSize = binkBuffer.cBPlane.BufferPitch * m_binkBuffers.cRcBBufferHeight;
		}

		// Calculate alpha plane size
		Uint32 aSize = 0;
		if ( binkBuffer.APlane.Allocate )
		{
			binkBuffer.APlane.BufferPitch = red::AlignUp( m_binkBuffers.YABufferWidth, ( Uint32 )c_texMemAlign );
			aSize = binkBuffer.APlane.BufferPitch * m_binkBuffers.YABufferHeight;
		}

		// Create all planes
		const Uint32 frameBuffersSize = ySize + cRSize + cBSize + aSize;
		mappedBinkBuffer.m_frameBuffers = red::CreateUniqueBuffer< rend::PoolBink >( frameBuffersSize, c_texMemAlign );

		// Set plane views
		Uint8* frameBuffersData = reinterpret_cast< Uint8* >( mappedBinkBuffer.m_frameBuffers.Get() );
		mappedBinkBuffer.m_yPlane = frameBuffersData;
		RED_ASSERT( ( reinterpret_cast< uintptr_t >( mappedBinkBuffer.m_yPlane ) & ( c_texMemAlign - 1 ) ) == 0 );
		mappedBinkBuffer.m_cRPlane = frameBuffersData + ySize;
		RED_ASSERT( ( reinterpret_cast< uintptr_t >( mappedBinkBuffer.m_cRPlane ) & ( c_texMemAlign - 1 ) ) == 0 );
		mappedBinkBuffer.m_cBPlane = frameBuffersData + ySize + cRSize;
		RED_ASSERT( ( reinterpret_cast< uintptr_t >( mappedBinkBuffer.m_cBPlane ) & ( c_texMemAlign - 1 ) ) == 0 );
		mappedBinkBuffer.m_aPlane = frameBuffersData + ySize + cRSize + cBSize;
		RED_ASSERT( ( reinterpret_cast< uintptr_t >( mappedBinkBuffer.m_aPlane ) & ( c_texMemAlign - 1 ) ) == 0 );
	}

	// register our locked texture pointer with Bink
	BINKFRAMEPLANESET* binkBufferFrames = m_binkBuffers.Frames;
	for ( Int32 i = 0; i < m_binkBuffers.TotalFrames; ++i )
	{
		binkBufferFrames[ i ].YPlane.Buffer = m_mappedBinkBuffer[ i ].m_yPlane;
		binkBufferFrames[ i ].cRPlane.Buffer = m_mappedBinkBuffer[ i ].m_cRPlane;
		binkBufferFrames[ i ].cBPlane.Buffer = m_mappedBinkBuffer[ i ].m_cBPlane;

		if ( binkBufferFrames[i].APlane.Allocate )
		{
			binkBufferFrames[i].APlane.Buffer = m_mappedBinkBuffer[i].m_aPlane;
		}
	}

	BinkRegisterFrameBuffers( m_bink, &m_binkBuffers );
	return true;
}

Bool RenderBinkVideo::CreateBinkTexture( Uint32 width, Uint32 height, const AnsiChar* name, GpuApi::TextureRef& outTexture )
{
	GpuApi::TextureDesc textureDesc;
	textureDesc.type = GpuWrapApi::TEXTYPE_2D;
	textureDesc.format = GpuWrapApi::TEXFMT_R8_Unorm;
	textureDesc.width = width;
	textureDesc.height = height;
	textureDesc.numMips = 1;
	textureDesc.usage = GpuApi::TEXUSAGE_Samplable;
	textureDesc.group = GpuWrapApi::TEXG_System_Video;
	outTexture = GpuApi::CreateTexture( textureDesc );

	GpuApi::SetResourceDebugName( outTexture, "BinkTexture" );

	return !outTexture.isNull();
}


Bool RenderBinkVideo::CreateBinkGpuResources()
{
	PC_SCOPE( RenderBink_CreateGpuRes );

	if ( m_binkBuffers.Frames[0].YPlane.Allocate )
	{
		if ( !CreateBinkTexture( m_binkBuffers.YABufferWidth, m_binkBuffers.YABufferHeight, "bink_Y", m_drawTexture.m_yTexture ) )
		{
			RED_LOG_ERROR( "Video: Failed to create Y texture." );
			return false;
		}
	}
	if ( m_binkBuffers.Frames[0].cRPlane.Allocate )
	{
		if ( !CreateBinkTexture( m_binkBuffers.cRcBBufferWidth, m_binkBuffers.cRcBBufferHeight, "bink_cR", m_drawTexture.m_cRTexture ) )
		{
			RED_LOG_ERROR( "Video: Failed to create cR texture." );
			return false;
		}
	}
	if ( m_binkBuffers.Frames[0].cBPlane.Allocate )
	{
		if ( !CreateBinkTexture( m_binkBuffers.cRcBBufferWidth, m_binkBuffers.cRcBBufferHeight, "bink_cB", m_drawTexture.m_cBTexture ) )
		{
			RED_LOG_ERROR( "Video: Failed to create cB texture." );
			return false;
		}
	}
	if ( m_binkBuffers.Frames[0].APlane.Allocate )
	{
		if ( !CreateBinkTexture( m_binkBuffers.YABufferWidth, m_binkBuffers.YABufferHeight, "bink_A", m_drawTexture.m_aTexture ) )
		{
			RED_LOG_ERROR( "Video: Failed to create A texture." );
			return false;
		}
	}

	return true;
}


void RenderBinkVideo::StartNextFrame()
{
	BinkDoFrameAsyncMulti( m_bink, m_threads, m_threadCount );
	m_decodingFrame = true;
}

Bool RenderBinkVideo::PrepareFrame()
{
	PC_SCOPE( RenderBink_PrepFrame );

	if ( m_finishedVideo || m_closeRequested )
	{
		return false;
	}

#if defined(__EMSCRIPTEN__)
    // The browser SDK worker serializes control requests with decoding. Poll
    // completion before asking it for timing; never block a render tick here.
    if(m_decodingFrame && !BinkDoFrameAsyncWait(m_bink,0))return false;
#endif

	// resume paused video for a while if we have some frames to skip
	const Bool isPaused = m_paused.GetValue();
	if ( isPaused && !m_wasPaused && m_numberOfFramesToSkip.GetValue() )
	{
		BinkPause( m_bink, 0 );
		m_wasPaused = true;
	}
	else if ( m_wasPaused )
	{
		BinkPause( m_bink, 1 );
		m_wasPaused = false;
	}

	if ( !BinkWait( m_bink ) )
	{
		PC_SCOPE( RenderBink_AsyncWaitLoop );
		Bool atLeastOneFrameWasDecoded = false;
#if defined(__EMSCRIPTEN__)
		Bool cpCatchUpPending = false;
#endif

		do
		{
			// check is the previous frame done yet (wait for a 20 us)
			if ( BinkDoFrameAsyncWait( m_bink, 20 ) )
			{
				// do we need to skip a frame?
				while ( BinkShouldSkip( m_bink ) )
				{
					// catch up when game has a low framerate
					BinkNextFrame( m_bink );
					StartNextFrame();
#if defined(__EMSCRIPTEN__)
                    // Decode runs on a separate browser worker. Yield this
                    // video update while it completes instead of monopolizing
                    // the renderer while a looping movie catches up. Retain
                    // the pending native frame and normal completion callbacks.
                    // Returning here skipped publishing the frame that had just completed, so a clip that
                    // decodes slightly slower than real time (the 1080p splash trailer) never reached
                    // SetCanvasVideo and stayed black. Stop catching up for this tick instead.
                    if (!BinkDoFrameAsyncWait(m_bink, 0)) { cpCatchUpPending = true; break; }
#else
					BinkDoFrameAsyncWait( m_bink, -1 );
#endif
				}

				atLeastOneFrameWasDecoded = true;
#if defined(__EMSCRIPTEN__)
				// The catch-up decode is still running on the browser worker (m_decodingFrame stays set);
				// publish the completed frame below and continue next tick.
				if ( cpCatchUpPending ) break;
#endif
				m_decodingFrame = false;

				// Triggered from external sources
				Bool skippedFrames = SkipFrames();

				if ( m_bink->FrameNum < m_bink->Frames )
				{
					if ( !skippedFrames )
					{
						BinkNextFrame( m_bink );
					}

					if ( !m_closeRequested )
					{
						StartNextFrame();
					}
					else
					{
						break;
					}
				}
				else if ( m_loopVideo.GetValue() )
				{
					BinkSetWillLoop( m_bink, 1 );
					
					if ( !m_closeRequested )
					{
						BinkNextFrame( m_bink );
						StartNextFrame();
					}
					else
					{
						break;
					}

					if ( m_loopCallback )
					{
						m_loopCallback();
					}
				}
				else
				{
					m_finishedVideo = true;
					break;
				}
			}
		} while ( m_forceVideoFrameRate.GetValue() && !BinkWait( m_bink ) );

		if ( atLeastOneFrameWasDecoded )
		{
			PC_SCOPE( RenderBink_UpdateTextures );

			m_dirtyTextures = true;

			if ( !m_firstDecodedFrame )
			{
				if ( m_canvasName != CName::NONE() )
				{
					SurfaceGenerator_Video& videoSurfaceGenerator = GetRenderer()->GetSurfaceCache().GetGenerator_Video();
					videoSurfaceGenerator.SetCanvasVideo(m_canvasName.GetHash(), TRenderPtr<IRenderVideo>::MakePtr( this ) );
				}
				m_firstDecodedFrame = true;
			}

			if ( m_needWaitForExternal )
			{
				m_waitsForExternal = true;
			}
		}
	}

	return true;
}

void RenderBinkVideo::ShowFrame( const SRenderNodeImplContext& rctx )
{
	if ( !m_bink )
	{
		return;
	}

	struct BinkShaderParams
	{
		Uint32 m_width;
		Uint32 m_height;
		Float m_alphaLevel;
		Bool m_isPremultAlpha;
	};

	BinkShaderParams shaderParams = {
		m_bink->Width,	// Movie width
		m_bink->Height,	// Movie height
		1.0f,			// Alpha level
		false,			// Premultiplied alpha
	};

	struct VER
	{
		Vector4 constuv[ 3 ];
	} vsData;

	struct PIX
	{
		Vector4 consta;
		Vector4 crc;
		Vector4 cbc;
		Vector4 adj;
		Vector4 yscale;
	} psData;

	// Pixel shader alpha constants
	psData.consta.X = shaderParams.m_isPremultAlpha ? shaderParams.m_alphaLevel : 1.0f;
	psData.consta.Y = psData.consta.X;
	psData.consta.Z = psData.consta.X;
	psData.consta.W = shaderParams.m_alphaLevel;

	// set the constants for the type of ycrcb we have
	red::Memcpy( &psData.crc, m_bink->ColorSpace, sizeof( m_bink->ColorSpace ) );

	// UV matrix
	{
		F32 luma_u_scale = (F32)shaderParams.m_width / (F32)m_binkBuffers.YABufferWidth;
		F32 luma_v_scale = (F32)shaderParams.m_height / (F32)m_binkBuffers.YABufferHeight;
		F32 chroma_u_scale = (F32)(shaderParams.m_width / 2) / (F32)m_binkBuffers.cRcBBufferWidth;
		F32 chroma_v_scale = (F32)(shaderParams.m_height / 2) / (F32)m_binkBuffers.cRcBBufferHeight;

		// Set up matrix columns for UV transform (currently just scale+translate, could add rotation)
		// X column
		vsData.constuv[0].X = luma_u_scale;
		vsData.constuv[0].Y = 0.0f;
		vsData.constuv[0].Z = chroma_u_scale;
		vsData.constuv[0].W = 0.0f;

		// Y column
		vsData.constuv[1].X = 0.0f;
		vsData.constuv[1].Y = luma_v_scale;
		vsData.constuv[1].Z = 0.0f;
		vsData.constuv[1].W = chroma_v_scale;

		// W column (translation)
		vsData.constuv[2].X = 0.0f;
		vsData.constuv[2].Y = 0.0f;
		vsData.constuv[2].Z = 0.0f;
		vsData.constuv[2].W = 0.0f;
	}

	rctx.SetConstant<ePIXEL>(psData);
	rctx.SetConstant<eVERTEX>(vsData);

	// turn on texture filtering and texture clamping
	rctx.BindSampler<0, ePIXEL>( GpuApi::SAMPSTATEPRESET_ClampLinearNoMip );

	const CRenderFrameInfo& info = rctx.GetFrameInfo();
	GpuApi::ViewportDesc viewportDesc;
	viewportDesc.x = 0;
	viewportDesc.y = 0;
	viewportDesc.width = info.GetViewportWidth();
	viewportDesc.height = info.GetViewportHeight();
	GpuApi::SetViewport( viewportDesc );

	// are we using an alpha plane? if so, turn on the 4th texture and set our pixel shader
	if ( m_drawTexture.m_aTexture )
	{
		GpuApi::TextureRef refs[] = {
			m_drawTexture.m_yTexture,
			m_drawTexture.m_cRTexture,
			m_drawTexture.m_cBTexture,
			m_drawTexture.m_aTexture
		};
		rctx.BindTextures<0, ePIXEL>(refs);

		GetRenderer()->GetShader( SHADER_NAME( "m_binkRGBA" ) )->Bind();
	}
	else
	{
		GpuApi::TextureRef refs[] = {
			m_drawTexture.m_yTexture,
			m_drawTexture.m_cRTexture,
			m_drawTexture.m_cBTexture,
		};
		rctx.BindTextures<0, ePIXEL>(refs);

		GetRenderer()->GetShader( SHADER_NAME( "m_binkRGB" ) )->Bind();
	}

	GpuApi::DrawPrimitiveNoBuffers( PRIMTYPE_TriangleStrip, 4 );
}


Vector4 RenderBinkVideo::GetLumaChromaScale() const
{
	F32 luma_u_scale = ( F32 ) m_bink->Width / ( F32 ) m_binkBuffers.YABufferWidth;
	F32 luma_v_scale = ( F32 ) m_bink->Height / ( F32 ) m_binkBuffers.YABufferHeight;
	F32 chroma_u_scale = ( F32 ) ( m_bink->Width / 2 ) / ( F32 ) m_binkBuffers.cRcBBufferWidth;
	F32 chroma_v_scale = ( F32 ) ( m_bink->Height / 2 ) / ( F32 ) m_binkBuffers.cRcBBufferHeight;

	Vector4 result;
	result.X = luma_u_scale;
	result.Y = luma_v_scale;
	result.Z = chroma_u_scale;
	result.W = chroma_v_scale;

	return result;
}


Vector4 RenderBinkVideo::GetAlphaFactor() const
{
	const Bool isPremulAlpha = false;
	const Float alpha = 1.0f;

	Vector4 result;
	result.X = isPremulAlpha ? alpha : 1.0f;
	result.Y = result.X;
	result.Z = result.X;
	result.W = alpha;
	return result;
}

Vector4 RenderBinkVideo::GetColorSpaceCr() const
{
	Vector4 result;
	if ( m_bink != nullptr )
	{
		memcpy( &result, &m_bink->ColorSpace[0], sizeof(Vector4) );
	}
	else
	{
		result = Vector4::ZEROS();
	}
	return result;
}

Vector4 RenderBinkVideo::GetColorSpaceCb() const
{
	Vector4 result;
	if ( m_bink != nullptr )
	{
		memcpy( &result, &m_bink->ColorSpace[4], sizeof(Vector4) );
	}
	else
	{
		result = Vector4::ZEROS();
	}
	return result;
}

Vector4 RenderBinkVideo::GetColorSpaceAdj() const
{
	Vector4 result;
	if ( m_bink != nullptr )
	{
		memcpy( &result, &m_bink->ColorSpace[8], sizeof(Vector4) );
	}
	else
	{
		result = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
	}
	return result;
}

Vector4 RenderBinkVideo::GetColorSpaceY() const
{
	Vector4 result;
	if ( m_bink != nullptr )
	{
		memcpy( &result, &m_bink->ColorSpace[12], sizeof(Vector4) );
	}
	else
	{
		result = Vector4::ZEROS();
	}
	return result;
}

// Tad: Remove changes to BackgroundIO thread as IO is balanced via queues anyway
//void RenderBinkVideo::BalanceRealtimeBufferFill()
//{
//	if ( !IsValid() )
//	{
//		return;
//	}
//
//	BINKREALTIME realtime;
//	BinkGetRealtime( m_bink, &realtime, 0 );
//
//	const Float fillRatio = (Float)realtime.ReadBufferUsed / realtime.ReadBufferSize;
//	if ( fillRatio > REALTIME_MAX_BUFFER_FILL_PERCENTAGE )
//	{
//		SuspendBinkLoad(realtime);
//	}
//	else if ( fillRatio < REALTIME_MIN_BUFFER_FILL_PERCENTAGE )
//	{
//		ResumeBinkLoad(realtime);
//	}
//}

//void RenderBinkVideo::SuspendBinkLoad(const BINKREALTIME& realtime)
//{
//	if (m_profilerCallback)
//	{
//		m_profilerCallback->OnSuspendBinkLoad(realtime);
//	}
//	TrySuspendBinkLoad();
//	TryResumeGameLoad();
//}
//
//void RenderBinkVideo::ResumeBinkLoad(const BINKREALTIME& realtime)
//{
//	if (m_profilerCallback)
//	{
//		m_profilerCallback->OnResumeBinkLoad(realtime);
//	}
//	TrySuspendGameLoad();
//	TryResumeBinkLoad();
//}
//
//void RenderBinkVideo::TrySuspendBinkLoad()
//{
//	if ( !m_binkLoadSuspended )
//	{
//		m_binkLoadSuspended = true;
//		BinkControlBackgroundIO( m_bink, BINKBGIOSUSPEND );
//	}
//}
//
//void RenderBinkVideo::TryResumeBinkLoad()
//{
//	if ( m_binkLoadSuspended )
//	{
//		m_binkLoadSuspended = false;
//		BinkControlBackgroundIO( m_bink, BINKBGIORESUME );
//	}
//}

void RenderBinkVideo::TrySuspendGameLoad()
{
	if ( !m_gameLoadSuspended )
	{
		m_gameLoadSuspended = true;
	}
	// #tbd: suspending game I/O
}

void RenderBinkVideo::TryResumeGameLoad()
{
	if ( m_gameLoadSuspended )
	{
		m_gameLoadSuspended = false;
	}
	// #tbd: resuming game I/O
}

Uint32 RenderBinkVideo::GetKeyFrame( Uint32 frame) const
{
	if ( IsLooped() )
	{
		Uint32 loopNumber = frame / m_bink->Frames;
		return loopNumber * m_bink->Frames + BinkGetKeyFrame( m_bink, frame % m_bink->Frames, BINKGETKEYCLOSEST );
	}
	else
	{
		return BinkGetKeyFrame( m_bink, Min( frame, m_bink->Frames ), BINKGETKEYCLOSEST );
	}
}

Bool RenderBinkVideo::SkipFrames()
{
	PC_SCOPE( RenderBink_SkipFrames );

	const Int32 framesToSkip = m_numberOfFramesToSkip.GetValue();
	if ( !framesToSkip )
	{
		return false;
	}

	Int32 frame = static_cast< Int32 >( m_bink->FrameNum ) + framesToSkip;

	if ( framesToSkip > 0 )
	{
		if ( frame > static_cast< Int32 >( m_bink->Frames ) )
		{
			frame = m_loopVideo.GetValue() ? frame % m_bink->Frames : m_bink->Frames;
		}
	}
	else
	{
		if ( frame < 0 )
		{
			frame = frame % m_bink->Frames;
			frame = m_loopVideo.GetValue() ? m_bink->Frames + frame : 1;
		}
	}

	// Tad: Remove changes to BackgroundIO thread as IO is balanced via queues anyway
	//TryResumeBinkLoad();

	serialization::TEMPHACK_SetBinkVideoOpen_ThreadLocal( true );
	BinkGoto( m_bink, frame, BINKGOTOQUICK );
	serialization::TEMPHACK_SetBinkVideoOpen_ThreadLocal( false );

	m_numberOfFramesToSkip.SetValue( 0 );
	return true;
}

Bool RenderBinkVideo::IsDecodingFrame() const
{
	return BinkDoFrameAsyncWait(m_bink, 0) == 0;
}

Bool RenderBinkVideo::IsInitializing() const
{
	return m_asyncOperation.GetValue() == AsyncOperation::Init;
}

#endif
