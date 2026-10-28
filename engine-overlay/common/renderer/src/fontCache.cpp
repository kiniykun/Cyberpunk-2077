//
// Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
//	

#include "build.h"
#include "fontCache.h"
#include "quadTreeAllocator.h"
#include "../../renderData/include/font.h"

using rend::Font;
using rend::FontStyle;
using rend::GlyphInfo;

static const Uint32 c_maxGlyphSize = 256;
static const Uint32 c_glyphSizeLevels = 5;
static const Uint32 c_minGlyphSize = c_maxGlyphSize >> ( c_glyphSizeLevels - 1 );
static const Uint32 c_numMipmaps = 3;

static_assert( ( 1 << c_numMipmaps ) <= c_minGlyphSize, "Too many mips" );

namespace Config
{
#ifndef RED_CONFIGURATION_FINAL
	static TConfigVar< Bool > cvDebugGlyphOutlines( "Rendering", "DebugGlyphOutlines", false, eConsoleVarFlag_Developer );
	static TConfigVar< Bool > cvFlushGlyphCache( "Rendering", "FlushGlyphCache", false, eConsoleVarFlag_Developer );
#endif
}

RenderGlyphCache::RenderGlyphCache()
	: m_atlasWidth( 0 )
	, m_atlasHeight( 0 )
	, m_pendingUploads{ rend::PoolGlyphCache() }
	, m_glyphsCache{ rend::PoolGlyphCache() }
{
	static_assert( sizeof( CacheEntry ) == 24, "RenderGlyphCache::CacheEntry glyph cache entry size has changed" );
}

RenderGlyphCache::~RenderGlyphCache()
{
}

Bool RenderGlyphCache::Initialize( Uint32 width, Uint32 height )
{
	RED_FATAL_ASSERT( red::IsPowerOf2( width ), "Gylph cache texture size must be power of two" );
	RED_FATAL_ASSERT( red::IsPowerOf2( height ), "Gylph cache texture size must be power of two" );
	RED_FATAL_ASSERT( width % c_maxGlyphSize == 0, "Gylph cache texture size must be power of max glyph size" );
	RED_FATAL_ASSERT( height % c_maxGlyphSize == 0, "Gylph cache texture size must be power of max glyph size" );

	m_atlasWidth = 0;
	m_atlasHeight = 0;

	// font atlas
	{
		GpuApi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.type = GpuWrapApi::TEXTYPE_2D;
		desc.format = GpuWrapApi::TEXFMT_R8_Unorm;
		desc.usage = GpuApi::TEXUSAGE_Samplable | GpuApi::TEXUSAGE_UnorderedAccess;
		desc.numMips = c_numMipmaps;
		desc.group = GpuWrapApi::TEXG_System_Generic;

		m_fontAtlas = GpuApi::Texture::Create( desc, GpuApi::TextureInitData::ClearToZero() );
		if ( m_fontAtlas.isNull() )
		{
			RED_LOG_ERROR( "RenderGlyphCache: Couldn't create font atlas" );
			return false;
		}

		GpuApi::SetResourceDebugName( m_fontAtlas, "RenderGlyphCache Atlas" );
	}

	// temporary upload buffer
	{
		GpuApi::BufferDesc desc;
		desc.size = c_maxGlyphSize * c_maxGlyphSize;
		desc.category = GpuWrapApi::BCC_Raw;
		desc.usage = GpuApi::BUT_Transient;
		desc.group = GpuWrapApi::BufferGroup::System;
		m_uploadBuffer = GpuApi::Buffer::Create( desc, GpuApi::BufferInitData::Uninitialized() );
		GpuApi::SetResourceDebugName( m_uploadBuffer, "RenderGlyphCache Upload Buffer" );
	}

	m_atlasWidth = width;
	m_atlasHeight = height;
	m_atlasWidthInv = 1.0f / (Float)width;
	m_atlasHeightInv = 1.0f / (Float)height;

	m_allocator = red::CreateUniquePtr<QuadTreeAllocator>();
	m_allocator->Initialize( m_atlasWidth / c_minGlyphSize );

	return true;
}

GpuApi::TextureRef RenderGlyphCache::GetFontTexture() const
{
	return m_fontAtlas;
}

