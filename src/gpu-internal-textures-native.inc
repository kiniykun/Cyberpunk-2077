// Original texture generators; only include routing and volume slice-offset repair differ.
/**
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "common/gpuApi/src/gpuApiTextureUtils.h"


namespace GpuApi
{

	static TextureRef InitInternalTextureData2D( const char* debugName, Uint32 size, Utils::Texture2DFillFunction funcFill, Bool enablePointMips )
	{
		red::FixedArray<Uint8, RED_KILO_BYTE(32)> dataStorage;
		Uint32 storageOffset = 0;

		// #TODO: GpuApi global constant.
		constexpr Uint32 MAX_MIPS = 14;

		RED_ASSERT(!enablePointMips || red::IsPowerOf2( size ), "Internal texture with mips must have pow2 dimension");

		TextureDesc texDesc;
		texDesc.type = GpuWrapApi::TEXTYPE_2D;
		texDesc.width = size;
		texDesc.height = size;
		texDesc.format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
		texDesc.numMips = (enablePointMips ? (MLog2(size) + 1) : 1);
		texDesc.usage = TEXUSAGE_Samplable|TEXUSAGE_Immutable|TEXUSAGE_Bindless;
		texDesc.group = GpuWrapApi::TEXG_System_Generic;

		Uint8* dataTab[ MAX_MIPS ];
		Uint32 bytesPerPixel = 4;
		Uint32 pitch = bytesPerPixel * size;
		for ( Uint32 mip_i = 0; mip_i < texDesc.numMips; ++mip_i )
		{
			// create init data
			math::Vector4 value;
			math::Vector2 coord;

			const Uint32 dataSize = ( size >> mip_i ) * ( size >> mip_i ) * 4;
			dataTab[ mip_i ] = &dataStorage[storageOffset];
			storageOffset += dataSize;
			RED_ASSERT( storageOffset <= dataStorage.Size(), "Texture data too big for dataStorage");

			if ( mip_i == 0 )
			{
				for ( Uint32 y = 0; y < size; ++y )
				{
					coord.Y = ( y + 0.5f ) / size;

					for ( Uint32 x = 0; x < size; ++x )
					{
						coord.X = ( x + 0.5f ) / size;

						funcFill( &value, &coord );

						Uint8* pos = dataTab[ mip_i ] + y * pitch + x * bytesPerPixel;

						pos[ 0 ] = ( Uint8 )( value.X * 255 );
						pos[ 1 ] = ( Uint8 )( value.Y * 255 );
						pos[ 2 ] = ( Uint8 )( value.Z * 255 );
						pos[ 3 ] = ( Uint8 )( value.W * 255 );
					}
				}
			}
			else
			{
				const Uint32 mipSize = size >> mip_i;
				for ( Uint32 y = 0; y < mipSize; ++y )
				{
					for ( Uint32 x = 0; x < mipSize; ++x )
					{
						const Uint8 *src = dataTab[ mip_i - 1 ] + 2 * y * ( pitch >> ( mip_i - 1 ) ) + 2 * x * bytesPerPixel;
						Uint8 *dst = dataTab[ mip_i ] + y * ( pitch >> mip_i ) + x * bytesPerPixel;
						dst[ 0 ] = src[ 0 ];
						dst[ 1 ] = src[ 1 ];
						dst[ 2 ] = src[ 2 ];
						dst[ 3 ] = src[ 3 ];
					}
				}
			}
		}

		TextureRef texture = CreateTexture( texDesc, TextureInitData::Uncooked([&]( TextureInitData::Mip& mip )
		{
			mip.rawPtr = dataTab[mip.mipIndex];
		}) );

		RED_ASSERT( texture, "Failed to create internal texture '%s'", debugName );
		GpuApi::SetResourceDebugName( texture, debugName );

		return texture;
	}

	static TextureRef InitInternalTextureDataCUBE( const char* debugName, Uint32 size, Utils::Texture3DFillFunction funcFill )
	{
		red::FixedArray<Uint8, RED_KILO_BYTE( 32 )> dataStorage;

		TextureDesc texDesc;
		texDesc.type = GpuWrapApi::TEXTYPE_CUBE;
		texDesc.width = size;
		texDesc.height = size;
		texDesc.format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
		texDesc.numMips = 1;
		texDesc.usage = TEXUSAGE_Samplable|TEXUSAGE_Immutable;
		texDesc.group = GpuWrapApi::TEXG_System_Generic;

		Uint32 bytesPerPixel = 4;
		Uint32 pitch = bytesPerPixel * size;
		Uint32 slicePitch = bytesPerPixel * size * size;
		{
			// create init data
			math::Vector4 value;
			math::Vector3 coord;

			const math::Vector3 perFaceAxis[6][3] = {
				{ math::Vector3( 0, 0, 1 ), math::Vector3( 0, 1, 0 ), math::Vector3( 1, 0, 0 ) },
				{ math::Vector3( 0, 0,-1 ), math::Vector3( 0, 1, 0 ), math::Vector3(-1, 0, 0 ) },
				{ math::Vector3( 1, 0, 0 ), math::Vector3( 0, 0,-1 ), math::Vector3( 0, 1, 0 ) },
				{ math::Vector3( 1, 0, 0 ), math::Vector3( 0, 0, 1 ), math::Vector3( 0,-1, 0 ) },
				{ math::Vector3( 1, 0, 0 ), math::Vector3( 0, 1, 0 ), math::Vector3( 0, 0, 1 ) },
				{ math::Vector3(-1, 0, 0 ), math::Vector3( 0, 1, 0 ), math::Vector3( 0, 0,-1 ) },
			};

			// #TODO: Make sure this fills in correctly.
			for ( Uint32 face = 0; face < 6; ++face )
			{
				for ( Uint32 y = 0; y < size; ++y )
				{
					for ( Uint32 x = 0; x < size; ++x )
					{
						math::Vector3 vx = perFaceAxis[face][0] * ((x+0.5f) * 2.0f / size - 1.0f);
						math::Vector3 vy = perFaceAxis[face][1] * ((y+0.5f) * 2.0f / size - 1.0f);
						coord = vx + ( vy + perFaceAxis[face][2] );

						funcFill( &value, &coord );

						const Uint32 baseIdx = ( x * bytesPerPixel ) + ( y * pitch ) + ( slicePitch * face );

						dataStorage[baseIdx + 0] = ( Uint8 )( value.X * 255 );
						dataStorage[baseIdx + 1] = ( Uint8 )( value.Y * 255 );
						dataStorage[baseIdx + 2] = ( Uint8 )( value.Z * 255 );
						dataStorage[baseIdx + 3] = ( Uint8 )( value.W * 255 );
					}
				}
			}
		}

		TextureRef texture = CreateTexture( texDesc, TextureInitData::Uncooked( [&]( TextureInitData::Mip& mip )
		{
			mip.rawPtr = &dataStorage[slicePitch*mip.sliceIndex];
		} ) );

		RED_ASSERT( texture, "Failed to create internal texture '%s'", debugName );
		GpuApi::SetResourceDebugName( texture, debugName );

		return texture;
	}

	static TextureRef InitInternalTextureData3D( const char* debugName, Uint32 size, Utils::Texture3DFillFunction funcFill )
	{
		red::FixedArray<Uint8, RED_KILO_BYTE( 32 )> dataStorage;

		TextureDesc texDesc;
		texDesc.type = GpuWrapApi::TEXTYPE_3D;
		texDesc.width = size;
		texDesc.height = size;
		texDesc.depthOrSliceNum = size;
		texDesc.format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
		texDesc.numMips = 1;
		texDesc.usage = TEXUSAGE_Samplable|TEXUSAGE_Immutable;
		texDesc.group = GpuWrapApi::TEXG_System_Generic;

		Uint32 bytesPerPixel = 4;
		Uint32 pitch = bytesPerPixel * size;
		Uint32 slicePitch = pitch * size;

		{
			// create init data
			math::Vector4 value;
			math::Vector3 coord;

			for ( Uint32 z = 0; z < size; ++z )
			{
				coord.Z = (z + 0.5f) / size;

				for ( Uint32 y = 0; y < size; ++y )
				{
					coord.Y = (y + 0.5f) / size;

					for ( Uint32 x = 0; x < size; ++x )
					{
						coord.X = (x + 0.5f) / size;

						funcFill( &value, &coord );

						const Uint32 baseIdx = z * slicePitch + y * pitch + x * bytesPerPixel; // Repair original volume slice offset.
						dataStorage[baseIdx + 0] = (Uint8)(value.X * 255);
						dataStorage[baseIdx + 1] = (Uint8)(value.Y * 255);
						dataStorage[baseIdx + 2] = (Uint8)(value.Z * 255);
						dataStorage[baseIdx + 3] = (Uint8)(value.W * 255);
					}
				}
			}
		}

		TextureRef texture = CreateTexture( texDesc, TextureInitData::Uncooked( [&]( TextureInitData::Mip& mip )
		{
			mip.rawPtr = dataStorage.Data();
		} ) );

		RED_ASSERT( texture, "Failed to create internal texture '%s'", debugName );
		GpuApi::SetResourceDebugName( texture, debugName );

		return texture;
	}



	TextureRef GetInternalTexture( eInternalTexture internalTexture )
	{
		RED_ASSERT(internalTexture >= 0 && internalTexture < INTERTEX_Max, "Invalid internal texture");
		const TextureRef& ref = GetDeviceData().m_InternalTextures[internalTexture];
		GPUAPI_ASSERT( ref, "Internal texture reference is invalid" );
		return ref;
	}


	void InitInternalTextures()
	{
		SDeviceData& dd = GetDeviceData();

#ifdef RED_ASSERTS_ENABLED
		for ( Uint32 i = 0; i < INTERTEX_Max; ++i )
		{
			RED_ASSERT(!dd.m_InternalTextures[i], "Internal textures already initialized");
		}
#endif

		// Create internal textures
		static_assert(INTERTEX_Max == 9, "eInternalTexture has changed");
		dd.m_InternalTextures[INTERTEX_Default2D]		= InitInternalTextureData2D(	"DefaultTexture",	GPUAPI_DEFAULT2D_TEXTURE_SIZE,			Utils::DefaultTextureFill,				false	);
		dd.m_InternalTextures[INTERTEX_DefaultCUBE]		= InitInternalTextureDataCUBE(	"DefaultCube",		GPUAPI_DEFAULTCUBE_TEXTURE_SIZE,		Utils::DefaultCubeTextureFill					);
		dd.m_InternalTextures[INTERTEX_PoissonRotation]	= InitInternalTextureData2D(	"PoissonRotation",	GPUAPI_POISSON_ROTATION_TEXTURE_SIZE,	Utils::GeneratePoissonRotationTexture,	false	);
		dd.m_InternalTextures[INTERTEX_SSAORotation]	= InitInternalTextureData2D(	"SSAORotation",		GPUAPI_SSAO_ROTATION_TEXTURE_SIZE,		Utils::GenerateSSAORotationNoise,		false	);
		dd.m_InternalTextures[INTERTEX_Blank2D]			= InitInternalTextureData2D(	"BlankTexture",		GPUAPI_BLANK2D_TEXTURE_SIZE,			Utils::BlankTextureFill<0, 0, 0, 1>,	false	);
		dd.m_InternalTextures[INTERTEX_MipNoise]		= InitInternalTextureData2D(	"MipNoise",			GPUAPI_MIP_NOISE_TEXTURE_SIZE,			Utils::GenerateMipNoise,				true	);
		dd.m_InternalTextures[INTERTEX_White]			= InitInternalTextureData2D(	"White2D",			GPUAPI_DEFAULT2D_TEXTURE_SIZE,			Utils::BlankTextureFill<1, 1, 1, 1>,	false	);
		dd.m_InternalTextures[INTERTEX_Zero]			= InitInternalTextureData2D(	"Zero2D",			GPUAPI_DEFAULT2D_TEXTURE_SIZE,			Utils::BlankTextureFill<0, 0, 0, 0>,	false	);
		dd.m_InternalTextures[INTERTEX_Blank3D]			= InitInternalTextureData3D(	"Blank3D",			GPUAPI_DEFAULT3D_TEXTURE_SIZE,			Utils::BlankTexture3DFill<0, 0, 0, 1>			);

#ifdef RED_ASSERTS_ENABLED
		for ( Uint32 i = 0; i < INTERTEX_Max; ++i )
		{
			RED_ASSERT(dd.m_InternalTextures[i], "Internal textures failed to initialize");
		}
#endif
	}


	void ShutInternalTextures()
	{
		SDeviceData& dd = GetDeviceData();

		for ( Uint32 i = 0; i < INTERTEX_Max; ++i )
		{
			TextureRef& ref = dd.m_InternalTextures[i];
			SafeRelease( ref );
		}
	}

}
