/**
* Copyright (C)2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#if defined(RED_PLATFORM_WEB)
#include "icu-data-web.h"
#include <time.h>
#include <stdlib.h>
#endif
#include "icuInternationalization.h"

RED_NO_EMPTY_FILE();

#ifndef INK_DISABLE_ICU

#include "unicode/uclean.h"

#include "../../redReflection/include/resourceDepot.h"
#include "../../redReflection/include/gatheredResource.h"
#include "icuCulture.h"
#include "icuUtils.h"
#include "breakIteratorInterface.h"
#include "collatorInterface.h"
#include "icuConversions.h"
#include "unicode/datefmt.h"
#include "internationalizationInterface.h"
#include "icuRegexCache.h"

#ifdef RED_PLATFORM_ORBIS
#include <system_service.h>
#endif

using namespace serialization;
using namespace io;

namespace
{
	static void* U_CALLCONV ICUMallocWrapper( const void* context, size_t size )
	{
		return RED_ALLOCATE( text::PoolText_ICU_Internal, size );
	}

	static void* U_CALLCONV ICUReallocWrapper( const void* context, void* memory, size_t size )
	{
		return RED_REALLOCATE( text::PoolText_ICU_Internal, memory, size );
	}

	static void U_CALLCONV ICUFreeWrapper( const void* context, void* memory )
	{
		RED_FREE( text::PoolText_ICU_Internal, memory );
	}

	static const res::GatheredResourceReference s_icuDataPackagePath( RED_CONST_RESOURCEPATH( "engine\\i18n\\icudt65l.dat" ), GRF_Default );
}

namespace text
{
	struct ICUPackageLoadProtector : public red::NonCopyable
	{
		RED_USE_MEMORY_POOL(PoolText_ICU_Internal);

		ICUPackageLoadProtector( const red::SharedPtr<ICUInternationalization>& sharedI18n )
			: m_weakI18n( sharedI18n )
		{
		}

		ICUPackageLoadProtector()
		{

		}

		~ICUPackageLoadProtector()
		{
		}

		red::SharedPtr<ICUInternationalization> GetI18n() const
		{
			return m_weakI18n.Lock();
		}

	private:
		red::WeakPtr< ICUInternationalization > m_weakI18n;
	};

	//////////////////////////////////////////////////////////////////////////

	ICUInternationalization::ICUInternationalization( red::ManualResetEvent* dataLoadedSyncEvt)
		: m_defaultCulture( nullptr )
		, m_dataLoadedSyncEvt( dataLoadedSyncEvt )
	{
	}

	ICUInternationalization::~ICUInternationalization()
	{
	}

	void ICUInternationalization::Initialize()
	{
		SetupCustomAllocators();

		SetupWrappersForFileSystem();

		PreloadDataPackage();
	}

	void ICUInternationalization::Shutdown()
	{
		text::RegexPatternCache::Uninitialize();

		ClearCultures();

		InternalICUClose();

		ClearWrappersForFileSystem();
	}

	void ICUInternationalization::SetActiveCulture(const char* localeName)
	{
		if ( localeName )
		{
			String localeNameWithStrictRules{ localeName };
			localeNameWithStrictRules += "@lb=strict";
			m_invariantCulture = red::CreateSharedPtr< ICUCulture >( localeNameWithStrictRules.AsChar() );
		}
		else
		{
			m_invariantCulture = red::CreateSharedPtr< ICUCulture >( "en_US_POSIX@lb=strict" );	// fallback
		}

		// temporary solution
		m_activeCulture = m_invariantCulture;
		m_defaultCulture = m_invariantCulture;
	}

	const red::SharedPtr<text::ICulture>& ICUInternationalization::GetActiveCulture() const
	{
		return m_activeCulture;
	}

	const red::SharedPtr<text::ICulture>& ICUInternationalization::GetCulture( const Utf16String& icuLanguageName ) const
	{
		for ( Uint32 i = 0; i < m_loadedCultures.Size(); ++i )
		{
			if ( m_loadedCultures[ i ]->GetName() == icuLanguageName )
			{
				return m_loadedCultures[i];
			}
		}

		return m_activeCulture;
	}

	red::Utf16String ICUInternationalization::ConvertTimestampToDateTime( Uint64 timestamp )
	{
		red::Utf16String formattedDateTimeString{};
		UErrorCode success = U_ZERO_ERROR;

#ifdef RED_PLATFORM_ORBIS
		Int32 orbisSystemTimeZone = 0;
		Int32 orbisSystemSummertime = 0;
		::sceSystemServiceParamGetInt( SCE_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE, &orbisSystemTimeZone );
		::sceSystemServiceParamGetInt( SCE_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME, &orbisSystemSummertime );
		orbisSystemTimeZone += orbisSystemSummertime * 60;
		red::String timeZoneGmt = red::String::Printf( "GMT%+03i:%02i", orbisSystemTimeZone / 60, abs( orbisSystemTimeZone ) % 60 );
		red::SharedPtr< icu::TimeZone, text::PoolText_ICU_Internal > timeZone( icu::TimeZone::createTimeZone( timeZoneGmt.AsChar() ) );
#elif defined( RED_PLATFORM_WEB )
		// [savetime] ICU host-zone detection cannot identify the browser zone: Emscripten exposes tzname as
		// "UTC-0600"/"UTC-0500", which ICU cannot remap to an Olson ID, so createDefault() falls back to the
		// raw standard offset and ignores daylight saving (saves showed 1 hour behind). Like the Orbis branch,
		// build a GMT offset zone, here from the browser's local offset at this timestamp (tm_gmtoff, DST-aware).
		Int32 webTimeZoneMinutes = 0;
		{
			const time_t webTime = static_cast< time_t >( timestamp );
			struct tm webLocalTime = {};
			if ( localtime_r( &webTime, &webLocalTime ) != nullptr )
				webTimeZoneMinutes = static_cast< Int32 >( webLocalTime.tm_gmtoff / 60 );
		}
		red::String timeZoneGmt = red::String::Printf( "GMT%+03i:%02i", webTimeZoneMinutes / 60, abs( webTimeZoneMinutes ) % 60 );
		red::SharedPtr< icu::TimeZone, text::PoolText_ICU_Internal > timeZone( icu::TimeZone::createTimeZone( timeZoneGmt.AsChar() ) );
#else
		red::SharedPtr< icu::TimeZone, text::PoolText_ICU_Internal > timeZone( icu::TimeZone::createDefault() );
#endif
		red::SharedPtr< icu::Calendar, text::PoolText_ICU_Internal > calendar( icu::Calendar::createInstance( *timeZone, m_activeCulture->GetLocale(), success ) );
		if ( success != U_ZERO_ERROR )
			return formattedDateTimeString;

		UDate icuDate = static_cast< Double >( timestamp * 1000 );	//ICU gets timestamp in milliseconds and we're storing it in seconds
		calendar->setTime( icuDate, success );
		if ( success != U_ZERO_ERROR )
			return formattedDateTimeString;

		red::SharedPtr < icu::DateFormat, text::PoolText_ICU_Internal > dateFormat( icu::DateFormat::createDateTimeInstance( icu::DateFormat::SHORT, icu::DateFormat::SHORT, m_activeCulture->GetLocale() ) );
		if ( dateFormat == nullptr )
			return formattedDateTimeString;

		dateFormat->setCalendar( *calendar );
		icu::UnicodeString unicodeDateTimeString{};
		dateFormat->format( *calendar, unicodeDateTimeString, nullptr, success );
		if ( success != U_ZERO_ERROR )
			return formattedDateTimeString;

		formattedDateTimeString = text::ConvertToRedString( unicodeDateTimeString );

		return formattedDateTimeString;
	}

	void ICUInternationalization::SetupWrappersForFileSystem()
	{
		u_setDataDirectory( "engine\\i18n" );

		UErrorCode icuErrorCode = U_ZERO_ERROR;
		u_setVirtualFileSystemFunctions( this, &ICUInternationalization::OpenFileWrapper, &ICUInternationalization::CloseFileWrapper, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );
}

	void ICUInternationalization::SetupCustomAllocators()
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;
		u_setMemoryFunctions( nullptr, &ICUMallocWrapper, &ICUReallocWrapper, &ICUFreeWrapper, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );
	}

	void ICUInternationalization::InternalICUInit()
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;
		u_init( &icuErrorCode );
		RED_ICU_CHECK(icuErrorCode);
	}

	void ICUInternationalization::InternalICUClose()
	{
		u_cleanup();
	}

	void ICUInternationalization::ClearCultures()
	{
		m_defaultCulture = nullptr;
		m_activeCulture = nullptr;
		m_invariantCulture = nullptr;

		m_loadedCultures.Clear();
	}

	void ICUInternationalization::ClearWrappersForFileSystem()
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;
		u_setVirtualFileSystemFunctions( nullptr, nullptr, nullptr, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );

		u_setDataDirectory( nullptr );
	}

	void ICUInternationalization::ClearCustomAllocators()
	{
		UErrorCode icuErrorCode = U_ZERO_ERROR;
		u_setMemoryFunctions( nullptr, nullptr, nullptr, nullptr, &icuErrorCode );
		RED_ICU_CHECK( icuErrorCode );
	}

	Bool ICUInternationalization::OpenFileWrapper( const void* context, void** outFileContext, void** outContents, const char* filePath )
	{
		ICUInternationalization* self = (ICUInternationalization*)context;

		res::ResourcePath resPath = res::ResourcePath::Build( filePath );

		if (resPath != s_icuDataPackagePath.GetPath())
		{
			RED_LOG_ERROR("ICU cannot open a file: %hs", filePath);
			return false;
		}

		*outContents = self->m_icuDataPackage.Get();
		(*reinterpret_cast<Uint64*>(outFileContext)) = s_icuDataPackagePath.GetPath().GetHash();

		return true;
	}

	void ICUInternationalization::CloseFileWrapper( const void* context, void* const fileContext, void* const contents )
	{
		if( !fileContext )
		{
			return;
		}

		ICUInternationalization* self = (ICUInternationalization*)context;

		const Uint64 resPathHash = reinterpret_cast<const Uint64>( fileContext );

		if (s_icuDataPackagePath.GetPath().GetHash() == resPathHash)
		{
			self->m_icuDataPackage.Reset();
		}
	}

	void OnBufferDataLoaded(AsyncSourceReadBuffer buffer, red::UniqueBuffer, void* userData, red::EAsyncResult)
	{
		ICUPackageLoadProtector* protector = static_cast<ICUPackageLoadProtector*>(userData);

		if (const red::SharedPtr< ICUInternationalization> i18n = protector->GetI18n())
		{
			const red::BlobView& bufferView = buffer.ValidRegion();

			i18n->m_icuDataPackage = red::CreateUniqueBuffer< text::PoolText_ICU_Data_Packagae >( bufferView.Size(), 16 );
		
			red::Memcpy( i18n->m_icuDataPackage.Get(), bufferView.Data(), bufferView.Size() );

			i18n->InternalICUInit();

			text::RegexPatternCache::Initialize();

			// special case for unit tests
			if (i18n->m_dataLoadedSyncEvt)
			{
				i18n->m_dataLoadedSyncEvt->SetEvent();
				i18n->m_dataLoadedSyncEvt = nullptr;
			}
		}

		buffer.Release();

		RED_DELETE(protector);
	}

	void ICUInternationalization::PreloadDataPackage()
    {
#if defined(RED_PLATFORM_WEB)
        // The exact original locale package is embedded by compile-icu-data.py.
        // Keep the engine's allocator, ICU file wrappers, initialization and
        // cache/event lifecycle; only delivery of this boot asset changes.
        m_icuDataPackage = red::CreateUniqueBuffer< text::PoolText_ICU_Data_Packagae >( cpweb_icu_data_size, 16 );
        red::Memcpy( m_icuDataPackage.Get(), icudt65_dat, cpweb_icu_data_size );
        InternalICUInit();
        text::RegexPatternCache::Initialize();
        if (m_dataLoadedSyncEvt)
        {
            m_dataLoadedSyncEvt->SetEvent();
            m_dataLoadedSyncEvt = nullptr;
        }
        return;
#endif
		auto* depot = GetResourceDepot();
		if (!depot->ResourceExists(s_icuDataPackagePath.GetPath()))
		{
			RED_LOG_ERROR("ICU cannot open a file: %hs", s_icuDataPackagePath.GetPath().ToDebugString() );
			return;
		}

		if (red::SharedPtr<IAsyncSource> asyncSource = depot->CreateResourceAsyncSource(s_icuDataPackagePath.GetPath()))
		{
			const Uint32 sizeOnDisk = asyncSource->GetSizeOnDisk();
			const AsyncSourceReadSizes readSizes = asyncSource->GetReadSizes(0, sizeOnDisk);

			io::ShareableIOMemory buffer{ red::CreateUniqueBuffer<text::PoolText_ICU_Data_Packagae>(readSizes.bufferSize, 16) };

			IAsyncSource::CallbackContext callbackContext;
			callbackContext.userData = RED_NEW(ICUPackageLoadProtector)( SharedFromThis() );
			callbackContext.callback = &OnBufferDataLoaded;

			IAsyncSource::ReadAsyncParams params;
			params.requestSource = io::RequestSource::ICUInternationalization;
			params.offset = 0;
			params.size = readSizes.diskSize;
			params.priority = eAsyncPriority_Normal;
			params.allocDeserializationMemoryForDecompressorIfCompressed = false;
			asyncSource->ReadAsync(params, AsyncSourceReadBuffer(std::move(buffer), 0, readSizes), callbackContext);
		}
	}
}

#endif	// INK_DISABLE_ICU
