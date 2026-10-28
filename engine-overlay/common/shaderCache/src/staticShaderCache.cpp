#include "build.h"
#include "staticShaderCache.h"
#include <cstdio>
namespace cp2077::web { int ConvertStaticShaderEntry(const char*, StaticShaderEntry&); }

#include "../../redSystem/include/stopWatch.h"
#include "../redContainers/include/string/stringUtils.h"

//////////////////////////////////////////////////////////////////////////

IStaticShaderCache::~IStaticShaderCache()
{
}

red::UniquePtr< IStaticShaderCache > IStaticShaderCache::CreateReadOnly( const red::AbsolutePath& absolutePath )
{
	red::UniquePtr< IStaticShaderCache > cache( RED_NEW( CStaticShaderCacheReadonly )() );
	if ( cache->Initialize( absolutePath ) )
	{
		return cache;
	}

	// failed to open the cache
	return nullptr;
}

red::UniquePtr< IStaticShaderCache > IStaticShaderCache::CreateReadWrite( const red::AbsolutePath& absolutePath )
{
	red::UniquePtr< IStaticShaderCache > cache( RED_NEW( CStaticShaderCacheReadWrite )() );
	if ( cache->Initialize( absolutePath ) )
	{
		return cache;
	}

	// failed to open the cache
	return nullptr;
}

red::UniquePtr< IStaticShaderCache > IStaticShaderCache::CreateNull()
{
	red::UniquePtr< IStaticShaderCache > cache( RED_NEW( CStaticShaderCacheNull )() );
	return cache;
}

//////////////////////////////////////////////////////////////////////////

CStaticShaderCacheReadWrite::CStaticShaderCacheReadWrite()
	: m_dirty( false )
{
}

CStaticShaderCacheReadWrite::~CStaticShaderCacheReadWrite()
{
}

Bool CStaticShaderCacheReadWrite::GetShader( const TShaderEntryHash hash, red::SharedPtr< StaticShaderEntry >& entry ) const
{
	CScopedLock lock( m_mutex );

	return m_shaderEntries.Find( hash, entry );
}

Bool CStaticShaderCacheReadWrite::AddShader( const TShaderEntryHash hash, red::SharedPtr< StaticShaderEntry > entry, IStaticShaderCacheLog& log )
{
	CScopedLock lock( m_mutex );

	if ( m_shaderEntries.KeyExist( hash ) )
	{
		red::SharedPtr< StaticShaderEntry > existingEntry = m_shaderEntries[ hash ];
		Bool result = *existingEntry.Get() == *entry.Get();
		if ( !result )
		{
			m_dirty = true;
			m_shaderEntries.Set( hash, entry );
			log.Log( "Successfully updated StaticShaderEntry." );
		}
		return true;
	}

	if ( m_shaderEntries.Insert( hash, entry ).IsSuccessful() )
	{
		m_dirty = true;
		return true;
	}
	RED_LOG( "Shaders: ShaderEntry with hash %u already exists in the cache!", hash );
	return false;
}

Bool CStaticShaderCacheReadWrite::GetMetaEntry( const TStaticMetaEntryHash hash, red::SharedPtr< StaticShaderMetaEntry >& metaEntry ) const
{
	return m_metaEntries.Find( hash, metaEntry );
}

Bool CStaticShaderCacheReadWrite::AddMetaEntry( const TStaticMetaEntryHash hash, red::SharedPtr< StaticShaderMetaEntry > metaEntry )
{
	CScopedLock lock( m_mutex );

	if ( m_metaEntries.Insert( hash, metaEntry ).IsSuccessful() )
	{
		m_dirty = true;
		return true;
	}

	red::SharedPtr< StaticShaderMetaEntry > existingMetaEntry;
	RED_VERIFY( m_metaEntries.Find( hash, existingMetaEntry ) );
	{
		if ( !m_dirty )
		{
			m_dirty = ( existingMetaEntry != metaEntry );
		}
		if ( m_dirty )
		{
			RED_VERIFY( m_metaEntries.Remove( hash ).IsSuccessful() );
			RED_VERIFY( m_metaEntries.Insert( hash, metaEntry ).IsSuccessful() );
		}

		return true;
	}

	return false;
}

