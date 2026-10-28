#include "build.h"
#include <cstdio>
#include <atomic>
#if defined( RED_PLATFORM_WEB )
#include <emscripten/emscripten.h>
#endif

#include "renderShaderMap.h"
#include "renderShaderPair.h"
#include "renderShader.h"

#include "../../core/include/cacheFilenames.h"
#include "../../core/include/stringConversion.h"
#include "../../shaderCache/include/shaderCacheManager.h"
#include "../../redSystem/include/stopWatch.h"
#include "../../redJobs2/include/jobRunner.h"

using red::DynArray;
using red::String;

namespace Config
{
	TConfigVar< Bool >	cvAllowShaderCacheReload( "Rendering", "AllowShaderCacheReload", true, eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
	TConfigVar< Float >	cvShaderCacheReloadTimeCheck( "Rendering", "ShaderCacheReloadTimeCheck", 2.0f, eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// #HACK: Layouts and everything should be baked into the shader cache, so that we just read out the final data in the
// end. But for a first step, this was simplest. Just map an enum value into the appropriate desc and topology.
static const GpuWrapApi::VertexLayoutDesc s_emptyVertexLayout = {};
RED_INLINE GpuApi::VertexLayoutRef GetVertexLayout( StaticShaderInputLayout layout )
{
	switch ( layout )
	{
	case StaticShaderInputLayout::DebugVertexBase:
	{
		static GpuApi::VertexLayoutRef vertexLayout = GpuApi::GetVertexLayout( DebugVertexBase::GetVertexLayoutDesc() );
		return vertexLayout;
	}

	case StaticShaderInputLayout::DebugVertexUV:
	case StaticShaderInputLayout::DebugVertexUV_Fullscreen:
	{
		static GpuApi::VertexLayoutRef vertexLayout = GpuApi::GetVertexLayout( DebugVertexUV::GetVertexLayoutDesc() );
		return vertexLayout;
	}

	case StaticShaderInputLayout::NoBuffers_Fullscreen:
	case StaticShaderInputLayout::NoBuffers_PointList:
		return GpuApi::VertexLayoutRef::Null();

	default:
		RED_HALT( "Invalid static shader vertex layout: %d", layout );
		return GpuApi::VertexLayoutRef::Null();
	}
}
RED_INLINE PrimitiveTopologyType GetPrimitiveTopologyType( StaticShaderInputLayout layout )
{
	switch ( layout )
	{
	case StaticShaderInputLayout::DebugVertexBase:
	case StaticShaderInputLayout::DebugVertexUV:
	case StaticShaderInputLayout::DebugVertexUV_Fullscreen:
	case StaticShaderInputLayout::NoBuffers_Fullscreen:
		return PrimitiveTopologyType::Triangle;

	case StaticShaderInputLayout::NoBuffers_PointList:
		return PrimitiveTopologyType::Point;

	default:
		RED_HALT( "Invalid static shader vertex layout: %d", layout );
		return PrimitiveTopologyType::Invalid;
	}
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#if defined( RED_PLATFORM_WEB )
namespace
{
	std::atomic<Uint32> s_webMapSize{ 0 };
	// Callers do GetShader( name )->AsCompute()->Dispatch(...) without null checks. A shader missing from
	// the cache returns the dummy, whose AsCompute() is null, so hand back a compute shader with no
	// program instead: its Dispatch/Bind are no-ops.
	class CRenderShaderWebDummy : public CRenderShaderDummy
	{
	public:
		CRenderShaderWebDummy() : m_noopCompute( RED_NEW( CRenderShaderCompute )( nullptr, red::StringView() ) ) { }
		// Leaving the previous program bound made missing fullscreen passes draw with whatever was bound last
		// (e.g. the Bink video shader). Unbind both stages so such draws are skipped instead.
		virtual void Bind() override
		{
			static std::atomic<unsigned> reports{ 0 };
			if ( reports.fetch_add( 1 ) < 6 )
			{
				std::fprintf( stderr, "CP2077 render shader map: dummy Bind (shader missing from map) mapSize=%u\n", s_webMapSize.load() );
			}
			GpuApi::SetShader( GpuApi::ShaderRef::Null(), GpuWrapApi::VertexShader );
			GpuApi::SetShader( GpuApi::ShaderRef::Null(), GpuWrapApi::PixelShader );
		}
		virtual void Dispatch( Uint32, Uint32, Uint32, GpuApi::ComputeCuUsage ) override { }
		virtual CRenderShaderCompute* AsCompute() override { return m_noopCompute.Get(); }

	private:
		TRenderPtr< CRenderShaderCompute > m_noopCompute;
	};
}
#endif

CRenderShaderMap::CRenderShaderMap()
	: m_renderShaderMap( rend::PoolShaderCache() )
#if defined( RED_PLATFORM_WEB )
	, m_dummyShader( RED_NEW( CRenderShaderWebDummy )() )
#else
	, m_dummyShader( RED_NEW( CRenderShaderDummy )() )
#endif
{
}

CRenderShaderMap::~CRenderShaderMap()
{
}

Bool CRenderShaderMap::Init( red::SharedPtr< IStaticShaderCache > cache )
{
	if ( !cache )
	{
		return false;
	}
	m_cache = cache;
#ifndef RED_CONFIGURATION_FINAL
	m_loadedCacheTimeStamp = cache->GetFileTimestamp();
#endif // RED_CONFIGURATION_FINAL
	return CreateRenderShaders();
}

Bool CRenderShaderMap::Reload( red::SharedPtr< IStaticShaderCache > cache )
{
	if ( !cache )
	{
		return false;
	}
	m_cache = cache;
#ifndef RED_CONFIGURATION_FINAL
	m_loadedCacheTimeStamp = cache->GetFileTimestamp();
#endif // RED_CONFIGURATION_FINAL
	for ( const auto& mapEntry : m_renderShaderMap )
	{
		RED_FATAL_ASSERT( mapEntry.Value(), "Invalid shader pointer in renderShaderMap" );
		if ( !mapEntry.Value()->Reload( *m_cache ) )
		{
			return false;
		}
	}

	if ( GpuApi::GetCapabilities().psoPrecache )
	{
		red::DynArray< red::SharedPtr< StaticShaderMetaEntry > > metaEntries { rend::PoolShaderCacheData() };
		m_cache->GetMetaEntries( metaEntries );
		PrecachePSOs( metaEntries );
	}

	return true;
}

void CRenderShaderMap::Tick( Float timeDelta )
{
#ifndef RED_CONFIGURATION_FINAL
	static Float timeSinceLastCheck = 0.0f;
	timeSinceLastCheck += timeDelta;

	if ( !Config::cvAllowShaderCacheReload.Get() )
	{
		return;
	}

	if ( timeSinceLastCheck >= Config::cvShaderCacheReloadTimeCheck.Get() )
	{
		if ( !IsCacheUpToDate() )
		{
			if ( GStaticShaderCache->IsInit() && GStaticShaderCache->Reload() )
			{
				RED_VERIFY( Reload( GStaticShaderCache->GetShaderCache() ) );
			}
		}
		timeSinceLastCheck = 0.0f;
	}
#endif
}

Bool CRenderShaderMap::IsCacheUpToDate() const
{
#ifdef RED_CONFIGURATION_FINAL
	return true;
#else
	RED_ASSERT( GStaticShaderCache && GStaticShaderCache->GetShaderCache(), "No static shader cache. Check it ASAP!");
	if ( !GStaticShaderCache || !GStaticShaderCache->GetShaderCache() )
	{
		// there was an error, better try reloading
		return false;
	}

	const red::AbsolutePath& currentShaderCachePath = GStaticShaderCache->GetShaderCache()->GetPath();
	CDateTime currentFileTimeStamp( GFileManager->GetFileTime( currentShaderCachePath ) );
	return currentFileTimeStamp <= m_loadedCacheTimeStamp;
#endif // RED_CONFIGURATION_FINAL
}

Bool ShouldUnloadBinaries()
{
#ifndef RED_PLATFORM_ORBIS
	return IsGameMode();
#else
	return false;
#endif // RED_PLATFORM_ORBIS
}

Bool CRenderShaderMap::CreateRenderShaders()
{
	red::DynArray< red::SharedPtr< StaticShaderMetaEntry > > metaEntries{ rend::PoolShaderCache() };
	m_cache->GetMetaEntries( metaEntries );
	m_renderShaderMap.Reserve( metaEntries.Size() );
	for ( auto& metaEntry : metaEntries )
	{
		RED_ASSERT( metaEntry, "StaticShaderMetaEntry cannot be null" );
		auto& entry = *metaEntry;
		if ( auto renderShaderBase = CreateRenderShaderBase( entry ) )
		{
			if ( m_renderShaderMap.Insert( metaEntry->m_hash, renderShaderBase ).IsSuccessful() )
			{
				RED_LOG( "Renderer: Created renderShader: '%hs' [hash: %u]", metaEntry->m_variableName.AsChar(), metaEntry->m_hash );
			}
			else
			{
				RED_ASSERT( false, "Error inserting shader '%hs' [hash: %u]", metaEntry->m_variableName.AsChar(), metaEntry->m_hash );
			}
		}
		else
		{
			RED_LOG_ERROR( "Renderer: Error creating shader '%hs'. Static shader cache has to be recompiled.", metaEntry->m_variableName.AsChar() );
#if defined( RED_PLATFORM_WEB )
			// Aborting here left every later shader out of the map (GetShader then returned the dummy). On web,
			// skip only the shader whose conversion is missing and keep building the rest.
			std::fprintf( stderr, "CP2077 render shader map: creation failed, skipped: %s [hash: %u]\n", metaEntry->m_variableName.AsChar(), metaEntry->m_hash );
			continue;
#else
			return false;
#endif
		}
	}

#if defined( RED_PLATFORM_WEB )
	s_webMapSize = Uint32( m_renderShaderMap.Size() );
	std::fprintf( stderr, "CP2077 render shader map: built entries=%u inserted=%u simpleCopy=%u present=%u\n",
		Uint32( metaEntries.Size() ), Uint32( m_renderShaderMap.Size() ),
		Uint32( m_renderShaderMap.Find( red::CalculateHash32( "m_simpleCopy" ) ) != m_renderShaderMap.End() ),
		Uint32( m_renderShaderMap.Find( red::CalculateHash32( "m_present" ) ) != m_renderShaderMap.End() ) );
#endif

	if ( GpuApi::GetCapabilities().psoPrecache )
	{
		PrecachePSOs( metaEntries );
	}

	if ( ShouldUnloadBinaries() )
	{
		// unlink binaries
		for ( const auto& metaEntry : metaEntries )
		{
			auto iter = m_renderShaderMap.Find( metaEntry->m_hash );
			if ( iter != m_renderShaderMap.End() && iter.Value() != nullptr )
			{
				iter.Value()->UnlinkBinary();
			}
		}
		
		// unload binaries for PC and XBOX, they keep the binaries separately
		m_cache->UnloadBinaries();
	}

	return true;
}

TRenderPtr<CRenderShaderBase> CRenderShaderMap::CreateRenderShaderBase( const StaticShaderMetaEntry& metaEntry ) const
{
	if ( metaEntry.m_entryHashes[ GpuWrapApi::ComputeShader ] )
	{
		RED_ASSERT( !metaEntry.m_entryHashes[ GpuWrapApi::VertexShader ], "Expected shader '%hs' to not have VS", metaEntry.m_variableName.AsChar() );
		RED_ASSERT( !metaEntry.m_entryHashes[ GpuWrapApi::PixelShader ], "Expected shader '%hs' to not have PS", metaEntry.m_variableName.AsChar() );

		return CRenderShaderCompute::Create( *m_cache, metaEntry, metaEntry.m_variableName.AsStringView() );
	}

	if ( metaEntry.m_entryHashes[ GpuWrapApi::ShaderLibrary ] )
	{
//#ifdef USE_RAY_TRACING
		return CRenderShaderLibrary::Create( GpuWrapApi::ShaderLibrary, *m_cache, metaEntry, metaEntry.m_variableName.AsStringView() );
//#endif
	}

	RED_ASSERT( metaEntry.m_entryHashes[ GpuWrapApi::VertexShader ], "Expected shader '%hs' to have VS", metaEntry.m_variableName.AsChar() );
	RED_ASSERT( metaEntry.m_entryHashes[ GpuWrapApi::PixelShader ], "Expected shader '%hs' to have PS", metaEntry.m_variableName.AsChar() );

	return CRenderShaderPair::Create( *m_cache, metaEntry, metaEntry.m_variableName.AsStringView() );
}


void CRenderShaderMap::PrecachePSOs( const red::ArraySpan< const red::SharedPtr< StaticShaderMetaEntry > >& metaEntries )
{
	red::StopWatch timer;

	RED_LOG( "Renderer: Precaching static shader PSOs..." );

	job::Builder builder;
	builder.DispatchParallelForJob( "Static shaders precache", { metaEntries.Size() }, [&]( Uint32 index, const job::RunContext& ctx )
	{
		const auto& metaEntry = metaEntries[index];

		job::Builder subBuilder( ctx );

		auto iter = m_renderShaderMap.Find( metaEntry->m_hash );
		if ( iter != m_renderShaderMap.End() && iter.Value() != nullptr )
		{
			if ( CRenderShaderCompute* cs = iter.Value()->AsCompute() )
			{
				if ( CRenderShader* shader = cs->GetRenderShader( CRenderShaderBase::eCOMPUTE ).Get() )
				{
					GpuApi::PrecachePSO( shader->GetShader(), &subBuilder );
				}
			}
			else if ( CRenderShaderPair* vsps = iter.Value()->AsPair() )
			{
				GpuApi::OMStateRef omState = vsps->GetOMState();

				if ( omState )
				{
					GpuApi::PSOPrecacheDesc psoState = {};
					psoState.m_omState = omState;
					psoState.m_shaderRefs[GpuWrapApi::VertexShader] = vsps->GetRenderShader( CRenderShaderBase::eVERTEX ) ? vsps->GetRenderShader( CRenderShaderBase::eVERTEX )->GetShader() : GpuApi::ShaderRef::Null();
					psoState.m_shaderRefs[GpuWrapApi::PixelShader] = vsps->GetRenderShader( CRenderShaderBase::ePIXEL ) ? vsps->GetRenderShader( CRenderShaderBase::ePIXEL )->GetShader() : GpuApi::ShaderRef::Null();

					for ( const PSODesc::RenderTargetSetup& rtSetup : metaEntry->m_allowedRTSetups )
					{
						psoState.m_rtFormats = rtSetup.m_rtFormats;
						psoState.m_dsFormat = rtSetup.m_dsFormat;
						psoState.m_sampleCount = rtSetup.m_sampleCount;
						for ( const StaticShaderInputLayout inputLayout : metaEntry->m_allowedInputLayouts )
						{
							psoState.m_vertexLayoutRef = GetVertexLayout( inputLayout );
							psoState.m_primitiveTopology = GetPrimitiveTopologyType( inputLayout );

							GpuApi::PrecachePSO( psoState, &subBuilder );
						}
					}
				}
			}
		}
	} );

	// #TODO: Move this later in engine startup. Or even when flushing render commands or something.
	job::FlushCounter( builder.ExtractWaitCounter() );

	RED_LOG( "Renderer: Precaching finished in %0.3f sec", timer.GetDeltaF() );
}
