#include <atomic>
#include <cstdio>
/*
 * Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
 */
#include "build.h"
#include "renderEngineHandler.h"
#include "baseEngine.h"

#include "screenshotSystem.h"

#include "../../redReflection/include/gatheredResource.h"
#include "../../redReflection/include/serializableMap.h"

#include "../../shaderCache/include/shaderCacheManager.h"

#include "../../resourceMaterial/include/materialTemplate.h"
#include "../../resourceMaterial/include/materialInstance.h"
#include "../../resourceMaterial/include/textureRegionSet.h"
#include "../../resourceMaterial/include/materialCompilationManager.h"
#include "../../resourceMesh/include/mesh.h"
#include "../../resourceMaterial/include/bitmapTexture.h"
#include "../../resourceMaterial/include/textureArray.h"
#include "../../resourceMaterial/include/cubeTexture.h"
#include "../../particles/include/particleEmitter.h"
#include "../../resourceMaterial/include/multilayerSetup.h"
#include "../../redJobs2/include/jobBuilder.h"

 // Enabling this may cause some issues with commandlets and tools when doing shutdown for the whole engine. Need to investigate it.
 // But mostly this code is either dead or legacy
#define USE_SERIALABLE_MAP 0

namespace
{
// Production
res::TStaticResource< CMaterialTemplate > resMultiLayerBakedMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\internal\\multilayered_baked.mt" ), GRF_SkipOnHeadless | GRF_Startup );
res::TStaticResource< CTextureRegionSet > resDefaultWrinklesTextureRegions( RED_CONST_RESOURCEPATH( "engine\\materials\\defaults\\defaultfaceregions.regionset" ), GRF_SkipOnHeadless | GRF_Startup );
res::TStaticResource< CTextureArray > resBlueNoiseTexArray( RED_CONST_RESOURCEPATH( "engine\\textures\\noise\\bluenoise_64_rg.texarray" ), GRF_SkipOnHeadless | GRF_Startup );
res::TStaticResource< CBitmapTexture > resBestFitNormal( RED_CONST_RESOURCEPATH( "engine\\textures\\normals_best_fit.xbm" ), GRF_SkipOnHeadless | GRF_Startup );

// #RemoveAfterCyberpunk
res::TStaticResource< CTextureArray > resNoise3DTexture( RED_CONST_RESOURCEPATH( "engine\\textures\\noise\\noise3d.texarray" ), GRF_SkipOnHeadless | GRF_Startup );

// Fallback for broken material
res::TStaticResource< CMaterialTemplate > resMaterialFallbackShader( RED_CONST_RESOURCEPATH( "engine\\materials\\fallback.remt" ), GRF_Fallback | GRF_Startup ); 

// Defaults for FragmentBuilder
res::TStaticResource< CMaterialTemplate > resMaterialDebugShader( RED_CONST_RESOURCEPATH( "engine\\materials\\debugdraw.remt" ), GRF_SkipOnHeadless | GRF_Startup ); // Not marked as debug as it is used in rendering the profiler
res::TStaticResource< CMaterialTemplate > resMaterialFontShader( RED_CONST_RESOURCEPATH( "engine\\materials\\font.mt" ), GRF_SkipOnHeadless | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resMaterialDebugWireframeShader( RED_CONST_RESOURCEPATH( "engine\\materials\\debugdraw_wireframe.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resMaterialBiasedDebugShader( RED_CONST_RESOURCEPATH( "engine\\materials\\debugdraw_bias.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resMaterialBiasedDebugWireframeShader( RED_CONST_RESOURCEPATH( "engine\\materials\\debugdraw_wireframe_bias.mt" ), GRF_Debug | GRF_Startup );

// Debug
res::TStaticResource< CMaterialTemplate > resShadowsDebugMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\shadows_debug.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resDebugColoringMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug_coloring.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resOverdrawMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\overdraw.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resOverdrawSeethroughMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\overdraw_seethrough.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resWireframeMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\wireframe.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resMultilayeredDebugMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\multilayered_debug.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resGIBackFaceDebugMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\internal\\gi_backface_debug.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resHitProxyMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\hit_proxy.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resSelectionMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\selection.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resUvDensityDebugMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\uv_density.mt" ), GRF_Debug | GRF_Startup );
res::TStaticResource< CMaterialTemplate > resLODColoringDebugMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\debug\\lod_coloring.mt" ), GRF_Debug | GRF_Startup );

// Prototype or Unused (NOTE: Remove if unused)
res::TStaticResource< CMaterialTemplate > resMetalBaseMaterial( RED_CONST_RESOURCEPATH( "engine\\materials\\metal_base.remt" ), GRF_SkipOnHeadless | GRF_Startup );

}

RenderEngineHandler::RenderEngineHandler()
{
}

RenderEngineHandler::~RenderEngineHandler()
{
}

void RenderEngineHandler::ReloadEngineShaders() const
{
	GShaderCache->CacheDirectoriesCRC();

#if !defined(NO_RUNTIME_MATERIAL_COMPILATION) && USE_SERIALABLE_MAP

	// Reload shaders
	for ( const auto& it : GSerializableMap->OfType< IMaterial >() )
	{
		THandle< IMaterial > mat = it.ToHandle();
		if ( mat )
		{
			RED_LOG( "Forcing recompilation %hs.", mat->GetPath().ToDebugString() );
			mat->ForceRecompilation();
		}
	}
#endif
}

void RenderEngineHandler::ReloadMultilayerMaterial() const
{
#if !defined(NO_RUNTIME_MATERIAL_COMPILATION) && USE_SERIALABLE_MAP
	if ( CMaterialTemplate* multiLayerBakedMaterial = resMultiLayerBakedMaterial.GetPtr() )
	{
		multiLayerBakedMaterial->ForceRecompilation();
	}
#endif // NO_RUNTIME_MATERIAL_COMPILATION
}

void RenderEngineHandler::ReloadTextures() const
{ 
#if USE_SERIALABLE_MAP
	// Unload all textures first

	// Grab textures to reload
	Uint32 numTextures = 0;
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		numTextures += 1;

		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture && texture->HasValidPath() )
		{
			texture->ReleaseRenderResource();
		}
	}

	Uint32 numTextureArrays = 0;
	for ( const auto& it : GSerializableMap->OfType< CTextureArray >() )
	{
		numTextureArrays += 1;

		THandle< CTextureArray > array = it.ToHandle();
		if ( array && array->HasValidPath() )
		{
			array->ReleaseRenderResource();
		}
	}

	Uint32 numCubes = 0;
	for ( const auto& it : GSerializableMap->OfType< CCubeTexture >() )
	{
		numCubes += 1;

		THandle< CCubeTexture > cube = it.ToHandle();
		if ( cube && cube->HasValidPath() )
		{
			cube->ReleaseRenderResource();
		}
	}

	// Create resources - bitmap textures
	Uint32 textureIndex = 0;
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture )
		{
			texture->CreateRenderResource();
		}
	}

	// Create resources - texture arrays
	Uint32 textureArrayIndex = 0;
	for ( const auto& it : GSerializableMap->OfType< CTextureArray >() )
	{
		THandle< CTextureArray > array = it.ToHandle();
		if ( array )
		{
			array->CreateRenderResource();
		}
	}

