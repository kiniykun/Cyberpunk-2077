/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "renderCommandHandler.h"

#include "renderTextureArray.h"
#include "renderViewport.h"
#include "renderSequenceGrabber.h"
#include "renderProxyFogVolume.h"
#include "renderProxySpotLight.h"
#include "renderProxyPointLight.h"
#include "renderParticleEmitter.h"
#include "renderScene.h"
#include "visibilityOccluderMeshInstance.h"
#include "visibilityScene.h"
#include "renderReflectionProbeManager.h"
#include "gameplayFx_CPFocusMode.h"
#include "gameplayFx_Fade.h"
#include "renderDynamicTexture.h"
#include "renderDrawBufferProcessor.h"
#include "particleSimulationController.h"
#include "renderSkinManager.h"
#include "renderProxyParticles.h"

#include "../../renderData/include/screenshotParameters.h"
#include "../../renderData/include/renderDrawBuffer.h"
#include "../../renderData/include/renderMaterialSetupParam.h"
#include "../../redJobs2/include/jobRunner.h"
#include "../../redMemory/include/circularAllocator.h"
#include "../../redJobs2/include/jobBuilder.h"

// [hang] Browser-port fix for a permanent engine hang (r1: ~55 min, ink DrawWindow job livelocked in PushToQueue).
// RenderScene's 'RenderScene/DispatchDrawBuffersFlush' job swapped the draw-buffer queue (AcquireQueue) without
// m_commmitConsumeRWLock while ink jobs pushed into it, so a page could end up both as the queue tail and on the
// free list; PushToQueue then popped that full page as its own successor forever. The acquire now takes the
// lock exclusively, and the push paths refuse corrupt free pages.
// ?rchFix=0 = original engine behaviour (A/B only), ?rchStress=1 = widen the push/acquire race windows.
#include <atomic>
#include <cstdio>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif
namespace
{
	int s_cpRchFix = 1;
	int s_cpRchStress = 0;
	std::atomic< Uint32 > s_cpRchReports{ 0 };
	RED_INLINE void CpRchStressPause( Uint32 strongUs = 50 )
	{
		if ( s_cpRchStress )
		{
			usleep( s_cpRchStress >= 2 ? strongUs : 50 );
		}
	}
	void CpRchReport( const char* what, const void* lastPage, const void* freePage, Int32 size, Uint32 spins )
	{
		if ( s_cpRchReports.fetch_add( 1 ) < 16 )
		{
			std::fprintf( stderr, "CP2077 hang guard: %s (tail=%p free=%p size=%d spins=%u) -> fresh page, free list dropped\n", what, lastPage, freePage, ( int )size, spins );
		}
	}
	void CpRchInitSwitches()
	{
#ifdef __EMSCRIPTEN__
		s_cpRchFix = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'rchFix' ); return ( v === '0' ) ? 0 : 1; } catch ( e ) { return 1; } } );
		s_cpRchStress = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'rchStress' ); return v ? ( parseInt( v ) || 0 ) : 0; } catch ( e ) { return 0; } } );
#endif
		std::fprintf( stderr, "CP2077 hang switches: rchFix=%d rchStress=%d\n", s_cpRchFix, s_cpRchStress );
	}
}

// For IsGameMode
#include "../../redCore/include/globalModeInfo.h"
#include "renderProxyMorphTargetMesh.h"

#include "../../../common/renderer/src/renderMultilayerMask.h"
#include "../../../common/renderData/include/renderMultilayerMaskBlob.h"
#include "../../renderData/include/renderFoliageData.h"
#include "../../renderData/include/renderVideo.h"
#include "windImpulseCollector.h"
#include "renderFoliageProfile.h"
#include "renderProxyCameraData.h"
#include "../../visibility/include/visVolumeHierarchyGrid.h"
#include "renderTXAACustomData.h"
#include "renderDistantShadows.h"
#include "renderRainMap.h"

namespace helper
{
	RED_FORCE_INLINE Bool IsProxyDrawable( const TRenderPtr<IRenderProxy>& proxy )
	{
		return proxy && static_cast<const IRenderProxyBase*>(proxy.Get())->IsDrawable();
	}
}

CRenderScreenshotSequnceGrabber* GScreenshotSequence = nullptr;

template<typename Func>
class RenderCommandGeneric : public IRenderCommand
{
public:
	RenderCommandGeneric( const char* debugName, Func&& callback, void* proxy = nullptr, 
						  IRenderCommand::ParallelQueueType parallelQueueType = IRenderCommand::PQT_NotSafeWithProxyAdditionAndRemoval )
		: IRenderCommand( debugName, proxy, parallelQueueType )
		, m_callback( std::forward<Func>( callback ) )
	{}

