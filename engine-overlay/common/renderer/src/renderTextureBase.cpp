/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include <atomic>
#include <cstdio>
#include "renderTextureBase.h"
#include "renderHelpers.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/textureGroup.h"
#include "../../renderData/include/renderTextureBlob.h"
#include "../../redCore/include/globalModeInfo.h"

using red::String;

#if defined( RED_PLATFORM_WEB )
#include <cstring>
#include <vector>
#include <emscripten/emscripten.h>
// Browser port branding: the main-menu logo ("cp_logo" in menu_background.inkatlas) is replaced by patching the
// cooked BC3 blocks of the atlas texture before it is created. Block files are produced by
// D:\2077\branding\make-menu-logo-bc3.js and served from /runtime/branding/. Loaded synchronously because texture
// creation already runs on a loader worker; a missing file leaves the original texture untouched.
EM_JS( int, cp2077FetchOverrideBytes, ( const char* url, void* out, int capacity ), {
	try {
		const request = new XMLHttpRequest();
		request.open( 'GET', UTF8ToString( Number( url ) ), false );
		request.overrideMimeType( 'text/plain; charset=x-user-defined' );
		request.send( null );
		if ( request.status !== 200 ) return -1;
		const text = request.responseText;
		if ( text.length > capacity ) return -1;
		growMemViews();
		const at = Number( out );
		for ( let i = 0; i < text.length; ++i ) HEAPU8[ at + i ] = text.charCodeAt( i ) & 255;
		return text.length;
	} catch ( error ) { return -1; }
} );

namespace cp2077::web { bool GpuDecodesBC(); } // [texdrop] gpu-device-web.cpp

