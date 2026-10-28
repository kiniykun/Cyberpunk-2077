/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#ifdef __EMSCRIPTEN__
#include <atomic>
#include <cstdio>
#include <algorithm>
#include <cstdint>
namespace cp2077::web { // [wcfix] defined in src/gpu-api-texture-web.cpp (multilayer fallback audit)
void MultilayerSetupRegister(const void* key, const char* name, const std::uint16_t* ids, unsigned count);
void MultilayerSetupUnregister(const void* key);
}
#endif
#include "renderMultilayerSetup.h"
#include "renderTexture.h"
#include "renderStructuredBuffer.h"
#include "../../../common/renderData/include/renderMultilayer.h"



// Must be compatible with MultilayerLayerParam in include_multilayer.fx
struct Shader_LayerData
{
	Vector3		m_tintColor;
	Float		m_layerOpacity;

	Float		m_roughScaleIn;
	Float		m_roughBiasIn;
	Float		m_roughScaleOut;
	Float		m_roughBiasOut;

	Float		m_metalScaleIn;
	Float		m_metalBiasIn;
	Float		m_metalScaleOut;
	Float		m_metalBiasOut;

	Float		m_colorMaskScaleIn;
	Float		m_colorMaskBiasIn;
	Float		m_colorMaskScaleOut;
	Float		m_colorMaskBiasOut;

	Float		m_layerTile;
	Float		m_microblendTile;
	Float		m_microblendContrastInv;
	Float		m_microblendNormalStrength;

	Vector2		m_microblendOffset;
	Vector2 pad0;
	Vector2		m_layerOffset;

	Float		m_normalScale;
	Uint32 pad1;
	Uint32		m_texMapping_Microblends;
	Uint32		m_texMapping_Diffuse_Normal;
	Uint32		m_texMapping_Roughness_Metalness;
	Uint32 pad2;
};
static_assert(sizeof(Shader_LayerData) == 8*sizeof(Vector4), "Shader_LayerData has unexpected size");