	virtual void Execute()
	{
		m_callback();
	}

private:
	typename red::RemoveCVRef< Func >::Type m_callback;
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


template<typename Func>
RED_FORCE_INLINE RenderCommandGeneric<Func> MakeRenderCommandGeneric( const char* debugName, Func&& callback, void* proxy = nullptr, 
																	  IRenderCommand::ParallelQueueType parallelQueueType = IRenderCommand::PQT_NotSafeWithProxyAdditionAndRemoval )
{
	return RenderCommandGeneric<Func>( debugName, std::forward<Func>( callback ), proxy, parallelQueueType );
}

#define DBGRENDER_TXT __func__


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


template<typename TQueue, typename TQueueElement>
RED_INLINE void CRenderCommandHandler::PushToQueue( TQueueElement&& element, TQueue& queue, typename TQueue::Page*& freePages )
{
	using TPage = typename TQueue::Page;

	Uint32 cpSpins = 0;
	for (;;)
	{
		if ( ++cpSpins == 1000000 )
		{
			std::fprintf( stderr, "CP2077 hang guard: PushToQueue spun 1000000 times (tail=%p size=%d)\n", ( const void* )queue.m_lastPage, ( int )queue.m_lastPage->m_size.GetValue() );
		}
		{
			RED_SCOPE_SHARED_LOCK( m_commmitConsumeRWLock );

			TPage* page = queue.m_lastPage;
			CpRchStressPause();
			Uint32 pushIndex = page->m_size.PostIncrement();
			if ( pushIndex < page->c_maxSize )
			{
				RED_ASSERT( page->m_elements[pushIndex] == nullptr, "Pushing to taken slot" );
				page->m_elements[pushIndex] = std::forward<TQueueElement>(element);
				break;
			}

			// Page is full, so back out our increment, start a new page, and try again.
			page->m_size.Decrement();
		}

		{
			RED_SCOPE_LOCK( m_commmitConsumeRWLock );

			TPage* lastPage = queue.m_lastPage;
			CpRchStressPause( 1500 ); // [hang] stress 2: hold the read-tail -> link-new-page window open
			if ( lastPage->m_size.GetValue() >= lastPage->c_maxSize )
			{
				RED_SCOPE_LOCK( m_freeListLock );

				if ( freePages == nullptr )
				{
					freePages = RED_NEW( TPage )();
				}
				TPage* newPage = freePages;
				// [hang] a free page that is the current tail or still holds elements means the list is corrupt
				if ( s_cpRchFix && ( newPage == lastPage || newPage->m_size.GetValue() != ( Int32 )TPage::c_padding ) )
				{
					CpRchReport( "PushToQueue corrupt free page", lastPage, newPage, newPage->m_size.GetValue(), cpSpins );
					newPage = RED_NEW( TPage )();
					freePages = nullptr;
				}
				else
				{
					freePages = newPage->m_next;
				}

				newPage->m_next = nullptr;
				RED_ASSERT( newPage->m_size.GetValue() == TPage::c_padding, "New page should not have any elements." );

				lastPage->m_next = newPage;
				queue.m_lastPage = newPage;
			}
		}
	}
}

// Method designated for dealing with in-order and parallel IRender commands.
template<typename TQueue, typename TQueueElement>
RED_INLINE void CRenderCommandHandler::PushIRenderCommandToQueue( TQueueElement&& element, TQueue& queue, typename TQueue::Page*& freePages )
{
	RED_STATIC_ASSERT( alignof( TQueueElement ) <= 16 );

	// check if the command will fit the queue page
	RED_STATIC_ASSERT( sizeof( TQueueElement ) + sizeof( Uint16 ) <= TQueue::c_maxSize );
	
	using TPage = typename TQueue::Page;

	for (;;)
	{
		constexpr Uint16 sizeOfElement = sizeof( TQueueElement );
		constexpr Uint16 totalEntrySize = sizeOfElement + sizeof( Uint16 );
		{
			RED_SCOPE_SHARED_LOCK( m_commmitConsumeRWLock );

			// We will be adding to the last page
			TPage* page = queue.m_lastPage;

			// Allocation offset

			for ( ;; )
			{
				const Uint32 pushIndex = page->m_size.GetValue();
				// Check if the memory for the actual data is aligned
				RED_ASSERT( (pushIndex + sizeof( Uint16 )) % 16 == 0, "Assuming alignment of 16." );

				// Move to the next block
				const Uint16 alignedBlobSize = red::AlignUp( totalEntrySize, ( Uint16 )16 );
				const Uint32 alocatedBlockSize = pushIndex + alignedBlobSize;

				if ( alocatedBlockSize <= page->c_maxSize )
				{
					// We fit
					if ( page->m_size.CompareExchange( alocatedBlockSize, pushIndex ) == pushIndex )
					{
						// We fit, and we own this chunk of memory
						// [SizeOfTheStruct][STRUCT_ITSELF]
						auto blobSize = new(&page->m_elements[pushIndex]) Uint16( alignedBlobSize );
						auto blobData = new(&page->m_elements[pushIndex + sizeof( Uint16 )]) typename std::remove_reference<TQueueElement>::type( std::move( element ) );
						
						return;
					}
				}
				else
				{
					// We don't fit!
					break;
				}
			}
		}

		{
			RED_SCOPE_LOCK( m_commmitConsumeRWLock );

			TPage* lastPage = queue.m_lastPage;
			if ( lastPage->m_size.GetValue() + sizeOfElement + sizeof( Uint16 ) >= lastPage->c_maxSize )
			{
				RED_SCOPE_LOCK( m_freeListLock );

				if ( freePages == nullptr )
				{
					freePages = RED_NEW( TPage )();
				}
				TPage* newPage = freePages;
				// [hang] same corrupt free-list guard as PushToQueue
				if ( s_cpRchFix && ( newPage == lastPage || newPage->m_size.GetValue() != ( Int32 )TPage::c_padding ) )
				{
					CpRchReport( "PushIRenderCommandToQueue corrupt free page", lastPage, newPage, newPage->m_size.GetValue(), 0 );
					newPage = RED_NEW( TPage )();
					freePages = nullptr;
				}
				else
				{
					freePages = newPage->m_next;
				}

				newPage->m_next = nullptr;
				RED_ASSERT( newPage->m_size.GetValue() == TPage::c_padding, "New page should not have any elements." );

				lastPage->m_next = newPage;
				queue.m_lastPage = newPage;
			}
		}
	}
}

template<typename TQueue>
RED_INLINE typename TQueue::Page* CRenderCommandHandler::AcquireQueue( TQueue& queue, typename TQueue::Page*& freePages )
{
	using TPage = typename TQueue::Page;

	RED_ASSERT( queue.m_firstPage != nullptr, "Queue should have at least one page." );
	TPage* result = nullptr;
	if ( queue.m_firstPage->m_size.GetValue() > TPage::c_padding )
	{
		result = queue.m_firstPage;
		{
			RED_SCOPE_LOCK( m_freeListLock );

			if ( freePages == nullptr )
			{
				freePages = RED_NEW( TPage )();
			}
			queue.m_firstPage = queue.m_lastPage = freePages;
			freePages = queue.m_firstPage->m_next;

			RED_ASSERT( queue.m_firstPage->m_size.GetValue() == TPage::c_padding, "New page should not have any elements." );
			queue.m_firstPage->m_next = nullptr;
		}
	}
	else
	{
		result = nullptr;
	}
	return result;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


CRenderCommandHandler::CRenderCommandHandler()
	: m_flushCounter( job::Priority::RenderPath )
	, m_flushCounterForProxyRemovalsFromPrevFrame( job::Priority::Latent )
{
	CpRchInitSwitches();
	m_inOrderQueue.m_firstPage = m_inOrderQueue.m_lastPage = RED_NEW( TInOrderQueue::Page )();
	m_subsceneQueue.m_firstPage = m_subsceneQueue.m_lastPage = RED_NEW( TSubsceneQueue::Page )();
	m_drawBufferQueue.m_firstPage = m_drawBufferQueue.m_lastPage = RED_NEW( TDrawBufferQueue::Page )();
	for ( Uint32 queueType = 0; queueType < IRenderCommand::PQT_MAX; ++queueType )
	{
		for ( Uint32 i = 0; i < c_parallelQueues; ++i )
		{
			m_parallelQueue[queueType][i].m_firstPage = m_parallelQueue[queueType][i].m_lastPage = RED_NEW( TParallelQueue::Page )( );
		}
	}

	m_inOrderFreePages = nullptr;
	for ( Uint32 queueType = 0; queueType < IRenderCommand::PQT_MAX; ++queueType )
	{
		m_parallelFreePages[queueType] = nullptr;
	}
	m_subsceneFreePages = nullptr;
	m_drawBufferFreePages = nullptr;
}

CRenderCommandHandler::~CRenderCommandHandler()
{
	m_inOrderQueue.m_lastPage->m_next = m_inOrderFreePages;
	m_inOrderFreePages = m_inOrderQueue.m_firstPage;
	m_inOrderQueue.m_firstPage = nullptr;

	m_subsceneQueue.m_lastPage->m_next = m_subsceneFreePages;
	m_subsceneFreePages = m_subsceneQueue.m_firstPage;
	m_subsceneQueue.m_firstPage = nullptr;

	m_drawBufferQueue.m_lastPage->m_next = m_drawBufferFreePages;
	m_drawBufferFreePages = m_drawBufferQueue.m_firstPage;
	m_drawBufferQueue.m_firstPage = nullptr;

	for ( Uint32 queueType = 0; queueType < IRenderCommand::PQT_MAX; ++queueType )
	{
		for ( Uint32 i = 0; i < c_parallelQueues; ++i )
		{
			m_parallelQueue[queueType][i].m_lastPage->m_next = m_parallelFreePages[queueType];
			m_parallelFreePages[queueType] = m_parallelQueue[queueType][i].m_firstPage;
			m_parallelQueue[queueType][i].m_firstPage = nullptr;
		}
	}

	while ( m_inOrderFreePages != nullptr )
	{
		TInOrderQueue::Page* nextPage = m_inOrderFreePages->m_next;
		RED_DELETE( m_inOrderFreePages );
		m_inOrderFreePages = nextPage;
	}
	while ( m_subsceneFreePages != nullptr )
	{
		TSubsceneQueue::Page* nextPage = m_subsceneFreePages->m_next;
		RED_DELETE( m_subsceneFreePages );
		m_subsceneFreePages = nextPage;
	}
	while ( m_subsceneFreePages != nullptr )
	{
		TSubsceneQueue::Page* nextPage = m_subsceneFreePages->m_next;
		RED_DELETE( m_subsceneFreePages );
		m_subsceneFreePages = nextPage;
	}
	while ( m_drawBufferFreePages != nullptr )
	{
		TDrawBufferQueue::Page* nextPage = m_drawBufferFreePages->m_next;
		RED_DELETE( m_drawBufferFreePages );
		m_drawBufferFreePages = nextPage;
	}
}

template<typename Func>
void CRenderCommandHandler::CommitCommand( Func&& cmd )
{
	const Bool isParallel = (cmd.GetProxyPtr() != nullptr);

	if ( isParallel )
	{
		const Uint32 parallelBucketIndex = Uint32( uintptr_t( cmd.GetProxyPtr() ) % c_parallelQueues );
		IRenderCommand::ParallelQueueType parallelQueueType = cmd.GetParallelQueueType();
		PushIRenderCommandToQueue( cmd, m_parallelQueue[parallelQueueType][parallelBucketIndex], m_parallelFreePages[parallelQueueType] );
	}
	else
	{
		PushIRenderCommandToQueue( cmd, m_inOrderQueue, m_inOrderFreePages );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 


// #define ENABLE_RENDER_COMMAND_HANDLER_LOGGING

// The method perform an Execute on the sequential queue
void CRenderCommandHandler::FlushInOrderCommands( TInOrderQueue::Page* pages )
{
	PC_SCOPE( FlushInOrderCommands );
	RED_ASSERT( pages != nullptr, "Shouldn't be flushing without any pages!" );

	Uint32 totalElements = 0;
	Uint32 numPages = 0;
	Uint32 maxPerPage = 0;

	TInOrderQueue::Page* lastPage = pages;
	for ( TInOrderQueue::Page* page = pages; page != nullptr; page = page->m_next )
	{
		const Uint32 numElements = page->m_size.GetValue();
		maxPerPage = Max( maxPerPage, numElements );
		totalElements += numElements;
		numPages += 1;

		// Point the data to where the size is
		ExecuteAllCommands( page );
		RED_VERIFY( numElements == page->m_size.Exchange( TInOrderQueue::Page::c_padding ) );
		lastPage = page;
	}

#ifdef ENABLE_RENDER_COMMAND_HANDLER_LOGGING
	RED_LOG_INFO( "FlushInOrderCommands: %u elements, %u pages, %u max per page, %f per page", totalElements, numPages, maxPerPage, (Float)totalElements / (Float)numPages );
#endif

	{
		RED_SCOPE_LOCK( m_freeListLock );

		lastPage->m_next = m_inOrderFreePages;
		m_inOrderFreePages = pages;
	}
}


// The method perform an Execute on the parallel queue
void CRenderCommandHandler::FlushParallelCommands( TParallelQueue::Page* pages, IRenderCommand::ParallelQueueType parallelQueueType )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( FlushParallelCommands );
	RED_ASSERT( pages != nullptr, "Shouldn't be flushing without any pages!" );

	Uint32 totalElements = 0;
	Uint32 numPages = 0;
	Uint32 maxPerPage = 0;

	TParallelQueue::Page* lastPage = pages;
	for ( TParallelQueue::Page* page = pages; page != nullptr; page = page->m_next )
	{
		const Uint32 numElements = page->m_size.GetValue();
		maxPerPage = Max( maxPerPage, numElements );
		totalElements += numElements;
		numPages += 1;

		// Point the data to where the size is
		ExecuteAllCommands( page );
		RED_VERIFY( numElements == page->m_size.Exchange( TInOrderQueue::Page::c_padding ) );
		lastPage = page;
	}

#ifdef ENABLE_RENDER_COMMAND_HANDLER_LOGGING
	RED_LOG_INFO( "FlushParallelCommands (%u queueType): %u elements, %u pages, %u max per page, %f per page", (Uint32)parallelQueueType, totalElements, numPages, maxPerPage, (Float)totalElements / (Float)numPages );
#endif

	{
		RED_SCOPE_LOCK( m_freeListLock );

		lastPage->m_next = m_parallelFreePages[parallelQueueType];
		m_parallelFreePages[parallelQueueType] = pages;
	}
}

void CRenderCommandHandler::FlushDrawBufferCommands( const TRenderPtr<IRenderFrame>& frame, job::Builder& builder, TDrawBufferQueue::Page* pages )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( FlushSubsceneCommands );
	RED_ASSERT( pages != nullptr, "Shouldn't be flushing without any pages!" );

	builder.DispatchJob( "RenderScene/FlushDrawBuffer", []( const job::RunContext& )
	{
		GetRenderer()->GetBatchDataAllocator().NewAllocFrame();
	} );

	// NOTE: first command list is reserved for glyph cache update
	Uint32 numCommandLists = 1;

	auto flushBatch = [&] ()
	{
		builder.DispatchFenceExplicitly();

		builder.DispatchJob( "RenderScene/FlushDrawBuffer", [this, numCommandLists] ( const job::RunContext& runContext )
		{
			m_drawBufferCommandLists[0] = GenerateCommandlistForGlpyhCacheUpload();

			job::Builder builder( runContext );
			GpuApi::CloseAndSubmitCommandLists( "Submit_DrawBuffers", red::ArraySpan<GpuApi::CommandListRef>( m_drawBufferCommandLists.TypedData(), numCommandLists ), GpuApi::CommandListSyncType::None, builder );
		} );

		numCommandLists = 1;
	};

	TDrawBufferQueue::Page* lastPage = pages;
	for ( TDrawBufferQueue::Page* page = pages; page != nullptr; page = page->m_next )
	{
		const Uint32 numElements = page->m_size.GetValue();

		for ( Uint32 idx = 0; idx < numElements; idx++ )
		{
			red::SharedPtr<rend::DrawBuffer>& drawBuffer = page->m_elements[idx];
			RED_ASSERT( drawBuffer, "Only valid draw buffers expected." );

			const Bool isFirst = ( idx == 0 ) && ( page == pages );
			const Bool isLast = ( idx + 1 == numElements ) && ( page->m_next == nullptr );

			builder.DispatchJob< job::Fence::None >( "RenderScene/FlushDrawBuffer",
				[ this, numCommandLists, page, frame, drawBuffer=std::move(drawBuffer), isFirst, isLast ]
			( const job::RunContext& runContext ) mutable
			{
				m_drawBufferCommandLists[numCommandLists] = ProcessDrawBuffer( frame, runContext, std::move( drawBuffer ), isFirst, isLast );
			} );

			++numCommandLists;

			// flush batch
			if ( numCommandLists >= MaxDrawBuffersInBatch )
			{
				flushBatch();
			}
		}

		RED_VERIFY( numElements == page->m_size.Exchange( 0 ) );
		lastPage = page;
	}

	builder.DispatchFenceExplicitly();

	builder.DispatchJob( "RenderScene/Cleanup", [this]( const job::RunContext& runContext )
	{
		GetRenderer()->GetBatchDataAllocator().Cleanup();
	} );

	if ( numCommandLists > 1 )
	{
		flushBatch();
	}

	{
		RED_SCOPE_LOCK( m_freeListLock );

		lastPage->m_next = m_drawBufferFreePages;
		m_drawBufferFreePages = pages;
	}
}

void CRenderCommandHandler::FlushSubsceneCommands( job::Builder& builder, TSubsceneQueue::Page* pages )
{
	PC_SCOPE( FlushSubsceneCommands );
	RED_ASSERT( pages != nullptr, "Shouldn't be flushing without any pages!" );

	TSubsceneQueue::Page* lastPage = pages;
	for ( TSubsceneQueue::Page* page = pages; page != nullptr; page = page->m_next )
	{
		const Uint32 numElements = page->m_size.GetValue();

		for ( Uint32 idx = 0; idx < numElements; idx++ )
		{
			TRenderPtr<IRenderFrame>& frame = page->m_elements[idx];
			RED_ASSERT( frame, "Only valid frames expected." );

			builder.DispatchJob( "RenderScene/IssueRenderSubScene", [frame]( const job::RunContext& runContext )
			{
				SRenderFrameContext ctx( runContext, frame );
				GetRenderer()->RenderFrame( ctx );
			} );
			frame = nullptr;
		}

		RED_VERIFY( numElements == page->m_size.Exchange( 0 ) );
		lastPage = page;
	}

	{
		RED_SCOPE_LOCK( m_freeListLock );

		lastPage->m_next = m_subsceneFreePages;
		m_subsceneFreePages = pages;
	}
}

template<typename Page>
void CRenderCommandHandler::ExecuteAllCommands( Page* page )
{
	PC_SCOPE( FlushHelper );

	// Just execute the commands and nullify the ptrs afterwards
	RED_ASSERT( page->m_elements + TInOrderQueue::Page::c_padding, "Null queue is not expected." );

	// blobOffset points to beginning of the entry blob
	// first 16 bytes are dedicated for the blob size - padding of 14 is required
	Uint32 blobOffset = TInOrderQueue::Page::c_padding;

	const Uint32 elementsSizeInPage = page->m_size.GetValue();
	while ( blobOffset < elementsSizeInPage )
	{
		// Offset to current data blob size 
		Uint8* currentDataBlobSizeOffset = &page->m_elements[blobOffset];

		// Cast Uint8 to Uint16
		Int16 dataSize = *((Int16*)(currentDataBlobSizeOffset));

		// Data blob is located after the data size ( Uint16 )
		const Int32 currentDataBlobOffset = blobOffset + sizeof( Uint16 );
		// Get the pointer for the current blob data
		Uint8* data = &page->m_elements[currentDataBlobOffset];
		// Cast the raw data to IRenderCommandme 
		IRenderCommand* cmd = (IRenderCommand*)data;

		cmd->Execute();
		cmd->~IRenderCommand();
		
		// Update the offset for the next element
		blobOffset = blobOffset + dataSize;
	}
}

#define DISABLE_SEPARATE_RELEASE_PROXY_JOBS

void CRenderCommandHandler::DispatchQueueFlushJobs( job::Builder& builder )
{
	// Kick off processing of all "general" render commands and all per-proxy render commands. Let them interleave, no worries here.
	// Sobiecki 04.06.2019 - actually not true, since general render commands may inside affect proxies as well.

	if ( TInOrderQueue::Page* inOrderPages = AcquireQueue( m_inOrderQueue, m_inOrderFreePages ) )
	{
		//RED_MESSAGE( "Removed Fence::None to fix crash  on dispatch, to be rethought over by rendering team post E3. Consulted with Tim Green" );
		// #TODO: Might be nice to remove the synchronization again, so InOrder and Parallel commands can run together.
		// Had problems with updating cameras (which touches camera custom data), while adding/removing render proxies
		// (which can also touch camera custom data).
		builder.DispatchJob( "RenderScene/FlushInOrder", [this, inOrderPages] ( const job::RunContext& runContext )
		{
			FlushInOrderCommands( inOrderPages );
		} );
	}

#ifndef DISABLE_SEPARATE_RELEASE_PROXY_JOBS
	//for safety to be sure that previous frame proxy releases have already finished and to not flood the queue with those jobs as their priority is Latent
	builder.DispatchWait( m_flushCounterForProxyRemovalsFromPrevFrame );
#endif

		CRenderInterface::SPendingSceneMap pendingScenes = GetRenderer()->GetPendingProxiesAndAdvanceFrame(); //this has to stay outside of the job, otherwise we're getting some weird crashes...
		builder.DispatchJob< job::Fence::None >( "RenderScene/RemoveAndAddPendingProxiesToScenes", [this, pendingScenes = std::move( pendingScenes )]( const job::RunContext& runContext )
		{
			for ( const CRenderInterface::TPendingScenesMap::ElementType& mapObj : pendingScenes.m_pendingSceneMap )
			{
				CRenderSceneEx* scene = static_cast<CRenderSceneEx*>( mapObj.Key().m_scene.Get() );
				CRenderInterface::PendingSceneProxiesOperationsData& sceneProxyOperations = const_cast<CRenderInterface::PendingSceneProxiesOperationsData&>( mapObj.Value() );

				struct PendingProxiesToRemoveSortFun
				{
					RED_INLINE Bool operator()( const CRenderInterface::SPendingProxyOpPair& entry1, const CRenderInterface::SPendingProxyOpPair& entry2 ) const
					{
						if ( entry1.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_NONE || entry2.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_NONE )
						{
							return false;
						}

						if ( entry1.m_op == entry2.m_op && entry1.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_ADDITION )
						{
							return entry1.m_clusterInd < entry2.m_clusterInd;
						}

						if ( Min( entry1.m_op, entry2.m_op ) > CRenderInterface::SceneStaticBVHOperationType::BVH_OP_ADDITION )
						{
							//each of them is BVH_move or BVH_Remove 
							const vis::MainSceneAccelerationStructure::LeafKey& entry1LeafKey = static_cast<const IRenderProxyBase*>( entry1.m_proxy.Get() )->GetBVHNodeID();
							const vis::MainSceneAccelerationStructure::LeafKey& entry2LeafKey = static_cast<const IRenderProxyBase*>( entry2.m_proxy.Get() )->GetBVHNodeID();
							if ( entry1LeafKey.m_nodeIndex > entry2LeafKey.m_nodeIndex )
								return true;
							if ( entry1LeafKey.m_nodeIndex == entry2LeafKey.m_nodeIndex && entry1LeafKey.m_objectIndex > entry2LeafKey.m_objectIndex )
								return true;
							return false;
						}

						if ( entry1.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL || entry1.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_MOVE )
							return true;
						return false;
					}
				};

				Uint32 removeProxyOpsCount = 0;
				{
					PC_SCOPE( "Prepare scene proxy addition / removal" );

					//Merge operations for the same proxies and sort

					red::HashMap<IRenderProxy*, Uint32 > proxyOpIndMap{ job::PoolJobScope() };
					proxyOpIndMap.Reserve( sceneProxyOperations.m_sortedProxyOperations.Size() );

					for ( Uint32 i = 0; i < sceneProxyOperations.m_sortedProxyOperations.Size(); ++i )
					{
						CRenderInterface::SPendingProxyOpPair& opPair = sceneProxyOperations.m_sortedProxyOperations[i];

						const auto insertionResult = proxyOpIndMap.Insert( opPair.m_proxy.Get(), i );
						if ( !insertionResult.IsSuccessful() )
						{
							Uint32 existingOpInd = insertionResult.Iterator().Value();
							sceneProxyOperations.m_sortedProxyOperations[existingOpInd].AddOperation( static_cast<CRenderInterface::SceneStaticBVHOperationType>( opPair.m_op ) );
							if ( opPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL )
							{
								removeProxyOpsCount--;
							}
							opPair.m_op = CRenderInterface::SceneStaticBVHOperationType::BVH_OP_NONE;
							opPair.m_proxy = nullptr;
						}
						else if ( opPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL )
						{
							removeProxyOpsCount++;
						}
					}

					std::sort( sceneProxyOperations.m_sortedProxyOperations.Begin(), sceneProxyOperations.m_sortedProxyOperations.End(), PendingProxiesToRemoveSortFun() );
				}

				red::DynArray<CRenderFrameCameraStorage::SCameraData*> camData{ job::PoolJobScope() };
				scene->GetCameraStorage()->GetAllCameraData( camData );

#ifdef VIS_BVH_PRINT_DEBUG_LOG
				scene->m_rpMainSceneProxies.AdvanceLogFrame();
				red::StringBuilder<red::String> logMsg;
#endif

				//First remove proxies
				{
					PC_SCOPE( "Remove proxies / Move in VisGrid" );

#ifndef DISABLE_SEPARATE_RELEASE_PROXY_JOBS
					red::DynArray<TRenderPtr<IRenderProxy>> proxiesToRelease{ job::PoolJobScope() };
					proxiesToRelease.Reserve( removeProxyOpsCount );
#endif
					for ( auto& proxyOpPair : sceneProxyOperations.m_sortedProxyOperations )
					{
						if ( proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL || proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_MOVE )
						{
							IRenderProxyBase* proxyBase = static_cast<IRenderProxyBase*>( proxyOpPair.m_proxy.Get() );

#ifdef VIS_BVH_PRINT_DEBUG_LOG
							logMsg.Append( red::String::Printf( "Proxy: op=%d, idInScene=%d, bvhNodeInd=%d, bvhObjectInd=%d\n", (Int32)proxyOpPair.m_op, proxyBase->GetIdInScene(),
																proxyBase->GetBVHNodeID().m_nodeIndex, proxyBase->GetBVHNodeID().m_objectIndex ) );
#endif
							if ( proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL )
							{
								scene->RemoveProxy( proxyBase, camData );
							}
							else if ( proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_MOVE )
							{
								scene->m_rpMainSceneProxies.MoveObject( proxyBase->GetBVHNodeID(), proxyBase->GetBoundingBox() );
							}

#ifndef DISABLE_SEPARATE_RELEASE_PROXY_JOBS
							if ( proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL )
							{
								proxiesToRelease.PushBack( std::move( proxyOpPair.m_proxy ) );
							}
							else
#endif
							{
								proxyOpPair.m_proxy = nullptr; //release the ref
							}
						}
					}

					//adjust bounding boxes in cells after all objects have moved or have been removed. Addition of object will quickly expand the cell's bounding box itself if needed
					scene->m_rpMainSceneProxies.RebalanceTree();

#ifndef DISABLE_SEPARATE_RELEASE_PROXY_JOBS
					job::Builder releaseProxyJobBuilder( job::Priority::Latent );
					const Uint32 releaseProxyBatchSize = 64;
					const Uint32 totalBatchesCount = ( proxiesToRelease.Size() + releaseProxyBatchSize - 1 ) / releaseProxyBatchSize;
					releaseProxyJobBuilder.DispatchParallelForJob( "RenderScene/ReleaseBatchOfRenderProxies", { totalBatchesCount },
																   [releaseProxyBatchSize, proxiesToRelease = std::move( proxiesToRelease )]( Uint32 batchIdx, const job::RunContext& context ) mutable
					{
						const Uint32 startIdx = batchIdx * releaseProxyBatchSize;
						const Uint32 endIdx = Min( proxiesToRelease.Size(), startIdx + releaseProxyBatchSize );
						for ( Uint32 i = startIdx; i < endIdx; ++i )
						{
							proxiesToRelease[i] = nullptr;
						}
					} );
					m_flushCounterForProxyRemovalsFromPrevFrame.Reset( releaseProxyJobBuilder.ExtractWaitCounter() );
#endif
				}

				//Then Add proxies
				{
					PC_SCOPE( "Add proxies" );
					for ( auto& proxyOpPair : sceneProxyOperations.m_sortedProxyOperations )
					{
						if ( proxyOpPair.m_op == CRenderInterface::SceneStaticBVHOperationType::BVH_OP_ADDITION )
						{
							IRenderProxyBase* proxyBase = static_cast<IRenderProxyBase*>( proxyOpPair.m_proxy.Get() );
							scene->AddProxy( proxyBase );
#ifdef VIS_BVH_PRINT_DEBUG_LOG
							logMsg.Append( red::String::Printf( "Proxy: op=%d, idInScene=%d, bvhNodeInd=%d, bvhObjectInd=%d\n", (Int32)proxyOpPair.m_op, proxyBase->GetIdInScene(),
																proxyBase->GetBVHNodeID().m_nodeIndex, proxyBase->GetBVHNodeID().m_objectIndex ) );
#endif
							//RED_LOG( "Proxy added: idInScene=%d, nodeInd=%d, objind=%d", proxyBase->GetIdInScene(), proxyBase->GetBVHNodeID().m_nodeIndex, proxyBase->GetBVHNodeID().m_objectIndex );
							proxyOpPair.m_proxy = nullptr; //release the ref
						}
					}
				}

#ifdef VIS_BVH_PRINT_DEBUG_LOG
				red::String logStr = logMsg.ToString();
				scene->m_rpMainSceneProxies.InsertLogMsg( logStr );
#endif
			}
		} );

	//Safe commands should be only those which never depend on any previous or future command from RenderScene/FlushParallel_NotSafeWithProxyAdditionAndRemoval
	// Should be executed completely independent of the commands in RenderScene/FlushParallel_NotSafeWithProxyAdditionAndRemoval.
	//Also safe commands should never modify bounding box or rely on data which is removed / created / modified in CRenderSceneEx::AddProxy or CRenderScene::RemoveProxy
	for ( Uint32 i = 0; i < c_parallelQueues; ++i )
	{
		if ( TParallelQueue::Page* parallelPages = AcquireQueue( m_parallelQueue[IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval][i], m_parallelFreePages[IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval] ) )
		{
			builder.DispatchJob< job::Fence::None >( "RenderScene/FlushParallel_SafeWithProxyAdditionAndRemoval", [this, parallelPages]( const job::RunContext& runContext )
			{
				FlushParallelCommands( parallelPages, IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval );
			} );
		}
	}

	//Could we somehow allow the safe parallel queue jobs run simultaneously with not safe ones below and dispatch wait here only for the RenderScene/RemoveAndAddPendingProxiesToScenes job?
	builder.DispatchFenceExplicitly(); 

	Bool didCommitAnything = false;
	for ( Uint32 i = 0; i < c_parallelQueues; ++i )
	{
		if ( TParallelQueue::Page* parallelPages = AcquireQueue( m_parallelQueue[IRenderCommand::PQT_NotSafeWithProxyAdditionAndRemoval][i], m_parallelFreePages[IRenderCommand::PQT_NotSafeWithProxyAdditionAndRemoval] ) )
		{
			builder.DispatchJob< job::Fence::None >( "RenderScene/FlushParallel_NotSafeWithProxyAdditionAndRemoval", [this, parallelPages] ( const job::RunContext& runContext )
			{
				FlushParallelCommands( parallelPages, IRenderCommand::PQT_NotSafeWithProxyAdditionAndRemoval );
			} );

			didCommitAnything = true;
		}
	}

	if ( didCommitAnything )
	{
		builder.DispatchFenceExplicitly();
	}
}

void CRenderCommandHandler::DispatchSubsceneRenderingFlushJobs( const TRenderPtr<IRenderFrame>& frame, job::Builder& builder )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	// Render sub-scenes
	if ( TSubsceneQueue::Page* subscenePages = AcquireQueue( m_subsceneQueue, m_subsceneFreePages ) )
	{
		// Go through and let all subscenes prepare. They check for duplicate prepares internally, so can just hit
		// everything in the queue.
		const Uint32 tickCounter = GetRenderer()->GetLastTickCounter();
		for ( TSubsceneQueue::Page* page = subscenePages; page != nullptr; page = page->m_next )
		{
			for ( Uint32 i = 0, count = page->m_size.GetValue(); i < count; ++i )
			{
				if ( auto scene = page->m_elements[ i ]->GetFrameInfo().GetScene() )
				{
					scene->PrepareForUpdate( tickCounter );
				}
			}
		}

		builder.DispatchJob( "RenderScene/FlushSubscenes", [this, subscenePages] ( const job::RunContext& runContext )
		{
			job::Builder depBuilder{ runContext };
			FlushSubsceneCommands( depBuilder, subscenePages );
		} );
	}
}

void CRenderCommandHandler::DispatchDrawBuffersFlushJobs( const TRenderPtr<IRenderFrame>& frame, job::Builder& builder )
{
	// Process draw buffers
	if ( TDrawBufferQueue::Page* drawBufferPages = AcquireQueue( m_drawBufferQueue, m_drawBufferFreePages ) )
	{
		FlushDrawBufferCommands( frame, builder, drawBufferPages );
	}
}

void CRenderCommandHandler::RetirePendingResources()
{
	PC_SCOPE( RetirePendingResources );

	job::Builder builder;

	RED_SCOPE_LOCK( m_commmitConsumeRWLock );

	// sync with previous frame
	builder.DispatchWait( m_flushCounter );

	DispatchQueueFlushJobs( builder );

	// After flushing commands, give a chance to let things go. Stripped down version of FrameTick.
	builder.DispatchJob( "Render/RetirePendingResources", []( const job::RunContext& runContext )
	{
		job::Builder childBuilder{ runContext };
		GetRenderer()->RetirePendingResources( childBuilder );
	} );

	m_flushCounter.Reset( builder.ExtractWaitCounter() );
}

void CRenderCommandHandler::FrameTick( red::ArraySpan < TRenderPtr<IRenderScene> > scenes, Bool flushSkinning )
{
	GetRenderer()->FrameTickPrepare( scenes );

	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( RenderFrameTickIssue );

	job::Builder builder{ job::Priority::RenderPath };

	RED_SCOPE_LOCK( m_commmitConsumeRWLock );

	// sync with previous frame
	builder.DispatchWait( m_flushCounter );

	// Schedule reseting pool frame - must be on synced job as for example TODVIS generator 
	// spits a lot of RenderFrames + FrameTicks and reseting them on thread that schedules them
	// may cause other frame on the fly to stomp.
	builder.DispatchJob( "RenderFrameTickResetPools", [] ( const job::RunContext& runContext )
	{
		rend::PoolFrameRendering::GetAllocator().Reset();
	} );

	DispatchQueueFlushJobs( builder );

	{// update scenes
		for( TRenderPtr<IRenderScene>& scenePtr : scenes )
		{
			CRenderSceneEx* sceneEx = static_cast<CRenderSceneEx*>(scenePtr.Get());
			sceneEx->ExecuteUpdateState( builder );
		}
	}

	// Finish with the main scene.
	builder.DispatchJob( "RenderFrameTick", [flushSkinning]( const job::RunContext& runContext )
	{
		job::Builder depBuilder { runContext };
		GetRenderer()->FrameTick( depBuilder, flushSkinning );
	} );

	m_flushCounter.Reset( builder.ExtractWaitCounter() );
}

void CRenderCommandHandler::RenderScene( const TRenderPtr<IRenderFrame>& frame )
{
	PC_SCOPE( RenderScene );

	RED_ASSERT( frame != nullptr, "Must provide a non-null render frame" );

	job::Builder builder{ job::Priority::RenderPath };

	RED_SCOPE_LOCK( m_commmitConsumeRWLock );

	// sync with previous frame
	builder.DispatchWait( m_flushCounter );
	
	DispatchQueueFlushJobs( builder );
	DispatchSubsceneRenderingFlushJobs( frame, builder );

#ifdef RENDER_ENABLE_RESOURCE_EVICT
	// If we're rendering a scene, and it's going on screen for a regular rendering, and an eviction was requested (such
	// as after a Settings change), we should process it.
	const CRenderFrameInfo& frameInfo = frame->GetFrameInfo();
	const Bool doEvict = (
		IRenderScene::IsValid( frameInfo.GetScene() ) &&
		frameInfo.m_present &&
		frameInfo.GetRenderingMode() == RM_Shaded &&
		frameInfo.GetFramePurpose() == FP_Normal &&
		GetRenderer()->ShouldEvictOnRender());
	if ( doEvict )
	{
		builder.DispatchJob( "RenderScene/Evict", [scenePtr = frameInfo.GetScenePtr()]( const job::RunContext& runContext )
		{
			job::Builder depBuilder { runContext };
			GetRenderer()->ProcessEvictOnRender( static_cast<CRenderSceneEx*>( scenePtr.Get() ) );
			GpuApi::FinishAllAndFlushDeletions( depBuilder );
		} );
	}
#endif

	builder.DispatchJob( "RenderScene/DispatchDrawBuffersFlush", [this, frame] ( const job::RunContext& runContext )
	{
		job::Builder builder{ job::Priority::RenderPath };
		builder.DispatchWait( m_drawBuffersWaitCounter ); // just in case, shouldn't be needed
		if ( s_cpRchStress >= 2 )
		{
			// [hang] stress 2: land the queue swap at a random point inside the next frame's ink pushes
			static std::atomic< Uint32 > s_cpRchSeed{ 12345 };
			const Uint32 r = s_cpRchSeed.fetch_add( 2654435761u ) * 2246822519u;
			usleep( ( r >> 8 ) % 12000 );
		}
		if ( s_cpRchFix )
		{
			// [hang] this job runs after RenderScene released the lock: swap the queue under it so no pusher
			// can be between reading m_lastPage and linking a new page.
			TDrawBufferQueue::Page* drawBufferPages = nullptr;
			{
				RED_SCOPE_LOCK( m_commmitConsumeRWLock );
				drawBufferPages = AcquireQueue( m_drawBufferQueue, m_drawBufferFreePages );
			}
			if ( drawBufferPages )
			{
				FlushDrawBufferCommands( frame, builder, drawBufferPages );
			}
		}
		else
		{
			DispatchDrawBuffersFlushJobs( frame, builder );
		}
		m_drawBuffersWaitCounter.Reset( builder.ExtractWaitCounter() );
	} );

	// Finish with the main scene.
	builder.DispatchJob( "RenderScene/FlushRenderScene", [frame]( const job::RunContext& runContext )
	{
		SRenderFrameContext ctx( runContext, frame );
		GetRenderer()->RenderFrame( ctx );
	} );

	m_flushCounter.Reset( builder.ExtractWaitCounter() );
}

void CRenderCommandHandler::SyncWithRenderCommands()
{
	job::Builder builder{ job::Priority::RenderPath };
	SyncWithRenderCommands( builder );

	// Wait until all this is done
	job::FlushCounter( builder.ExtractWaitCounter() );
}

void CRenderCommandHandler::SyncWithRenderCommands( job::Builder& builder )
{
	job::Builder depBuilder;

	RED_SCOPE_LOCK( m_commmitConsumeRWLock );

	// sync with previous frame
	depBuilder.DispatchWait( m_flushCounter );

	DispatchQueueFlushJobs( depBuilder );
	DispatchSubsceneRenderingFlushJobs( nullptr, depBuilder );
	DispatchDrawBuffersFlushJobs( nullptr, depBuilder );

	m_flushCounter.Reset( depBuilder.ExtractWaitCounter() );
	builder.DispatchWait( m_flushCounter );
}

void CRenderCommandHandler::RenderSubScene( const TRenderPtr<IRenderFrame>& frame )
{
	PC_SCOPE( RenderSubScene );
	PushToQueue( frame, m_subsceneQueue, m_subsceneFreePages );
}

void CRenderCommandHandler::SubmitDrawBuffer( const red::SharedPtr<rend::DrawBuffer>& drawBuffer )
{
	PC_SCOPE( SubmitDrawBuffer );
	if ( drawBuffer != nullptr )
	{
		PushToQueue( drawBuffer, m_drawBufferQueue, m_drawBufferFreePages );
	}
}

void CRenderCommandHandler::FlushPreviousFrameCommandsProcessing()
{
	job::FlushCounterOnProcessFrame( m_flushCounter );
}

////////////////////////////////////////////////////////////////////////// Actual commands below //////////////////////////////////////////////////////////////////////////

void CRenderCommandHandler::RegisterCamera( const TScenePtr& scene, const CRenderCamera& camera, const SCameraRenderViewport &cameraViewport, Uint64 cameraName, Bool pernament )
{
	// Do not register camera in case it has nothing to render.
	if( cameraViewport.m_renderArea.IsEmpty() )
	{
		return;
	}

	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDRegisterCamera );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->RegisterCamera( cameraName, camera, cameraViewport );
	} ) );
}