void RenderGlyphCache::NextFrame()
{
	PC_SCOPE( RenderGlyphCache/NextFrame );

	RED_SCOPE_LOCK( m_lock );

	// get rid of old entries
	{
		const Uint32 MaxEntriesToRemove = 4;
		const Uint32 NumFrameUnusedBeforeRemove = 500;

		red::StaticArray< CacheKey, MaxEntriesToRemove > keysToRemove;

		// give back all old entries to the allocator so they can be merged/split
		for ( auto it = m_glyphsCache.Begin(); it != m_glyphsCache.End() && !keysToRemove.Full(); ++it )
		{
			const CacheEntry& cacheEntry = it.Value();
			if ( cacheEntry.m_lastFrameUsed + NumFrameUnusedBeforeRemove < m_frameCounter )
			{
				RED_ASSERT( cacheEntry.m_atlasRect.m_x % c_minGlyphSize == 0 );
				RED_ASSERT( cacheEntry.m_atlasRect.m_y % c_minGlyphSize == 0 );
				const QuadTreeAllocator::Range range{ Uint16( cacheEntry.m_atlasRect.m_x / c_minGlyphSize ), Uint16( cacheEntry.m_atlasRect.m_y / c_minGlyphSize ) };
				m_allocator->Free( range );
				keysToRemove.PushBack( it.Key() );
			}
		}

		for ( const CacheKey& key : keysToRemove )
		{
			m_glyphsCache.Remove( key );
		}
	}

	m_frameCounter++;
	m_wasOverflowLastFrame = false;

#ifndef RED_CONFIGURATION_FINAL
	if ( m_enableDebugGlyphOutlines != Config::cvDebugGlyphOutlines.Get() )
	{
		m_enableDebugGlyphOutlines = Config::cvDebugGlyphOutlines.Get();
		Flush_NoLock();
	}
	if ( Config::cvFlushGlyphCache.Get() )
	{
		Flush_NoLock();
	}
#endif // RED_CONFIGURATION_FINAL
}

void RenderGlyphCache::Flush()
{
	RED_SCOPE_LOCK( m_lock );

	Flush_NoLock();
}

void RenderGlyphCache::Flush_NoLock()
{
	for ( const auto& iter : m_glyphsCache )
	{
		const CacheEntry& cacheEntry = iter.Value();
		RED_ASSERT( cacheEntry.m_atlasRect.m_x % c_minGlyphSize == 0 );
		RED_ASSERT( cacheEntry.m_atlasRect.m_y % c_minGlyphSize == 0 );
		const QuadTreeAllocator::Range range{ Uint16( cacheEntry.m_atlasRect.m_x / c_minGlyphSize ), Uint16( cacheEntry.m_atlasRect.m_y / c_minGlyphSize ) };
		m_allocator->Free( range );
	}

	m_glyphsCache.Clear();
}