Bool CStaticShaderCacheReadWrite::RemoveShaderEntry( const TShaderEntryHash hash )
{
	CScopedLock lock( m_mutex );

	if ( m_shaderEntries.Remove( hash ).IsSuccessful() )
	{
		m_dirty = true;
		return true;
	}

	return false;
}

Bool CStaticShaderCacheReadWrite::RemoveMetaEntry( const TStaticMetaEntryHash hash )
{
	CScopedLock lock( m_mutex );

	if ( m_metaEntries.Remove( hash ).IsSuccessful() )
	{
		m_dirty = true;
		return true;
	}

	return false;
}

Bool CStaticShaderCacheReadWrite::Flush( const CDateTime& timestamp, const red::THash64 hash, IStaticShaderCacheLog& log )
{
	CScopedLock lock( m_mutex );

	m_dirty |= m_info.m_hash != hash;

	if ( !m_dirty )
	{
		log.Log( "Shaders: Static ShaderCache is up-to-date, no need for saving" );
		return true;
	}

	auto file = GFileManager->CreateFileWriter( m_absoluteFilePath, FOF_AbsolutePath | FOF_Buffered );
	if ( !file )
	{
		log.Error( "Shaders: Static ShaderCache: cannot open '%hs' for saving.", m_absoluteFilePath.AsChar() );
		return false;
	}

	red::StopWatch timer;

	m_info.m_magic = SHADER_CACHE_MAGIC;
	m_info.m_version = STATIC_SHADER_CACHE_VERSION_CURRENT;
	m_info.m_timestamp = timestamp;
	m_info.m_hash = hash;

	m_info.m_numMetaEntries = m_metaEntries.Size();
	for ( const auto& metaEntry : m_metaEntries )
	{
		metaEntry.Value()->Serialize( *file );
	}
	log.Log( "Static shaders flush: Serialized MetaEntries." );

	m_info.m_numShaderEntries = m_shaderEntries.Size();
	for ( const auto& shader : m_shaderEntries )
	{
		shader.Value()->Serialize( *file );
	}
	log.Log( "Static shaders flush: Serialized Entries." );
	m_info.Serialize( *file );
	log.Log( "Static shaders flush: Serialized StaticShaderFileInfo." );
	log.Log( "  Info::numMetaEntries: [%d]", m_info.m_numMetaEntries );
	log.Log( "  Info::numShaderEntries: [%d]", m_info.m_numShaderEntries );
	log.Log( "  Info::magic: [%d]", m_info.m_magic );
	log.Log( "  Info::version: [%d]", m_info.m_version );
	log.Log( "  Info::timestamp: [date: %d][time: %d]", m_info.m_timestamp.GetDateRaw(), m_info.m_timestamp.GetTimeRaw() );
	log.Log( "  Info::hash: [%" RED_PRIu64 "]", m_info.m_hash );

	m_dirty = false;

	log.LogAlways( "Shaders: Static ShaderCache: saved %u MetaEntries, %u ShaderEntries, took %1.2fms", m_info.m_numMetaEntries, m_info.m_numShaderEntries, timer.GetDeltaMS() );

	return true;
}