#ifndef NO_CUBEMAP_GENERATION
	// Create resources - cube textures
	Uint32 cubeIndex = 0;
	for ( const auto& it : GSerializableMap->OfType< CCubeTexture >() )
	{
		THandle< CCubeTexture > cube = it.ToHandle();
		if ( cube )
		{
			cube->CreateRenderResource();
		}
	}
#endif

#endif
}

void RenderEngineHandler::CreateTextureResourcesOnInitDevice() const
{
#if USE_SERIALABLE_MAP
#ifdef RED_COMPILER_MSC
	// Recreate resources
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture )
		{
			texture->CreateRenderResource();
		}
	}
#endif
#endif
}

void RenderEngineHandler::ReleaseTexturesOnCloseDevice() const
{
#if USE_SERIALABLE_MAP
	// Release textures
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture )
		{
			texture->ReleaseRenderResource();
		}
	}
#endif
}

void RenderEngineHandler::ReleaseRenderingResources() const
{
#if USE_SERIALABLE_MAP
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture )
		{
			texture->ReleaseRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CTextureArray >() )
	{
		THandle< CTextureArray > array = it.ToHandle();
		if ( array )
		{
			array->ReleaseRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CCubeTexture >() )
	{
		THandle< CCubeTexture > cube = it.ToHandle();
		if ( cube )
		{
			cube->ReleaseRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CParticleEmitter >() )
	{
		THandle< CParticleEmitter > emitter = it.ToHandle();
		if ( emitter )
		{
			emitter->ReleaseRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CMesh >() )
	{
		//it->ReleaseRenderResource();
		//it->ReleaseMaterialSetupResource();
	}

	for ( const auto& it : GSerializableMap->OfType< IMaterial >() )
	{
		THandle< IMaterial > mat = it.ToHandle();
		if ( mat )
		{
			mat->InvalidateRenderResource();
		}
	}

	// ctremblay Component are gone. Refactor or Delete.
	//CDrawableComponent::RecreateProxiesOfRenderableComponents();
#endif
}

void RenderEngineHandler::ResetDevice() const
{
#if USE_SERIALABLE_MAP
	for ( const auto& it : GSerializableMap->OfType< CBitmapTexture >() )
	{
		THandle< CBitmapTexture > texture = it.ToHandle();
		if ( texture )
		{
			texture->CreateRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CCubeTexture >() )
	{
		THandle< CCubeTexture > cube = it.ToHandle();
		if ( cube )
		{
			cube->CreateRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< IMaterial >() )
	{
		THandle< IMaterial > mat = it.ToHandle();
		if ( mat )
		{
			mat->InvalidateRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CParticleEmitter >() )
	{
		THandle< CParticleEmitter > emitter = it.ToHandle();
		if ( emitter )
		{
			emitter->CreateRenderResource();
		}
	}

	for ( const auto& it : GSerializableMap->OfType< CMesh >() )
	{
		//it)->CreateRenderResource();
		//it->CreateMaterialSetupResource();
	}

	// ctremblay Component are gone. Refactor or Delete.
	//CDrawableComponent::RecreateProxiesOfRenderableComponents();
#endif
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetFallbackMaterial() const
{
	// Special case -- during startup, while loading gathered resources, we might ask to precache some invalid
	// technique, which will then try to get a fallback technique. But if the fallback material hasn't loaded yet, then
	// we can't try and access it. GetPtr() requires the resource to be loaded, so we'll just check that first.
	if ( !resMaterialFallbackShader.IsLoaded() )
	{
		return nullptr;
	}

	CMaterialTemplate* fallbackMaterial = resMaterialFallbackShader.GetPtr();
	return fallbackMaterial ? fallbackMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetDebugMaterial() const
{
	CMaterialTemplate* debugMaterial = resMaterialDebugShader.GetPtr();
	return debugMaterial ? debugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetFontMaterial() const
{
	CMaterialTemplate* fontMaterial = resMaterialFontShader.GetPtr();
	return fontMaterial ? fontMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetDebugWireframeMaterial() const
{
	CMaterialTemplate* debugMaterial = resMaterialDebugWireframeShader.GetPtr();
	return debugMaterial ? debugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetBiasedDebugMaterial() const
{
	CMaterialTemplate* debugMaterial = resMaterialBiasedDebugShader.GetPtr();
	return debugMaterial ? debugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetBiasedDebugWireframeMaterial() const
{
	CMaterialTemplate* debugMaterial = resMaterialBiasedDebugWireframeShader.GetPtr();
	return debugMaterial ? debugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetDefaultMeshMaterial() const
{
	CMaterialTemplate* metalbaseMaterial = resMetalBaseMaterial.GetPtr();
	return metalbaseMaterial ? metalbaseMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetMultiLayerBakedMaterial() const
{
	CMaterialTemplate* multiLayerBakedMaterial = resMultiLayerBakedMaterial.GetPtr();
	return multiLayerBakedMaterial ? multiLayerBakedMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetDefaultTextureRegionSet() const
{
	CTextureRegionSet* defaultWrinkleRegions = resDefaultWrinklesTextureRegions.GetPtr();
	return defaultWrinkleRegions ? defaultWrinkleRegions->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetShadowsDebugMaterial() const
{
	CMaterialTemplate* shadowsDebugMaterial = resShadowsDebugMaterial.GetPtr();
	return shadowsDebugMaterial ? shadowsDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetDebugColoringMaterial() const
{
	CMaterialTemplate* shadowsDebugMaterial = resDebugColoringMaterial.GetPtr();
	return shadowsDebugMaterial ? shadowsDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetWireframeMaterial() const
{
	CMaterialTemplate* material = resWireframeMaterial.GetPtr();
	return material ? material->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetHitProxyMaterial() const
{
	CMaterialTemplate* material = resHitProxyMaterial.GetPtr();
	return material ? material->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetSelectionMaterial() const
{
	CMaterialTemplate* material = resSelectionMaterial.GetPtr();
	return material ? material->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetUvDensityDebugMaterial() const
{
	CMaterialTemplate* material = resUvDensityDebugMaterial.GetPtr();
	return material ? material->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetOverdrawDebugMaterial() const
{
	CMaterialTemplate* overdrawDebugMaterial = resOverdrawMaterial.GetPtr();
	return overdrawDebugMaterial ? overdrawDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetOverdrawSeethroughDebugMaterial() const
{
	CMaterialTemplate* overdrawSeethroughDebugMaterial = resOverdrawSeethroughMaterial.GetPtr();
	return overdrawSeethroughDebugMaterial ? overdrawSeethroughDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetMultilayeredDebugMaterial() const
{
	CMaterialTemplate* multilayeredDebugMaterial = resMultilayeredDebugMaterial.GetPtr();
	return multilayeredDebugMaterial ? multilayeredDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetGIBackFaceDebugMaterial() const
{
	CMaterialTemplate* GIDebugMaterial = resGIBackFaceDebugMaterial.GetPtr();
	return GIDebugMaterial ? resGIBackFaceDebugMaterial->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetLODColoringDebugMaterial() const
{
	CMaterialTemplate* material = resLODColoringDebugMaterial.GetPtr();
	return material ? material->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetBlueNoiseTexArray() const
{
	CTextureArray* texArray = resBlueNoiseTexArray.GetPtr();
	auto resource = texArray ? texArray->GetRenderResource() : nullptr;
    static std::atomic<unsigned> blueNoiseReports{0};
    if (blueNoiseReports.fetch_add(1)<4)
        std::fprintf(stderr,"CP2077 blue noise probe: loaded=%u render=%u\n",
            unsigned(texArray!=nullptr),
            unsigned(resource.Get()!=nullptr));
    return resource;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetNoiseTexArray() const
{
	CTextureArray* texArray = resNoise3DTexture.GetPtr();
	return texArray ? texArray->GetRenderResource() : nullptr;
}

TRenderPtr<IRenderResource> RenderEngineHandler::GetBetFitNormalTexture() const
{
	CBitmapTexture* bestFit = resBestFitNormal.GetPtr();
	return bestFit ? bestFit->GetRenderResource() : nullptr;
}

Float RenderEngineHandler::GetRawEngineTime() const
{
	return GEngine->GetRawEngineTime();
}

void RenderEngineHandler::FlushAllLoading() const
{
}

void RenderEngineHandler::RequestExit() const
{
	RED_LOG_INFO( "RenderEngineHandler: Requesting game exit" );
	return GEngine->RequestExit();
}

Bool RenderEngineHandler::IsTakingScreenshot() const
{
	return GetScreenshotSystem().IsTakingScreenshot();
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// CP2077 web [mlblob]: ?mlBlob=1 restores the boot-time preload of base\resource.cooked_mlsetup.
EM_JS(int, CpMlBlobAtBootJS, (), { try { return new URLSearchParams(self.location.search).get("mlBlob") === "1" ? 1 : 0; } catch (e) { return 0; } });
#endif

void RenderEngineHandler::LoadRenderResources( job::Builder& builder )
{
#ifdef __EMSCRIPTEN__
	// CP2077 web [mlblob]: the cooked mlsetup blob imports every multilayer setup/template and their textures
	// (~1000 files, ~175 MB, 2026-10-23 resLog trace) and engine init waited for all of it before the menu.
	// It only keeps those setups resident; without it each Multilayer_Setup loads on demand when a mesh uses it
	// (the original non-blob path). On the web that is fewer first-visit bytes and less resident memory.
	if ( !CpMlBlobAtBootJS() )
	{
		RED_LOG( "CP2077 web: cooked mlsetup blob skipped (on-demand multilayer setups); ?mlBlob=1 restores it" );
		return;
	}
#endif
	res::IssueLoadingRequestParameter param;
	param.path = CookedMultilayer_Setup::GetCookedResourcePath();
	res::ResourceTokenHandle mlsetupBlobToken = GResourceLoader->IssueLoadingRequest( param );
	builder.DispatchWait( mlsetupBlobToken->GetWaitCounter() );

	builder.DispatchJob( "OnCookedMlsetupLoaded", [this, mlsetupBlobToken ]( const job::RunContext& context )
	{
		if( !mlsetupBlobToken->HasFailed() )
		{
			// ctremblay: For all mlsetup request to be rerouted to blob, only need to keep it alive.
			m_mlsetupBlob = red::StaticCast< CookedMultilayer_Setup >( mlsetupBlobToken->GetResource() );
		}
	} );
}