static void CpApplyMenuLogoOverride( GpuWrapApi::eTextureFormat format, Uint32 width, Uint32 height, Uint8* mip0, Uint64 available )
{
	const char* formatName = GpuWrapApi::GetTextureFormatName( format );
	const bool isBC3 = formatName && ( std::strstr( formatName, "BC3" ) || std::strstr( formatName, "DXT5" ) );
	if ( !isBC3 || !mip0 ) return;
	const Uint64 blocksPerRow = ( width + 3 ) / 4, blockRows = ( height + 3 ) / 4, mipBytes = blocksPerRow * blockRows * 16;
	if ( available < mipBytes ) return;
	char url[ 96 ];
	std::snprintf( url, sizeof( url ), "/runtime/branding/menu-logo-%ux%u.bc3", width, height );
	std::vector< Uint8 > file( 512 * 1024 );
	const int size = cp2077FetchOverrideBytes( url, file.data(), int( file.size() ) );
	if ( size < 20 || std::memcmp( file.data(), "CPLG", 4 ) != 0 ) return;
	Uint32 header[ 4 ];
	std::memcpy( header, file.data() + 4, sizeof( header ) );
	const Uint32 blockX = header[ 0 ], blockY = header[ 1 ], blocksWide = header[ 2 ], blocksHigh = header[ 3 ];
	if ( Uint64( blockX ) + blocksWide > blocksPerRow || Uint64( blockY ) + blocksHigh > blockRows ||
		 Uint64( size ) != 20 + Uint64( blocksWide ) * blocksHigh * 16 ) return;
	for ( Uint32 row = 0; row < blocksHigh; ++row )
		std::memcpy( mip0 + ( ( Uint64( blockY ) + row ) * blocksPerRow + blockX ) * 16, file.data() + 20 + Uint64( row ) * blocksWide * 16, Uint64( blocksWide ) * 16 );
	std::fprintf( stderr, "CP2077 menu logo override applied: %ux%u blocks=%ux%u\n", width, height, blocksWide, blocksHigh );
}
// The atlas is sampled minified at 720p, so every level of the mip chain is patched (one block file per level size).
static bool CpIsMenuLogoAtlas( Uint32 width, Uint32 height )
{
	return ( width == 1024 && height == 1776 ) || ( width == 976 && height == 1660 ) || ( width == 1952 && height == 3320 );
}
static void CpApplyMenuLogoOverrideChain( GpuWrapApi::eTextureFormat format, Uint32 baseWidth, Uint32 baseHeight,
	Uint32 firstMip, Uint32 mipCount, Uint8* data, Uint64 available )
{
	if ( !CpIsMenuLogoAtlas( baseWidth, baseHeight ) || !data ) return;
	Uint64 offset = 0;
	for ( Uint32 mip = firstMip; mip < firstMip + mipCount; ++mip )
	{
		const Uint32 w = ( baseWidth >> mip ) ? ( baseWidth >> mip ) : 1, h = ( baseHeight >> mip ) ? ( baseHeight >> mip ) : 1;
		const Uint64 bytes = Uint64( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * 16;
		if ( offset + bytes > available ) break;
		CpApplyMenuLogoOverride( format, w, h, data + offset, bytes );
		offset += bytes;
	}
}
#endif

GpuApi::TextureRef CRenderTextureBase::LoadFromRenderTextureBlob( const rend::IRenderTextureBlob& blob, const struct STextureGroupSetup& setup, Uint8 residentMip, Bool allowBindless )
{
	const rend::IRenderTextureBlob::Header& header = blob.m_header;
	const rend::TextureInfo& texInfo = header.m_textureInfo;
	const rend::SizeInfo& sizeInfo = header.m_sizeInfo;
    static std::atomic<unsigned> blobReports{0};
    const bool traceBlob = blobReports.fetch_add(1)<16;
    if (traceBlob) std::fprintf(stderr,"CP2077 blob upload probe: phase=entry type=%u width=%u height=%u slices=%u mips=%u version=%u bytes=%llu raw=%u compression=%u optimized=%u\n",
        unsigned(texInfo.m_type),sizeInfo.m_width,sizeInfo.m_height,texInfo.m_sliceCount,texInfo.m_mipCount,
        unsigned(blob.GetVersion()),static_cast<unsigned long long>(blob.m_textureData.Data().GetSize()),
        unsigned(setup.m_rawFormat),unsigned(setup.m_compression),unsigned(blob.IsOptimized()));


	// On PC we've only ever set Optimized flag, not Tiled. In the future it may be useful to add in Tiled flag, which
	// would allow semi-inplace loading on PC (would be padded out to Upload-ready layout).
#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
	const Bool isValidData = blob.IsOptimized();
#else
	const Bool isValidData = ( blob.IsOptimized() && blob.IsTiled() );
#endif
	if ( !isValidData )
	{
		RED_DATA_ERROR_BEGIN( red::LoggerCategory_Rendering, isValidData, "Texture render blob does not have properly optimized data. Please reimport or resave or something.");
		RED_DATA_ERROR_ACTION_SHOW_ASSET( blob.GetPath().ToDebugString() );
		RED_DATA_ERROR_END();

		return GpuApi::TextureRef::Null();
	}

	// HACK, UI textures should be fixed
	const Bool isGamma = setup.m_isGamma || setup.m_group == GpuWrapApi::TEXG_Generic_UI;

	const GpuWrapApi::eTextureFormat texFormat = Texture::GetCompressedFormat( setup.m_rawFormat, setup.m_compression, isGamma );
    if (traceBlob) std::fprintf(stderr,"CP2077 blob upload probe: phase=format format=%u\n",unsigned(texFormat));
	if ( texFormat == GpuWrapApi::TEXFMT_Max )
	{
		RED_LOG( "Renderer: Invalid texture format" );
		return GpuApi::TextureRef::Null();
	}

#ifdef RED_PLATFORM_CONSOLE
	if( setup.m_allowTextureDowngrade && sizeInfo.m_width >= 512 ) // ctremblay Limit downgrade on console to bigger texture. 
	{
		residentMip = Min( residentMip, texInfo.m_mipCount - 1 );
	}
	else
	{
		residentMip = 0;
	}
#else

#if defined( RED_PLATFORM_WEB )
	// [texdrop] 2026-10-24: GPUs without BC (phones) store decoded textures 4-8x larger; 4 GB phones lost the device.
	// Like the console downgrade, drop mips from big non-UI textures: default 1 there (?texDrop=N overrides, 0 = off,
	// also usable on desktop for A/B). The BC multiple-of-4 fixup below still applies.
	{
		static const int cpTexDrop = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'texDrop' ); return v !== null && /^[0-4]$/.test( v ) ? Number( v ) : -1; } catch ( e ) { return -1; } } );
		const int drop = cpTexDrop >= 0 ? cpTexDrop : ( cp2077::web::GpuDecodesBC() ? 1 : 0 );
		if ( drop > 0 && setup.m_allowTextureDowngrade && setup.m_group != GpuWrapApi::TEXG_Generic_UI && sizeInfo.m_width >= 512 && sizeInfo.m_height >= 512 &&
			 texInfo.m_type != GpuWrapApi::TEXTYPE_3D && texInfo.m_mipCount > Uint32( drop ) + 2 )
			residentMip = Max< Uint8 >( residentMip, Uint8( drop ) );
	}