void RenderGlyphCache::UpdateTexture()
{
	PC_SCOPE( RenderGlyphCache/UpdateTexture );
	PC_GPU_SCOPE( RenderGlyphCache/UpdateTexture );

	red::StaticArray<Uint8, c_maxGlyphSize*c_maxGlyphSize > cacheEntryBitmap;

	red::DynArray<BitmapToUpload> pendingUploads{ rend::PoolGlyphCache() };
	{
		// TODO won't be needed when updating is done in CRenderInterface::FrameTick
		RED_SCOPE_LOCK( m_lock );
		pendingUploads = std::move( m_pendingUploads );
	}

	for ( const BitmapToUpload& upload : pendingUploads )
	{
		PC_SCOPE( RenderGlyphCache/UploadGlyph );

		VerifyAtlasRect( upload.m_atlasRect );
		RED_FATAL_ASSERT( upload.m_width <= upload.m_atlasRect.m_size, "Bitmap upload rect out of cache entry size" );
		RED_FATAL_ASSERT( upload.m_height <= upload.m_atlasRect.m_size, "Bitmap upload rect out of cache entry size" );

		// Note: updating the whole cache entry (more than the actual glyph) to avoid having garbage on the edges
		const Uint32 numPixels = upload.m_atlasRect.m_size * upload.m_atlasRect.m_size;
		cacheEntryBitmap.Resize( numPixels );
		red::Memzero( cacheEntryBitmap.Data(), numPixels * sizeof( Uint8 ) );

		for ( Uint32 i = 0; i < upload.m_height; ++i )
		{
			Uint8* dest = cacheEntryBitmap.TypedData() + i * upload.m_atlasRect.m_size;
			const Uint8* src = upload.m_bitmapData.TypedData() + i * upload.m_width;
			red::Memcpy( dest, src, upload.m_width * sizeof( Uint8 ) );
		}

		if ( !upload.m_outlineBitmapData.Empty() )
		{
			RED_FATAL_ASSERT( 2 * upload.m_height <= upload.m_atlasRect.m_size, "Bitmap upload rect out of cache entry size" );

			const Uint32 outlineGlyphOffsetX = 0; // upload.m_width;
			const Uint32 outlineGlyphOffsetY = upload.m_height;
			for ( Uint32 i = 0; i < upload.m_height; ++i )
			{
				Uint8* dest = cacheEntryBitmap.TypedData() + ( i + outlineGlyphOffsetY ) * upload.m_atlasRect.m_size + outlineGlyphOffsetX;
				const Uint8* src = upload.m_outlineBitmapData.TypedData() + i * upload.m_width;
				red::Memcpy( dest, src, upload.m_width * sizeof( Uint8 ) );
			}
		}

#ifndef RED_CONFIGURATION_FINAL
		if ( m_enableDebugGlyphOutlines )
		{
			const Uint32 stride = upload.m_atlasRect.m_size;
			Uint8* data = cacheEntryBitmap.TypedData();

			// left/right
			for ( Uint32 i = 0; i < upload.m_height; ++i )
			{
				data[ i * stride + 0 ] = 255;
				data[ i * stride + 1 ] = 255;
				data[ i * stride + upload.m_width - 1 ] = 255;
				data[ i * stride + upload.m_width - 2 ] = 255;
			}

			// top/bottom
			for ( Uint32 i = 0; i < upload.m_width; ++i )
			{
				data[ i ] = 255;
				data[ stride + i ] = 255;
				data[ stride * ( upload.m_height - 1 ) + i ] = 255;
				data[ stride * ( upload.m_height - 2 ) + i ] = 255;
			}
		}
#endif // RED_CONFIGURATION_FINAL

		for ( Uint32 mip = 0; mip < c_numMipmaps; ++mip )
		{
			const Uint32 mipSize = upload.m_atlasRect.m_size >> mip;
			RED_FATAL_ASSERT( mipSize > 0 );

			if ( mip > 0 )
			{
				// generate mipmap from previous in-place
				for ( Uint32 y = 0; y < mipSize; ++y )
				{
					for ( Uint32 x = 0; x < mipSize; ++x )
					{
						// corresponding 2x2 pixels from previous mip
						const Uint8 a = cacheEntryBitmap[ 2 * ( 2 * y * mipSize + x ) ];
						const Uint8 b = cacheEntryBitmap[ 2 * ( 2 * y * mipSize + x ) + 1 ];
						const Uint8 c = cacheEntryBitmap[ 2 * ( ( 2 * y + 1 ) * mipSize + x ) ];
						const Uint8 d = cacheEntryBitmap[ 2 * ( ( 2 * y + 1 ) * mipSize + x ) + 1 ];

						// average pixels from previous mip
						cacheEntryBitmap[ y * mipSize + x ] = (Uint8)( ( (Uint32)a + (Uint32)b + (Uint32)c + (Uint32)d + 2 ) / 4 );
					}
				}
			}

			const Rect rect = Rect::Make( upload.m_atlasRect.m_x >> mip, upload.m_atlasRect.m_y >> mip, mipSize, mipSize );
#if defined( RED_PLATFORM_WEB )
			// WebGPU: copy the CPU bitmap straight into the atlas mip (recorded in the bound command list,
			// rows padded to 256 bytes by GpuApi) instead of the smallTextureUpdate_R8_Unorm compute pass,
			// which needs an r8unorm storage texture and had no pipeline, leaving the atlas empty (no UI text).
			GpuApi::LoadTextureData2D( m_fontAtlas, mip, 0, &rect, cacheEntryBitmap.Data(), mipSize );
#else
			GpuApi::LoadBufferData( m_uploadBuffer, 0, mipSize * mipSize, cacheEntryBitmap.Data() );

			GetRenderer()->LoadSmallTextureData2D( m_uploadBuffer, m_fontAtlas, mip, rect, mipSize );
#endif
		}
	}
}