void CRenderCommandHandler::UnregisterCamera( const TScenePtr& scene, Uint64 cameraName )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUnregisterCamera );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->UnregisterCamera( cameraName );
	} ) );
}

void CRenderCommandHandler::AddCameraDependency( const TScenePtr& scene, Uint64 parentCamera, Uint64 childCamera )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDAddCameraDependency );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->AddCameraDependency( parentCamera, childCamera );
	} ) );
}

void CRenderCommandHandler::RemoveCameraDependency( const TScenePtr& scene, Uint64 parentCamera, Uint64 childCamera )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDRemoveCameraDependency );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->RemoveCameraDependency( parentCamera, childCamera );
	} ) );
}

void CRenderCommandHandler::UpdateCameraData( const TScenePtr& scene, Uint64 cameraName, const CRenderCameraFrameInfo& cameraInfo )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );

	CRenderCameraFrameInfo newInfo = cameraInfo;
	newInfo.m_renderScene = newInfo.m_renderScene ? newInfo.m_renderScene : scene.Get();

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateCameraData );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->UpdateCameraData( cameraName, newInfo );
	} ) );
}

void CRenderCommandHandler::MarkCameraCut( const TScenePtr& scene, Uint64 cameraName )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDMarkCameraCut );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->MarkCameraCut( cameraName );
	} ) );
}