#endif
	residentMip = Min( residentMip, texInfo.m_mipCount - 1 );

#endif

	GpuApi::TextureDesc desc;
	desc.type		= (GpuWrapApi::eTextureType)texInfo.m_type;
	desc.width		= Max<Uint32>(sizeInfo.m_width >> residentMip, 1);
	desc.height		= Max<Uint32>(sizeInfo.m_height >> residentMip, 1);
	desc.numMips	= texInfo.m_mipCount - residentMip;
	desc.format		= texFormat;
	desc.usage		= GpuApi::TEXUSAGE_Samplable | GpuApi::TEXUSAGE_Immutable;
	desc.group		= setup.m_group;

	if ( texInfo.m_type == GpuWrapApi::TEXTYPE_ARRAY )
	{
		desc.depthOrSliceNum = texInfo.m_sliceCount;
	}
	else if ( texInfo.m_type == GpuWrapApi::TEXTYPE_3D )
	{
		desc.depthOrSliceNum = Max<Uint32>( sizeInfo.m_depth >> residentMip, 1 );
	}

	if ( desc.type == GpuWrapApi::TEXTYPE_2D && allowBindless )
	{
		desc.usage |= GpuApi::TEXUSAGE_Bindless;
	}

	// #HACK: If we're compressed, and don't have resident==0, ensure we are still getting a proper texture size. With
	// a non-rectangular texture, and with the resolution clamping in CRenderTexture::Create, it's possible that we
	// might drop enough mips that the resulting mip 0 is not multiple of 4. We can't just pad the dimensions, because
	// the encoded BC blocks might not have valid values in the padded area.
	if ( GpuWrapApi::IsTextureFormatDXT(desc.format) )
	{
		while ((residentMip > 0) && ((desc.width&3) != 0 || (desc.height&3) != 0))
		{
			--residentMip;
			desc.width				= Max<Uint32>(sizeInfo.m_width >> residentMip, 1);
			desc.height				= Max<Uint32>(sizeInfo.m_height >> residentMip, 1);
			if ( texInfo.m_type == GpuWrapApi::TEXTYPE_3D )
			{
				desc.depthOrSliceNum = sizeInfo.m_depth >> residentMip;
			}
			desc.numMips = texInfo.m_mipCount - residentMip;
		}
	}

	//desc.inPlaceType = GpuApi::INPLACE_Texture;

	GpuApi::TextureInitData initData;
	if ( blob.GetVersion() < rend::IRenderTextureBlob::RenderTextureBlobVersions::VER2_FixedConsoleData )
	{
#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )
		// #HACK: PC data is currently cooked in the _wrong_ layout! We want it mip0tex0, mip0tex1, ..., mip0texN, mip1tex0,
		// etc. but instead mip / slice ordering is reversed. Which means we can't just point to an offset in the data to
		// get a smaller mip. Once we figure out any other requirements we might have for texture data (for in-place loading
		// and such), we'll need to do a resave of everything.
		if ( residentMip > 0 )
		{
			RED_ASSERT( !blob.IsTiled(), "Not expecting fully cooked data on PC!" );

			initData = GpuApi::TextureInitData::Uncooked( [&]( GpuApi::TextureInitData::Mip& outData )
			{
				Uint32 textureSliceMipPitch = outData.sliceIndex * desc.numMips;
				const Uint8* textureSliceDataPtr = static_cast<const Uint8*>(blob.m_textureData.Data().Data()) + outData.sliceIndex * texInfo.m_sliceSize;

				const rend::TMipMapInfoArray& mips = blob.m_header.m_mipMapInfo;
				const rend::MipMapInfo& mip = mips[outData.mipIndex + residentMip];

				if ( mip.m_placement.m_offset > blob.m_textureData.Data().GetSize() ||
					mip.m_placement.m_offset + mip.m_placement.m_size > blob.m_textureData.Data().GetSize() )
				{
					RED_LOG_ERROR( "Invalid cooked texture data" );
					return;
				}

				outData.rawPtr = textureSliceDataPtr + mip.m_placement.m_offset;
#if defined( RED_PLATFORM_WEB )
				if ( outData.sliceIndex == 0 )
					CpApplyMenuLogoOverrideChain( desc.format, blob.m_header.m_sizeInfo.m_width, blob.m_header.m_sizeInfo.m_height, outData.mipIndex + residentMip, 1,
						const_cast< Uint8* >( textureSliceDataPtr ) + mip.m_placement.m_offset, mip.m_placement.m_size );
#endif
			} );
		}
		else
#endif
		{
			// #TODO: Eventually we should probably be pulling this from the blob -- it has an array of mip info already,
			// but those are not currently correct on consoles so we'll need to do a resave first. After all texture cook
			// issues are resolved, then we can have offsets precomputed and not have to do this.
			GpuApi::CookedTextureMipPlacement mipPlacement;
			{
				GpuApi::TextureDesc baseDesc = desc;
				baseDesc.width				= Max<Uint32>( sizeInfo.m_width, 1 );
				baseDesc.height				= Max<Uint32>( sizeInfo.m_height, 1 );
				baseDesc.depthOrSliceNum	= Max<Uint32>( sizeInfo.m_depth, 1 );
				baseDesc.numMips			= texInfo.m_mipCount;
				mipPlacement = GpuApi::CalculateCookedTextureMipChainOffsetAndSize( baseDesc, residentMip );
			}

			if ( mipPlacement.offset >= blob.m_textureData.Data().GetSize() || mipPlacement.offset + mipPlacement.size > blob.m_textureData.Data().GetSize() )
			{
				RED_LOG_ERROR( "Invalid cooked texture data" );
				return GpuApi::TextureRef();
			}

			const void* dataPtr = blob.m_textureData.Data().Data();
#if defined( RED_PLATFORM_WEB )
			CpApplyMenuLogoOverrideChain( desc.format, sizeInfo.m_width, sizeInfo.m_height, residentMip, texInfo.m_mipCount - residentMip,
				const_cast< Uint8* >( static_cast< const Uint8* >( dataPtr ) ) + mipPlacement.offset, mipPlacement.size );
#endif
			initData = GpuApi::TextureInitData::Cooked( red::OffsetPtr( dataPtr, mipPlacement.offset ) );
		}
	}
	else
	{
		const void* dataPtr = blob.m_textureData.Data().Data();
		Uint32 mipOffset = header.m_mipMapInfo[residentMip].m_placement.m_offset;
#if defined( RED_PLATFORM_WEB )
		if ( mipOffset < blob.m_textureData.Data().GetSize() )
			CpApplyMenuLogoOverrideChain( desc.format, sizeInfo.m_width, sizeInfo.m_height, residentMip, desc.numMips,
				const_cast< Uint8* >( static_cast< const Uint8* >( dataPtr ) ) + mipOffset, blob.m_textureData.Data().GetSize() - mipOffset );
#endif
		initData = GpuApi::TextureInitData::Cooked( red::OffsetPtr( dataPtr, mipOffset ) );
	}

	GpuApi::TextureRef newTexture = GpuApi::CreateTexture( desc, initData );
    if (traceBlob) std::fprintf(stderr,"CP2077 blob upload probe: phase=created format=%u mode=%u ref=%u\n",unsigned(desc.format),unsigned(initData.m_mode),newTexture.ID());
	if ( !newTexture )
	{
		const Bool isCompressed = IsTextureFormatDXT(desc.format);
		const Bool isMultipleOf4 = ((desc.width % 4 == 0) && (desc.height % 4) == 0);

		RED_DATA_ERROR_BEGIN( red::LoggerCategory_Rendering, !isCompressed || isMultipleOf4,
			"Compressed textures must have dimensions that are a multiple of 4. Texture is using format %s with %ux%u. Make sure the texture dimensions are appropriate for the texture group and compression mode.", GpuWrapApi::GetTextureFormatName(desc.format), desc.width, desc.height);
		RED_DATA_ERROR_ACTION_SHOW_ASSET( blob.GetPath().ToDebugString() );
		RED_DATA_ERROR_END();
	}

	if( IsGameMode() ) // ctremblay: see comment below. It used to be console only. I've extended this to game, but excluding editor.
	{
		// Until we have proper in-place loading, we should clear out the source texture data. We're copying it into the
		// created texture, so it's just a huge waste to keep it alive here as well.
		const_cast<rend::IRenderTextureBlob::TextureDataBuffer&>(blob.m_textureData).Clear();
	}

	return newTexture;
}