const FontStyle RenderGlyphCache::AdjustFontStyle( const Font* font, const FontStyle& style )
{
	FontStyle adjustedStyle = style;

	const Uint32 maxSteps = 10;
	for ( Uint32 i = 0; i < maxSteps; ++i )
	{
		// check if the font (largest glyph) will fit the largest cache entry
		// if not, reduce the size a bit and try again
		Font::Metrics metrics = font->GetMetrics( adjustedStyle );

		if ( style.outlineWidth > 0 )
		{
			// glyphs with outline generate two bitmaps placed side by side in atlas slot
			if ( metrics.maxGlyphWidth > metrics.maxGlyphHeight )
			{
				metrics.maxGlyphHeight *= 2;
			}
			else
			{
				metrics.maxGlyphWidth *= 2;
			}
		}

		if ( metrics.maxGlyphWidth <= c_maxGlyphSize && metrics.maxGlyphHeight <= c_maxGlyphSize )
		{
			break;
		}

		adjustedStyle.size = static_cast<Uint16>( 0.75f * adjustedStyle.size );
	}

	return adjustedStyle;
}

void RenderGlyphCache::VerifyAtlasRect( const AtlasRect& rect ) const
{
	RED_FATAL_ASSERT( (Uint32)rect.m_x < m_atlasWidth, "Bitmap upload rect out of atlas bounds" );
	RED_FATAL_ASSERT( (Uint32)rect.m_y < m_atlasHeight, "Bitmap upload rect out of atlas bounds" );
	RED_FATAL_ASSERT( (Uint32)rect.m_x + (Uint32)rect.m_size <= m_atlasWidth, "Bitmap upload rect out of atlas bounds" );
	RED_FATAL_ASSERT( (Uint32)rect.m_y + (Uint32)rect.m_size <= m_atlasHeight, "Bitmap upload rect out of atlas bounds" );
}

RenderGlyphCache::AtlasRect RenderGlyphCache::AddGlyphToAtlas( red::DynArray<Uint8>& bitmap, red::DynArray<Uint8>& outlineBitmap, Uint32 width, Uint32 height )
{
	PC_SCOPE( AddGlyphToAtlas );

	RED_FATAL_ASSERT( width > 0 && height > 0, "Invalid glyph size" );

	const Uint32 heightWithOutline = outlineBitmap.Empty() ? height : 2 * height; // if there's outline, pack both glyphs together in one slot
	const Uint32 widthWithOutline = width; // TODO place verticaly or horizontally, depending on max glyph bounding box
	const Uint32 requiredCacheEntrySize = Max( c_minGlyphSize, red::RoundUpToPowerOf2( widthWithOutline ), red::RoundUpToPowerOf2( heightWithOutline ) );

	if ( requiredCacheEntrySize > c_maxGlyphSize )
	{
		return AtlasRect();
	}

	AtlasRect atlasRect;

	// try allocating first
	{
		QuadTreeAllocator::Range range = m_allocator->Allocate( requiredCacheEntrySize / c_minGlyphSize );
		if ( range.IsValid() )
		{
			atlasRect.m_x = range.x * c_minGlyphSize;
			atlasRect.m_y = range.y * c_minGlyphSize;
			atlasRect.m_size = requiredCacheEntrySize;
		}
	}

	if ( !atlasRect.IsValid() )
	{
		// try reusing some old character
		for ( auto it = m_glyphsCache.Begin(); it != m_glyphsCache.End(); ++it )
		{
			const CacheEntry& cacheEntry = it.Value();
			if ( cacheEntry.m_lastFrameUsed < m_frameCounter )
			{
				if ( cacheEntry.m_atlasRect.m_size == requiredCacheEntrySize )
				{
					atlasRect = cacheEntry.m_atlasRect;
					VerifyAtlasRect( atlasRect );

					m_glyphsCache.Remove( it.Key() );
					break;
				}
			}
		}
	}

	if ( !atlasRect.IsValid() )
	{
		red::DynArray<CacheKey> keysToRemove{ job::PoolJobScope() };

		// give back all old entries to the allocator so they can be merged/split
		// TODO more clever to do this - we don't want to release everything,
		// just the entries that can make space for the allocation
		for ( auto it = m_glyphsCache.Begin(); it != m_glyphsCache.End(); ++it )
		{
			const CacheEntry& cacheEntry = it.Value();
			if ( cacheEntry.m_lastFrameUsed < m_frameCounter )
			{
				RED_ASSERT( cacheEntry.m_atlasRect.m_x % c_minGlyphSize == 0 );
				RED_ASSERT( cacheEntry.m_atlasRect.m_y % c_minGlyphSize == 0 );
				const QuadTreeAllocator::Range range{ Uint16( cacheEntry.m_atlasRect.m_x / c_minGlyphSize ), Uint16( cacheEntry.m_atlasRect.m_y / c_minGlyphSize ) };
				m_allocator->Free( range );
				keysToRemove.PushBack( it.Key() );
			}
		}

		for ( const CacheKey& key : keysToRemove )
		{
			m_glyphsCache.Remove( key );
		}

		QuadTreeAllocator::Range range = m_allocator->Allocate( requiredCacheEntrySize / c_minGlyphSize );
		if ( range.IsValid() )
		{
			atlasRect.m_x = range.x * c_minGlyphSize;
			atlasRect.m_y = range.y * c_minGlyphSize;
			atlasRect.m_size = requiredCacheEntrySize;
		}
	}

	if ( !atlasRect.IsValid() )
	{
		// ran out of atlas space
		return AtlasRect();
	}

	//store the bitmap for later to be uploaded into the texture
	BitmapToUpload& upload = m_pendingUploads.EmplaceBack();
	upload.m_bitmapData = std::move( bitmap );
	upload.m_outlineBitmapData = std::move( outlineBitmap );
	upload.m_atlasRect = atlasRect;
	upload.m_width = width;
	upload.m_height = height;

	return atlasRect;
}