void CRenderCommandHandler::AddProxyToScene( const TScenePtr& scene, const TProxyPtr& proxy )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	if ( scene ) //we should never get scene which is nullptr but adding this condition due to one random crash that occured just once out of nowhere on Final conf
	{
		GetRenderer()->AddPendingProxy( scene, proxy, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_ADDITION );
	}
}

void CRenderCommandHandler::AddProxiesToScene( const TScenePtr& scene, const red::ArraySpan< TRenderPtr<IRenderProxy> >& proxies )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	if ( scene ) //we should never get scene which is nullptr but adding this condition due to one random crash that occured just once out of nowhere on Final conf
	{
		GetRenderer()->AddPendingProxies( scene, proxies, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_ADDITION );
	}
}

void CRenderCommandHandler::RemoveProxyFromScene( const TScenePtr& scene, const TProxyPtr& proxy )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	if ( scene ) //we should never get scene which is nullptr but adding this condition due to one random crash that occured just once out of nowhere on Final conf
	{
		GetRenderer()->AddPendingProxy( scene, proxy, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL );
	}
}

void CRenderCommandHandler::RemoveProxiesFromScene( const TScenePtr& scene, const red::ArraySpan< TRenderPtr<IRenderProxy> >& proxies )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	if ( scene ) //we should never get scene which is nullptr but adding this condition due to one random crash that occured just once out of nowhere on Final conf
	{
		GetRenderer()->AddPendingProxies( scene, proxies, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_REMOVAL );
	}
}