void CStaticShaderCacheReadWrite::Trim( IStaticShaderCacheLog& log )
{
	log.LogAlways( "" );

	red::HashSet< TShaderEntryHash > unusedShaderEntries{ rend::PoolShaderCompilation() };
	for ( const TShaderEntryHash& shaderEntryHash : m_shaderEntries.GetKeys() )
	{
		unusedShaderEntries.Insert( shaderEntryHash );
	}

	for ( const auto& metaEntryPtr : m_metaEntries )
	{
		const red::SharedPtr< StaticShaderMetaEntry >& metaEntry = metaEntryPtr.Value();
		if ( !metaEntry )
		{
			log.Error( "Trim: Missing MetaEntry with hash %" RED_PRIu64, metaEntryPtr.Key() );
			continue;
		}

		for ( Uint32 i = 0; i < GpuWrapApi::ShaderTypeMax; ++i )
		{
			GpuWrapApi::eShaderType shaderType = (GpuWrapApi::eShaderType)i;
			if ( metaEntry->HasShaderHash( shaderType ) )
			{
				const TShaderEntryHash shaderEntryHash = metaEntry->m_entryHashes[ shaderType ];
				red::SharedPtr< StaticShaderEntry > shaderEntry;
				if ( !GetShader( shaderEntryHash, shaderEntry ) )
				{
#ifndef RED_OPTIMIZED_STATIC_SHADER_ENTRY
					log.Error( "Trim: '%hs', no shaderBinary [shaderType: %d] hash %" RED_PRIu64, metaEntry->m_variableName.AsChar(), i, shaderEntryHash );
#endif // RED_OPTIMIZED_STATIC_SHADER_ENTRY
					continue;
				}
				RED_ASSERT( shaderEntry );
				unusedShaderEntries.Remove( shaderEntryHash );
			}
		}
	}

	Uint32 totalSavedDataSize_Entries = 0;
	for ( const TShaderEntryHash& unusedShaderEntryHash : unusedShaderEntries )
	{
		red::SharedPtr< StaticShaderEntry > unusedShaderEntry;
		if ( GetShader( unusedShaderEntryHash, unusedShaderEntry ) )
		{
			RED_ASSERT( unusedShaderEntry );
			totalSavedDataSize_Entries += unusedShaderEntry->m_data.Size();
			totalSavedDataSize_Entries += sizeof( unusedShaderEntry->m_hash );
		}
		m_shaderEntries.Remove( unusedShaderEntryHash );
	}

	log.LogAlways( "Trim: %d unused StaticShaderEntries, saved %hs", unusedShaderEntries.Size(), red::FormatByteNumber( totalSavedDataSize_Entries, 2 ).AsChar() );
	log.LogAlways( "" );
}

Bool CStaticShaderCacheReadWrite::ValidateCache_NoLock()
{
	if ( m_info.m_magic != SHADER_CACHE_MAGIC )
	{
		std::fprintf( stderr, "CP2077 static shader cache: magic mismatch %u\n", m_info.m_magic );
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': magic mismatch (loaded: '%u', expected: '%u')", m_absoluteFilePath.AsChar(), m_info.m_magic, SHADER_CACHE_MAGIC );
		return false;
	}
	if ( m_info.m_version != STATIC_SHADER_CACHE_VERSION_CURRENT )
	{
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': version mismatch (loaded: '%u', current: '%u')", m_absoluteFilePath.AsChar(), m_info.m_version, STATIC_SHADER_CACHE_VERSION_CURRENT );
		return false;
	}
	if ( m_info.m_numMetaEntries <= 0 )
	{
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': no entries (loaded: '%u')", m_absoluteFilePath.AsChar(), m_info.m_numMetaEntries );
		return false;
	}
	if ( m_info.m_numShaderEntries <= 0 )
	{
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': no entries (loaded: '%u')", m_absoluteFilePath.AsChar(), m_info.m_numShaderEntries );
		return false;
	}

	return true;
}