RenderGlyphCache::CacheEntry* RenderGlyphCache::AddGlyphToCache_NoLock( const Font* font, const FontStyle& style, rend::GlyphIndex glyphIndex )
{
	RED_FATAL_ASSERT( font );

	CacheEntry entry;
	entry.m_glyphIndex = glyphIndex;
	entry.m_lastFrameUsed = m_frameCounter;
	entry.m_atlasRect = AtlasRect();

	const Font::GlyphMetrics glyphMetrics = font->GetGlyphMetrics( style, glyphIndex );

	const Uint32 bitmapSizeInPixels = glyphMetrics.rect.m_width * glyphMetrics.rect.m_height;

	if ( bitmapSizeInPixels > 0 )
	{
		red::DynArray<Uint8> bitmapData{ rend::PoolGlyphCache() };
		bitmapData.Resize( bitmapSizeInPixels );

		red::DynArray<Uint8> outlineBitmapData{ rend::PoolGlyphCache() };
		if ( style.outlineWidth > 0 )
		{
			outlineBitmapData.Resize( bitmapSizeInPixels );
		}

		font->RenderGlyph_Internal( style, glyphIndex, glyphMetrics.rect, bitmapData.TypedData(), style.outlineWidth > 0 ? outlineBitmapData.TypedData() : nullptr );

		entry.m_atlasRect = AddGlyphToAtlas( bitmapData, outlineBitmapData, glyphMetrics.rect.m_width, glyphMetrics.rect.m_height );

		if ( !entry.m_atlasRect.IsValid() )
		{
			RED_LOG_ERROR( "RenderGlyphCache: Failed to allocate cache entry for glyph %u in font '%s', width=%u, height=%u",
				glyphIndex, font->GetPath().ToDebugString(), glyphMetrics.rect.m_width, glyphMetrics.rect.m_height );
			m_wasOverflowLastFrame = true;
			return nullptr;
		}
	}
	else
	{
		return nullptr;
	}

	//fill the UVs
	Vector4 uvs;
	uvs.X = (float)entry.m_atlasRect.m_x * m_atlasWidthInv;
	uvs.Y = (float)entry.m_atlasRect.m_y * m_atlasHeightInv;
	uvs.Z = (float)( entry.m_atlasRect.m_x + glyphMetrics.rect.m_width ) * m_atlasWidthInv;
	uvs.W = (float)( entry.m_atlasRect.m_y + glyphMetrics.rect.m_height ) * m_atlasHeightInv;

	entry.m_uvs = uvs;

	CacheKey key = { font, glyphIndex, style };

	auto result = m_glyphsCache.Insert( std::move( key ), entry );
	RED_FATAL_ASSERT( result.IsSuccessful(), "Failed to add entry to glyph cache. Font: '%s (%s)', font size: %u, glyph index: %u",
		font->GetFontName(), font->GetStyleName(), style.size, glyphIndex );

	return &result.Iterator().Value();
}