void CRenderCommandHandler::MoveProxyInSceneVisStructure( const TScenePtr& scene, const TProxyPtr& proxy )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	
	GetRenderer()->AddPendingProxy( scene, proxy, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_MOVE );
}

void CRenderCommandHandler::MoveProxiesInSceneVisStructure( const TScenePtr& scene, const red::ArraySpan< TRenderPtr<IRenderProxy> >& proxies )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );

	GetRenderer()->AddPendingProxies( scene, proxies, CRenderInterface::SceneStaticBVHOperationType::BVH_OP_MOVE );
}

void CRenderCommandHandler::UpdateSecondaryRefPoint( const TProxyPtr& proxy, const Vector3& newSecondaryRefPoint )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( helper::IsProxyDrawable( proxy ) );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateSecondaryRefPoint );
		static_cast<IRenderProxyDrawable*>(proxy.Get())->SetSecondaryReferencePoint( newSecondaryRefPoint );
	}, static_cast<void*>(proxy.Get()) ) );
}

void CRenderCommandHandler::UpdateLocalShadowsInfo( const TScenePtr& scene, const TProxyPtr& proxy, Bool castsLocalShadows, Bool castShadowsEvenIfNotVisible )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( helper::IsProxyDrawable( proxy ) );
	RED_FATAL_ASSERT( proxy->GetType() == ERenderProxyType::RPT_Mesh /*|| proxy->GetType() == ERenderProxyType::RPT_Garment*/ );	// co do kurwy?

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [ = ] ()
	{
		PC_SCOPE( CMDUpdateLocalShadowsInfo );
		
		CRenderProxy_Mesh* mesh = static_cast< CRenderProxy_Mesh* >( proxy.Get() );
		RED_ASSERT( mesh );

		// In case leaf key is invalid, it means that the proxy was just detached from the scene and we should not update it. I think.
		auto& leafKey = mesh->GetBVHNodeID();
		if ( leafKey.IsValid() )
		{
			// Update render proxy mesh properties.
			mesh->SetCastLocalShadows( castsLocalShadows );
			mesh->SetCastLocalShadowsEvenIfNotVisible( castShadowsEvenIfNotVisible );

			// Update visibility grid.
			static_cast< CRenderSceneEx* >( scene.Get() )->m_rpMainSceneProxies.UpdateNodeInclusionMaskLocalShadowsSetup( leafKey /* Change this name? */, castsLocalShadows );
		}

	}, static_cast< void* >( proxy.Get() ) ) );
}

void CRenderCommandHandler::UpdateHitProxyID( const TProxyPtr& proxy, const IRenderSelectable* selectable )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	const Uint32 id = selectable ? selectable->Selectable_GetID() : 0;

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, id]()
	{
		PC_SCOPE( CMDUpdateHitProxyID );
		static_cast<IRenderProxyBase*>(proxy.Get())->SetGlobalSelectionID( id );
	}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::AppendVertexShaderCustomSlotParams( const TProxyPtr& proxy, const SVertexShaderCustomSlotParams& params )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDAppendVertexShaderCustomSlotParams );
		if( IRenderProxyBase* proxyBase = static_cast<IRenderProxyBase*>(proxy.Get()) )
		{
			proxyBase->AppendVertexShaderCustomSlotParams( params );
		}
	}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

#include "renderPostFx.h"
#include "renderPostFxSelection.h"

void CRenderCommandHandler::SetSelectionFlag( const TScenePtr& scene, const TProxyPtr& proxy, Bool flag, Uint8 selectionInfo )
{
	RED_FATAL_ASSERT( scene != nullptr, "NULL scene pointer" );
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDSetSelectionFlag );
		RenderSelection::Select( static_cast<const CRenderSceneEx*>(scene.Get()), red::StaticCast<IRenderProxyBase>( proxy ), flag, selectionInfo );
	} ) );
}

void CRenderCommandHandler::UpdateMeshDebugColor( const TProxyPtr& proxy, const Color newDebugColor )
{
#ifndef RED_CONFIGURATION_FINAL
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, newDebugColor]()
	{
		PC_SCOPE( CMDUpdateMeshDebugColor );
		static_cast<CRenderProxy_Mesh*>(proxy.Get())->SetDebugColor( newDebugColor );
	},  static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
#endif // RED_CONFIGURATION_FINAL
}

void CRenderCommandHandler::TakeScreenshot(
	const CRenderFrameGrab& frameInfoGrab,
	const String& filePath,
	ESaveFormat saveFormat,
	rend::OnScreenshotCreatedCallback onCreated,
	EFramePurpose framePurpose /*= FP_ScreenShot */ )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDTakeScreenshot );
		red::AbsolutePath screenshotFileName;
		if ( red::paths::IsAbsolutePath( filePath ) )
		{
			screenshotFileName = red::AbsolutePath::CreateFilePath( filePath );
		}
		else
		{
			const red::AbsolutePath screenshotDirectory = red::paths::GetUserDirectory().AddDirPath( "screenshots" );
			screenshotFileName = screenshotDirectory.AddFilePath( filePath );
		}
		SScreenshotParameters scrParams(
			frameInfoGrab.GetViewportWidth(),
			frameInfoGrab.GetViewportHeight(),
			saveFormat,
			4 );
		scrParams.m_absoluteFilePath = screenshotFileName;
		scrParams.m_onScreenshotCreated = onCreated;

		if( frameInfoGrab.m_highResoulutionScreenshotInterface )
		{
			GetRenderer()->TakeOneUberScreenshotNow( frameInfoGrab, scrParams );
		}
		else
		{
			GetRenderer()->TakeOneRegularScreenshotNow( frameInfoGrab, scrParams, framePurpose );
		}
	} ) );
}

void CRenderCommandHandler::TakeScreenshot(
	const class CRenderFrameGrab& frameInfoGrab,
	IFile* output,
	ESaveFormat saveFormat,
	rend::OnScreenshotCreatedCallback onCreatedCallback,
	EFramePurpose framePurpose /*= FP_ScreenShot */)
{
	RED_FATAL_ASSERT( !frameInfoGrab.m_highResoulutionScreenshotInterface, "Cannot use this function for high resolution screenshots" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=, f=frameInfoGrab, onCreatedCallback = std::move( onCreatedCallback )]() mutable
	{
		PC_SCOPE( CMDTakeScreenshot );

		SScreenshotParameters scrParams{
			f.GetViewportWidth(),
			f.GetViewportHeight(),
			saveFormat,
			4 };

		scrParams.m_outputFile = output;
		scrParams.m_onScreenshotCreated = std::move( onCreatedCallback );

		if ( !f.m_highResoulutionScreenshotInterface )
		{
			GetRenderer()->TakeOneRegularScreenshotNow(f, scrParams, framePurpose );
		}
	} ) );
}

void CRenderCommandHandler::ToggleContinuousScreenshot( Bool isEnabled, ESaveFormat saveFormat )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDToggleContinuousScreenshot );
		// Delete current one
		if( GScreenshotSequence )
		{
			RED_DELETE( GScreenshotSequence );
			GScreenshotSequence = nullptr;
		}

		// Create new grabber
		if( isEnabled )
		{
			GScreenshotSequence = RED_NEW( CRenderScreenshotSequnceGrabber )(saveFormat);
		}
	} ) );
}

void CRenderCommandHandler::ToggleGameplayEffect( const EPostFXEffect effect, Bool enabled )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDToggleGameplayEffect );
		if( GetRenderer()->GetPostProcess() )
		{
			GetRenderer()->GetPostProcess()->SetGameplayEffectEnabled( effect, enabled );
		}
	} ) );
}

void CRenderCommandHandler::DisableAllGameplayEffects()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, []()
	{
		PC_SCOPE( CMDDisableAllGameplayEffects );
		if( GetRenderer()->GetPostProcess() )
		{
			GetRenderer()->GetPostProcess()->DisableAllGameplayEffects();
		}
	} ) );
}

void CRenderCommandHandler::SetGameplayEffectParameters( const PostFXEffectParameters& params )
{
	if( params.effect >= EPE_MAX )
	{
		RED_LOG_WARNING( "PostFX: SetGameplayEffectParameters given invalid effect '%u'", params.effect );
		return;
	}

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDSetGameplayEffectParameters );
		if( GetRenderer()->GetPostProcess() )
		{
			GetRenderer()->GetPostProcess()->SetGameplayEffectParameters( params );
		}
	} ) );
}

void CRenderCommandHandler::SetVisionModeHighlight( const TScenePtr& scene, const TProxyPtr& proxy, Bool isHighlighted, Uint8 highlightInfo, Float opacityValue, Bool initial )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, proxy, isHighlighted, highlightInfo, opacityValue, initial ]()
	{
		PC_SCOPE( CMDSetVisionModeHighlight );
		CRenderPostProcess* pp = GetRenderer()->GetPostProcess();
		if( CGameplayEffect_CPFocusMode* focusMode = pp->GetGameplayEffect<CGameplayEffect_CPFocusMode>() )
		{
			focusMode->SetHighlight( static_cast<const CRenderSceneEx*>(scene.Get()), static_cast<IRenderProxyBase*>(proxy.Get())->AsDrawable(), isHighlighted, highlightInfo, opacityValue, initial );
		}
	} ) );
}