Bool CStaticShaderCacheReadWrite::Initialize( const red::AbsolutePath& absoluteFilePath )
{
	CScopedLock lock( m_mutex );

	IStaticShaderCache::Initialize( absoluteFilePath );

	m_absoluteFilePath = absoluteFilePath;
	m_dirty = true;

	auto file = GFileManager->CreateFileReader( absoluteFilePath, FOF_AbsolutePath | FOF_Buffered );
	if ( file )
	{
		const Uint64 size = file->GetSize();
		const Uint64 infoSize = sizeof( StaticShaderCacheFileInfo );
		if ( size >= infoSize )
		{
			file->Seek( size - infoSize );
			m_info.Serialize( *file );

			m_dirty = !ValidateCache_NoLock();
			if ( !m_dirty )
			{
				file->Seek( 0 );

				for ( Uint32 i = 0; i < m_info.m_numMetaEntries; ++i )
				{
					red::SharedPtr< StaticShaderMetaEntry > metaEntry( RED_NEW( StaticShaderMetaEntry )() );
					metaEntry->Serialize( *file );
					m_metaEntries.Insert( metaEntry->m_hash, metaEntry );
				}

				m_shaderEntries.Reserve( m_info.m_numShaderEntries );
				for ( Uint32 i = 0; i < m_info.m_numShaderEntries; ++i )
				{
					red::SharedPtr< StaticShaderEntry > shader( RED_NEW( StaticShaderEntry )() );
					shader->Serialize( *file );
					m_shaderEntries.Insert( shader->m_hash, shader );
				}
			}
		}
		else
		{
			RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': size invalid (loaded: %" RED_PRIu64 ", expected more than: %" RED_PRIu64 ")", absoluteFilePath.AsChar(), size, infoSize );
		}
	}
	else
	{
		RED_LOG_ERROR( "Shaders: StaticShaderCache: could not find '%hs'", absoluteFilePath.AsChar() );
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////

CStaticShaderCacheReadonly::CStaticShaderCacheReadonly()
{
}

CStaticShaderCacheReadonly::~CStaticShaderCacheReadonly()
{
}

Bool CStaticShaderCacheReadonly::GetShader( const TShaderEntryHash hash, red::SharedPtr< StaticShaderEntry >& entry ) const
{
	return m_shaderEntries.Find( hash, entry );
}

Bool CStaticShaderCacheReadonly::AddShader( const TShaderEntryHash hash, red::SharedPtr< StaticShaderEntry > entry, IStaticShaderCacheLog& log )
{
	RED_HALT( "Trying to add ShaderEntry to readonly cache." );
	return false;
}

Bool CStaticShaderCacheReadonly::GetMetaEntry( const TStaticMetaEntryHash hash, red::SharedPtr< StaticShaderMetaEntry >& metaEntry ) const
{
	return m_metaEntries.Find( hash, metaEntry );
}

Bool CStaticShaderCacheReadonly::AddMetaEntry( const TStaticMetaEntryHash hash, red::SharedPtr< StaticShaderMetaEntry > metaEntry )
{
	RED_HALT( "Trying to add ShaderMetaEntry to readonly cache." );
	return false;
}

Bool CStaticShaderCacheReadonly::Flush( const CDateTime& timestamp, const red::THash64 hash, IStaticShaderCacheLog& log )
{
	RED_HALT( "Trying to Flush readonly ShaderCache." );
	return false;
}

void CStaticShaderCacheReadonly::Trim( IStaticShaderCacheLog& log )
{
	RED_HALT( "Trying to Trim readonly ShaderCache." );
}

Bool CStaticShaderCacheReadonly::Initialize( const red::AbsolutePath& absoluteFilePath )
{
	IStaticShaderCache::Initialize( absoluteFilePath );

	m_absoluteFilePath = absoluteFilePath;

	auto file = GFileManager->CreateFileReader( absoluteFilePath, FOF_AbsolutePath | FOF_Buffered );
	if ( !file )
	{
		std::fprintf( stderr, "CP2077 static shader cache: could not open '%s'\n", absoluteFilePath.AsChar() );
		RED_LOG_ERROR( "Shaders: StaticShaderCache: could not find '%hs'", absoluteFilePath.AsChar() );
		return false;
	}

	rend::PoolShaderCacheData pool;
	file->SetInternalMemoryPool( pool );

	const Uint64 size = file->GetSize();
	const Uint64 infoSize = sizeof( StaticShaderCacheFileInfo );
	if ( size < infoSize )
	{
		std::fprintf( stderr, "CP2077 static shader cache: size invalid %llu\n", static_cast< unsigned long long >( size ) );
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': size invalid (loaded: %" RED_PRIu64 ", expected more than: %" RED_PRIu64 ")", absoluteFilePath.AsChar(), size, infoSize );
		return false;
	}

	file->Seek( size - infoSize );
	m_info.Serialize( *file );

	if ( m_info.m_magic != SHADER_CACHE_MAGIC )
	{
		std::fprintf( stderr, "CP2077 static shader cache: magic mismatch %u\n", m_info.m_magic );
		RED_LOG_ERROR( "Shaders: StaticShaderCache '%hs': magic mismatch (loaded: '%u', expected: '%u'). File is probably corrupted." , absoluteFilePath.AsChar(), m_info.m_magic, SHADER_CACHE_MAGIC );
		return false;
	}

	if ( m_info.m_version != STATIC_SHADER_CACHE_VERSION_CURRENT )
	{
		RED_LOG_WARNING( "Shaders: StaticShaderCache '%hs': version mismatch (loaded: '%u', current: '%u')." , absoluteFilePath.AsChar(), m_info.m_version, STATIC_SHADER_CACHE_VERSION_CURRENT );
	}

	file->m_version = m_info.m_version;
	file->Seek( 0 );

	m_metaEntries.Reserve( m_info.m_numMetaEntries );
	for ( Uint32 i = 0; i < m_info.m_numMetaEntries; ++i )
	{
		red::SharedPtr< StaticShaderMetaEntry > entry( RED_NEW( StaticShaderMetaEntry )() );
		entry->Serialize( *file );
		m_metaEntries.Insert( entry->m_hash, entry );
	}

	m_shaderEntries.Reserve( m_info.m_numShaderEntries );
	for ( Uint32 i = 0; i < m_info.m_numShaderEntries; ++i )
	{
		red::SharedPtr< StaticShaderEntry > shader( RED_NEW( StaticShaderEntry )() );
		shader->Serialize( *file );
		m_shaderEntries.Insert( shader->m_hash, shader );
	}

    // Preserve the full original metadata/entry maps. Converted exact compute
    // variants replace only their corresponding binary payloads.
    // A single compute variant that cannot be found or converted must not discard the whole cache
    // (that left every static shader - present, mip copies, post - as a no-op dummy). Skip it instead.
    unsigned missingCompute = 0, failedCompute = 0, convertedCompute = 0;
    for (const auto& item : m_metaEntries)
    {
        const auto& meta = item.Value();
        if (!meta->HasShaderHash(GpuWrapApi::ComputeShader)) continue;
        red::SharedPtr<StaticShaderEntry> entry;
        if (!GetShader(meta->m_entryHashes[GpuWrapApi::ComputeShader], entry))
        {
            if (missingCompute++ < 8) std::fprintf(stderr, "CP2077 static shader cache: compute entry missing: %s\n", meta->m_variableName.AsChar());
            continue;
        }
        const int converted = cp2077::web::ConvertStaticShaderEntry(meta->m_variableName.AsChar(), *entry);
        if (converted < 0)
        {
            if (failedCompute++ < 8) std::fprintf(stderr, "CP2077 static shader cache: compute conversion failed: %s\n", meta->m_variableName.AsChar());
            continue;
        }
        ++convertedCompute;
    }
    std::fprintf(stderr, "CP2077 static shader cache: loaded meta=%u shaders=%u version=%u compute converted=%u missing=%u failed=%u\n",
        m_info.m_numMetaEntries, m_info.m_numShaderEntries, m_info.m_version, convertedCompute, missingCompute, failedCompute);

	return true;
}

//////////////////////////////////////////////////////////////////////////

CStaticShaderCacheNull::~CStaticShaderCacheNull()
{
}