static constexpr Uint32 c_maxMultilayerSetupLayers = 32;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class RenderMultilayerSetupPool
{
public:
	struct Allocation
	{
		TRenderPtr<CRenderStructuredBuffer> buffer;
		Uint32 offsetInElements;
	};


	Allocation AddToPool( const Shader_LayerData* layersData, Uint32 layersCount )
	{
		Allocation result;

		const Uint64 hash = red::CalculateHash64( layersData, layersCount*sizeof(Shader_LayerData) );

		{
			RED_SCOPE_LOCK( m_lock );

			if ( m_offsetInLastPage + layersCount > c_pageSizeInElements )
			{
				// #TODO: This would be a nice place to add some hinting mechanism, to tell GpuApi that this is mostly
				// immutable, so it shouldn't track resource state, and we would just explicitly change the state when
				// we actually modify it.
				GpuApi::BufferDesc bufferDesc;
				bufferDesc.size = c_pageSize;
				bufferDesc.structureStride = sizeof( Shader_LayerData );
				bufferDesc.category = GpuWrapApi::BCC_Structured;
				bufferDesc.usage = GpuApi::BUT_Default;
				bufferDesc.group = GpuWrapApi::BufferGroup::Multilayer;
				GpuApi::BufferRef newBuffer = GpuApi::CreateBuffer( bufferDesc );

				TRenderPtr<CRenderStructuredBuffer> newPage = CRenderStructuredBuffer::Create( newBuffer, false );
				newPage->SetDebugName( "mlsetup_pool" );

				m_pages.PushBack( newPage );
				m_offsetInLastPage = 0;
			}

			result.buffer = m_pages.Back();
			result.offsetInElements = m_offsetInLastPage;

			m_offsetInLastPage += layersCount;
		}

#ifdef RED_PLATFORM_CONSOLE
		// Copy the data in. On consoles we can write it directly, this is a new setup, and we never reuse buffer space,
		// so we don't really have to worry about synchronization or anything.
		GpuApi::LoadBufferDataImmediate( result.buffer->GetBufferRef(),
			result.offsetInElements*sizeof( Shader_LayerData ), layersCount*sizeof( Shader_LayerData ), layersData );
#else
		PendingUpload newUpload;
		newUpload.data.Reserve( layersCount );
		newUpload.data.PushBack( red::MakeArraySpan( layersData, layersCount ) );
		newUpload.allocation = result;
		{
			RED_SCOPE_LOCK( m_lock );
			m_pendingUploads.PushBack( std::move(newUpload) );
		}
#endif

		return result;
	}


	void FlushPendingUploads()
	{
#ifndef RED_PLATFORM_CONSOLE
		RED_SCOPE_LOCK( m_lock );

		if ( !m_pendingUploads.Empty() )
		{
			PC_SCOPE( RenderMultilayerSetup::FlushPendingUploads );
			PC_GPU_SCOPE( RenderMultilayerSetup::FlushPendingUploads );

			for ( auto& pendingUpload : m_pendingUploads )
			{
				GpuApi::LoadBufferData( pendingUpload.allocation.buffer->GetBufferRef(),
					pendingUpload.allocation.offsetInElements*sizeof( Shader_LayerData ),
					pendingUpload.data.DataSize(), pendingUpload.data.Data() );
			}

			// Clear and shrink. After engine startup we shouldn't really be seeing anything more coming here, so no
			// reason to leave an allocated buffer.
			m_pendingUploads.Clear();
			m_pendingUploads.Shrink();
		}
#endif
	}


private:

	static constexpr Uint32 c_pageSize = RED_MEGA_BYTE(1);
	static constexpr Uint32 c_pageSizeInElements = c_pageSize / sizeof(Shader_LayerData);
	static_assert( (c_pageSize % sizeof(Shader_LayerData)) == 0, "c_pageSize must be multiple of element size" );

	red::RWSpinLock m_lock;

	red::DynArray<TRenderPtr<CRenderStructuredBuffer>> m_pages { rend::PoolRenderResources() };
	Uint32 m_offsetInLastPage { c_pageSize };

#ifndef RED_PLATFORM_CONSOLE
	struct PendingUpload
	{
		red::DynArray<Shader_LayerData> data { rend::PoolRenderResources() };
		Allocation allocation;
	};
	red::DynArray<PendingUpload> m_pendingUploads { rend::PoolRenderResources() };
#endif
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


RenderMultilayerSetupPool RenderMultilayerSetup::s_multilayerSetupPool;


static Uint32 ExtractMLLayerTexture( const TRenderPtr<IRenderResource>& texResource, red::DynArray<TRenderPtr<IRenderResource>>& textureResources )
{
	GpuApi::STextureIndirection indirection;
	if ( texResource != nullptr )
	{
		indirection = GpuApi::GetTextureIndirection( static_cast<CRenderTexture*>(texResource.Get())->GetTextureRef() );
		red::alg::PushBackUnique( textureResources, texResource );
	}
	if ( !indirection.IsValid() )
	{
		indirection = GpuApi::GetTextureIndirection( GpuApi::GetInternalTexture( GpuApi::INTERTEX_Default2D ) );
		RED_ASSERT(indirection.IsValid(), "Internal texture should be available to bindless!");
	}
	return indirection.m_index;
}


RenderMultilayerSetup::RenderMultilayerSetup() = default;
RenderMultilayerSetup::~RenderMultilayerSetup()
{
#ifdef __EMSCRIPTEN__
	cp2077::web::MultilayerSetupUnregister( this ); // [wcfix]
#endif
}


void RenderMultilayerSetup::FlushPendingUploads()
{
	s_multilayerSetupPool.FlushPendingUploads();
}


TRenderPtr<RenderMultilayerSetup> RenderMultilayerSetup::Create( const SRenderDataSource& src, const rend::MultilayerSetupData& data )
{
	if ( data.layers.Size() > c_maxMultilayerSetupLayers )
	{
		RED_LOG_WARNING("Renderer: RenderMultilayerSetup %s has %u layers (max allowed is %u)", src.m_resourcePath.ToDebugString(), data.layers.Size(), c_maxMultilayerSetupLayers );
	}

	const Uint32 numLayers = Min( data.layers.Size(), c_maxMultilayerSetupLayers );
	if ( numLayers == 0 )
	{
		RED_LOG_ERROR("Renderer: Failed to create RenderMultilayerSetup with 0 layers: %s", src.m_resourcePath.ToDebugString() );
		return nullptr;
	}

	TRenderPtr<RenderMultilayerSetup> setup( RED_NEW( RenderMultilayerSetup )() );

	setup->m_layerMask = 0;

	red::StaticArray< Shader_LayerData, c_maxMultilayerSetupLayers > layerParams( numLayers );
	for ( Uint32 i = 0; i < numLayers; ++i )
	{
		const rend::MultilayerLayer& srcLayer = data.layers[i];
		Shader_LayerData& layer = layerParams[i];

		layer.m_tintColor = srcLayer.colorScale*2.0f;
		layer.m_layerTile = srcLayer.materialTile;
		layer.m_microblendTile = srcLayer.microblendTile;
		layer.m_microblendContrastInv = 1.0f/srcLayer.microblendContrast;
		layer.m_microblendNormalStrength = srcLayer.microblendNormalStrength;
		layer.m_microblendOffset = srcLayer.microblendOffset;

		layer.m_layerOpacity = srcLayer.layerOpacity;
		layer.m_layerOffset = srcLayer.layerOffset;

		layer.m_layerOpacity = srcLayer.layerOpacity;
		layer.m_normalScale = srcLayer.normalScale;

		layer.m_roughScaleIn = srcLayer.roughScaleIn;
		layer.m_roughBiasIn = srcLayer.roughBiasIn;
		layer.m_roughScaleOut = srcLayer.roughScaleOut;
		layer.m_roughBiasOut = srcLayer.roughBiasOut;
		layer.m_metalScaleIn = srcLayer.metalScaleIn;
		layer.m_metalBiasIn = srcLayer.metalBiasIn;
		layer.m_metalScaleOut = srcLayer.metalScaleOut;
		layer.m_metalBiasOut = srcLayer.metalBiasOut;

		layer.m_colorMaskScaleIn = srcLayer.colorMaskScaleIn;
		layer.m_colorMaskBiasIn = srcLayer.colorMaskBiasIn;
		layer.m_colorMaskScaleOut = srcLayer.colorMaskScaleOut;
		layer.m_colorMaskBiasOut = srcLayer.colorMaskBiasOut;

		layer.m_texMapping_Microblends = ExtractMLLayerTexture( srcLayer.microblendTexture, setup->m_textureResources );
		layer.m_texMapping_Diffuse_Normal = ExtractMLLayerTexture( srcLayer.colorTexture, setup->m_textureResources ) | ( ExtractMLLayerTexture( srcLayer.normalTexture, setup->m_textureResources ) << 16 );
		layer.m_texMapping_Roughness_Metalness = ExtractMLLayerTexture( srcLayer.roughnessTexture, setup->m_textureResources ) | ( ExtractMLLayerTexture( srcLayer.metalnessTexture, setup->m_textureResources ) << 16 );

		if ( layer.m_layerOpacity > 0.0f )
		{
			setup->m_layerMask |= (1u << i);
		}
	}


#ifdef __EMSCRIPTEN__
    // Inspect immutable original layer values. No ID remapping or pruning.
    static std::atomic<unsigned> demandReports{0};
    if(demandReports.fetch_add(1)<64) {
        Uint16 ids[c_maxMultilayerSetupLayers*5];unsigned n=0;
        const unsigned layerCount=unsigned(layerParams.Size());
        if(layerCount<=c_maxMultilayerSetupLayers) {
            for(unsigned layerIndex=0;layerIndex<layerCount;++layerIndex) {
                const auto& layer=layerParams.TypedData()[layerIndex];
                ids[n++]=Uint16(layer.m_texMapping_Microblends);
                ids[n++]=Uint16(layer.m_texMapping_Diffuse_Normal);
                ids[n++]=Uint16(layer.m_texMapping_Diffuse_Normal>>16);
                ids[n++]=Uint16(layer.m_texMapping_Roughness_Metalness);
                ids[n++]=Uint16(layer.m_texMapping_Roughness_Metalness>>16);
            }
            std::sort(ids,ids+n);
            unsigned unique=0;
            for(unsigned i=0;i<n;++i)if(!i||ids[i]!=ids[i-1])ids[unique++]=ids[i];
            char values[1600];unsigned written=0;values[0]=0;
            for(unsigned i=0;i<unique;++i) {
                const int count=std::snprintf(values+written,sizeof(values)-written,"%s%u",i?",":"",unsigned(ids[i]));
                if(count<0 || unsigned(count)>=sizeof(values)-written)break;
                written+=unsigned(count);
            }
            std::fprintf(stderr,"CP2077 multilayer demand probe: layers=%u activeMask=%u uniqueTextures=%u ids=%s\n",layerCount,unsigned(setup->m_layerMask),unique,values);
        } else {
            std::fprintf(stderr,"CP2077 multilayer demand probe: layers=%u exceedsNativeBound=%u\n",layerCount,unsigned(c_maxMultilayerSetupLayers));
        }
    }
#endif

	if ( data.hintAlwaysLoaded )
	{
		RenderMultilayerSetupPool::Allocation allocation = s_multilayerSetupPool.AddToPool( layerParams.TypedData(), layerParams.Size() );
		setup->m_layersBuffer = allocation.buffer;
		setup->m_layersBufferStart = allocation.offsetInElements;
	}
	else
	{
		setup->m_layersBuffer = CRenderStructuredBuffer::Create( layerParams.Data(), layerParams.DataSize(), layerParams.Size(), GpuWrapApi::BufferGroup::Multilayer );
		if ( setup->m_layersBuffer == nullptr )
		{
			RED_LOG_ERROR( "Renderer: Failed to create layers buffer for MultilayerSetup %s", src.m_resourcePath.ToDebugString() );
			return nullptr;
		}
		setup->m_layersBufferStart = 0;

		if ( src.m_resourcePath.IsEmpty() )
		{
			setup->m_layersBuffer->SetDebugName( "mlsetup:noDebugName" );
		}
		else
		{
			setup->m_layersBuffer->SetDebugName( src.m_resourcePath.ToDebugString() );
		}
	}

#ifdef __EMSCRIPTEN__
	// [wcfix] register this setup's layer texture indices (microblend, diffuse, normal, roughness, metalness per layer)
	// for the per-second multilayer fallback audit / on-demand re-pool (src/gpu-api-texture-web.cpp).
	{
		std::uint16_t cpIds[ c_maxMultilayerSetupLayers * 5 ]; unsigned cpCount = 0;
		for ( Uint32 cpLayer = 0; cpLayer < numLayers; ++cpLayer )
		{
			const Shader_LayerData& cpData = layerParams.TypedData()[ cpLayer ];
			cpIds[ cpCount++ ] = std::uint16_t( cpData.m_texMapping_Microblends );
			cpIds[ cpCount++ ] = std::uint16_t( cpData.m_texMapping_Diffuse_Normal );
			cpIds[ cpCount++ ] = std::uint16_t( cpData.m_texMapping_Diffuse_Normal >> 16 );
			cpIds[ cpCount++ ] = std::uint16_t( cpData.m_texMapping_Roughness_Metalness );
			cpIds[ cpCount++ ] = std::uint16_t( cpData.m_texMapping_Roughness_Metalness >> 16 );
		}
		cp2077::web::MultilayerSetupRegister( setup.Get(), src.m_resourcePath.ToDebugString(), cpIds, cpCount );
	}
#endif

	setup->m_numLayers = numLayers;
	setup->m_texAspectRatio = data.ratio;

	// Layers buffer stores indirections for each layer's textures, so we don't need to separately add the textures to
	// the hash. If there's a setup with exact same parameters, but different textures, then the layerParams will be
	// different.
	setup->m_dataHash = red::CalculateHash64( layerParams.Data(), layerParams.DataSize() );
	setup->m_dataHash = red::CalculateHash64( &data.ratio, sizeof(data.ratio), setup->m_dataHash );

	return setup;
}

TRenderPtr<RenderMultilayerSetup> RenderMultilayerSetup::CreateEmpty()
{
	TRenderPtr<RenderMultilayerSetup> setup( RED_NEW( RenderMultilayerSetup )() );

	Shader_LayerData layerParams;
	{
		layerParams.m_tintColor = 1.0f;
		layerParams.m_layerTile = 1.0f;
		layerParams.m_microblendTile = 1.0f;
		layerParams.m_microblendContrastInv = 1.0f;
		layerParams.m_microblendNormalStrength = 1.0f;
		layerParams.m_microblendOffset = Vector2::ZEROS();

		layerParams.m_layerOpacity = 1.0f;
		layerParams.m_layerOffset = Vector2::ZEROS();

		layerParams.m_layerOpacity = 1.0f;
		layerParams.m_normalScale = 1.0f;

		layerParams.m_roughScaleIn = 1.0f;
		layerParams.m_roughBiasIn = 0.0f;
		layerParams.m_roughScaleOut = 1.0f;
		layerParams.m_roughBiasOut = 0.0f;
		layerParams.m_metalScaleIn = 1.0f;
		layerParams.m_metalBiasIn = 0.0f;
		layerParams.m_metalScaleOut = 1.0f;
		layerParams.m_metalBiasOut = 0.0f;

		layerParams.m_colorMaskScaleIn = 1.0f;
		layerParams.m_colorMaskBiasIn = 0.0f;
		layerParams.m_colorMaskScaleOut = 1.0f;
		layerParams.m_colorMaskBiasOut = 0.0f;

		layerParams.m_texMapping_Microblends = ExtractMLLayerTexture( nullptr, setup->m_textureResources );
		layerParams.m_texMapping_Diffuse_Normal = ExtractMLLayerTexture( nullptr, setup->m_textureResources ) | (ExtractMLLayerTexture( nullptr, setup->m_textureResources ) << 16);
		layerParams.m_texMapping_Roughness_Metalness = ExtractMLLayerTexture( nullptr, setup->m_textureResources ) | (ExtractMLLayerTexture( nullptr, setup->m_textureResources ) << 16);
	}

	setup->m_layersBuffer = CRenderStructuredBuffer::Create( &layerParams, sizeof(layerParams), 1, GpuWrapApi::BufferGroup::Multilayer );
	setup->m_layersBuffer->SetDebugName( "empty_mlsetup" );
	setup->m_layersBufferStart = 0;

	setup->m_numLayers = 1;
	setup->m_texAspectRatio = 1.0f;
	setup->m_layerMask = 1;

	setup->m_dataHash = red::CalculateHash64( &layerParams, sizeof(layerParams) );

	return setup;
}



TRenderPtr<IRenderResource> CRenderInterface::UploadMultilayerSetup( const SRenderDataSource& src, const rend::MultilayerSetupData& data )
{
	RED_ASSERT( !IsDeviceLost(), "Unable to create new render resources when device is lost" );

	TRenderPtr<RenderMultilayerSetup> setup = RenderMultilayerSetup::Create( src, data );
	return setup;
}