void CRenderCommandHandler::SetVisionModeScanning( const TScenePtr& scene, const TProxyPtr& proxy, rend::PostFx_ScanningState state )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, proxy, state]()
	{
		PC_SCOPE( CMDSetVisionModeScanning );
		CRenderPostProcess* pp = GetRenderer()->GetPostProcess();
		if( CGameplayEffect_CPFocusMode* focusMode = pp->GetGameplayEffect<CGameplayEffect_CPFocusMode>() )
		{
			focusMode->SetProxyScanState( static_cast<const CRenderSceneEx*>(scene.Get()), static_cast<IRenderProxyBase*>(proxy.Get())->AsDrawable(), state );
		}
	} ) );
}

void CRenderCommandHandler::ScreenFadeOut( const TScenePtr& scene, const Color& color, Float time )
{
	RED_FATAL_ASSERT( scene );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, color, time]()
	{
		PC_SCOPE( CMDScreenFadeOut );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->GetCustomData<SceneCustomData_ColorFadeParams>().FadeOut( color, time );
	} ) );
}

void CRenderCommandHandler::ScreenFadeIn( const TScenePtr& scene, Float time )
{
	RED_FATAL_ASSERT( scene );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, time]()
	{
		PC_SCOPE( CMDScreenFadeIn );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->GetCustomData<SceneCustomData_ColorFadeParams>().FadeIn( time );
	} ) );
}

void CRenderCommandHandler::ChangeSceneForcedLOD( const TScenePtr& scene, Int32 forceLOD )
{
	RED_ASSERT( scene != nullptr, "Must provide an IRenderScene" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, forceLOD]()
	{
		PC_SCOPE( CMDChangeSceneForcedLOD );
		static_cast<CRenderSceneEx*>(scene.Get())->SetForcedLOD( forceLOD );
	} ) );
}

void CRenderCommandHandler::SetProxyDissolveVisibility( const TProxyPtr& proxy, const Bool isVisibile, const Uint8 force )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=] ()
	{
		proxy->SetDissolveVisible( isVisibile, force );
	} ) );
}

void CRenderCommandHandler::DisableClusteredProxyInstances( const TProxyPtr& proxy, const red::BitSet64Dynamic& clusteredProxyInstances )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );

	RED_FATAL_ASSERT( proxy->GetType() == RPT_Mesh, "Only CRenderProxy_Mesh should be invoked with this handler" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, clusteredProxyInstances = std::move(clusteredProxyInstances)]()
	{
		static_cast<CRenderProxy_Mesh*>(proxy.Get())->ScheduleDisabledClusteredProxyInstances( clusteredProxyInstances );
	}, static_cast<void*>( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::ForceLODForProxy( const TProxyPtr& proxy, Int8 lod )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( helper::IsProxyDrawable( proxy ) );
	RED_FATAL_ASSERT( (Int32)lod < (Int32)CRenderLODSetup::MAX_LODS, "Invalid forced LOD: %d", (Int32)lod );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDForceProxyLOD );
		static_cast< IRenderProxyDrawable* >( proxy.Get() )->SetForcedLOD( lod );
	}));
}

void CRenderCommandHandler::ForceLODForProxies( const red::DynArray< TRenderPtr<IRenderProxy> >& proxies, Int8 lod )
{
	for ( auto& proxy : proxies )
	{
		RED_FATAL_ASSERT( helper::IsProxyDrawable( proxy ) );
	}
	RED_FATAL_ASSERT( (Int32)lod < (Int32)CRenderLODSetup::MAX_LODS, "Invalid forced LOD: %d", (Int32)lod );

	CommitCommand(MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDForceLODForProxies );
		for ( auto& proxy : proxies )
		{
			static_cast<IRenderProxyDrawable*>( proxy.Get() )->SetForcedLOD( lod );
		}
	}));
}

void CRenderCommandHandler::LODLevelScalesForProxy( const TProxyPtr& proxy, Uint32 lodLevelScales )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( helper::IsProxyDrawable( proxy ) );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]() 
	{
		PC_SCOPE( CMDBaseProxyLOD );
		static_cast< IRenderProxyDrawable* >( proxy.Get() )->SetLODLevelScales( lodLevelScales );
	} ) );
}

void CRenderCommandHandler::SuspendRendering()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, []()
	{
		PC_SCOPE( CMDSuspendRendering );
		GetRenderer()->Suspend();
	} ) );
}

void CRenderCommandHandler::ResumeRendering()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, []()
	{
		PC_SCOPE( CMDResumeRendering );
		GetRenderer()->Resume();
	} ) );
}

void CRenderCommandHandler::GamePaused()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, []()
	{
		GetRenderer()->GetSkinningManager()->Pause();
	} ) );
}

void CRenderCommandHandler::GameUnpaused()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, []()
	{
		GetRenderer()->GetSkinningManager()->Unpause();
	} ) );
}

#ifndef NO_EDITOR
void CRenderCommandHandler::SendDebugTexturePreview( const TScenePtr& scene, const TRenderPtr<IRenderResource>& texture, const DebugTexturePreviewDesc& desc )
{
	//RED_ASSERT( texture, "Must provide an IRenderResource for texture." );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, texture, desc]()
	{
		PC_SCOPE( CMDSendDebugTexturePreview );
		auto texturePtr = texture ? static_cast<const CRenderTextureBase*>(texture.Get()) : nullptr;

		SDynamicPreviewTexture settings;
		settings.m_texture			= texturePtr ? texturePtr->GetTextureRef() : GpuApi::GetInternalTexture(GpuApi::INTERTEX_Blank2D);
		settings.m_textureMip		= desc.mipIdx;
		settings.m_textureSlice		= desc.sliceIdx;
		settings.m_channelMask		= desc.channelMask;
		settings.m_numChannels		= desc.numChannels;
		settings.m_textureColorMin	= desc.min;
		settings.m_textureColorMax	= desc.max;
		settings.m_sampler			= desc.pointSample ? SDynamicPreviewTexture::Sampler::Point : SDynamicPreviewTexture::Sampler::Bilinear;
		settings.m_rect				= desc.rect;

		if( texturePtr && desc.allowInterpret )
		{
			switch( texturePtr->GetSetup().m_compression )
			{
			case TCM_Normalmap:
			{
				settings.m_interpretMode = SDynamicPreviewTexture::Interpret::Normals2Channel;
			} break;
			}
		}

		GetRenderer()->PushTexturePreviewFill( settings, static_cast<CRenderSceneEx*>(scene.Get()) );
	} ) );
}

void CRenderCommandHandler::EditorForceAutoHideDistance( const TProxyPtr& proxy, Bool forceNoAuothideDistance, Bool isGlobal )
{
	RED_ASSERT( proxy != nullptr, "Must provide an IRenderProxy" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, forceNoAuothideDistance, isGlobal]()
	{
		PC_SCOPE( CMDEditorForceAutoHideDistance );
		static_cast<IRenderProxyBase*>( proxy.Get() )->Editor_ForceAutoHideDistance( forceNoAuothideDistance, isGlobal );
	} ) );
}

void CRenderCommandHandler::UseExtendedDynamicRangeSky( const TScenePtr& scene, bool enable )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUseExtendedDynamicRangeSky );
		static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>().UseExtendedDynamicRangeSky( enable );
	} ) );
}
#endif

void CRenderCommandHandler::HandleResizeEvent( Uint32 width, Uint32 height )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDHandleResizeEvent );
		GetRenderer()->ResizeRenderSurfaces( width, height );
	} ) );
}

void CRenderCommandHandler::SetAutoFoliageRuntimeMap( const TRenderPtr<IRenderScene>& scene, red::UniquePtr<rend::AutoFoliageRuntimeMap>&& runtimeMap )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, runtimeMap =std::move( runtimeMap )]() mutable
	{
		PC_SCOPE( CMDSetAutoFoliageRuntimeMap );
		//First go through and detach all SpeedTreeWindSimulations from the current AutoFoliageRuntimeMap
		if( scene )
		{		
			if( rend::AutoFoliageRuntimeMap* foliageMapping = GetRenderer()->GetAutoFoliageRuntimeMap() )
			{
				CRenderSceneEx* renderScene = static_cast<CRenderSceneEx*>(scene.Get());
				for( const auto& item : foliageMapping->items )
				{
					if( TRenderPtr<CRenderProxy_Mesh> renderProxy = red::StaticCast<CRenderProxy_Mesh>( item.meshRenderProxy ) )
					{
						renderProxy->ConditionalDetachSpeedTreeWindSimulation( renderScene );
					}
				}
			}
		}
		
		GetRenderer()->SetAutoFoliageRuntimeMap( runtimeMap );

		if ( scene == nullptr )
		{
			RED_LOG_ERROR( "SetAutoFoliageRuntimeMap: Passed Scene was nullptr - couldn't set up wind simulations for auto generated meshes on terrain." );
		}
		else
		{
			if( rend::AutoFoliageRuntimeMap* foliageMapping = GetRenderer()->GetAutoFoliageRuntimeMap() )
			{
				CRenderSceneEx* renderScene = static_cast<CRenderSceneEx*>(scene.Get());
				for( const auto& item : foliageMapping->items )
				{
					if( TRenderPtr<CRenderProxy_Mesh> renderProxy = red::StaticCast<CRenderProxy_Mesh>( item.meshRenderProxy ) )
					{
						renderProxy->ConditionalAttachSpeedTreeWindSimulation( renderScene );
					}
				}
			}
		}
	} ) );
}

void CRenderCommandHandler::AddWindImpulseShapes( const TRenderPtr<IRenderScene>& scene, red::DynArray<rend::WindImpulseSphere>&& sphereShapes, red::DynArray<rend::WindImpulseCapsule>&& capsuleShapes )
{
	CWindImpulseCollector& windImpulseCollector = static_cast<CRenderSceneEx*>( scene.Get() )->GetCameraStorage()->GetCustomData<CWindImpulseCollector>();

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, sphereShapes = std::move( sphereShapes ), capsuleShapes = std::move(capsuleShapes) ]() mutable
	{
		PC_SCOPE( CMDAddWindImpulseShapes );
		CWindImpulseCollector& windImpulseCollector = static_cast<CRenderSceneEx*>( scene.Get() )->GetCameraStorage()->GetCustomData<CWindImpulseCollector>();
		if( sphereShapes.Size() > 0 )
		{
			windImpulseCollector.AddPendingSpheres( sphereShapes );
		}
		if ( capsuleShapes.Size() > 0 )
		{
			windImpulseCollector.AddPendingCapsules( capsuleShapes );
		}
	}, &windImpulseCollector, IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}
void CRenderCommandHandler::SetInteriorMapCellData( const TRenderPtr<IRenderScene>& scene, Uint64 hash, red::UniquePtr<RendInteriorMapData, rend::PoolEnvironment>&& data, Bool force )
{
    CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, hash, interiorMapData = std::move( data ), force ]() mutable
    {
		PC_SCOPE( CMDSetInteriorMapCellData );
        CRenderSceneEx* sceneEx = static_cast<CRenderSceneEx*>(scene.Get());
        sceneEx->SetInteriorMapCell( hash, std::move( interiorMapData ), force );
    } ) );
}