CRenderTextureBase::CRenderTextureBase( const GpuApi::TextureRef &texture, Bool dynamicAssignment )
	: m_streamingMipIndex( -1 )
	, m_residentMipIndex( -1 )
	, m_pendingMipIndex( -1 )
	, m_texture( texture )
	, m_lastBindTime( 0 )
	, m_dynamicAssignment( dynamicAssignment )
{	
	m_approxSize = texture ? GpuApi::GetTextureUsedVideoMemory( m_texture ) : 0; // Cache memory size of the texture, this is used in the texture streaming
}

CRenderTextureBase::~CRenderTextureBase()
{
	// Release textures
	GpuApi::SafeRelease( m_texture );

	CancelStreaming();
}

void CRenderTextureBase::InitWithGpuTexture( const GpuApi::TextureRef& texture )
{
	RED_ASSERT( m_texture.isNull() || !IsGameMode() || m_dynamicAssignment, "Trying to change TextureRef for a texture without dynamic assignment enabled" );

	if( m_texture != texture )
	{
		GpuApi::SafeRefCountAssign( m_texture , texture );

		m_approxSize = m_texture ? GpuApi::GetTextureUsedVideoMemory( m_texture ) : 0; // Cache memory size of the texture, this is used in the texture streaming
	}
}

void CRenderTextureBase::SetTextureSetup( const STextureGroupSetup& setup )
{
	m_setup = setup;
}