void RenderGlyphCache::PopulateGlyphCache( const GlyphInfo* glyphBuffer, Uint32 numGlyphs, const rend::Font* font, const rend::FontStyle& fontStyle )
{
	const FontStyle style = AdjustFontStyle( font, fontStyle );

	red::BitSet64Dynamic missingGlyph( numGlyphs, job::PoolJobScope() );

	{
		RED_SCOPE_SHARED_LOCK( m_lock );

		for( Uint32 i = 0; i < numGlyphs; i++ )
		{
			const GlyphInfo& glyphInfo = glyphBuffer[ i ];

			const CacheKey key{font, glyphInfo.glyphIndex, style};

			CacheEntry* entry = m_glyphsCache.FindPtr( key );
			if( !entry )
			{
				missingGlyph.Set( i );
			}
			else
			{
				entry->m_lastFrameUsed = m_frameCounter;
			}
		}
	}

	if( missingGlyph.PopulationCount() != 0 )
	{
		RED_SCOPE_LOCK( m_lock );

		for( red::BitSetConstIterator< red::BitSet64Dynamic > iter( missingGlyph ); iter.IsValid(); iter.FindNext() )
		{
			const GlyphInfo& glyphInfo = glyphBuffer[ iter.GetIndex() ];

			const CacheKey key{font, glyphInfo.glyphIndex, style};

			CacheEntry* entry = m_glyphsCache.FindPtr( key );
			if( !entry )
			{
				entry = AddGlyphToCache_NoLock( font, style, glyphInfo.glyphIndex );
				if ( entry )
				{
					entry->m_lastFrameUsed = m_frameCounter;
				}
			}
		}
	}
	
}

void RenderGlyphCache::BuildTextVertexBuffer( const GlyphInfo* glyphBuffer, Uint32 numGlyphs, const rend::Font* font, const rend::FontStyle& fontStyle, TextVertex* outVertexBuffer, Uint32 vertexBufferSizeInBytes )
{
	PC_SCOPE( RenderGlyphCache/BuildTextVertexBuffer );

	const FontStyle adjustedStyle = AdjustFontStyle( font, fontStyle );

	RED_SCOPE_SHARED_LOCK( m_lock );

	Uint32 vertexIndex = 0;

	for ( Uint32 i = 0; i < numGlyphs; i++ )
	{
		const GlyphInfo& glyphInfo = glyphBuffer[ i ];

		const CacheKey key{ font, glyphInfo.glyphIndex, adjustedStyle };

		CacheEntry* entry = m_glyphsCache.FindPtr( key );
		if ( !entry )
		{
			// empty glyph - generate dummy vertex, as we need to fill the whole vertex buffer
			const DebugVertexUV dummyVertex( math::Vector4::ZEROS(), math::Color::CLEAR(), 0.0f, 0.0f );
			outVertexBuffer[ vertexIndex++ ] = dummyVertex;
			outVertexBuffer[ vertexIndex++ ] = dummyVertex;
			outVertexBuffer[ vertexIndex++ ] = dummyVertex;
			outVertexBuffer[ vertexIndex++ ] = dummyVertex;
			continue;
		}

		RED_FATAL_ASSERT( entry->m_lastFrameUsed == m_frameCounter, "Frame counter mismatch. PopulateGlyphCache() not called for this glyph buffer?" );

		Float outlineGlyphOffset = 0.0f;
		if ( fontStyle.outlineWidth > 0 )
		{
			const Font::GlyphMetrics glyphMetrics = font->GetGlyphMetrics( adjustedStyle, glyphInfo.glyphIndex );
			outlineGlyphOffset = glyphMetrics.rect.m_height * m_atlasHeightInv;
		}

		const Float posX1 = glyphInfo.x;
		const Float posX2 = posX1 + glyphInfo.width;
		const Float posY1 = glyphInfo.y;
		const Float posY2 = posY1 + glyphInfo.height;

		const math::Half4& uv = entry->m_uvs;
		const math::Half4 colorValue = glyphInfo.color.ToHalfLinearAccu();

		outVertexBuffer[ vertexIndex++ ] = TextVertex{ Vector4( posX1, posY1, outlineGlyphOffset ), colorValue, uv.X, uv.Y };
		outVertexBuffer[ vertexIndex++ ] = TextVertex{ Vector4( posX2, posY1, outlineGlyphOffset ), colorValue, uv.Z, uv.Y };
		outVertexBuffer[ vertexIndex++ ] = TextVertex{ Vector4( posX2, posY2, outlineGlyphOffset ), colorValue, uv.Z, uv.W };
		outVertexBuffer[ vertexIndex++ ] = TextVertex{ Vector4( posX1, posY2, outlineGlyphOffset ), colorValue, uv.X, uv.W };
	}

	RED_FATAL_ASSERT( vertexBufferSizeInBytes == vertexIndex * sizeof( DebugVertexUV ), "Vertex buffer overflow" );
}