void CRenderCommandHandler::SetAutohideDistanceNear( const TProxyPtr& proxy, Float value )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( proxy->GetType() == RPT_Mesh );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		static_cast<IRenderProxyDrawable*>(proxy.Get())->SetAutohideDistanceNear( value );
	} ) );
}

void CRenderCommandHandler::UpdateParticleEffectContext( const TProxyPtr& proxy, const SParticleEffectContextUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_Particles, "Particle proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateParticleEffectContext );
			CRenderProxy_Particles* proxyParticles = static_cast<CRenderProxy_Particles*>(proxy.Get());
			proxyParticles->UpdateEffectContext( update );
		}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateFogVolumeEffectContext( const TProxyPtr& proxy, const SFogVolumeEffectContextUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_FogVolume, "FogVolume proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateFogVolumeEffectContext );
			CRenderProxy_FogVolume* proxyFog = static_cast<CRenderProxy_FogVolume*>(proxy.Get());
			proxyFog->UpdateEffectContext( update );
		}, static_cast<void*>(proxy.Get()) ) );
	}
}

void CRenderCommandHandler::UpdateMaterialParameterEffectContext( const TProxyPtr& proxy, const SMaterialParameterEffectContextUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_Mesh || proxy->GetType() == RPT_MorphTargetMesh, "Mesh proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateMaterialParameterEffectContext );
			CRenderProxy_Mesh* proxyMesh = static_cast<CRenderProxy_Mesh*>(proxy.Get());
			proxyMesh->UpdateMaterialParameterEffectContext( update );
		}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateDestructionRegionsEffectContext(const TProxyPtr& proxy, const SDestructionRegionsEffectContextUpdate& update )
{
	if (proxy != nullptr)
	{
		RED_ASSERT(proxy->GetType() == RPT_Mesh || proxy->GetType() == RPT_MorphTargetMesh, "Mesh proxy expected.");
		CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=, floats = update.m_regionPlayTimes ]() mutable
		{
			PC_SCOPE(CMDUpdateDestructionRegionsEffectContext);
			CRenderProxy_Mesh* proxyMesh = static_cast<CRenderProxy_Mesh*>(proxy.Get());
			proxyMesh->UpdateDestructionRegionsEffectContext( std::move(floats) );
		}, static_cast<void*>(proxy.Get())));
	}
}

void CRenderCommandHandler::UpdateLightParameterEffectContext( const TProxyPtr& proxy, const SLightParameterEffectContextUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_PointLight || proxy->GetType() == RPT_SpotLight || proxy->GetType() == RPT_AreaLight, "Light proxy expected." );
		if ( !update.HasOverrides() )
		{
			return;
		}
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateLightParameterEffectContext );
			IRenderProxyLight* proxyLight = static_cast<IRenderProxyLight*>(proxy.Get());
			proxyLight->UpdateParameter( update );

		}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateLightFlickerParams( const TProxyPtr& proxy, const SLightFlickerParamsUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_PointLight || proxy->GetType() == RPT_SpotLight || proxy->GetType() == RPT_AreaLight, "Light proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateLightFlickerParams );
			IRenderProxyLight* proxyLight = static_cast<IRenderProxyLight*>(proxy.Get());
			proxyLight->SetFlickerParams( update.m_flickerStrength, update.m_flickerPeriod, update.m_positionOffset );
		}, static_cast<void*>(proxy.Get()), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateColorGradeEffectContext( const SColorGradeEffectContextUpdate& update )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateColorGradeEffectContext );
		GetRenderer()->RefColorGradingLUT().OverrideColorGradeParameters( update.m_contrast, update.m_saturate, update.m_brightness );
		GetRenderer()->RefColorGradingLUT().OverrideLut( update.m_sdrLutParams, update.m_hdrLutParams, update.m_lutWeight );
	} ) );
}

void CRenderCommandHandler::UpdateTonemappingEffectContext( const STonemappingParams& tonemappingParams )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=] ()
	{
		PC_SCOPE( CMDUpdateColorGradeEffectContext );
		GetRenderer()->RefColorGradingLUT().OverrideTonemappingParameters( tonemappingParams );
	} ) );
}

void CRenderCommandHandler::RestoreTonemappingEffectContext()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=] ()
	{
		PC_SCOPE( CMDUpdateColorGradeEffectContext );
		GetRenderer()->RefColorGradingLUT().RestoreTonemappingParameters();
	} ) );
}

void CRenderCommandHandler::SaveTonemappingEffectContext()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=] ()
	{
		PC_SCOPE( CMDUpdateColorGradeEffectContext );
		GetRenderer()->RefColorGradingLUT().SaveTonemappingParameters();
	} ) );
}

void CRenderCommandHandler::UpdateChromaticAberrationEffectContext(const SChromaticAberrationParams& chromaticAberrationParams)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetChromaticAberration(chromaticAberrationParams);
	}));
}

void CRenderCommandHandler::UpdateFilmGrainEffectContext(const SFilmGrainParams& filmGrainParams)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetFilmGrain(filmGrainParams);
	}));
}

void CRenderCommandHandler::UpdateBloomnEffectContext(const SBloomParams& bloomParams)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetBloomParams(bloomParams);
	}));
}

void CRenderCommandHandler::UpdateVignetteEffectContext(const SVignetteParams& vignetteParams)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetVignetteParams(vignetteParams);
	}));
}

void CRenderCommandHandler::UpdateWeaponPlaneBlurEffectContext(const SWeaponPlaneBlurParams& weaponPlaneBlurParams)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetWeaponPlaneBlurParams(weaponPlaneBlurParams);
	}));
}

void CRenderCommandHandler::UpdateFOVEffectContext( const TScenePtr& scene, const Float fov )
{
	RED_FATAL_ASSERT( nullptr != scene, "No scene" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		static_cast<CRenderSceneEx*>( scene.Get() )->GetCameraStorage()->UpdateCameraFovModifier( fov );
	} ) );
}

void CRenderCommandHandler::UpdateDataMoshEffectContext(const SDataMoshParams& dataMoshParams)
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		GetRenderer()->SetDataMoshParams( dataMoshParams );
	} ) );
}

void CRenderCommandHandler::UpdateExposureScaleEffectContext( const SExposureScaleParams& exposureScaleParams )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=] ()
	{
		PC_SCOPE( CMDExposureScaleEffectContext );
		GetRenderer()->SetExposureScale( exposureScaleParams );
	} ) );
}

void CRenderCommandHandler::UpdateMotionBlurScaleEffectContext( Float motionBlurScale )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateMotionBlurScaleEffectContext );
		GetRenderer()->SetMotionBlurScale( motionBlurScale );
	} ) );
}

void CRenderCommandHandler::UpdateColorGradeV2EffectContext(const SColorGradeV2Params& colorGradeV2Params)
{
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateColorGradeV2EffectContext );
		GetRenderer()->RefColorGradingLUT().OverrideColorGradeParameters(colorGradeV2Params);
	} ) );
}

void CRenderCommandHandler::UpdateEmissiveEffectContext(const SEmissiveParams& sEmissiveParams)
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateEmissiveEffectContext );
		GetRenderer()->SetEmissive(sEmissiveParams);
	} ) );
}

void CRenderCommandHandler::UpdateDecalEffectContext( const TProxyPtr& proxy, const SDecalEffectContextUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_SSDecal, "Decal proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateDecalEffectContext );
			CRenderProxy_Decal* proxyDecal = static_cast<CRenderProxy_Decal*>(proxy.Get());
			proxyDecal->UpdateEffectContext( update );
		}, static_cast<void*>(proxy.Get()) ) );
	}
}

void CRenderCommandHandler::UpdateVehicleParams( const TProxyPtr& proxy, const SVehicleParamsUpdate& update )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_Mesh, "Mesh expected." );

		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUpdateVehicleParams );
			CRenderProxy_Mesh* proxyMesh = static_cast< CRenderProxy_Mesh* >( proxy.Get() );
			proxyMesh->SetVehicleParams( update );
		}, static_cast< void* >( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateClothMeshData(const TProxyPtr& proxy, Uint8 chunk, red::DynArray<Vector3>&& positions, Box bounds)
{
	if( proxy != nullptr )
	{
		RED_ASSERT( proxy->GetType() == RPT_Mesh, "Mesh proxy expected." );

		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=, copy=std::move(positions)]() mutable {
			PC_SCOPE( CMDUpdateClothMeshData );
			const auto& meshProxy = red::StaticCast<CRenderProxy_Mesh>(proxy);
			meshProxy->UpdateClothMeshData(chunk, copy, bounds);
		}, proxy.Get(), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::Add_SetDismembermentData( const TProxyPtr& proxy, const RenderDismCullObjectCB* data )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( (proxy->GetType() == RPT_Mesh) || (proxy->GetType() == RPT_MorphTargetMesh), "Mesh proxy expected." );
		if( data )
		{
			const RenderDismCullObjectCB dataCopy = *data;
			CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, dataCopy]()
			{
				PC_SCOPE( CMDAdd_SetDismembermentData );
				CRenderProxy_Mesh* meshProxy = static_cast<CRenderProxy_Mesh*>(proxy.Get());
				meshProxy->SetDismembermentData( &dataCopy );
			}, static_cast<void*>(proxy.Get()) ) );
		}
		else
		{
			CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy]()
			{
				PC_SCOPE( CMDAdd_ClearDismembermentData );
				CRenderProxy_Mesh* meshProxy = static_cast<CRenderProxy_Mesh*>(proxy.Get());
				meshProxy->SetDismembermentData( nullptr );
			}, static_cast<void*>(proxy.Get()) ) );
		}
		
	}
}

void CRenderCommandHandler::SetRainData( const TProxyPtr& proxy, const RenderRainData& rainData )
{
	if (proxy != nullptr)
	{
		RED_ASSERT( ( proxy->GetType() == RPT_Mesh ) || ( proxy->GetType() == RPT_MorphTargetMesh ), "Mesh proxy expected." );

		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, rainData]()
		{
			PC_SCOPE( CMDAdd_ClearDismembermentData );
			CRenderProxy_Mesh* meshProxy = static_cast< CRenderProxy_Mesh* >( proxy.Get() );
			meshProxy->SetRainData( rainData );
		}, static_cast< void* >( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::SuspendParticles( const TScenePtr& scene )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDSuspendParticles );
		static_cast<CRenderSceneEx*>(scene.Get())->SetParticlesSuspended( true );
	} ) );
}

void CRenderCommandHandler::ResumeParticles( const TScenePtr& scene )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDResumeParticles );
		static_cast<CRenderSceneEx*>(scene.Get())->SetParticlesSuspended( false );
	} ) );
}