void CRenderTextureBase::SetDebugName( String&& debugName )
{
#ifndef RED_CONFIGURATION_FINAL
	m_debugName = std::move( debugName );

	if( m_texture )
	{
		// #TODO: Pass in StringView directly.
		GpuApi::SetResourceDebugName( m_texture, m_debugName.AsChar() );
	}
#endif // RED_CONFIGURATION_FINAL
}

Uint32 CRenderTextureBase::GetUsedVideoMemory() const
{
	Uint32 size = GpuApi::GetTextureUsedVideoMemory( m_texture );
	return size;
}

void CRenderTextureBase::OnDeviceLost()
{
	// Release textures
	GpuApi::SafeRelease( m_texture );

	// Cancel streaming
	CancelStreaming();
}

void CRenderTextureBase::OnDeviceReset()
{
	// Nothing, need to recreate stuff on engine side
}


Uint8 CRenderTextureBase::GetMaxMipCount() const
{
	/*// If we can be streamed, get the number of mips from there. Take into account maxStreamingMipIndex, in case we've got a
	// reduced texture quality setting.
	{
		return ss->GetNumMipmaps() - m_maxStreamingMipIndex;
	}*/

	// If we have a resident texture, give the number of mips there.
	if ( m_texture )
	{
		return (Uint8)GpuApi::GetTextureDesc( m_texture ).numMips;
	}

	return 0;
}


Uint16 CRenderTextureBase::GetMaxWidth() const
{
	// If we can be streamed, get the number of mips from there. Take into account maxStreamingMipIndex, in case we've got a
	// reduced texture quality setting.
	/*
	{
		return ss->GetBaseWidth() >> m_maxStreamingMipIndex;
	}*/

	// If we have a resident texture, give the number of mips there.
	if ( m_texture )
	{
		return GpuApi::GetTextureDesc( m_texture ).width;
	}

	return 0;
}

Uint16 CRenderTextureBase::GetMaxHeight() const
{
	// If we can be streamed, get the number of mips from there. Take into account maxStreamingMipIndex, in case we've got a
	// reduced texture quality setting.
	/*
	{
		return ss->GetBaseHeight() >> m_maxStreamingMipIndex;
	}*/

	// If we have a resident texture, give the number of mips there.
	if ( m_texture )
	{
		return GpuApi::GetTextureDesc( m_texture ).height;
	}

	return 0;
}

void CRenderTextureBase::SetHistogramStretchBias( const void* histogramData, Uint32 size )
{
	if( histogramData && size )
	{
		RED_FATAL_ASSERT( (size % sizeof( SHistogramBias )) == 0, "Data seems to not contain SHistogramBias objects" );
		red::Memcpy( &m_histogramBias, histogramData, sizeof( SHistogramBias ) );
	}
}