void CRenderCommandHandler::SetMaterialParameterOverrides( const TProxyPtr& proxy, SMaterialSetupParameters&& params )
{
	if( proxy != nullptr )
	{
		RED_ASSERT( (proxy->GetType() == RPT_Mesh) || (proxy->GetType() == RPT_MorphTargetMesh), "Mesh proxy expected." );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=, params=std::move( params )]()
		{
			PC_SCOPE( CMDSetMaterialParameterOverrides );
			CRenderProxy_Mesh* meshProxy = static_cast<CRenderProxy_Mesh*>(proxy.Get());
			meshProxy->SetMaterialParameterOverrides( params );
		}, static_cast<void*>( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UpdateMaterialData( const TRenderPtr< IRenderResource >& material, SMaterialUpdateData&& updateData )
{
	if ( material )
	{
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=, params = std::move( updateData )]() mutable
		{
			PC_SCOPE( CMDUpdateMaterialData );
			CRenderMaterial* renderMaterial = static_cast< CRenderMaterial* >( material.Get() );
			renderMaterial->UpdateMaterialData( std::move(params), true );
		}, static_cast< void* >( material.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}


void CRenderCommandHandler::ApplyMorphTargetWeights( const TProxyPtr& morphTargetProxy, const red::DynArray< MorphTargets::MorphTargetWeight >& weights )
{
	if ( morphTargetProxy )
	{
		CommitCommand( MakeRenderCommandGeneric(DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDApplyMorphTargetWeights );
			CRenderProxy_MorphTargetMesh* renderProxy = static_cast< CRenderProxy_MorphTargetMesh* >( morphTargetProxy.Get() );
			renderProxy->ApplyMorphTargetWeights( weights );
		}, static_cast< void* >( morphTargetProxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::UnloadMorphData( const TProxyPtr& morphTargetProxy )
{
	if ( morphTargetProxy )
	{
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDUnloadMorphData );
			CRenderProxy_MorphTargetMesh* renderProxy = static_cast< CRenderProxy_MorphTargetMesh* >( morphTargetProxy.Get() );
			renderProxy->UnloadMorphData();
		}, static_cast< void* >( morphTargetProxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
	}
}

void CRenderCommandHandler::ResizeDynamicTexture( const TRenderPtr<IRenderResource>& dynamicTexture, Uint32 newWidth, Uint32 newHeight, Bool mipChain )
{
	if( dynamicTexture != nullptr )
	{
		RED_ASSERT( dynamicTexture->GetCategory() == RenderDynamicTexture::Category(), "Given object is not a RenderDynamicTexture" );
		RED_ASSERT( newWidth > 0 && newHeight > 0, "Invalid dimensions %ux%u, cannot create 0-sized texture", newWidth, newHeight );
		RED_ASSERT( newWidth < GpuApi::MAX_TEXTURE_2D_SIZE && newHeight < GpuApi::MAX_TEXTURE_2D_SIZE, "Invalid dimensions %ux%u, cannot be greater than %u", newWidth, newHeight, GpuApi::MAX_TEXTURE_2D_SIZE );
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
		{
			PC_SCOPE( CMDResizeDynamicTexture );
			RenderDynamicTexture* dynTex = static_cast<RenderDynamicTexture*>(dynamicTexture.Get());
			dynTex->Resize( newWidth, newHeight, mipChain );
		} ) );
	}
}

void CRenderCommandHandler::ResizeRenderSurfaces( Uint32 width, Uint32 height )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDResizeRenderSurfaces );
		GetRenderer()->ResizeRenderSurfaces( width, height );
	} ) );
}

void CRenderCommandHandler::UpdateMultilayerMask( TRenderPtr<RenderMultilayerMask>& mlMask, const rend::RenderMultilayerMaskBlobHeader& header,
												  const GpuApi::BufferRef& tiles, GpuApi::TextureRef& atlas )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]() mutable
	{
		PC_SCOPE( UpdateMultilayerMask );
		mlMask->UpdateInternals( header, tiles, atlas );
	} ) );
}

void CRenderCommandHandler::UpdateFoliageProfileSurfaceCache( TRenderPtr<CRenderFoliageProfile>& foliageProfile )
{
#ifndef RED_CONFIGURATION_FINAL //We don't want this code in the game. It's needed just for the editor
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]() mutable
	{
		GetRenderer()->GetSurfaceCache().UpdateStaticParametersAfterFoliageProfileChanged( foliageProfile );
	} ) );
#endif
}


void CRenderCommandHandler::AddOccluderToScene( const TScenePtr& scene, const vis::OccluderInstancePtr& occluder, const Matrix& localToWorld )
{
	RED_ASSERT( scene != nullptr, "Must provide an IRenderScene" );
	RED_ASSERT( occluder != nullptr, "Must provide a non-null OccluderInstancePtr" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDAddOccluderToScene );
		vis::OccluderMeshInstance* occluderBase = static_cast<vis::OccluderMeshInstance*>(occluder.Get());
		occluderBase->Attach( static_cast<CRenderSceneEx*>(scene.Get())->GetVisibilityScenePtr(), localToWorld );
	}, static_cast<void*>( occluder.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::AddOccludersToScene( const TScenePtr& scene, red::DynArray<vis::OccluderAddInfo>&& occluders )
{
	if ( !occluders.Empty() )
	{
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [ scene, occludersList=std::move(occluders) ]()
		{
			PC_SCOPE( CMDAddOccludersToScene );
			for ( const vis::OccluderAddInfo& addInfo : occludersList )
			{
				vis::OccluderMeshInstance* occluderBase = static_cast<vis::OccluderMeshInstance*>( addInfo.occluder.Get() );
				occluderBase->Attach( static_cast<CRenderSceneEx*>( scene.Get() )->GetVisibilityScenePtr(), addInfo.localToWorld );
			}
		} ) );
	}
}

void CRenderCommandHandler::RemoveOccluderFromScene( const vis::OccluderInstancePtr& occluder )
{
	RED_ASSERT( occluder != nullptr, "Must provide a non-null OccluderInstancePtr" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDRemoveOccluderFromScene );
		vis::OccluderMeshInstance* occluderBase = static_cast<vis::OccluderMeshInstance*>(occluder.Get());
		occluderBase->Detach();
	}, static_cast<void*>( occluder.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::RemoveOccludersFromScene( red::DynArray<vis::OccluderInstancePtr>&& occluders )
{
	if ( !occluders.Empty() )
	{
		CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [occludersList=std::move(occluders)] ()
		{
			PC_SCOPE( CMDRemoveOccludersFromScene );
			for ( const vis::OccluderInstancePtr& occluder : occludersList )
			{
				vis::OccluderMeshInstance* occluderBase = static_cast<vis::OccluderMeshInstance*>( occluder.Get() );
				occluderBase->Detach();
			}
		} ) );
	}
}

void CRenderCommandHandler::UpdateOccluder( const vis::OccluderInstancePtr& occluder, const Matrix& localToWorld )
{
	RED_ASSERT( occluder != nullptr, "Must provide a non-null OccluderInstancePtr" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDUpdateOccluder );
		vis::OccluderMeshInstance* occluderBase = static_cast<vis::OccluderMeshInstance*>(occluder.Get());
		occluderBase->Update( localToWorld );
	}, static_cast<void*>( occluder.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::SetVideoToRender( CName canvasName, const TRenderPtr<IRenderVideo>& video )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDSetVideoToRender );
		Uint64 canvasName64 = canvasName ? rend::CalculateNameU64( canvasName ) : 0;
		GetRenderer()->SetVideoToRender( canvasName64, video );
	} ) );
}

void CRenderCommandHandler::FlushTextureGrabs()
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]()
	{
		PC_SCOPE( CMDFlushTextureGrabs );
		GetRenderer()->FlushTextureGrabs();
	} ) );
}

void CRenderCommandHandler::ReloadReflectionProbe( const TResAsyncRef< CReflectionProbeDataResource >& path, Uint64 dataHash )
{
	// Have to pass path by value to be safe
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [path, dataHash]()
	{
		PC_SCOPE( CMDReloadReflectionProbe );
		GetRenderer()->GetReflectionProbeManager()->ReloadCubemap( path, dataHash );
	} ) );
}

void CRenderCommandHandler::SetMeshChunkMask(const TProxyPtr& proxy, Uint64 mask)
{
	RED_FATAL_ASSERT(proxy != nullptr, "NULL proxy pointer");
	RED_FATAL_ASSERT(proxy->GetType() == RPT_Mesh, "non-mesh proxy pointer");
	CommitCommand(MakeRenderCommandGeneric(DBGRENDER_TXT, [proxy, mask]()
	{
		PC_SCOPE(CMDSetMeshChunkMask);
		static_cast<CRenderProxy_Mesh*>(proxy.Get())->SetMeshChunkMask(mask);
	}, static_cast<void*>( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::SetIndexBufferOverride( const TProxyPtr& proxy, const TRenderPtr<IRenderResource>& ibOverride )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( proxy->GetType() == RPT_Mesh, "non-mesh proxy pointer" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [ proxy, ibOverride ]()
	{
		PC_SCOPE( CMDSetMeshChunkMask );
		static_cast<CRenderProxy_Mesh*>(proxy.Get())->SetIndexBufferOverride( ibOverride );
	}, static_cast<void*>( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::SetMeshMaterialOverlay( const TProxyPtr& proxy, const TRenderPtr< IRenderResource >& material, const TRenderPtr< IRenderResource >& materialParam, bool overrideMaterial )
{
	RED_FATAL_ASSERT( proxy != nullptr, "NULL proxy pointer" );
	RED_FATAL_ASSERT( proxy->GetType() == RPT_Mesh || proxy->GetType() == RPT_MorphTargetMesh, "non-mesh proxy pointer" );
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [proxy, material, materialParam, overrideMaterial] ()
	{
		PC_SCOPE( CMDSetMeshMaterialOverlay );
		static_cast<CRenderProxy_Mesh*>( proxy.Get() )->SetMaterialOverlay( material, materialParam, overrideMaterial );
	}, static_cast<void*>( proxy.Get() ), IRenderCommand::PQT_SafeWithProxyAdditionAndRemoval ) );
}

void CRenderCommandHandler::ResetSceneCustomData( const TScenePtr& scene )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [=]() mutable
	{
		PC_SCOPE( ResetSceneCustomData );

		CRenderSceneEx* sc = static_cast<CRenderSceneEx*>( scene.Get() );
		if ( sc )
		{
			{
				SceneCustomData_DistantShadows& sceneCustomData = sc->GetCameraStorage()->GetCustomData< SceneCustomData_DistantShadows>();
				sceneCustomData.Reset();
			}

			{
				SceneCustomData_RainMap& sceneCustomData = sc->GetCameraStorage()->GetCustomData< SceneCustomData_RainMap>();
				sceneCustomData.Reset();
			}
		}

	} ) );
}

void CRenderCommandHandler::EnforceScreenSpaceReflectionsUberQuality( Bool enable )
{
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [enable]()
	{
		PC_SCOPE( EnforceScreenSpaceReflectionsUberQuality );
		Config::GGlobalRenderingSettings.allowSSR_UberQuality = enable;
	} ) );
}

void CRenderCommandHandler::ForceRelitGlobalProbe( const TScenePtr& scene )
{
	RED_FATAL_ASSERT( scene );
	
	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene]()
	{
		PC_SCOPE( CMDForceRelitGlobalProbe );
		auto& data = static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>();
		data.ForceRelitGlobalProbe();
	} ) );
}

void CRenderCommandHandler::ForceRelitLocalProbes( const TScenePtr& scene, const Vector3& position, Float radius )
{
	RED_FATAL_ASSERT( scene );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene,position,radius]()
	{
		PC_SCOPE( CMDForceRelitLocalProbes );
		auto& data = static_cast<CRenderSceneEx*>(scene.Get())->GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>();
		data.ForceRelitLocalProbes( position, radius );
	} ) );
}

void CRenderCommandHandler::NotifyPlayerInsideVehicle( const TScenePtr& scene, const Bool isPlayerDrivingVehicle )
{
	RED_FATAL_ASSERT( scene );

	CommitCommand( MakeRenderCommandGeneric( DBGRENDER_TXT, [scene, isPlayerDrivingVehicle]()
	{
		PC_SCOPE( CMDNotifyPlayerInsideVehicle );
		static_cast<CRenderSceneEx*>( scene.Get() )->GetCameraStorage()->InvokeForOnScreenCamera( [isPlayerDrivingVehicle](CRenderFrameCameraStorage::SCameraData* onScreenCam)
		{
			onScreenCam->GetCustomData<TXAACustomData>().isPlayerDrivingVehicle = isPlayerDrivingVehicle;
		} );

	} ) );
}
