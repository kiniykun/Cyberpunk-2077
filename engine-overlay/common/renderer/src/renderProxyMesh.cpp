/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "renderProxyMesh.h"
#include "renderSkinningData.h"
#include "renderShadowManager.h"
#include "renderElementMeshChunk.h"
#include "renderScene.h"
#include "renderHelpers.h"
#include "renderMaterialSetup.h"
#include "renderMaterial.h"
#include "renderMesh.h"
#include "renderMeshCloth.h"
#include "renderDynamicDecalChunk_Mesh.h"
#include "renderDecalMesh.h"
#include "renderIndexBuffer.h"

#include "../../renderData/include/meshEnum.h"
#include "../../core/include/dataError.h"
#include "renderGeometryBatcher.h"
#include "speedTreeWindSimulation.h"
#include "../../renderData/include/materialLayerParams.h"
#include "../../renderData/include/renderStageContext.h"
#include "../../renderData/include/effectContextUpdateData.h"
#include "../../renderData/include/renderEngineHandler.h"
#include "renderSurfaceCache.h"
#include "renderDynamicDecal.h"
#include "../../renderData/include/renderDismemberment.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/timeOfDayVisibilityResource.h"
#include "../../renderData/include/renderUtils.h"
#include "../../../common/renderer/src/speedTreeWindSimulation.h"
#include "../../redCore/include/globalModeInfo.h"
#include "../../redReflection/include/gatheredResource.h"
#include "../../resourceMaterial/include/materialTemplate.h"
#include "renderProxyCameraData.h"
#include "renderProxyAutoSpawner.h"

#ifdef __EMSCRIPTEN__
#include <atomic>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <set>
#include <cstring>
extern "C" int cp2077_terrain_proxyprobe;
namespace cp2077::web { extern std::atomic<int> skperfEncodeProbe; } // [skperf] round 10: perfprobe flag (gpu-command-web.cpp)
#include <emscripten/emscripten.h> // [wcfix] emscripten_get_now
// [wcfix] Mesh chunks whose render material is null get engine/materials/fallback.remt (the original behaviour).
// Logged with the mesh path and chunk: first 64 events, then powers of two, plus a per-second count.
namespace
{
	std::atomic<unsigned> s_cpFallbackMaterialTotal{ 0 };
	std::atomic<unsigned> s_cpFallbackMaterialWindow{ 0 };
	std::atomic<double> s_cpFallbackMaterialWindowStartMs{ 0.0 };
	// [skperf] round 5: only with ?perfprobe=1 (fresh-player builds print nothing and skip the counters).
	bool CpFallbackMaterialProbeOn()
	{
		static const int value = MAIN_THREAD_EM_ASM_INT( { return new URLSearchParams( location.search ).get( 'perfprobe' ) === '1' ? 1 : 0; } );
		return value != 0;
	}
	void CpFallbackMaterialProbe( const char* where, const void* mesh, unsigned chunk, unsigned lod, unsigned triangles )
	{
		if ( !CpFallbackMaterialProbeOn() ) return;
		const unsigned n = s_cpFallbackMaterialTotal.fetch_add( 1, std::memory_order_relaxed ) + 1;
		const unsigned inWindow = s_cpFallbackMaterialWindow.fetch_add( 1, std::memory_order_relaxed ) + 1;
		if ( n <= 64 || ( n & ( n - 1 ) ) == 0 )
			std::fprintf( stderr, "CP2077 fallback material probe: where=%s mesh=%p chunk=%u lod=%u triangles=%u total=%u\n", where, mesh, chunk, lod, triangles, n );
		const double nowMs = emscripten_get_now();
		double start = s_cpFallbackMaterialWindowStartMs.load( std::memory_order_relaxed );
		if ( start == 0.0 ) { s_cpFallbackMaterialWindowStartMs.compare_exchange_strong( start, nowMs ); return; }
		if ( nowMs - start >= 1000 && s_cpFallbackMaterialWindowStartMs.compare_exchange_strong( start, nowMs ) )
		{
			s_cpFallbackMaterialWindow.store( 0, std::memory_order_relaxed );
			std::fprintf( stderr, "CP2077 fallback material per-second: chunks=%u windowMs=%.0f total=%u lastMesh=%p\n", inWindow, nowMs - start, n, mesh );
		}
	}
}
 // [terrain] set by world runtimeSystemNodeStreaming.cpp (?proxyprobe=1)
// Browser port diagnostics (character preview shows no body): where do mesh proxies leave the collect path?
// One counter per (reason, camera kind, skinned proxy); each logs its first 16 hits, then powers of two.
namespace
{
	enum CpMeshCollectReason : unsigned
	{
		CPMC_NoRenderInScene, CPMC_Invisible, CPMC_CameraTarget, CPMC_RenderFilter, CPMC_Collect,
		CPMC_LodInvisible, CPMC_NoBatchData, CPMC_ChunkNoPasses, CPMC_ChunkPasses, CPMC_Attach, CPMC_Count
	};
	const char* const c_cpMeshCollectReasonNames[ CPMC_Count ] =
	{
		"no-render-in-scene", "invisible", "camera-target", "render-filter", "collect",
		"lod-invisible", "no-batch-data", "chunk-no-passes", "chunk-passes", "attach"
	};
	std::atomic<unsigned> s_cpMeshCollectCounts[ CPMC_Count ][ 3 ][ 2 ];
	// offscreen: 0 = onscreen camera, 1 = offscreen camera (render-to-texture preview), 2 = unknown here
	// [skperf] round 10: this ran for EVERY collected proxy and chunk: one seq-cst atomic add on a handful of
	// shared cache lines from all 9 render job threads (MainSceneDrawables_/Cascades_TestTargetGroups collect),
	// i.e. heavy cross-thread contention on the render chain. Now only with ?perfprobe=1 (flag resolved by the GPU
	// layer, no JS call here); default builds skip it entirely.
	void CpMeshCollectProbe( CpMeshCollectReason reason, unsigned offscreen, bool skinned, unsigned extraA = 0, unsigned extraB = 0 )
	{
		if ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) == 0 ) return;
		const unsigned n = s_cpMeshCollectCounts[ reason ][ offscreen > 2 ? 2 : offscreen ][ skinned ? 1 : 0 ].fetch_add( 1, std::memory_order_relaxed ) + 1;
		if ( n <= 16 || ( n & ( n - 1 ) ) == 0 )
		{
			std::fprintf( stderr, "CP2077 mesh collect probe: reason=%s offscreen=%u skinned=%u count=%u a=%u b=%u\n",
				c_cpMeshCollectReasonNames[ reason ], offscreen, unsigned( skinned ), n, extraA, extraB );
		}
	}
}

// [cull] Distance / screen-size culling of small static world decoration, reusing the engine's own per-proxy far
// auto-hide distance (m_autoHideDistance). Past that distance IsVisibleInCurrentFrame() is false, UpdateDissolveState
// drops RDF_VisibleInScene/Shadows and the engine's dissolve system dithers the proxy out (fade), exactly like its
// own auto-hide. Nothing new in the shaders, the visibility BVH (it does not test auto-hide) or the collect path.
// The clamp is decided once, in the proxy constructor, from data the engine already has:
//   KEEP (never clamped): entity mesh components (m_isDynamicGeometryComponent / m_isForceNoAutohide: devices,
//   interactables, quest/POI objects, anything with a visual controller), skinned meshes (NPCs, player, garments),
//   character/vehicle/weapon/enemy/custom object types, terrain + roads, prefab proxy meshes (RMPF_IsProxyMesh = the
//   distant skyline / merged city LOD), non-scene rendering planes, big architecture (bbox diagonal >= 30 m),
//   meshes whose materials are emissive/neon/screen/sign/window-parallax by name (?cullKeepEmissive=0 to test).
//   Lights, particles (steam/smoke), fog volumes, env probes, decals and the sky are other proxy types: untouched.
//   CANDIDATES: static world-node meshes (mesh, instanced, bended, destructible nodes) and foliage.
// Limits (metres of the engine's FOV-adjusted cached distance; never raised, never below 20 m):
//   size rule  : limit = K * bboxDiagonal, K = 771 / px (px = projected diameter at 1080p / 70 deg).
//                ?cullSmall (default 1 = ON, ~1 px = sub-pixel, invisible; =0 off).
//   ?cull=1/2/3: size rule at 2 / 4 / 8 px, plus a distance cap for small props (diag < 4 m, ?cullPropDiag=<m>) of 400 / 250 / 150 m
//                and for foliage of 250 / 150 / 90 m. Bigger pieces (4..30 m, e.g. facade kit) only get the size rule (no holes).
//   ?cullDist=<m> props cap (implies ?cull=1 when no preset), ?cullPx=<px> size threshold, ?cullFol=<m> foliage cap,
//   ?cullFade=0 instant hide instead of the dissolve, ?cullLod=<x> LOD distance multiplier for static meshes (x > 1
//   = coarser LODs earlier), ?cullprobe=1 classification log; ?perfprobe=1 / ?cullprobe=1 per-second counters.
//   Shadows: ?cullShadowPx=<px> (cull=1/2/3 = 6/12/24 px; =0 off) skips small static casters in shadow collects;
//   ?cullShadowLod=<n> (cull=2/3 = 1) extra shadow LOD bias for static meshes. Decals: ?cullDecal=<m> (cull=1/2/3 = 120/80/50).
//   ?cullPropDiag=<m> (default 4) = bbox diagonal below which the props cap applies. ?potato=N implies cull=N.
//   Sun-shadow cascades (r4): ?cullShadow=0..3 (default = cull preset): static casters need bbox diag >= 0/0.5/1.5/4 m (L1),
//   0/1/3/8 m (L2), 0.5/2/5/12 m (L3) for cascades 0..3; small/medium (diag < 8 m) casters beyond 120/80/50 m (?cullShadowDist)
//   cast none; foliage/clustered casters only in cascades < 3/2/1. Shadow LOD bias presets now 1/1/2.
//   Emissive keep (r4): exact engine template names (see c_emissiveExact); ?cullKeepEmissive=2 also keeps lit-window templates.
//   Measuring FPS: the present pacer caps at 30 FPS by default (gpu-canvas-web.inl, ?fps=0 = uncapped).
// [cull r5] native software-occlusion switches (?occl=0 off, ?occlShadow=0|1|2, ?occlprobe=1), published by
// CpCull::Get() below and read (relaxed loads only) by visibilityQuerySingleFrustum.cpp / visibilityQueryMultiFrustum.cpp.
// -1 = not read yet -> those units behave exactly like the engine.
std::atomic<int> g_cp2077OcclMode{ -1 };
std::atomic<int> g_cp2077OcclProbe{ -1 };
std::atomic<int> g_cp2077OcclShadowMode{ -1 };

namespace CpCull
{
	constexpr Uint16 c_clampedFlag = 1u; // RED_FLAG(0): unused bit of ERenderMeshProxyFlags, set when we lowered the far auto-hide
	struct Settings
	{
		float sizeK = 0.0f;      // 0 = size rule off
		float propCap = 0.0f;    // 0 = off
		float folCap = 0.0f;     // 0 = off
		float decalCap = 0.0f;   // 0 = off
		float propDiag = 4.0f;   // props cap applies below this bbox diagonal (m)
		float shadowK = 0.0f;    // 0 = off; skip small static casters in shadow collects when K * diag < distance
		int shadowLodBias = 0;
		int shadowLevel = 0;        // sun-shadow cascade rules level (0 off, 1..3)
		float cascMinDiag[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f }; // min bbox diagonal of a static caster per cascade
		float cascMaxDist = 0.0f;   // small/medium (diag < 8 m) static casters beyond this camera distance cast no sun shadow
		int folMaxCascade = 4;      // foliage/clustered casters only in cascades < this
		float lodMul2 = 1.0f;
		int preset = 0;
		int small = 1;
		int fade = 1;
		int keepEmissive = 1;
		int probe = 0;
		int counting = 0;
		bool active = false;
	};
	Settings s_settings;
	std::atomic<int> s_ready{ 0 };
	std::mutex s_initMutex;
	double Param( int which )
	{
		// -1 = absent, -2 = not a number; one literal name per call (no string marshalling across MEMORY64)
		switch ( which )
		{
		case 0: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cull' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 1: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullSmall' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 2: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullDist' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 3: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullPx' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 4: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullFol' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 5: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullFade' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 6: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullLod' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 7: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullKeepEmissive' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 8: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullprobe' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 9: return MAIN_THREAD_EM_ASM_DOUBLE( { try { return new URLSearchParams( location.search ).get( 'perfprobe' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
		case 12: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullPropDiag' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 13: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullShadowPx' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 14: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullShadowLod' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 15: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullShadow' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 16: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullShadowDist' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 11: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'potato' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 10: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'cullDecal' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		// [cull r5] native software-occlusion switches, read here once (no MAIN_THREAD_EM_ASM in the vis query TUs)
		case 17: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'occl' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 18: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'occlprobe' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		case 19: return MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'occlShadow' ); if ( v === null ) return -1; const n = parseFloat( v ); return isNaN( n ) ? -2 : n; } catch ( e ) { return -1; } } );
		default: return -1;
		}
	}
	const Settings& Get()
	{
		if ( s_ready.load( std::memory_order_acquire ) )
		{
			return s_settings;
		}
		// No lock is held across the MAIN_THREAD_EM_ASM calls (a main-thread caller blocked on a lock held by a worker
		// that waits for the main thread would deadlock): every early caller reads the switches itself (same values),
		// then the first one publishes under a short lock.
		{
			Settings st;
			const double cull = Param( 0 ), small = Param( 1 ), dist = Param( 2 ), px = Param( 3 ), fol = Param( 4 );
			const double fade = Param( 5 ), lod = Param( 6 ), keepEm = Param( 7 ), probe = Param( 8 ), perf = Param( 9 ), decal = Param( 10 );
			st.preset = cull >= 1 ? ( cull >= 3 ? 3 : int( cull ) ) : 0;
			// [potato] ?potato=N (N >= 1) turns the matching preset on when ?cull is absent (an explicit ?cull=0 wins)
			const double potato = Param( 11 );
			// [skperf] round 21: potato=1 + cull preset 1 together made the game tick wait ~100 ms in the bar (tc5 K1 8-9 fps;
			// tc7 potato=1&cull=0 17 fps, cull=1 alone 15-19 fps). potato=1 no longer implies a cull preset (?cull=1 still works);
			// potato=2|3 keep their presets.
			if ( cull == -1 && potato >= 2 ) st.preset = potato >= 3 ? 3 : int( potato );
			if ( st.preset == 0 && dist > 0 ) st.preset = 1;
			st.small = ( small == 0 ) ? 0 : 1;
			static const float c_presetPx[ 4 ] = { 1.0f, 2.0f, 4.0f, 8.0f };
			static const float c_presetProp[ 4 ] = { 0.0f, 400.0f, 250.0f, 150.0f };
			static const float c_presetFol[ 4 ] = { 0.0f, 250.0f, 150.0f, 90.0f };
			static const float c_presetDecal[ 4 ] = { 0.0f, 120.0f, 80.0f, 50.0f };
			float usePx = ( st.preset > 0 ) ? c_presetPx[ st.preset ] : ( st.small ? c_presetPx[ 0 ] : 0.0f );
			if ( px > 0 ) usePx = float( px );
			st.sizeK = usePx > 0.0f ? 771.0f / usePx : 0.0f;
			st.propCap = dist > 0 ? float( dist ) : c_presetProp[ st.preset ];
			st.folCap = fol > 0 ? float( fol ) : c_presetFol[ st.preset ];
			st.decalCap = decal > 0 ? float( decal ) : c_presetDecal[ st.preset ];
			{ const double pd = Param( 12 ); if ( pd > 0 ) st.propDiag = float( pd ); }
			{
				static const float c_presetShadowPx[ 4 ] = { 0.0f, 6.0f, 12.0f, 24.0f };
				const double sp = Param( 13 );
				const float spx = sp > 0 ? float( sp ) : ( sp == 0 ? 0.0f : c_presetShadowPx[ st.preset ] );
				st.shadowK = spx > 0.0f ? 771.0f / spx : 0.0f;
				static const int c_presetShadowLod[ 4 ] = { 0, 1, 1, 2 };
				const double sl = Param( 14 );
				st.shadowLodBias = sl >= 0 ? ( sl > 4 ? 4 : int( sl ) ) : c_presetShadowLod[ st.preset ];
				// [cull r4] sun-shadow cascade rules (?cullShadow=0..3, default = the cull preset; ?cullShadowDist=<m>)
				const double sh = Param( 15 ), shd = Param( 16 );
				st.shadowLevel = sh >= 0 ? ( sh >= 3 ? 3 : int( sh ) ) : st.preset;
				static const float c_cascMin[ 4 ][ 4 ] = { { 0, 0, 0, 0 }, { 0.0f, 0.5f, 1.5f, 4.0f }, { 0.0f, 1.0f, 3.0f, 8.0f }, { 0.5f, 2.0f, 5.0f, 12.0f } };
				static const float c_cascDist[ 4 ] = { 0.0f, 120.0f, 80.0f, 50.0f };
				static const int c_folCasc[ 4 ] = { 4, 3, 2, 1 };
				for ( int c = 0; c < 4; ++c ) st.cascMinDiag[ c ] = c_cascMin[ st.shadowLevel ][ c ];
				st.cascMaxDist = shd > 0 ? float( shd ) : c_cascDist[ st.shadowLevel ];
				st.folMaxCascade = c_folCasc[ st.shadowLevel ];
			}
			st.fade = ( fade == 0 ) ? 0 : 1;
			st.lodMul2 = lod > 0 ? float( lod * lod ) : 1.0f;
			st.keepEmissive = ( keepEm == 0 ) ? 0 : ( keepEm >= 2 ? 2 : 1 );
			st.probe = probe > 0 ? 1 : 0;
			st.counting = ( st.probe || perf > 0 ) ? 1 : 0;
			st.active = st.sizeK > 0.0f || st.propCap > 0.0f || st.folCap > 0.0f || st.decalCap > 0.0f || st.shadowK > 0.0f || st.shadowLodBias > 0 || st.shadowLevel > 0;
			const double occl = Param( 17 ), occlProbe = Param( 18 ), occlShadow = Param( 19 );
			std::lock_guard< std::mutex > lock( s_initMutex );
			if ( s_ready.load( std::memory_order_relaxed ) )
			{
				return s_settings;
			}
			s_settings = st;
			// [cull r5] publish the occlusion switches for visibilityQuery{Single,Multi}Frustum.cpp
			{
				const int om = occl == 0 ? 0 : 1;
				const int osm = occlShadow >= 0 ? ( occlShadow >= 2 ? 2 : int( occlShadow ) ) : -1;
				const int op = occlProbe > 0 ? 1 : 0;
				g_cp2077OcclMode.store( om, std::memory_order_relaxed );
				g_cp2077OcclShadowMode.store( osm, std::memory_order_relaxed );
				g_cp2077OcclProbe.store( op, std::memory_order_release );
				std::fprintf( stderr, "CP2077 occl switches: occl=%d (engine software occlusion: main view, sun-shadow receiver test, distant-shadow global occlusion) occlShadow=%d (-1 = engine MultiFrustumOcclusionMode 1) occlprobe=%d\n", om, osm, op );
			}
			s_ready.store( 1, std::memory_order_release );
			std::fprintf( stderr, "CP2077 cull switches: cull=%d cullSmall=%d sizePx=%.2f (K=%.0f) propCap=%.0f folCap=%.0f decalCap=%.0f propDiag=%.1f shadowK=%.0f shadowLod=%d sunShadow=%d (minDiag %.1f/%.1f/%.1f/%.1f maxDist=%.0f folCascades<%d) cullFade=%d cullLod=%.2f cullKeepEmissive=%d cullprobe=%d counters=%d\n",
				st.preset, st.small, usePx, st.sizeK, st.propCap, st.folCap, st.decalCap, st.propDiag, st.shadowK, st.shadowLodBias, st.shadowLevel, st.cascMinDiag[ 0 ], st.cascMinDiag[ 1 ], st.cascMinDiag[ 2 ], st.cascMinDiag[ 3 ], st.cascMaxDist, st.folMaxCascade, st.fade, std::sqrt( st.lodMul2 ), st.keepEmissive, st.probe, st.counting );
		}
		return s_settings;
	}

	// construction-time classes (counted only with counters on)
	enum Class : unsigned
	{
		CL_Entity, CL_Dynamic, CL_Skinned, CL_ObjType, CL_TerrainRoad, CL_PrefabProxy, CL_Plane, CL_Large, CL_Emissive, CL_AuthoredFar,
		CL_ClampSize, CL_ClampDist, CL_ClampFoliage, CL_Unchanged, CL_Count
	};
	const char* const c_classNames[ CL_Count ] = { "entity", "dynamic", "skinned", "objType", "terrainRoad", "prefabProxy", "plane", "large", "emissive", "authoredFar",
		"clampSize", "clampDist", "clampFoliage", "candidateUnchanged" };
	std::atomic<unsigned> s_classCounts[ CL_Count ];
	std::atomic<unsigned> s_probeLogged{ 0 };
	std::atomic<unsigned> s_decalCounts[ 5 ]; // entity, emissive, clampSize, clampDist, unchanged

	// per-frame counters (main camera; only with counters on)
	std::atomic<unsigned long long> s_considered{ 0 }, s_culledDist{ 0 }, s_culledSize{ 0 }, s_keptPoi{ 0 }, s_collected{ 0 };
	std::atomic<unsigned long long> s_calls{ 0 };
	std::atomic<unsigned long long> s_cascadeSkipped[ 4 ], s_cascadeCollects[ 4 ];
	std::atomic<unsigned long long> s_mainPasses{ 0 }, s_shadowPasses{ 0 }, s_shadowSkipped{ 0 }, s_shadowCollects{ 0 };
	std::atomic<Int32> s_lastFrame{ 0 };
	std::atomic<unsigned> s_frames{ 0 };
	std::atomic<double> s_windowStart{ 0.0 };

	// [cull r4] Emissive keep by ENGINE MATERIAL TEMPLATE name (GetDisplayableName() = the .mt/.remt template name, e.g.
	// "metal_base", "mesh_decal_emissive"; names seen in this build's shader captures). r3 used substrings ("light",
	// "screen", "sign", "parallax", ...), which kept 102k proxies in Watson (lit-window templates, vehicle_mesh_decal_parallax,
	// ...). Now: exact light-emitting templates, plus the name prefixes neon / signage / hologram and any name containing
	// "emissive" or "blackbody". Lit-window templates (window_parallax_interior_proxy, window_very_long_distance) are only
	// kept with ?cullKeepEmissive=2. Returns 0 = not emissive, 1 = emitter, 2 = lit window.
	const char* const c_emissiveExact[] = { "mesh_decal_emissive", "mesh_decal__blackbody", "blackbodyradiation", "device_diode",
		"device_diode_multi_state", "diode_pavements", "diode_pavements_ui", "lights_interactive", "lightning_plasma",
		"metal_base_trafficlight_proxy", "metal_base_ui", "parallaxscreen", "parallaxscreen_transparent", "proxy_ad", "screen_glitch",
		"television_ad", "vehicle_lights", "particles_hologram" };
	constexpr unsigned c_emissiveExactCount = sizeof( c_emissiveExact ) / sizeof( c_emissiveExact[ 0 ] );
	std::atomic<unsigned> s_emissiveByName[ c_emissiveExactCount + 2 ]; // + generic (prefix/substring), + lit window
	int EmissiveClass( red::StringView name )
	{
		const Uint32 n = Uint32( name.Length() );
		if ( n == 0 || n >= 160 ) return 0;
		char buf[ 160 ];
		for ( Uint32 i = 0; i < n; ++i )
		{
			const char c = name.Data()[ i ];
			buf[ i ] = ( c >= 'A' && c <= 'Z' ) ? char( c - 'A' + 'a' ) : c;
		}
		buf[ n ] = 0;
		// a path-like name: match on the file stem
		const char* stem = buf;
		for ( const char* q = buf; *q; ++q ) if ( *q == '\\' || *q == '/' ) stem = q + 1;
		char base[ 160 ];
		std::strncpy( base, stem, sizeof( base ) - 1 ); base[ sizeof( base ) - 1 ] = 0;
		if ( char* dot = std::strrchr( base, '.' ) ) *dot = 0;
		for ( unsigned i = 0; i < c_emissiveExactCount; ++i )
		{
			if ( std::strcmp( base, c_emissiveExact[ i ] ) == 0 ) { s_emissiveByName[ i ].fetch_add( 1, std::memory_order_relaxed ); return 1; }
		}
		if ( std::strncmp( base, "neon", 4 ) == 0 || std::strncmp( base, "signage", 7 ) == 0 || std::strncmp( base, "hologram", 8 ) == 0 ||
			std::strstr( base, "emissive" ) != nullptr || std::strstr( base, "blackbody" ) != nullptr )
		{
			s_emissiveByName[ c_emissiveExactCount ].fetch_add( 1, std::memory_order_relaxed );
			return 1;
		}
		if ( std::strcmp( base, "window_parallax_interior_proxy" ) == 0 || std::strcmp( base, "window_very_long_distance" ) == 0 )
		{
			s_emissiveByName[ c_emissiveExactCount + 1 ].fetch_add( 1, std::memory_order_relaxed );
			return 2;
		}
		return 0;
	}
	bool NameLooksEmissive( red::StringView name, int keepLevel )
	{
		const int c = EmissiveClass( name );
		return c == 1 || ( c == 2 && keepLevel >= 2 );
	}
	void NoteFrameAndMaybePrint()
	{
		const Int32 frame = Int32( GpuApi::FrameIndex() );
		Int32 last = s_lastFrame.load( std::memory_order_relaxed );
		if ( frame != last && s_lastFrame.compare_exchange_strong( last, frame, std::memory_order_relaxed ) )
		{
			s_frames.fetch_add( 1, std::memory_order_relaxed );
			const double now = emscripten_get_now();
			double start = s_windowStart.load( std::memory_order_relaxed );
			if ( start == 0.0 ) { s_windowStart.compare_exchange_strong( start, now ); return; }
			if ( now - start >= 1000.0 && s_windowStart.compare_exchange_strong( start, now ) )
			{
				const unsigned frames = s_frames.exchange( 0, std::memory_order_relaxed );
				const double f = frames ? double( frames ) : 1.0;
				const unsigned long long considered = s_considered.exchange( 0 ), cd = s_culledDist.exchange( 0 ), cs = s_culledSize.exchange( 0 );
				const unsigned long long kp = s_keptPoi.exchange( 0 ), col = s_collected.exchange( 0 );
				const unsigned long long mp = s_mainPasses.exchange( 0 ), sp = s_shadowPasses.exchange( 0 ), ss = s_shadowSkipped.exchange( 0 ), sc = s_shadowCollects.exchange( 0 );
				std::fprintf( stderr, "CP2077 cull passes: perFrame mainChunkPasses=%.0f shadowChunkPasses=%.0f shadowProxyCollects=%.0f shadowSkippedSmall=%.0f\n", mp / f, sp / f, sc / f, ss / f );
				{
					double cc[ 4 ], cs4[ 4 ];
					for ( int c = 0; c < 4; ++c ) { cc[ c ] = double( s_cascadeCollects[ c ].exchange( 0 ) ) / f; cs4[ c ] = double( s_cascadeSkipped[ c ].exchange( 0 ) ) / f; }
					std::fprintf( stderr, "CP2077 cull cascades: perFrame collected c0=%.0f c1=%.0f c2=%.0f c3=%.0f | skipped c0=%.0f c1=%.0f c2=%.0f c3=%.0f\n", cc[ 0 ], cc[ 1 ], cc[ 2 ], cc[ 3 ], cs4[ 0 ], cs4[ 1 ], cs4[ 2 ], cs4[ 3 ] );
				}
				std::fprintf( stderr, "CP2077 cull probe: frames=%u perFrame considered=%.0f culledDist=%.0f culledSize=%.0f keptPoiVisible=%.0f collectedWithDraws=%.0f | built entity=%u dynamic=%u skinned=%u objType=%u terrainRoad=%u prefabProxy=%u plane=%u large=%u emissive=%u authoredFar=%u clampSize=%u clampDist=%u clampFoliage=%u unchanged=%u\n",
					frames, considered / f, cd / f, cs / f, kp / f, col / f,
					s_classCounts[ CL_Entity ].load(), s_classCounts[ CL_Dynamic ].load(), s_classCounts[ CL_Skinned ].load(), s_classCounts[ CL_ObjType ].load(),
					s_classCounts[ CL_TerrainRoad ].load(), s_classCounts[ CL_PrefabProxy ].load(), s_classCounts[ CL_Plane ].load(), s_classCounts[ CL_Large ].load(),
					s_classCounts[ CL_Emissive ].load(), s_classCounts[ CL_AuthoredFar ].load(), s_classCounts[ CL_ClampSize ].load(), s_classCounts[ CL_ClampDist ].load(), s_classCounts[ CL_ClampFoliage ].load(),
					s_classCounts[ CL_Unchanged ].load() );
				std::fprintf( stderr, "CP2077 cull decals: built entity=%u emissive=%u clampSize=%u clampDist=%u unchanged=%u\n",
					s_decalCounts[ 0 ].load(), s_decalCounts[ 1 ].load(), s_decalCounts[ 2 ].load(), s_decalCounts[ 3 ].load(), s_decalCounts[ 4 ].load() );
					{
						char line[ 1024 ]; int at = std::snprintf( line, sizeof( line ), "CP2077 cull emissive keep by template:" );
						for ( unsigned i = 0; i < c_emissiveExactCount + 2 && at > 0 && at < int( sizeof( line ) ) - 64; ++i )
						{
							const unsigned c = s_emissiveByName[ i ].load( std::memory_order_relaxed );
							if ( c ) at += std::snprintf( line + at, sizeof( line ) - at, " %s=%u", i < c_emissiveExactCount ? c_emissiveExact[ i ] : ( i == c_emissiveExactCount ? "generic(neon/signage/hologram/*emissive*)" : "litWindow" ), c );
						}
						std::fprintf( stderr, "%s\n", line );
					}
			}
		}
	}
}

// [cull] static world decals (renderProxyDecal.cpp overlay). Same size rule as meshes; decal distance cap
// ?cull=1/2/3 = 120 / 80 / 50 m (?cullDecal=<m> override). Entity-attached decals and emissive/sign materials kept.
extern "C" float cp2077_cull_decal_limit( float orig, float diag, int isEntityDecal, const char* materialName, unsigned materialNameLength )
{
	const CpCull::Settings& cs = CpCull::Get();
	if ( !cs.active && !cs.counting ) return orig;
	unsigned cls = 4;
	float limit = orig;
	if ( isEntityDecal ) cls = 0;
	else if ( cs.active && diag > 0.0f )
	{
		if ( cs.sizeK > 0.0f && cs.sizeK * diag < limit ) { limit = cs.sizeK * diag; cls = 2; }
		if ( cs.decalCap > 0.0f && cs.decalCap < limit ) { limit = cs.decalCap; cls = 3; }
		limit = limit < 15.0f ? 15.0f : limit;
		if ( limit < orig * 0.999f )
		{
			if ( cs.keepEmissive && materialName && materialNameLength && CpCull::NameLooksEmissive( red::StringView( materialName, materialNameLength ), cs.keepEmissive ) )
			{
				limit = orig; cls = 1;
			}
		}
		else
		{
			limit = orig; cls = 4;
		}
	}
	if ( cs.counting ) CpCull::s_decalCounts[ cls ].fetch_add( 1, std::memory_order_relaxed );
	return limit;
}
#endif


RTTI_BEGIN_TYPE( RenderProxyCustomData_Mirror );
RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_MaterialParams );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_MiscMeshParams );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_Skinning );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_OriginalLodGroups );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_ClusteredProxyDisabledInstances );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( RenderProxyCustomData_Garment );
	RTTI_PARENT_TYPE( IRenderProxyCustomData );
RTTI_END_TYPE();

// Helpers for treating an enum type as bitfield
#define CHECK_FLAG( mask, flag )	( ( mask & flag ) != 0 )
#define SET_FLAG( mask, flag )		*(Uint16*)&mask |= (Uint16)flag
#define CLEAR_FLAG( mask, flag )	*(Uint16*)&mask &= ~(Uint16)flag
#define CLEAR_MASK( mask )			*(Uint16*)&mask = 0;

using red::DynArray;

namespace Config
{
	TConfigVar< Float > cvDepthPrepassDistanceFactor( "Rendering", "DepthPrepassDistanceFactor", 12.0f, eConsoleVarFlag_Developer );

	TConfigVar< Float > cvMinMeshSizeInCascade0( "Rendering/Shadows", "CascadesHeuristicMinMeshSizeInCascade0", 0.1f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInCascade1( "Rendering/Shadows", "CascadesHeuristicMinMeshSizeInCascade1", 0.5f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInCascade2( "Rendering/Shadows", "CascadesHeuristicMinMeshSizeInCascade2", 1.5f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInCascade3( "Rendering/Shadows", "CascadesHeuristicMinMeshSizeInCascade3", 2.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInDistantShadowsCascade0( "Rendering/Shadows", "DistantHeuristicMinMeshSizeInCascade0", 2.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInDistantShadowsCascade1( "Rendering/Shadows", "DistantHeuristicMinMeshSizeInCascade1", 3.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshSizeInDistantShadowsCascade2( "Rendering/Shadows", "DistantHeuristicMinMeshSizeInCascade2", 6.0f, eConsoleVarFlag_Developer );

	TConfigVar< Float > cvMinMeshAutohideDistanceInCascade0( "Rendering/Shadows", "CascadesHeuristicMinMeshAutohideDistInCascade0", 20.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInCascade1( "Rendering/Shadows", "CascadesHeuristicMinMeshAutohideDistInCascade1", 40.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInCascade2( "Rendering/Shadows", "CascadesHeuristicMinMeshAutohideDistInCascade2", 50.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInCascade3( "Rendering/Shadows", "CascadesHeuristicMinMeshAutohideDistInCascade3", 50.0f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInDistantShadowsCascade0( "Rendering/Shadows", "DistantHeuristicMinMeshAutohideDistInCascade0", 50.f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInDistantShadowsCascade1( "Rendering/Shadows", "DistantHeuristicMinMeshAutohideDistInCascade1", 100.f, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvMinMeshAutohideDistanceInDistantShadowsCascade2( "Rendering/Shadows", "DistantHeuristicMinMeshAutohideDistInCascade2", 300.f, eConsoleVarFlag_Developer );

	TConfigVar< Bool > cvForceGeometryInsteadOfBillboardsForFoliageInDistantShadows( "Rendering/Shadows", "DistantShadowsForceFoliageGeometry", false, eConsoleVarFlag_Developer );
}

static void ConditionalInvalidateLights( const Box& bbox, CRenderSceneEx* scene, const CRenderProxy_Mesh& meshProxy )
{
	//PC_SCOPE( "ConditionalInvalidateLights" );
	// NOTE: proxy can be detached from scene but still be on relink list and will invoke the procedure.
	if ( meshProxy.CanCastLocalShadows() && scene )
	{
		const Bool isMeshDynamic = meshProxy.IsDynamic();
		auto shadowMgr = &scene->GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
		shadowMgr->InvalidateShadowSlicesTouchingBox( bbox, isMeshDynamic );
	}
}

#define HACK_USE_GARMENT_TEXTURE_CACHE

#ifdef HACK_USE_GARMENT_TEXTURE_CACHE
static red::Mutex HACK_garmentTextureCacheLock;
static red::HashMap<Uint64, GpuApi::TextureRef> HACK_garmentTextureCache{ rend::PoolRenderData() };
#endif // HACK_USE_GARMENT_TEXTURE_CACHE

RenderProxyCustomData_Garment::~RenderProxyCustomData_Garment()
{
	for ( GpuApi::TextureRef& textureRef : m_morphOffsetScalesTextures )
	{
		if ( textureRef )
		{
#ifdef HACK_USE_GARMENT_TEXTURE_CACHE
			RED_SCOPE_LOCK( HACK_garmentTextureCacheLock );
			Int32 newRefCount = GpuApi::Release( textureRef );
			if ( newRefCount == 1 )
			{
				HACK_garmentTextureCache.RemoveValue( textureRef );
				GpuApi::Release( textureRef );
			}
#else
			GpuApi::Release( textureRef );
#endif // HACK_USE_GARMENT_TEXTURE_CACHE
		}
	}
}

static GpuApi::TextureRef CreateGarmentVertexMorphOffsetScalesTexture( const red::DynArray< Uint8 >& garmentVertexMorphOffsetScales )
{
	const Uint32 numVerts = garmentVertexMorphOffsetScales.Size();

	const Uint32 c_offsetsTextureWidth = 256;            // 256x256 = 65k texels, we can handle full 64k range of vertex indices.

	const Uint32 offsetsTextureHeight = ( numVerts + ( c_offsetsTextureWidth - 1u ) ) / c_offsetsTextureWidth;   // Rounded up.
	const Uint32 numTexels = c_offsetsTextureWidth * offsetsTextureHeight;

	if ( offsetsTextureHeight == 0 )
	{
		RED_ERROR_ONCE( offsetsTextureHeight == 0, "Creating mesh render proxy with garment data, but no vertices. Not sure why, needs to be investigated." );
		return GpuApi::TextureRef::Null();
	}

	red::DynArray< Uint8 > tempTexData{ rend::PoolRenderMesh() };

	if ( numTexels != numVerts )
	{
		// The data in vertexMorphOffsetScales is sub-optimal, we cannot create texture from it directly. We need to add proper padding.
		RED_FATAL_ASSERT( numTexels >= numVerts );
		RED_LOG_WARNING( "CreateGarmentVertexMorphOffsetScalesTexture: Input morph offset scales data should have multiple of 256 elements, %u elements were found instead", numVerts );

		tempTexData.Resize( numTexels, 0 );
		for ( Uint32 i = 0; i < numVerts; ++i )
		{
			tempTexData[ i ] = garmentVertexMorphOffsetScales[ i ];
		}
	}

	GpuApi::TextureDesc desc;
	desc.type = GpuWrapApi::TEXTYPE_2D;
	desc.width = c_offsetsTextureWidth;
	desc.height = offsetsTextureHeight;
	desc.numMips = 1;
	desc.format = GpuWrapApi::TEXFMT_L8_Unorm;
	desc.usage = GpuApi::TEXUSAGE_Samplable | GpuApi::TEXUSAGE_Immutable;
	desc.group = GpuWrapApi::TEXG_System_Garment;

	return GpuApi::CreateTexture( desc, GpuApi::TextureInitData::UncookedSingle( tempTexData.Empty() ? garmentVertexMorphOffsetScales.TypedData() : tempTexData.TypedData() ) );
}

//////////////////////////////////////////////////////////////////////////

RenderProxyCustomData_MiscMeshParams::~RenderProxyCustomData_MiscMeshParams()
{
	if ( m_dismembermentData != nullptr )
	{
		m_dismembermentData->Destroy();
	}

	GpuApi::SafeRelease( m_destructionRegions );
}

//////////////////////////////////////////////////////////////////////////

Uint8 CRenderProxy_Mesh::ExtractMeshOrder( const RenderProxyMeshComponentInitData& initInfo )
{
	return Min< Uint8 >( initInfo.m_order, BATCHMASK_ORDER_MAX_VALUE );
}

Int32 CRenderProxy_Mesh::ExtractForceLODLevel( const RenderProxyInitInfo& initInfo )
{
	if ( initInfo.m_componentData && initInfo.m_componentData->IsMeshTypeComponentInitData() )
	{
		const RenderProxyMeshComponentInitData * mc = static_cast< const RenderProxyMeshComponentInitData* >( initInfo.m_componentData );
		return mc->m_forcedLODLevel;
	}

	return -1;
}

Uint32 CRenderProxy_Mesh::ExtractLODLevelScales( const RenderProxyInitInfo& initInfo )
{
	if ( initInfo.m_componentData && initInfo.m_componentData->IsMeshTypeComponentInitData() )
	{
		const RenderProxyMeshComponentInitData * mc = static_cast< const RenderProxyMeshComponentInitData* >( initInfo.m_componentData );
		return mc->m_LODLevelScales;
	}

	return 0u;
}

Bool CRenderProxy_Mesh::ExtractForceNoAutohide( const RenderProxyInitInfo& initInfo )
{
	if ( initInfo.m_componentData && initInfo.m_componentData->IsDrawableComponentInitData() )
	{
		const RenderProxyDrawableComponentInitData * dc = static_cast< const RenderProxyDrawableComponentInitData* >( initInfo.m_componentData );
		return dc->m_isForceNoAutohide;
	}

	return false;
}

Bool CRenderProxy_Mesh::ExtractCastShadowsEvenIfNotVisible( const RenderProxyInitInfo& initInfo )
{
	if ( initInfo.m_componentData && initInfo.m_componentData->IsMeshTypeComponentInitData() )
	{
		const RenderProxyMeshComponentInitData * mc = static_cast< const RenderProxyMeshComponentInitData* >( initInfo.m_componentData );
		return mc->m_isCastingShadowsEvenIfNotVisible;
	}

	return false;
}


static RED_FORCE_INLINE Bool CheckRenderMask( const Uint16 baseRenderMask, const Uint16 mask ) 
{
	return 0 != (baseRenderMask & mask);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////

static RED_FORCE_INLINE SRenderStageContext GetMeshChunkRenderStageContext(
	const Uint16 elementFlags,
	const EMaterialVertexFactory vertexFactory,
	const CRenderMaterial& renderMaterial,
	const EDissolveMode dissolveMode,
	const Bool hasComputeSkinningBuffers,
	const Bool allowTangentUpdate,
	const Bool allowDismemberment )
{
	const Bool chunkIsPreskinned = ((elementFlags & RMCF_IsPreskinned) != 0);
	const Bool chunkHasTopology = ((elementFlags & RMCF_HasTopologyInfo) != 0);
	const Bool chunkHasDismemberment = ((elementFlags & RMCF_SupportsDismemberment) != 0);

	SRenderStageContext outContext;
	outContext.m_isPreSkinned = hasComputeSkinningBuffers && ( chunkIsPreskinned || ( allowTangentUpdate && chunkHasTopology ) );
	outContext.m_vertexFactory = vertexFactory;
	outContext.m_isDiscarded = ( dissolveMode != DISSOLVEMODE_Opaque );
	outContext.m_hasDismemberment = allowDismemberment && chunkHasDismemberment && renderMaterial.CanHaveDismemberment();
	return outContext;
}

static void FillInstances( SRenderProxyInstance* dstInstances, const red::ArraySpan<const rend::RenderProxyTransform> srcInstances )
{
	for ( Uint32 i = 0; i < srcInstances.Size(); ++i )
	{
		dstInstances[i].SetInstanceMatrix( srcInstances[ i ] );
	}
}

RenderProxyCustomData_MaterialParams::RenderProxyCustomData_MaterialParams()
{
	for ( Uint32 i = 0; i < NumParams; ++i )
	{
		m_params[ i ] = Vector4::ZEROS();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////

ERenderProxyType CRenderProxy_Mesh::GetType() const
{
	return RPT_Mesh;
}

void CRenderProxy_Mesh::GenerateInclusionMask( vis::QueryMask& outMask )
{
	// Add shadow specific information to the mask and pass to underlying proxy class
	outMask.m_shadows = m_shadowFlags;
	BaseClass::GenerateInclusionMask( outMask );
}

CRenderProxy_Mesh::CRenderProxy_Mesh( const RenderProxyInitInfo& initInfo )
	: BaseClass( initInfo )
	, m_meshProxyFlags( ERenderMeshProxyFlags( 0 ) )
	, m_forcedLODLevel( -1 )
	, m_renderMask( EMeshChunkFlags( MCF_RenderInScene | MCF_RenderInShadows ) )
	, m_hasForcedLOD( 0 )
	, m_isDissolveVisible( 1 )
	, m_objectTypeID( 0 )
	, m_instancesData( nullptr )
	, m_numInstances( 0 )
	, m_offsetInStaticInstanceBuffer( UINT32_MAX )
	, m_meshChunkMask( ~0ull )
	, m_attachedFrameIndex( 0 )
	, m_order( 0 )
{
	if ( initInfo.m_componentData && initInfo.m_componentData->IsMeshTypeComponentInitData() )
	{
		const RenderProxyMeshComponentInitData *mc = static_cast<const RenderProxyMeshComponentInitData *>( initInfo.m_componentData );
		m_customCameraTarget = mc->m_customCameraTarget;

		if ( mc->m_hasMeshResource )
		{	
			if ( mc->m_meshIsPrefabProxy )
			{
				SetMeshProxyFlag( RMPF_IsProxyMesh );
			}

			if ( mc->m_isTerrain )
			{
				SetMeshProxyFlag( RMPF_Terrain );
			}

			if ( mc->m_isClusteredProxy )
			{ 
				SetMeshProxyFlag( RMPF_IsClusteredProxy );
			}

			if( mc->m_isGeneratedByVegetationSystem )
			{
				SetMeshProxyFlag( RMPF_GeneratedByVegetationSystem );				
			}

			if ( mc->m_windImpulseEnabled )
			{
				SetMeshProxyFlag( RMPF_WindImpulseEnabled );
			}

			if ( mc->m_skipDynamicDecals )
			{
				SetMeshProxyFlag( RMPF_SkipDynamicDecals );
			}

			if (mc->m_forceStatic)
			{
				SetMeshProxyFlag( RMPF_ForcedStatic );
			}

			if ( mc->m_numVirtualInstances )
			{
				m_numInstances = mc->m_numVirtualInstances;
			}

			if ( mc->m_isCastingGlobalShadows )
			{
				SET_FLAG( m_drawableFlags, RPF_CastingGlobalShadows );
			}

			if ( mc->m_isCastingLocalShadows )
			{
				SET_FLAG( m_drawableFlags, RPF_CastingLocalShadows );
			}

			if ( mc->m_isTwoSided )
			{
				SET_FLAG( m_drawableFlags, RPF_IsTwoSided );
			}

			if ( mc->m_calcLodDistFromBboxEdge )
			{
				SET_FLAG( m_drawableFlags, RPF_CalcLodDistFromBboxEdge );
			}

			// Setup autohide distance
			{
				if ( ExtractForceNoAutohide( initInfo ) )
				{
					m_autoHideDistance = 80000.0f; 
				}
				else
				{
					m_autoHideDistance = mc->m_autoHideDistanceFar;
					if ( mc->m_constrainAutoHideDistanceToTerrainHeightMap )
					{
						m_autoHideDistance = Min( static_cast<Float>( CRenderTerrainCellCollector::TERRAIN_HEIGHTMAP_MAX_VISIBLE_DISTANCE ), m_autoHideDistance );
					}
				}
			}

			// Initialize renderMesh
			{
				m_mesh =  red::StaticCast<CRenderMesh>( mc->m_renderMesh );
				RED_WARNING( m_mesh, "RenderMesh wasn't created. Check log for broken assets / resource loading problems." );

				if ( m_mesh != nullptr )
				{
					// #TODO: Only create this if we're going to be doing compute skinning.
					GpuComputeSkinningBuffersPtr gpuComputeSkinningBuffers = m_mesh->CreateGpuComputeSkinningBuffers();
					if ( gpuComputeSkinningBuffers )
					{
						RenderProxyCustomData_Skinning* skinningCustomData = AcquireCustomData< RenderProxyCustomData_Skinning >();
						RED_FATAL_ASSERT( skinningCustomData );
						skinningCustomData->m_gpuComputeSkinningBuffers = gpuComputeSkinningBuffers;
					}

					CreateLodGroups( ExtractLODLevelScales( initInfo )  );
					if ( mc->m_materialSetup )
					{
						ApplyMaterialSetup( mc->m_materialSetup );
					}
				}
			}

			// initialize material overlay
			if ( mc->m_materialOverlay )
			{
				RenderProxyCustomData_MiscMeshParams* data = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
				RED_FATAL_ASSERT( data );

				RenderMaterialInfo& materialOverlay = data->m_materialOverlay;

				materialOverlay.m_materialPtr = red::StaticCast<CRenderMaterial>( mc->m_materialOverlay );
				if ( mc->m_materialOverlayParam )
				{
					materialOverlay.m_materialParameters = red::StaticCast<CRenderMaterialParameters>( mc->m_materialOverlayParam );
				}
				else if ( materialOverlay.m_materialPtr )
				{
					materialOverlay.m_materialParameters = materialOverlay.m_materialPtr;
				}
				data->m_materialOverlayIsOverride = mc->m_materialOverride;
			}
			
			SetupRenderMask( *mc );
			m_shadowFlags = SetupShadowFlags( mc->m_boundingBox.CalcSize().Mag3(), GetAutoHideDistance() );

			// Initialize optional garment data
			if ( !mc->m_garmentMorphOffsetScales.Empty() )
			{
				RenderProxyCustomData_Garment* garmentParams = AcquireCustomData< RenderProxyCustomData_Garment >();
				RED_FATAL_ASSERT( garmentParams );

				for ( Uint32 chunkIndex = 0; chunkIndex < mc->m_garmentMorphOffsetScales.Size(); ++chunkIndex )        // Iterate over chunks, prepare offsets texture for each chunk.
				{
					const red::DynArray< Uint8 >& chunkVertexOffsets = mc->m_garmentMorphOffsetScales[ chunkIndex ];
					const Uint64 precomputedHash = (mc->m_garmentMorphOffsetScaleHashes.Size() > chunkIndex) ? mc->m_garmentMorphOffsetScaleHashes[chunkIndex] : 0;
					/// @todo DP: what about chunks with different vertex type?
#ifdef HACK_USE_GARMENT_TEXTURE_CACHE
					GpuApi::TextureRef textureRef;
					{
						Uint64 dataHash = precomputedHash;
						if( !dataHash )
						{
							if( !chunkVertexOffsets.Empty() )
							{
								dataHash = red::CalculateHash64( chunkVertexOffsets.Data(), chunkVertexOffsets.DataSize() );
							}
						}
						else
						{
							// if you need a sanity check, just uncomment the line below
							//ALWAYSENABLED_RED_FATAL_ASSERT( dataHash == red::CalculateHash64( chunkVertexOffsets.Data(), chunkVertexOffsets.DataSize() ) );
						}
						{
							RED_SCOPE_LOCK( HACK_garmentTextureCacheLock );
							auto iter = HACK_garmentTextureCache.Find( dataHash );
							if ( iter == HACK_garmentTextureCache.End() )
							{
								GpuApi::TextureRef newTexture = CreateGarmentVertexMorphOffsetScalesTexture( chunkVertexOffsets );
								iter = HACK_garmentTextureCache.Insert( dataHash, newTexture ).Iterator();
							}

							textureRef = iter.Value();
							if ( textureRef )
							{
								GpuApi::AddRef( textureRef );
							}
						}
					}
#else
					GpuApi::TextureRef textureRef = CreateVertexMorphOffsetScalesTexture( chunkVertexOffsets );
#endif // HACK_USE_GARMENT_TEXTURE_CACHE
					garmentParams->m_morphOffsetScalesTextures.PushBack( textureRef );

					garmentParams->m_indexBufferOverride = red::StaticCast< CRenderIndexBuffer >( mc->m_garmentVisibleTriangleIndexBuffer );
				}

				RED_FATAL_ASSERT( garmentParams->m_indexBufferOverride == nullptr || garmentParams->m_morphOffsetScalesTextures.Size() == garmentParams->m_indexBufferOverride->GetChunkCount() );
			}

			Int32 forcedLodLevel = ExtractForceLODLevel( initInfo );
			if ( forcedLodLevel >= 0 )
			{
				m_hasForcedLOD = 1;
				m_forcedLODLevel = forcedLodLevel;
			}
			else
			{
				m_hasForcedLOD = 0;
			}

            m_isDissolveVisible = mc->m_isDissolveVisible;

#ifdef __EMSCRIPTEN__
			// [cull] see CpCull at the top of this file: lower the far auto-hide of small static decoration only.
			{
				const CpCull::Settings& cs = CpCull::Get();
				if ( cs.active || cs.counting )
				{
					const Uint8 ot = mc->m_objectTypeID;
					const Bool isFoliage = ot == ROT_Foliage || ot == ROT_Grass || mc->m_isGeneratedByVegetationSystem || mc->m_isClusteredProxy;
					const Float diag = mc->m_boundingBox.CalcSize().Mag3();
					CpCull::Class cls = CpCull::CL_Unchanged;
					if ( ExtractForceNoAutohide( initInfo ) ) cls = CpCull::CL_Entity;
					else if ( mc->m_isDynamicGeometryComponent || mc->m_isCloth ) cls = CpCull::CL_Dynamic;
					else if ( mc->m_skinningData ) cls = CpCull::CL_Skinned;
					else if ( !isFoliage && !( ot == ROT_Static || ( ot > ROT_Road && ot <= ROT_STATIC_LAST ) ) )
						cls = ( ot == ROT_Terrain || ot == ROT_Road ) ? CpCull::CL_TerrainRoad : CpCull::CL_ObjType;
					else if ( mc->m_isTerrain || mc->m_constrainAutoHideDistanceToTerrainHeightMap ) cls = CpCull::CL_TerrainRoad;
					else if ( mc->m_meshIsPrefabProxy ) cls = CpCull::CL_PrefabProxy;
					else if ( mc->m_renderingPlane != RPl_Scene ) cls = CpCull::CL_Plane;
					else if ( diag >= 30.0f && !mc->m_isClusteredProxy ) cls = CpCull::CL_Large;
					else if ( cs.active && m_mesh != nullptr && !( diag > 0.0f ) ) cls = CpCull::CL_Large; // empty/invalid box: leave alone
					// distant crowd dots (gamePopulation distantCrowdController: destruction regions, 4 km auto-hide): atmosphere, keep.
					// (t16 probe: 12560 of ~29k Street Kid proxies had an engine far auto-hide >= 1.5 km, so a large auto-hide alone
					// is NOT a keep reason any more; those are exactly the far small props the presets are for.)
					else if ( mc->m_destructionRegions != 0 ) cls = CpCull::CL_AuthoredFar;
					else if ( cs.active && m_mesh != nullptr )
					{
						const Float orig = m_autoHideDistance;
						Float limit = orig;
						CpCull::Class why = CpCull::CL_Unchanged;
						// clustered proxies' boxes cover the whole cluster: only the foliage cap applies to them
						if ( cs.sizeK > 0.0f && !mc->m_isClusteredProxy )
						{
							const Float sizeLimit = cs.sizeK * diag;
							if ( sizeLimit < limit ) { limit = sizeLimit; why = CpCull::CL_ClampSize; }
						}
						if ( isFoliage && cs.folCap > 0.0f && cs.folCap < limit ) { limit = cs.folCap; why = CpCull::CL_ClampFoliage; }
						else if ( !isFoliage && diag < cs.propDiag && cs.propCap > 0.0f && cs.propCap < limit ) { limit = cs.propCap; why = CpCull::CL_ClampDist; }
						limit = Max( limit, 20.0f );
						if ( limit < orig * 0.999f )
						{
							Bool emissive = false;
							if ( cs.keepEmissive && !m_materialLodGroups.Empty() && GetNumLods() > 0 )
							{
								RED_SCOPE_SHARED_LOCK( m_materialLodGroupsFlag );
								const CRenderMesh::MeshLodGroup& lod0 = GetLodGroups()[ 0 ];
								for ( Uint32 c = 0; c < lod0.GetChunkCount() && !emissive; ++c )
								{
									const RenderMaterialInfo& info = m_materialLodGroups.GetMaterialEntryForChunk( 0, c );
									if ( info.m_materialPtr )
									{
										emissive = CpCull::NameLooksEmissive( info.m_materialPtr->GetDisplayableName(), cs.keepEmissive );
									}
								}
							}
							if ( emissive )
							{
								cls = CpCull::CL_Emissive;
							}
							else
							{
								m_autoHideDistance = limit;
								SetMeshProxyFlag( ERenderMeshProxyFlags( CpCull::c_clampedFlag ) );
								cls = why;
							}
						}
						if ( cs.probe )
						{
							const unsigned n = CpCull::s_probeLogged.fetch_add( 1, std::memory_order_relaxed );
							if ( n < 96 )
							{
								red::StringView mat0;
								if ( !m_materialLodGroups.Empty() && GetNumLods() > 0 && GetLodGroups()[ 0 ].GetChunkCount() > 0 )
								{
									const RenderMaterialInfo& info = m_materialLodGroups.GetMaterialEntryForChunk( 0, 0 );
									if ( info.m_materialPtr ) mat0 = info.m_materialPtr->GetDisplayableName();
								}
								std::fprintf( stderr, "CP2077 cull classify: class=%s objType=%u diag=%.2f engineFar=%.1f newFar=%.1f instances=%u clustered=%d material0=%.*s\n",
									CpCull::c_classNames[ cls ], unsigned( ot ), diag, orig, m_autoHideDistance, unsigned( mc->m_numVirtualInstances ),
									int( mc->m_isClusteredProxy ), int( mat0.Length() ), mat0.Length() ? mat0.Data() : "" );
							}
						}
					}
					if ( cs.counting )
					{
						CpCull::s_classCounts[ cls ].fetch_add( 1, std::memory_order_relaxed );
					}
				}
			}
#endif
		}

		if( mc->m_isCloth )
		{
			auto clothMesh = red::StaticCast<RenderMeshCloth>(mc->m_renderMesh);
			clothMesh->InitializePhysicalLODs(mc->m_numClothLODs);
			m_drawableFlags |= RPF_Dynamic;
		}

		if ( mc->m_skinningData )
		{
			RenderProxyCustomData_Skinning* skinningCustomData = AcquireCustomData<RenderProxyCustomData_Skinning>();
			RED_FATAL_ASSERT( skinningCustomData );
			skinningCustomData->m_skinningData = red::StaticCast<CRenderSkinningData>( mc->m_skinningData );
		}

		// Secondary visibility reference point
		if ( mc->m_useSecondaryReferencePoint )
		{
			SetSecondaryReferencePoint( mc->m_secondaryReferencePoint );
		}

		if ( mc->m_initAsHidden )
		{
			// hidden at the beginning
			// e.g. we don't want a mesh to pop up even though it shouldn't be visible due to render distance
			SetMeshProxyFlag( RMPF_InitAsHiden, true );
		}

		m_meshChunkMask = mc->m_chunkMask;
		m_timeOfDayVisibilityMask = mc->m_todVisMask;
		m_objectTypeID = mc->m_objectTypeID;
		m_order = ExtractMeshOrder( *mc );

		if (mc->m_destructionRegions > 0)
		{
			RenderProxyCustomData_MiscMeshParams* miscMeshParams = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
			RED_FATAL_ASSERT( miscMeshParams );

			GpuApi::BufferDesc desc;
			desc.size = mc->m_destructionRegions * sizeof(Float);
			desc.structureStride = sizeof(Float);
			desc.category = GpuWrapApi::BCC_Structured;
			desc.usage = GpuApi::BUT_Dynamic_Legacy;
			desc.group = GpuWrapApi::BufferGroup::MeshCustom;
			miscMeshParams->m_destructionRegions = GpuApi::CreateBuffer( desc );

#ifdef USE_RAY_TRACING
			// #TODO: Add support for baked destructions
			// https://jira.cdprojektred.com/browse/CYB-632744
			// Baked destructions do not use RPF_Dynamic flag
			// SetMeshProxyFlag( RMPF_ForcedStatic );
#endif // USE_RAY_TRACING
		}

		if ( !mc->m_mirrorCullingBox.IsEmpty() )
		{
			RenderProxyCustomData_Mirror* mirrorParams = AcquireCustomData<RenderProxyCustomData_Mirror>();
			RED_FATAL_ASSERT(mirrorParams);

			mirrorParams->m_mirrorCullingBox = mc->m_mirrorCullingBox;
		}

		if ( mc->m_hasMeshResource )
		{
			if ( !mc->m_removeFromRainMap )
			{
				SET_FLAG( m_renderMask, MCF_RenderInRainMap );
			}
			else
			{
				CLEAR_FLAG( m_renderMask, MCF_RenderInRainMap );
			}
		}

		if ( mc->m_forceStatic )
		{
			CLEAR_FLAG( m_drawableFlags, RPF_Dynamic );
		}

		// by default, dynamic object do not cast distant shadows
		if ( mc->m_isCastingGlobalShadows && ( !IsDynamic() || mc->m_isCastingDynamicDistantShadow ) )
		{
			SET_FLAG( m_drawableFlags, RPF_CastingDistantShadows );
		}
	}

#ifndef RED_CONFIGURATION_FINAL
	if ( m_renderMask == 0 && initInfo.m_componentData && initInfo.m_componentData->IsMeshTypeComponentInitData() )
	{
		const RenderProxyMeshComponentInitData *mc = static_cast<const RenderProxyMeshComponentInitData *>( initInfo.m_componentData );
		RED_LOG_WARNING( "Renderer: Render mesh proxy for '%hs' is not going to be rendered (no render mask)", mc->m_componentFriendlyName );
	}
#endif

	//
	// We set both flags according to single bool as the distinction between global and local shadows
	// is introduced only for purposes on distance based heuristics for local shadows and any change to
	// RMPF_CastingLocalShadowsEvenIfNotVisible is done via controlled path (not general initialization).
	// Please refer to CRenderCommandHandler::UpdateLocalShadowsInfo.
	//
	{
		const Bool castingShadowsWhenInvisibleRequested = ExtractCastShadowsEvenIfNotVisible( initInfo );
		SetMeshProxyFlag( RMPF_CastingGlobalShadowsEvenIfNotVisible, castingShadowsWhenInvisibleRequested );
		SetMeshProxyFlag( RMPF_CastingLocalShadowsEvenIfNotVisible, castingShadowsWhenInvisibleRequested );
	}

	CreateProxyBatchData();

#ifdef PRECACHE_PSO
	PrecachePSOs();
#endif // PRECACHE_PSO

#ifdef USE_RAY_TRACING
	RayTracingInit();
#endif
}

void CRenderProxy_Mesh::CreateLodGroups( Uint32 lodLevelScales )
{
	RED_FATAL_ASSERT( m_mesh != nullptr, "Caller should not call this is there's no render mesh!" );

	const Uint32 numLODs = m_mesh->GetNumLODs();
	m_lodSetup.SetupFromMesh( m_mesh.Get(), lodLevelScales, GetInstanceAproxScaling() );
}

void CRenderProxy_Mesh::SetupRenderMask( const RenderProxyMeshComponentInitData& mcInitData )
{
	CLEAR_MASK( m_renderMask );
	Bool hasSkinnedChunks = false;

	if ( !mcInitData.m_removeFromRainMap )
	{
		SET_FLAG( m_renderMask, MCF_RenderInRainMap );
	}
	else
	{
		CLEAR_FLAG( m_renderMask, MCF_RenderInRainMap );
	}

	// Aggregate render masks from all chunks. Create a superset of render flags for easy early out when collecting stuff.
	for ( const auto& lod : GetLodGroups() )
	{
		for ( Uint32 i=0, end = lod.GetChunkCount(); i<end; ++i )
		{
			const auto* chunk = lod.GetChunk( i );
			SET_FLAG( m_renderMask, chunk->m_renderMask );

			if ( DoesMaterialVertexFactorySupportSkinning( chunk->m_vertexFactory ) || chunk->m_elementFlags & RMCF_UsesSkinning )
			{
				hasSkinnedChunks = true;
			}
		}
	}

	SET_FLAG( m_renderMask, mcInitData.m_additionalMeshChunkRenderMask );
#ifndef RED_CONFIGURATION_FINAL
	if ( mcInitData.m_isGIVisible )
	{
		SET_FLAG( m_renderMask, MCF_IsGIVisible );
	}
	if ( mcInitData.m_isGITransparent )
	{
		SET_FLAG( m_renderMask, MCF_IsGITransparent );
	}
#endif //  RED_CONFIGURATION_FINAL

	if ( hasSkinnedChunks )
	{
		m_drawableFlags |= RPF_Dynamic;
		SetMeshProxyFlag( RMPF_HasAnySkinnedChunk );
	}

#ifdef USE_RAY_TRACING
	// Make sure this code executes after m_customCameraTarget is set!
	if ( CHECK_FLAG( m_renderMask, MCF_RenderInScene ) || ( CHECK_FLAG( m_renderMask, MCF_RenderInShadows ) && m_customCameraTarget == ECCTV_OnlyOnscreen ) ) // Detect player shadow meshes
	{
		SET_FLAG( m_renderMask, MCF_CollectInRayTraceBVH );
	}
#endif
}

const ShadowFlags CRenderProxy_Mesh::SetupShadowFlags( Float meshSize, Float autoHideDistance ) const
{
	ShadowFlags newFlags;

	if ( CHECK_FLAG( m_renderMask, MCF_RenderInShadows ) && std::isfinite( meshSize ) )
	{
		// The render mask says we can render to shadows. Good, we're in business.
		// If node/component says we can render to *local* shadows specifically, we can mark localShadows right away.
		if ( CHECK_FLAG( m_drawableFlags, RPF_CastingLocalShadows ) ) newFlags.m_localShadows = true;

		// Now for the global shadows. We have two flavours - up to four regular cascades and some amount of distant shadow "cascades".
		// Apply heuristic for all of these.
		// It MAY be worth considering adding more fine-grained heuristic for distants, BUT, at large ranges only large objects are supposed to be streamied in anyway. 
		// Just something to think about.
		if ( CHECK_FLAG( m_drawableFlags, RPF_CastingGlobalShadows ) )
		{
			if ( meshSize >= Config::cvMinMeshSizeInCascade0.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInCascade0.Get() ) newFlags.m_cascade0 = true;
			if ( meshSize >= Config::cvMinMeshSizeInCascade1.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInCascade1.Get() ) newFlags.m_cascade1 = true;
			if ( meshSize >= Config::cvMinMeshSizeInCascade2.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInCascade2.Get() ) newFlags.m_cascade2 = true;
			if ( meshSize >= Config::cvMinMeshSizeInCascade3.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInCascade3.Get() ) newFlags.m_cascade3 = true;
			if ( meshSize >= Config::cvMinMeshSizeInDistantShadowsCascade0.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInDistantShadowsCascade0.Get() ) newFlags.m_distantCascade0 = true;
			if ( meshSize >= Config::cvMinMeshSizeInDistantShadowsCascade1.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInDistantShadowsCascade1.Get() ) newFlags.m_distantCascade1 = true;
			if ( meshSize >= Config::cvMinMeshSizeInDistantShadowsCascade2.Get() && autoHideDistance >= Config::cvMinMeshAutohideDistanceInDistantShadowsCascade2.Get() ) newFlags.m_distantCascade2 = true;
		}
	}

	return newFlags;
}

Bool CRenderProxy_Mesh::ShouldUseGeometryPerChunk() const
{
	// if we have garment params, we should use per chunk geometry
	return HasCustomData< RenderProxyCustomData_Garment >();
}

GpuApi::TextureRef CRenderProxy_Mesh::GetPerChunkExtraData( Uint16 chunkIndex ) const
{
	if ( const RenderProxyCustomData_Garment* garmentParams = GetCustomData< RenderProxyCustomData_Garment >() )
	{
		return chunkIndex < garmentParams->m_morphOffsetScalesTextures.Size() ? garmentParams->m_morphOffsetScalesTextures[ chunkIndex ] : GpuApi::TextureRef::Null();
	}
	return GpuApi::TextureRef::Null();
}

const red::DynArray< GpuApi::TextureRef >& CRenderProxy_Mesh::GetMorphOffsetScalesTextures() const
{
	const RenderProxyCustomData_Garment* garmentParams = GetCustomData< RenderProxyCustomData_Garment >();
	RED_FATAL_ASSERT( garmentParams );
	return garmentParams->m_morphOffsetScalesTextures;
}

CRenderProxy_Mesh::~CRenderProxy_Mesh()
{
	ReleaseProxyBatchData();

#ifdef USE_RAY_TRACING
	RayTracingRelease();

	if ( m_instancesDataCopy )
	{
		RED_DELETE_ARRAY( m_instancesDataCopy, m_numInstances, rend::PoolRenderProxy_InstanceData );
		m_instancesDataCopy = nullptr;
	}
#endif

	if ( m_instancesData )
	{
		RED_DELETE_ARRAY( m_instancesData, m_numInstances, rend::PoolRenderProxy_InstanceData );
		m_instancesData = nullptr;
	}

	if ( m_offsetInStaticInstanceBuffer != UINT32_MAX )
	{
		GetRenderer()->GetGeometryBatcher()->FreeStaticInstance( m_offsetInStaticInstanceBuffer );
	}
}

Bool CRenderProxy_Mesh::CheckRenderFilters( const rend::DebugFilterMask& filterMask ) const
{
#ifndef RED_CONFIGURATION_FINAL
	if ( CheckMeshProxyFlag( RMPF_Terrain ) )
	{
		return filterMask.Test( rend::DF_RenderMask_Terrain );
	}
    if( CheckMeshProxyFlag( RMPF_GeneratedByVegetationSystem ) )
    {
        return filterMask.Test( rend::DF_RenderMask_Foliage );
    }
	if ( CheckMeshProxyFlag( RMPF_IsProxyMesh ) && !filterMask.Test( rend::DF_RenderMask_GeometryProxies ) )
	{
		return false;
	}

	Bool hasSkinningData = false;
	if ( RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
	{
		hasSkinningData = skinningCustomData->m_skinningData != nullptr;
	}
	if ( !hasSkinningData && !filterMask.Test( rend::DF_RenderMask_GeometryStatic ) )
	{
		return false;
	}
	if ( hasSkinningData && !filterMask.Test( rend::DF_RenderMask_GeometrySkinned ) )
	{
		return false;
	}
#endif
	return true;
}

Bool CRenderProxy_Mesh::CollectSingleLodModeInstances( SUpdateContext& ctx, CollectContext& collectCtx, FeatureFlagsMask featureFlags, Int8 lods[ 2 ], EDissolveMode modes[ 2 ], const InstancesCollectParams& instancesParams )
{
	Bool isThisLodModeVisible = false;

	if ( !DissolveHelpers::IsLodVisible( lods[ 0 ], lods[ 1 ] ) )
	{
#ifdef __EMSCRIPTEN__
		if ( collectCtx.renderMask == MCF_RenderInScene )
			CpMeshCollectProbe( CPMC_LodInvisible, 2u, GetSkinningData() ? true : false, unsigned( Int32( lods[ 0 ] ) + 128 ), unsigned( modes[ 0 ] ) );
#endif
		return false;
	}

	if ( m_proxyBatchDataId == UINT32_MAX )
	{
#ifdef __EMSCRIPTEN__
		if ( collectCtx.renderMask == MCF_RenderInScene )
			CpMeshCollectProbe( CPMC_NoBatchData, 2u, GetSkinningData() ? true : false );
#endif
		return false;
	}

	RenderBatchData commonBatchData;
	commonBatchData.Clear();

	if ( !HasMultipleInstances() )
	{
#ifndef RED_CONFIGURATION_FINAL
		if ( CheckMeshProxyFlag( RMPF_IsClusteredProxy ) )
		{
			ctx.collector.m_renderingStats.m_visibleClusteredInstances.Increment();
		}
		if ( CheckMeshProxyFlag( RMPF_GeneratedByVegetationSystem ) )
		{
			ctx.collector.m_renderingStats.m_visibleFoliageInstances.Increment();
		}
#endif // RED_CONFIGURATION_FINAL

		CBatchDataAllocator &batchDataAllocator = collectCtx.batchDataAllocator;
		Uint32 instanceOffset = batchDataAllocator.GetInstanceData().Allocate( m_instanceMatrix );
		if ( batchDataAllocator.GetInstanceData().CheckOverflowValue( instanceOffset ) )
		{
			return isThisLodModeVisible;
		}

		commonBatchData.SetInstanceCount( 1 );
		commonBatchData.SetProxy( m_proxyBatchDataId );
		commonBatchData.SetInstanceOffset( instanceOffset );
	}

	const Bool isShadow = collectCtx.collectFlags & COLLECT_IsShadow;

	if ( !isShadow )
	{
		FeatureFlagsMask dynDecalsFeatureFlags;
		if ( collectCtx.renderMask == MCF_RenderInScene )
		{
			dynDecalsFeatureFlags = FeatureFlagsMask().Set( FEATFLAG_DynamicDecals );
		}
		else
		{
			dynDecalsFeatureFlags = featureFlags;
		}

		CollectDynamicDecals( ctx.collector, collectCtx, dynDecalsFeatureFlags, modes[ 0 ], commonBatchData );
	}

	const RenderProxyCustomData_MiscMeshParams* miscMeshParams = collectCtx.miscMeshParams;
	const Bool haveMaterialOverlay = miscMeshParams && miscMeshParams->m_materialOverlay.IsValid();
	const Bool haveMaterialReplacement = haveMaterialOverlay && miscMeshParams->m_materialOverlayIsOverride;

	const RenderProxyCustomData_Garment* garmentParams = GetCustomData<RenderProxyCustomData_Garment>();

	const Uint32 totalNumLods = GetMaterialLodGroups().GetNumLods();
	const Bool isSpeedtree = IsSpeedTree();
#ifdef __EMSCRIPTEN__
	// [cull] ?cullShadowLod=<n> (presets cull=2/3 = 1, default 0): extra LOD bias for static meshes in shadow collects
	// (coarser LODs = fewer chunks = fewer shadow draws).
	const Uint32 cpEngineShadowBias = Config::GGlobalRenderingSettings.ShadowLODBias;
	const Uint32 cpExtraShadowBias = ( isShadow && !isSpeedtree && cpEngineShadowBias != UINT32_MAX && !IsDynamic() && !GetSkinningData() ) ? Uint32( CpCull::Get().shadowLodBias ) : 0u;
	const Uint32 lodBias = ( isShadow && !isSpeedtree ) ? cpEngineShadowBias + cpExtraShadowBias : 0;
#else
	const Uint32 lodBias = ( isShadow && !isSpeedtree ) ? Config::GGlobalRenderingSettings.ShadowLODBias : 0;
#endif

	//Based on the "m_isMasked" property in material and the bool below we decide not to render specific chunk into distant shadows. The only exception is foliage which gets rendered into distant shadows 
	//despite being alpha tested. Still we do never render foliage into rain map and we do not render into distant shadows when we're baking TodVis.
	const Bool shouldIgnoreAlphaTestedProxyInDistShadows = featureFlags.Get( FEATFLAG_DistantShadows ) && 
		( ctx.collector.m_info->GetRenderingMode() == ERenderingMode::RM_TodvisBake || ctx.isRainMap || !isSpeedtree );

	ChunkToCollect chunkToCollect;
	chunkToCollect.isGeometryPerChunk = ShouldUseGeometryPerChunk();

	// fill index buffer override, TODO: what if we have both garment and destruction or smth else in MiscMeshParams?
	if ( garmentParams )
	{
		chunkToCollect.ibOverride = garmentParams->m_indexBufferOverride.Get();
	}
	
	if ( !chunkToCollect.ibOverride && miscMeshParams )
	{
		chunkToCollect.ibOverride = miscMeshParams->m_indexBufferOverride.Get();
	}

	const Uint32 threadIndex = job::GetDispatcherThreadIndex();

	for ( Uint32 lod = 0; lod < 2; ++lod )
	{
		chunkToCollect.lodIndex = lods[ lod ];
		if ( !DissolveHelpers::IsLodVisible( chunkToCollect.lodIndex ) )
		{
			continue;
		}

		// Apply LOD bias
		if( lodBias == UINT32_MAX )
		{
			chunkToCollect.lodIndex = totalNumLods - 1;
		}
		else
		{
			chunkToCollect.lodIndex = chunkToCollect.lodIndex + lodBias;
			if ( featureFlags.Get( FEATFLAG_DistantShadows ) && isSpeedtree )
			{
				// cvForceGeometryInsteadOfBillboardsForFoliageInDistantShadows is by default disabled because it extends time of distant shadow cascade generation. Doesn't seem to increase perf cost a lot but 
				// the cascades generation is just split across much more frames... Needs to be tested more carefully. Maybe worth to add just on Ultra PC? It makes billboard shadow look good in all possible sun locations.
				if ( !Config::cvForceGeometryInsteadOfBillboardsForFoliageInDistantShadows.Get() )
				{
					// For foliage in distant shadows we want to render only last 2 LODs (geometry and billboard LOD) for performance reasons and because last geometry LOD gives visually similar results like the previous LODs.
					chunkToCollect.lodIndex = Max( (Int8)( CRenderLODSetup::MAX_LODS - 2 ), chunkToCollect.lodIndex );
				}
				else
				{
					//never render billboard to distant shadows - just least detailed geometry LOD
					chunkToCollect.lodIndex = (Int8)( CRenderLODSetup::MAX_LODS - 2 );
				}
			}
			chunkToCollect.lodIndex = Min( chunkToCollect.lodIndex, (Int8)( totalNumLods - 1u ) );
		}

		RED_FATAL_ASSERT( static_cast<Uint32>( chunkToCollect.lodIndex ) < totalNumLods, "Invalid LOD index: %u, has only %u LODs", chunkToCollect.lodIndex, totalNumLods );

		isThisLodModeVisible = true;

		const EDissolveMode dissolveMode = modes[ lod ];
		const Uint32 numChunksInLod = GetMaterialLodGroups().GetNumChunksInLOD( chunkToCollect.lodIndex );

		if ( !haveMaterialReplacement )
		{
			for ( Uint32 chunkIndexInLOD = 0; chunkIndexInLOD < numChunksInLod; ++chunkIndexInLOD )
			{
				const MaterialLodGroups::Chunk& chunk = GetMaterialLodGroups().GetChunkInfo( chunkToCollect.lodIndex, chunkIndexInLOD );

				// Check per-chunk render mask against proxy render mask.
				if ( !CheckRenderMask( chunk.renderMask, collectCtx.renderMask ) )
				{
					continue;
				}

				if ( ( m_meshChunkMask & ( 1ull << (Uint64)chunk.chunkIndex ) ) == 0 )
				{
					continue;
				}

				const RenderMaterialInfo& materialInfo = chunk.materialInfo;
				materialInfo.GetMaterialAndParams( chunkToCollect.material, chunkToCollect.materialParameters );

				// For now (E3) we are skipping alpha tested meshes in distant shadows. This is due to leaks that happen 
				// on treating distant shadows buffer as an occluder and receiving false positives resulting in not rendering meshes
				// that should provide an occlusion themselves for todvis.
				if ( shouldIgnoreAlphaTestedProxyInDistShadows && chunkToCollect.materialParameters && chunkToCollect.materialParameters->IsMasked() )
				{
					continue;
				}

				chunkToCollect.chunk = &chunk;
				chunkToCollect.chunkIndex = chunk.chunkIndex;
				chunkToCollect.chunkIndexInLOD = chunkIndexInLOD;

				collectCtx.numTriangles += chunk.numTriangles;

#ifndef RED_CONFIGURATION_FINAL
				Bool force = false;
				chunkToCollect.fallbackMaterial = nullptr;
				chunkToCollect.fallbackMaterialParameters = nullptr;
				const EEnvManagerModifier envMod = GetDebugFallbackMaterial( ctx.collector, &materialInfo, chunkToCollect.fallbackMaterial, chunkToCollect.fallbackMaterialParameters, force );

				if ( force )
				{
					if ( envMod == EMM_LODColoring )
					{
						Vector4 color = Vector4( 0.0f, 0.0f, 0.0f, 1.0f );
						SMaterialParameterEffectContextUpdate updateInfo = {};

						if ( CheckMeshProxyFlag( RMPF_IsProxyMesh ) )
						{
							color = Vector4( 1.0f, 0.0f, 0.0f, 1.0f );
						}
						else
						{
							switch ( chunkToCollect.lodIndex )
							{
							case 0: color = Vector4( 0.0f, 0.0f, 1.0f, 1.0f );	break;
							case 1: color = Vector4( 0.0f, 1.0f, 0.0f, 1.0f );	break;
							case 2: color = Vector4( 1.0f, 1.0f, 0.0f, 1.0f );	break;
							case 3: color = Vector4( 0.5f, 0.0f, 0.5f, 1.0f );	break;
							default:
								color = Vector4( 0.0f, 0.0f, 0.0f, 1.0f );
							}
						}

						updateInfo.m_paramMask |= ECMP_CustomParam0;
						updateInfo.m_parameters[ 0 ] = color;

						UpdateMaterialParameterEffectContext( updateInfo );
					}

					chunkToCollect.material = chunkToCollect.fallbackMaterial;
					chunkToCollect.materialParameters = chunkToCollect.fallbackMaterialParameters;
					chunkToCollect.fallbackMaterial = nullptr;
					chunkToCollect.fallbackMaterialParameters = nullptr;
				}
#endif // RED_CONFIGURATION_FINAL

				const Uint32 numInstancesCollected =
					CollectChunkWithMaterial( threadIndex, chunkToCollect, collectCtx, dissolveMode, featureFlags, commonBatchData, instancesParams );

#ifndef RED_CONFIGURATION_FINAL
				if ( ctx.outVisibilityStats && numInstancesCollected > 0 )
				{
                    ctx.outVisibilityStats->CollectUsedResource( m_mesh->GetMeshResourcePath(), chunk.chunkIndex, numInstancesCollected );
				}

				if ( ctx.collector.GetRenderFrameInfo().m_enableDebugMeshColoring )
				{
					chunkToCollect.material = static_cast<CRenderMaterial*>( GetRenderer()->GetEngineHandler()->GetDebugColoringMaterial().Get() );
					chunkToCollect.materialParameters = chunkToCollect.material;
					CollectChunkWithMaterial( threadIndex, chunkToCollect, collectCtx, dissolveMode, featureFlags, commonBatchData, instancesParams );
				}
#endif // RED_CONFIGURATION_FINAL
			}
		}

		// collect chunks with material overlay
		if ( !isShadow && haveMaterialOverlay )
		{
			for ( Uint32 chunkIndexInLOD = 0; chunkIndexInLOD < numChunksInLod; ++chunkIndexInLOD )
			{
				const MaterialLodGroups::Chunk& chunk = GetMaterialLodGroups().GetChunkInfo( chunkToCollect.lodIndex, chunkIndexInLOD );

				if ( !CheckRenderMask( chunk.renderMask, collectCtx.renderMask ) )
				{
					continue;
				}

				if ( ( m_meshChunkMask & ( 1ull << (Uint64)chunk.chunkIndex ) ) == 0 )
				{
					continue;
				}

				collectCtx.numTriangles += chunk.numTriangles;

				miscMeshParams->m_materialOverlay.GetMaterialAndParams( chunkToCollect.material, chunkToCollect.materialParameters );

				chunkToCollect.chunk = &chunk;
				chunkToCollect.chunkIndex = chunk.chunkIndex;
				chunkToCollect.chunkIndexInLOD = chunkIndexInLOD;

				const Uint32 numInstancesCollected =
					CollectChunkWithMaterial( threadIndex, chunkToCollect, collectCtx, dissolveMode, featureFlags, commonBatchData, instancesParams );

#ifndef RED_CONFIGURATION_FINAL
				if ( ctx.outVisibilityStats && numInstancesCollected > 0 )
				{
                    ctx.outVisibilityStats->CollectUsedResource( m_mesh->GetMeshResourcePath(), chunk.chunkIndex, numInstancesCollected );
				}
#endif // RED_CONFIGURATION_FINAL
			}
		}
	}

	return isThisLodModeVisible;
}

Uint32 CRenderProxy_Mesh::CollectChunkWithMaterial(
	const Uint32 threadIndex,
	const ChunkToCollect& chunkToCollect,
	const CollectContext& collectCtx,
	const EDissolveMode dissolveMode,
	const FeatureFlagsMask featureFlags,
	const RenderBatchData commonBatchData,
	const InstancesCollectParams& instancesParams )
{
	CBatchDataAllocator& batchDataAllocator = collectCtx.batchDataAllocator;

	const RenderProxyCustomData_Skinning* skinningCustomData = collectCtx.skinningCustomData;

	const Bool hasComputeSkinningBuffers = skinningCustomData && skinningCustomData->m_gpuComputeSkinningBuffers;
	const Uint32 chunkIndex = chunkToCollect.chunkIndex;
	const MaterialLodGroups::Chunk* chunk = chunkToCollect.chunk;

	const SRenderStageContext stageContext = GetMeshChunkRenderStageContext(
		chunk->elementFlags, chunk->vertexFactory, *chunkToCollect.material, dissolveMode, hasComputeSkinningBuffers,
		collectCtx.useTangentUpdate && (chunkToCollect.lodIndex == 0), collectCtx.hasDismemberment );

	TRenderStagePasses passes;
	chunkToCollect.material->GetRenderPassesForFeatureFlags( batchDataAllocator, featureFlags, stageContext, chunkToCollect.materialParameters, passes );

#ifndef RED_CONFIGURATION_FINAL
	if ( chunkToCollect.fallbackMaterial && passes.Empty() )
	{
		chunkToCollect.fallbackMaterial->GetRenderPassesForFeatureFlags( batchDataAllocator, featureFlags, stageContext, chunkToCollect.fallbackMaterialParameters, passes );
	}
#endif // RED_CONFIGURATION_FINAL

#ifdef __EMSCRIPTEN__
	// a = render passes produced by the material (0 = material/shader not ready -> chunk silently not drawn),
	// b = vertex factory | computeSkinning<<8 | lod<<12
	if ( collectCtx.renderMask == MCF_RenderInScene )
		CpMeshCollectProbe( passes.Empty() ? CPMC_ChunkNoPasses : CPMC_ChunkPasses, 2u, skinningCustomData != nullptr,
			unsigned( passes.Size() ), unsigned( chunk->vertexFactory ) | ( unsigned( hasComputeSkinningBuffers ) << 8 ) | ( ( unsigned( chunkToCollect.lodIndex ) & 0xF ) << 12 ) );
	if ( !passes.Empty() && CpCull::Get().counting ) // [cull] chunk passes ~= draws, main view vs shadows
	{
		( ( collectCtx.collectFlags & COLLECT_IsShadow ) ? CpCull::s_shadowPasses : CpCull::s_mainPasses ).fetch_add( passes.Size(), std::memory_order_relaxed );
	}
#endif
	Uint32 numInstancesCollected = 0;

	const Bool hasTangentUpdate = (collectCtx.useTangentUpdate && hasComputeSkinningBuffers && skinningCustomData->m_skinningData && m_mesh->HasTopologyData( chunkIndex ));
	const Bool hasMultipleInstances = HasMultipleInstances();
	const Uint32 overrideInstanceCount = (hasMultipleInstances || commonBatchData.IsInstanced()) ? UINT32_MAX : m_numInstances;

	const TBatchDataId geometryBatchDataId = (!hasMultipleInstances && chunkToCollect.ibOverride) ?
		chunkToCollect.ibOverride->GetBindBatchDataIndex( m_mesh.Get(), batchDataAllocator.GetGeometryData(), chunkIndex, chunkToCollect.lodIndex, overrideInstanceCount ) :
		m_mesh->GetBindBatchDataIndex( batchDataAllocator.GetGeometryData(), chunkIndex, chunkToCollect.lodIndex, overrideInstanceCount );
	const Uint32 geoBatchDataIndex = geometryBatchDataId.GetValue();

	RenderBatchKey commonKey, commonKeyDiscarded;
	{
		commonKey.Clear();
		commonKey.SetGeometry( geoBatchDataIndex );
		if ( chunkToCollect.isGeometryPerChunk )
		{
			commonKey.isGeometryPerChunk = 1;
		}
		commonKey.SetDistance( collectCtx.distanceToCamera );
		commonKey.SetOrder( m_order );

		commonKeyDiscarded = commonKey;
		commonKeyDiscarded.SetDissolveMode( dissolveMode );
	}

	RenderStageCollector& stageCollector = collectCtx.stageCollector;
	SRenderStageCluster stageCluster = collectCtx.stageCluster;

	if ( !hasMultipleInstances )
	{
		for ( const auto &pass : passes )
		{
			SRenderChunk& newChunk = stageCollector.AddChunk( threadIndex, pass.m_stageId, stageCluster );

			newChunk.m_key = pass.m_renderChunkKey | (pass.m_isDiscarded ? commonKeyDiscarded : commonKey);
			newChunk.m_data = pass.m_renderChunkData | commonBatchData;

			if ( hasTangentUpdate )
			{
				stageCollector.PushTangentUpdateChunk( newChunk, chunkIndex );
			}
		}

#ifndef RED_CONFIGURATION_FINAL
		numInstancesCollected++; // only for stats
#endif // RED_CONFIGURATION_FINAL
	}
	else
	{
		RED_FATAL_ASSERT( m_numInstances > 0 );

		const Bool usesStaticInstanceBuffer = instancesParams.offsetInStaticInstanceBuffer != UINT32_MAX;

		if ( usesStaticInstanceBuffer )
		{
			commonKey.usesStaticInstanceBuffer = 1;
			commonKeyDiscarded.usesStaticInstanceBuffer = 1;
		}

		constexpr Uint32 maxInstancesPerBatch = (1 << BATCHMASK_DATA_INSTANCE_NUM_NUMBITS) - 1;
		const Uint32 numInstances = instancesParams.numInstancesInStaticInstanceBuffer;
		const Uint32 numBatches = red::DivideRoundingUp( numInstances, maxInstancesPerBatch );

#ifndef RED_CONFIGURATION_FINAL
		for ( const auto &pass : passes )
		{
			RED_FATAL_ASSERT( pass.m_renderChunkKey.GetDissolveMode() == DISSOLVEMODE_Opaque, "Key expected to be by default setup to opaque dissolve, and able to bitwiseOr any other dissolve mode" );

			if ( CheckMeshProxyFlag( RMPF_IsClusteredProxy ) )
			{
				collectCtx.collector.m_renderingStats.m_visibleClusteredInstances.ExchangeAdd( numInstances );
			}
			if ( CheckMeshProxyFlag( RMPF_GeneratedByVegetationSystem ) )
			{
				collectCtx.collector.m_renderingStats.m_visibleFoliageInstances.ExchangeAdd( numInstances );
			}
		}
#endif // RED_CONFIGURATION_FINAL

		Uint32 instancesBaseOffset = instancesParams.offsetInStaticInstanceBuffer;
		if ( !usesStaticInstanceBuffer )
		{
			instancesBaseOffset = batchDataAllocator.GetInstanceData().Allocate( numInstances );
			if ( batchDataAllocator.GetInstanceData().CheckOverflowValue( instancesBaseOffset ) )
			{
				return 0;
			}

			Float* ptr = batchDataAllocator.GetInstanceData().GetPtr( instancesBaseOffset );
			const IRenderProxyInstance* instanceTransforms;
			if ( IsClusteredProxyWithMultipleInstances() )
			{
				instanceTransforms = &instancesParams.clusteredInstancesReorderedTransforms[instancesParams.clusteredInstancesReorderedTransformsOffset];
			}
			else
			{
				instanceTransforms = m_instancesData;
			}

			static_assert(sizeof( IRenderProxyInstance ) == sizeof( Float )*CInstanceDataAllocator::MatrixNumElements, "IRenderProxyInstance has unexpected size");
			red::Memcpy( ptr, instanceTransforms, numInstances * CInstanceDataAllocator::MatrixNumElements * sizeof( Float ) );
		}

		for ( const auto &pass : passes )
		{
			RenderBatchKey passKey = pass.m_renderChunkKey | (pass.m_isDiscarded ? commonKeyDiscarded : commonKey);
			RenderBatchData passData = pass.m_renderChunkData;
			passData.SetProxy( m_proxyBatchDataId );
			passData.isInstanced = 1;
			if ( usesStaticInstanceBuffer )
			{
				passData.allowInstanceSplit = 0;
			}

			for ( Uint32 ibatch = 0; ibatch < numBatches; ++ibatch )
			{
				const Uint32 beginInstances = ibatch * maxInstancesPerBatch;
				const Uint32 endInstances = Min( beginInstances + maxInstancesPerBatch, numInstances );
				const Uint32 numInstancesInBatch = endInstances - beginInstances;
				const Uint32 instanceOffset = instancesBaseOffset + beginInstances;

				SRenderChunk& newChunk = stageCollector.AddChunk( threadIndex, pass.m_stageId, stageCluster );

				newChunk.m_key = passKey;
				newChunk.m_data = passData;
				newChunk.m_data.SetInstanceCount( numInstancesInBatch );
				newChunk.m_data.SetInstanceOffset( instanceOffset );

				if ( hasTangentUpdate )
				{
					stageCollector.PushTangentUpdateChunk( newChunk, chunkIndex );
				}
			}
		}

#ifndef RED_CONFIGURATION_FINAL
		numInstancesCollected += numInstances; // only for stats
#endif // RED_CONFIGURATION_FINAL
	}

	return numInstancesCollected;
}


Bool CameraTargetTypeMatchesMeshSettings( const CRenderFrameCameraStorage::SCameraData& camData, ECustomCameraTarget currentType )
{
	// Check if the camera for which the collection happens matches with mesh camera taraget setup.
	if ( ( currentType == ECCTV_OnlyOffscreen && camData.IsOnscreen() ) ||
		 ( currentType == ECCTV_OnlyOnscreen && camData.IsOffscreen() ) )
	{
		return false;
	}

	return true;
}

Uint32 CRenderProxy_Mesh::CollectDistantShadowElements( SUpdateContext& ctx, RenderStageCollector& stageCollector, FeatureFlagsMask featureFlags, const SRenderStageCluster& stageCluster )
{
	RED_FATAL_ASSERT( m_scene != nullptr, "Trying to collect proxy that is not in a scene - hacky hacky rendering" );
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

#ifndef RED_CONFIGURATION_FINAL
	// In non-final build, allow for refreshing heuristics at every collect. This way we can easily tweak the heuristic to maintain the best balance between quality and performance
	if ( Config::cvForceVolumeHeuristicRefresh.Get() )
	{
		const ShadowFlags tempShadowFlags = SetupShadowFlags( GetBoundingBox().CalcSize().Mag3(), GetAutoHideDistance() );
		if ( ( ctx.cascadeIndex == 0 && !tempShadowFlags.m_distantCascade0 ) ||
			 ( ctx.cascadeIndex == 1 && !tempShadowFlags.m_distantCascade1 ) ||
			 ( ctx.cascadeIndex >= 2 && !tempShadowFlags.m_distantCascade2 ) )
		{
			return 0;
		}
	}
#endif // RED_CONFIGURATION_FINAL

	if ( !IsVisible() )
	{
		return 0;
	}

	if ( ( m_shadowFlags.m_mask & ShadowFlags::DISTANT_SHADOWS_MASK() ) == 0 )
	{
		// Do not qualify for distant shadows
		return 0;
	}

	if ( !CameraTargetTypeMatchesMeshSettings( *ctx.cameraData, m_customCameraTarget ) )
	{
		// On-screen/Off-screen mismatch (eg. we are an off-screen only mesh, but current camera is of on-screen type)
		return 0;
	}

	auto& collector = ctx.collector;

	// Debug only - check if this mesh type is not filtered out for debug purposes
	if ( !CheckRenderFilters( collector.m_cameraInfo->GetRenderingMask() ) )
		return 0;

	// Update stuff that should be updated once per frame
	UpdateOncePerFrame( ctx );

	Uint32 collectFlags = COLLECT_None;
	if ( featureFlags.Get( FEATFLAG_Shadows ) ) //for distant shadows collecting
	{
		collectFlags |= COLLECT_IsShadow;
		collectFlags |= COLLECT_NoDissolve; // dissolve in distant shadows does not make sense
	}

	//TODO? Even in case of "Clustered Proxy" all of its instances will have the exact same distanceFromCamera saved into renderChunk.key
	const Float distanceToCamera = ( ctx.cameraPosition - GetTranslation() ).Mag3();

	if ( ctx.isRainMap && !CHECK_FLAG( m_renderMask, MCF_RenderInRainMap ) )
		return 0;

	CollectContext collectCtx =
	{
		/* .stageCollector	 = */ stageCollector,
		/* .collector		 = */ collector,
		/* .stageCluster	 = */ stageCluster,
		/* .renderMask		 = */ MCF_RenderInShadows,
		/* .collectFlags	 = */ collectFlags,
		/* .dissolveContext	 = */ RDC_ShadowMain,
		/* .distanceToCamera = */ distanceToCamera,
		/* .skinningCustomData=*/ GetCustomData<RenderProxyCustomData_Skinning>(),
		/* .miscMeshParams	 = */ GetCustomData<RenderProxyCustomData_MiscMeshParams>(),
		/* .useTangentUpdate = */ collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate ),
	};

	CollectAllLodModeInstances( ctx, collectCtx, featureFlags );

	return collectCtx.numTriangles;
}

Bool CRenderProxy_Mesh::CollectAllLodModeInstances( SUpdateContext& ctx, CollectContext& collectCtx, FeatureFlagsMask featureFlags )
{
	Int8 lods[2] = { -1, -1 };
	EDissolveMode modes[2] = { DISSOLVEMODE_Invisible, DISSOLVEMODE_Invisible };
	Bool isThisProxyVisible = false;
	const Bool isShadowCollecting = ( collectCtx.collectFlags & COLLECT_IsShadow );
	const Bool isDissolveCollecting = !( collectCtx.collectFlags & COLLECT_NoDissolve);
	Int8 mostDetailedLod = INT8_MAX;
#ifdef USE_RAY_TRACING
	Uint32 visibleBucketNum = 0;
#endif

#ifdef __EMSCRIPTEN__
	// [cull] shadow-pass screen-size cull (?cullShadowPx=<px>, presets cull=1/2/3 = 6/12/24 px, default off): small static
	// world meshes whose projected size from the camera (cached once-per-frame distance) is below the threshold are not
	// collected into shadow cascades / local shadows. They still draw in the main view. Entities, skinned, prefab
	// proxies, terrain, clustered/foliage and anything >= 30 m diagonal keep their shadows.
	if ( isShadowCollecting )
	{
		const CpCull::Settings& cs = CpCull::Get();
		if ( cs.counting ) CpCull::s_shadowCollects.fetch_add( 1, std::memory_order_relaxed );
		if ( cs.shadowK > 0.0f && !IsDynamic() && !GetSkinningData() && !CheckMeshProxyFlag( RMPF_IsProxyMesh ) && !CheckMeshProxyFlag( RMPF_Terrain )
			&& !CheckMeshProxyFlag( RMPF_IsClusteredProxy ) && m_objectTypeID <= ROT_STATIC_LAST && m_objectTypeID != ROT_Terrain && m_objectTypeID != ROT_Road )
		{
			const Float diag = GetBoundingBox().CalcSize().Mag3();
			const Float reach = cs.shadowK * diag;
			if ( diag < 30.0f && reach * reach < GetCachedDistanceSquared() )
			{
				if ( cs.counting ) CpCull::s_shadowSkipped.fetch_add( 1, std::memory_order_relaxed );
				return false;
			}
		}
	}
#endif

	if ( IsClusteredProxyWithMultipleInstances() )
	{
		CameraCustomData_RenderProxyData& rpData = ctx.cameraData->GetCustomData<CameraCustomData_RenderProxyData>();
		CameraCustomData_RenderProxyData::ClusteredProxyData* clusteredProxyData = rpData.GetClusteredProxyData( this->GetIdInScene() );
		if ( clusteredProxyData )
		{
			Uint32 clusteredInstancesReorderedTransformsOffset = 0;
			for ( const auto& bucket : clusteredProxyData->m_instancesBuckets )
			{
				TInstanceLodModeKey lodModeKey = bucket.Key();
				auto& dissolveStateInfo = const_cast<CameraCustomData_RenderProxyData::ClusteredProxyDissolveStateInfo&>(bucket.Value());

				if ( dissolveStateInfo.instancesIndices.Size() )
				{
					//we still need to Get desired LODs and Dissolves through GetLOD because synchronizer might not give us the same LODs / dissolves as in lodModeKey yet, could precache it during UpdateLodModesForClusteredProxyInstances
					dissolveStateInfo.dissolveState.GetLOD( GetDissolveSync(), collectCtx.dissolveContext, lods, modes );

#ifdef USE_RAY_TRACING
					if (ctx.isMainCameraView)
					{
						m_mainViewFrameIndex = GpuApi::FrameIndex();

						Bool isDissolving = modes[0] == DISSOLVEMODE_FadeIn || modes[0] == DISSOLVEMODE_FadeOut;
						dissolveStateInfo.mainViewLOD = lods[0];
						dissolveStateInfo.isDissolving = isDissolving;

						if (!visibleBucketNum)
						{
							m_mainViewLod = lods[0];
							m_dissolveState = isDissolving ? DissolveState::DISSOLVING : 0;
						}

						if (lods[0] >= 0)
						{
							m_dissolveState |= (0x1 << lods[0]);
							++visibleBucketNum;
						}
					}
#endif

					mostDetailedLod = lods[0] < mostDetailedLod ? lods[0] : mostDetailedLod;

					InstancesCollectParams instancesParams;
					instancesParams.offsetInStaticInstanceBuffer = dissolveStateInfo.staticInstanceBufferOffset;
					instancesParams.numInstancesInStaticInstanceBuffer = dissolveStateInfo.instancesIndices.Size();
					instancesParams.clusteredInstancesReorderedTransformsOffset = clusteredInstancesReorderedTransformsOffset;
					instancesParams.clusteredInstancesReorderedTransforms = clusteredProxyData->m_dynBuffReorderedTransforms;

					isThisProxyVisible |= CollectSingleLodModeInstances( ctx, collectCtx, featureFlags, lods, modes, instancesParams );
					clusteredInstancesReorderedTransformsOffset += dissolveStateInfo.instancesIndices.Size();
				}
			}
		}

#ifdef USE_RAY_TRACING
		if (visibleBucketNum > 1)
		{
			m_dissolveState |= DissolveState::MULTIPLE_BUCKETS;
		}
#endif
	}
	else
	{
		// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -

		// For distant and local shadows we skip dissolving as local shadows manager cache static objects in designated shadow map
		// and have no notion about objects "fading in/out". Since we have shadows fade in/out etc popping should be
		// minimized if not evicted at all.
		// Same rule applies to reflection probes. Regenerating a probe reuses mesh proxies already attached to the scene
		// and they may had "init as hidden" flag enabled. This will force the meshes to be visible.
		//
		// NOTE: this branching is not present in IsClusteredProxyWithMultipleInstances(). It might be error prone
		//       but afaik only foliage goes that path and it is collected for local shadows as dynamics only.
		{
			
			if( !isDissolveCollecting )
			{
				Bool getLods	= true;

				if( ctx.allowMeshDissolve )
				{
					const Bool isVisibleBecauseOfShadow = isShadowCollecting && (CheckMeshProxyFlag( RMPF_CastingLocalShadowsEvenIfNotVisible ) || CheckMeshProxyFlag( RMPF_CastingGlobalShadowsEvenIfNotVisible ));
					getLods		= IsVisibleInCurrentFrame() && (m_isDissolveVisible || isVisibleBecauseOfShadow);
				}

				if( getLods )
				{
					Bool immediate	= false;
					ComputeLOD( ctx, lods[0], immediate );
					modes[0] = DISSOLVEMODE_Opaque;
				}
			}
			else
			{
				CameraCustomData_RenderProxyData& rpData = ctx.cameraData->GetCustomData<CameraCustomData_RenderProxyData>();
				const CRenderDissolveState& dissolveState = rpData.GetDissolveState( this );
				dissolveState.GetLOD( GetDissolveSync(), collectCtx.dissolveContext, lods, modes );

#ifndef RED_CONFIGURATION_FINAL
				if( !ctx.allowMeshDissolve )
				{
					modes[0] = DISSOLVEMODE_Opaque;
					modes[1] = DISSOLVEMODE_Invisible;
				}
#endif // RED_CONFIGURATION_FINAL
			}
		}

		// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -

		mostDetailedLod = lods[0];

		InstancesCollectParams instancesParams;
		instancesParams.offsetInStaticInstanceBuffer = m_offsetInStaticInstanceBuffer;
		instancesParams.numInstancesInStaticInstanceBuffer = m_numInstances;

		isThisProxyVisible = CollectSingleLodModeInstances( ctx, collectCtx, featureFlags, lods, modes, instancesParams );

#ifdef USE_RAY_TRACING
		if (ctx.isMainCameraView)
		{
			m_mainViewFrameIndex = GpuApi::FrameIndex();
			m_mainViewLod = lods[0];
			m_dissolveState = modes[0] == DISSOLVEMODE_FadeIn || modes[0] == DISSOLVEMODE_FadeOut ? DissolveState::DISSOLVING : 0;
		}
#endif
	}

#ifndef RED_CONFIGURATION_FINAL
	if ( isThisProxyVisible )
	{
		ctx.collector.m_renderingStats.m_collectedMeshProxies.Increment();
		if ( isShadowCollecting )
		{
			ctx.collector.m_renderingStats.m_numCascadeProxiesTotal.Increment();
		}
	}
#endif

#ifdef __EMSCRIPTEN__
	if ( isThisProxyVisible && !isShadowCollecting && CpCull::Get().counting && !ctx.cameraData->IsOffscreen() )
	{
		CpCull::s_collected.fetch_add( 1, std::memory_order_relaxed ); // [cull] main-view proxies that emitted chunks
	}
#endif

	return isThisProxyVisible;
}

void CRenderProxy_Mesh::CollectDynamicDecals( const CRenderCollector& collector, CollectContext& collectCtx, FeatureFlagsMask featureFlags, EDissolveMode dissolveMode, RenderBatchData& commonBatchData )
{
#ifdef NO_DYNAMIC_DECALS
	return;
#endif // NO_DYNAMIC_DECALS

	if ( !collector.m_cameraInfo->HasFeature( RFF_DynamicDecals ) )
	{
		return;
	}

	DynamicDecalChunkList dynamicDecalChunksToCheck{ job::PoolJobScope() };
	{
		RED_SCOPE_SHARED_LOCK( m_dynamicDecalChunksLock );
		dynamicDecalChunksToCheck = m_dynamicDecalChunks;
	}

	const Bool isTangentUpdate = collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate );

	for ( Uint32 i = 0; i < dynamicDecalChunksToCheck.Size(); ++i )
	{
		const TRenderPtr< CRenderDynamicDecalChunk >& dynamicDecalChunk = dynamicDecalChunksToCheck[ i ];
		if ( !dynamicDecalChunk )
		{
			continue;
		}

		const TRenderPtr< CRenderDynamicDecal >& ownerDecal = dynamicDecalChunk->GetOwnerDecal();
		if ( !ownerDecal )
		{
			continue;
		}

		const CRenderMaterial* material = ownerDecal->GetMaterialForSkinned();
		if ( !material )
		{
			continue;
		}
		
		const SRenderMeshChunkInfo* chunk = dynamicDecalChunk->GetChunkInfo();
		if ( !chunk )
		{
			continue;
		}

		const CRenderMaterialParameters* materialParameters = material;

		const Bool hasComputeSkinningBuffers = collectCtx.skinningCustomData && collectCtx.skinningCustomData->m_gpuComputeSkinningBuffers;

		const SRenderStageContext stageContext = GetMeshChunkRenderStageContext(
			chunk->m_elementFlags, chunk->m_vertexFactory, *material, dissolveMode, hasComputeSkinningBuffers,
			collectCtx.useTangentUpdate && (dynamicDecalChunk->GetSourceLODIndex() == 0), collectCtx.hasDismemberment );

		RED_ASSERT( chunk->m_vertexFactory == stageContext.m_vertexFactory, "DynamicDecal, mismatch in VertexFactories, VF_chunk: %d, VF_context: %d, chunkIndex: %d, context: '%hs'", chunk->m_vertexFactory, stageContext.m_vertexFactory, dynamicDecalChunk->GetChunkIndex(), stageContext.ToShortString().AsChar() );

		CBatchDataAllocator& batchDataAllocator = collectCtx.batchDataAllocator;

		TRenderStagePasses passes;
		material->GetRenderPassesForFeatureFlags( batchDataAllocator, featureFlags, stageContext, materialParameters, passes );

		for ( const auto &pass : passes )
		{
			const Uint32 geoBatchDataIndex = dynamicDecalChunk->GetBindBatchDataIndex( batchDataAllocator.GetGeometryData() ).GetValue();

			if ( !commonBatchData.IsValid() )
			{
				Uint32 instanceOffset = batchDataAllocator.GetInstanceData().Allocate( m_instanceMatrix );
				if ( batchDataAllocator.GetInstanceData().CheckOverflowValue( instanceOffset ) )
				{
					continue;
				}

				commonBatchData.Clear();
				commonBatchData.SetProxy( m_proxyBatchDataId );
				commonBatchData.SetInstanceCount( 1 );
				commonBatchData.SetInstanceOffset( instanceOffset );
			}

			SRenderChunk& newChunk = collectCtx.stageCollector.AddChunk( pass.m_stageId, collectCtx.stageCluster );
			newChunk.m_key = pass.m_renderChunkKey;
			newChunk.m_key.SetGeometry( geoBatchDataIndex );
			newChunk.m_key.isGeometryPerChunk = 1;
			RED_FATAL_ASSERT( newChunk.m_key.GetDissolveMode() == DISSOLVEMODE_Opaque, "Key expected to be by default setup to opaque dissolve, and able to bitwiseOr any other dissolve mode" );
			if ( pass.m_isDiscarded )
			{
				newChunk.m_key.SetDissolveMode( dissolveMode );
			}

			newChunk.m_data = pass.m_renderChunkData | commonBatchData;
		}
	}
}

void CRenderProxy_Mesh::CollectElements( SUpdateContext& ctx )
{
	RED_FATAL_ASSERT( m_scene != nullptr, "Trying to collect proxy that is not in a scene - hacky hacky rendering" );
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

	// filter by render mask	
	auto& collector = ctx.collector;

#ifdef __EMSCRIPTEN__
	const unsigned cpOffscreen = ctx.cameraData->IsOffscreen() ? 1u : 0u;
	const bool cpSkinned = GetSkinningData() ? true : false;
#endif

	// should not be rendered at all ?
	if ( !( m_renderMask & MCF_RenderInScene ) )
	{
#ifdef __EMSCRIPTEN__
		CpMeshCollectProbe( CPMC_NoRenderInScene, cpOffscreen, cpSkinned, unsigned( m_renderMask ) );
#endif
		return;
	}

	// proxy not visible
	if ( !IsVisible() )
	{
#ifdef __EMSCRIPTEN__
		CpMeshCollectProbe( CPMC_Invisible, cpOffscreen, cpSkinned, unsigned( m_renderMask ) );
#endif
		return;
	}

#ifdef __EMSCRIPTEN__
	// [terrain] which prefab proxies draw close to the camera, with the renderer's own distance numbers
	if ( cp2077_terrain_proxyprobe > 0 && cpOffscreen == 0 && CheckMeshProxyFlag( RMPF_IsProxyMesh ) && !ctx.IsCameraOrthogonal() )
	{
		static std::atomic< unsigned > s_fovLogged{ 0 };
		if ( s_fovLogged.fetch_add( 1, std::memory_order_relaxed ) < 3 )
		{
			std::fprintf( stderr, "CP2077 terrain render camera: fovMultiplier=%.4f fov=%.2f\n", ctx.GetProjectionCameraDistanceMultiplier(),
				ctx.collector.m_camera ? ctx.collector.m_camera->GetFOV() : -1.0f );
		}
		const float cpDist = std::sqrt( GetCachedDistanceSquared() );
		if ( cpDist < 80.0f )
		{
			static std::mutex s_mutex;
			static std::set< const void* > s_seen;
			bool fresh = false;
			{
				std::lock_guard< std::mutex > guard( s_mutex );
				if ( s_seen.size() < 400 ) fresh = s_seen.insert( this ).second;
			}
			if ( fresh )
			{
				const Box& bb = GetBoundingBox();
				const Vector3 c = bb.CalcCenter().AsVector3();
				const Vector3 e = bb.CalcExtents().AsVector3();
				std::fprintf( stderr, "CP2077 terrain render proxy: cached=%.1f near=%.1f far=%.0f terrain=%d bboxEdge=%d center=(%.0f,%.0f,%.0f) half=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f)\n",
					cpDist, GetAutoHideDistanceNear(), std::sqrt( GetAutoHideDistanceSquared() ), CheckMeshProxyFlag( RMPF_Terrain ) ? 1 : 0, ( m_drawableFlags & RPF_CalcLodDistFromBboxEdge ) ? 1 : 0,
					c.X, c.Y, c.Z, e.X, e.Y, e.Z, ctx.cameraPosition.X, ctx.cameraPosition.Y, ctx.cameraPosition.Z );
			}
		}
	}
#endif

#ifndef RED_CONFIGURATION_FINAL
	const renderDev::SConfig* renderDevConfig = GetRenderer()->GetDevConfig();

	if ( ( m_renderMask & MCF_IsGITransparent ) && renderDevConfig->GlobalIllumination.m_hideGITransparent )
	{
		return;
	}
	if ( !( m_renderMask & MCF_IsGIVisible ) && renderDevConfig->GlobalIllumination.m_hideGIInvisible )
	{
		return;
	}
#endif // RED_CONFIGURATION_FINAL

	FeatureFlagsMask featureFlags = FeatureFlagsMask().Set( collector.m_baseFeatureFlag );

	const Bool hasSpeedTreeWind = GetSpeedTreeWindSharedData();
	// check if should write to velocity buffer
	if ( IsDynamic() && ctx.cameraData->HasFeature( RFF_VelocityBuffer ) )
	{
		// disable motion blur for instanced meshes
		// right now the velocity information is passed via material modifier (single matrix)
		// with multiple instances there's no way to pass all the matrices of all the instances
		// that would require some refactoring (e.g. having additional input element in input layout) and pass matrices using additional vertex buffer

		if ( hasSpeedTreeWind )
		{
		 	// Regular SpeedTree material doesn't use MotionMatrix since instanceMatrix stays the same but still needs to output the motion vector based on wind simulation and wind impulse in vertex shader.
			// Destructible SpeedTree render proxy however makes use of MotionMatrix but it uses different vertex factory and is never clustered so it's fine in this case as well.
			featureFlags.Set( FEATFLAG_VelocityBuffer );
		}
		else if ( !HasMultipleInstances() )
		{
			const Bool hasMotionData = m_motionData && m_motionData->ShouldRenderMotionVectors();
			const Bool hasDynamicSkinning = GetSkinningData() && GetSkinningData()->IsMotionVectorsDataPinned() && !GetSkinningData()->IsMotionVectorsDataStatic();

			if ( hasMotionData || hasDynamicSkinning )
			{
				featureFlags.Set( FEATFLAG_VelocityBuffer );
			}
		}
	}

	if ( hasSpeedTreeWind )
	{
		CSpeedTreeWindDataUpdater& windStorage = m_scene->GetCameraStorage()->GetCustomData<CSpeedTreeWindDataUpdater>();
		windStorage.RegisterVisibleMesh( this->m_mesh.Get() );
	}

	if (ctx.cameraData->HasFeature( RFF_ScreenSpaceRain ))
	{
		const Bool hasRainData = HasRainData();
		if (hasRainData)
		{
			featureFlags.Set( FEATFLAG_Rain );
		}
	}

	const SRenderStageCluster stageCluster = SRenderStageCluster::Init( m_renderingPlane );

	if ( !CameraTargetTypeMatchesMeshSettings( *ctx.cameraData, m_customCameraTarget ) )
	{
#ifdef __EMSCRIPTEN__
		CpMeshCollectProbe( CPMC_CameraTarget, cpOffscreen, cpSkinned, unsigned( m_customCameraTarget ) );
#endif
		return;
	}

	if ( !CheckRenderFilters( collector.m_cameraInfo->GetRenderingMask() ) )
	{
#ifdef __EMSCRIPTEN__
		CpMeshCollectProbe( CPMC_RenderFilter, cpOffscreen, cpSkinned, unsigned( m_renderMask ) );
#endif
		return;
	}

#ifndef RED_CONFIGURATION_FINAL
	// filter mesh by resource name
	if ( renderDevConfig->MeshProxyCollectFilter.IsEnabled() && m_mesh )
	{
		const red::StringView filterStr( renderDevConfig->MeshProxyCollectFilter.m_resourceName );
		if ( !filterStr.Empty() )
		{
            if( filterStr == "!" )
            {
                if( m_mesh->GetMeshResourcePath().ToStringView().Length() != 0 )
                {
                    return;
                }
            }
            else if ( m_mesh->GetMeshResourcePath().ToStringView().Find( filterStr ) == red::StringView::npos )
			{
				return;
			}
		}
	}
#endif // RED_CONFIGURATION_FINAL

	const RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData< RenderProxyCustomData_Skinning >();
	if ( skinningCustomData && skinningCustomData->m_floatTrackData )
	{
		featureFlags.Set( FEATFLAG_FloatTracks );
	}

	// Update stuff that should be updated once per frame
	UpdateOncePerFrame( ctx );

	//TODO? Even in case of "Clustered Proxy" all of its instances will have the exact same distanceFromCamera saved into renderChunk.key
	const Float distanceToCameraSqr = ( ctx.cameraPosition - GetTranslation() ).SquareMag3();
	const Float distanceToCamera = sqrtf( distanceToCameraSqr );

	// enable depth prepass only for gbuffer
	// if we don't do this check, the depth prepass is collected in wireframe or overdraw modes and we get missing meshes there,
	// because we won't fallback to appropriate debug material (this happens only when we don't collect any renderpass)
	if ( featureFlags.Get( FEATFLAG_Default ) )
	{
		// TODO excluding meshes from depth prepass manually, making some meshes obligatory, etc.
		// TODO more heuristics, e.g. based on triangle count

		// collect only bigger meshes to depth prepass
		if ( distanceToCameraSqr < Config::cvDepthPrepassDistanceFactor.Get() * GetBoundingBox().CalcSize().SquareMag3() )
		{
			featureFlags.Set( FEATFLAG_DepthPrepass );
		}
	}

	Bool useTangentUpdate = collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate );
	if ( m_mesh )
	{
		// A little bit hacky, but...
		// If mesh is runtime dynamic (cloth at the moment), it uses MeshStatic VertexFactory, which does not allow for runtime computation of previous frame
		// position. Previous frame position is used to determine proper motion vector to do motion blur, TXAA and so on.
		// But cloth always provides previous frame vertex positions via an additional buffer. It is bound and used properly when Runtime Tangent Update is
		// enabled.
		// So in case when Runtime Tangent Update is disabled (eg. low quality platforms) we still want to use the previous frame information for cloth, just
		// as we would with RTU on. So for RuntimeDynamic meshes (cloth) we turn RTU on artificially.
		useTangentUpdate |= m_mesh->IsRuntimeDynamic();
	}

	CollectContext collectCtx =
	{
		/* .stageCollector	 = */ collector.m_renderStageCollector,
		/* .collector		 = */ collector,
		/* .stageCluster	 = */ stageCluster,
		/* .renderMask		 = */ MCF_RenderInScene,
		/* .collectFlags	 = */ COLLECT_None,
		/* .dissolveContext	 = */ RDC_Scene,
		/* .distanceToCamera = */ distanceToCamera,
		/* .skinningCustomData=*/ skinningCustomData,
		/* .miscMeshParams	 = */ GetCustomData<RenderProxyCustomData_MiscMeshParams>(),
		/* .useTangentUpdate = */ useTangentUpdate,
	};

#ifdef __EMSCRIPTEN__
	CpMeshCollectProbe( CPMC_Collect, cpOffscreen, cpSkinned, unsigned( GetMaterialLodGroups().GetNumLods() ),
		unsigned( skinningCustomData && skinningCustomData->m_gpuComputeSkinningBuffers ) );
#endif
	CollectAllLodModeInstances( ctx, collectCtx, featureFlags );
}

void CRenderProxy_Mesh::SetMeshProxyFlag( ERenderMeshProxyFlags flag, Bool flagValue /*= true*/ )
{
	if ( flagValue )
	{
		m_meshProxyFlags = ERenderMeshProxyFlags( m_meshProxyFlags | flag );
	}
	else
	{
		m_meshProxyFlags = ERenderMeshProxyFlags( m_meshProxyFlags & ~flag );
	}
}

void CRenderProxy_Mesh::ClearMeshProxyFlag( ERenderMeshProxyFlags flag )
{
	m_meshProxyFlags = ERenderMeshProxyFlags( m_meshProxyFlags & ~flag );
}

#ifdef USE_RAY_TRACING
void CRenderProxy_Mesh::RayTracingInit()
{
	if ( m_mesh && !m_mesh->m_isUsedWithRayTracing )
	{
		m_rayTracingLodMask = 0;
		return;
	}

	RED_SCOPE_SHARED_LOCK( m_materialLodGroupsFlag );

	// Skip proxy meshes
	if ( !m_materialLodGroups.Empty() && ( m_materialLodGroups.GetNumChunksInLOD( 0 ) > 0 ) )
	{
		const RenderMaterialInfo& renderMaterialInfo = m_materialLodGroups.GetMaterialEntryForChunk( 0, 0 );
		if ( renderMaterialInfo.m_materialPtr && !renderMaterialInfo.m_materialPtr->GetEngineStagePassDataCollectionArray().Empty() )
		{
			const EngineStagePassDataCollection& enginePasses = renderMaterialInfo.m_materialPtr->GetEngineStagePassDataCollectionArray()[ 0 ];
			for ( auto it = enginePasses.Begin(); it != enginePasses.End(); ++it )
			{
				if ( it->m_stageNameRegular == RED_NAME_CONSTEXPR_NOREG( "renderstage_top_down_car_proxy" ) )
				{
					m_rayTracingLodMask = 0;
					break;
				}
			}
		}
	}
}

void CRenderProxy_Mesh::RayTracingRelease()
{
	for (Uint32 i = 0; i < m_lodNumMax; ++i)
	{
		RayTracingClearLod( i );
	}
}

void CRenderProxy_Mesh::RayTracingClearLod( Int8 lod )
{
	AccelerationStructureLodData& lodData = m_accelerationStructureLodData[lod];
	if (lodData.accelerationStructure)
	{
		if (lodData.isCustomAccelerationStructure)
		{
			CRenderAccelerationStructure* accelerationStructureManager = GetRenderer() ? GetRenderer()->GetAccelerationStructureManager() : nullptr;
			if (accelerationStructureManager)
			{
				accelerationStructureManager->RemoveBottomLevelFromCache( &lodData.accelerationStructure );
				GpuApi::SafeRelease( lodData.accelerationStructure );
			}
		}

		lodData.accelerationStructure = GpuApi::AccelerationStructureRef::Null();
	}

	lodData.geometryInstanceFlags = 0;
	lodData.lastUpdatedInFrame = 0;
	lodData.isCustomAccelerationStructure = false;
	m_accelerationStructureUpdateNum = 0;
}

GpuApi::AccelerationStructureRef CRenderProxy_Mesh::SetAccelerationStructure( Int8 lod, GpuApi::AccelerationStructureRef accelerationStructure, Uint32 geometryInstanceFlags, Bool isDynamicMesh, Bool isCustomAccelerationStructure )
{
	RED_ASSERT( lod >= 0 && lod < m_lodNumMax );
	RED_ASSERT( m_mesh.Get() == m_mesh->GetLodGroups()[lod].GetChunk( 0 )->m_mesh );
	RED_ASSERT( accelerationStructure != GpuApi::AccelerationStructureRef::Null() );

	AccelerationStructureLodData& lodData = m_accelerationStructureLodData[lod];
	lodData.geometryInstanceFlags = geometryInstanceFlags;
	if (isDynamicMesh)
	{
		lodData.geometryInstanceFlags |= GeometryInstanceBits::DYNAMIC;
#ifndef RED_CONFIGURATION_FINAL
		lodData.geometryInstanceFlags |= GeometryInstanceBits::UPDATED;
#endif
	}

	// Previous acceleration structure may become invalid after m_meshChunkMask update for the current LOD
	if (lodData.isCustomAccelerationStructure)
	{
		CRenderAccelerationStructure* accelerationStructureManager = GetRenderer() ? GetRenderer()->GetAccelerationStructureManager() : nullptr;

		if (accelerationStructureManager && lodData.accelerationStructure)
		{
			accelerationStructureManager->RemoveBottomLevelFromCache( &lodData.accelerationStructure );
			GpuApi::SafeRelease( lodData.accelerationStructure );
		}
	}

	if (!isCustomAccelerationStructure)
	{
		accelerationStructure = m_mesh->SetAccelerationStructure( accelerationStructure, lod, geometryInstanceFlags );
	}

	lodData.accelerationStructure = accelerationStructure;
	lodData.lastUpdatedInFrame = GpuApi::FrameIndex();
	lodData.isCustomAccelerationStructure = isCustomAccelerationStructure;
	m_accelerationStructureUpdateNum = 0;

	return accelerationStructure;
}

void CRenderProxy_Mesh::UpdateAccelerationStructure( Int8 lod, GpuApi::AccelerationStructureRef accelerationStructure, Bool wasUpdated )
{
	RED_ASSERT( lod >= 0 && lod < m_lodNumMax );
	RED_ASSERT( m_mesh.Get() == m_mesh->GetLodGroups()[lod].GetChunk( 0 )->m_mesh );

	AccelerationStructureLodData& lodData = m_accelerationStructureLodData[lod];

	RED_ASSERT( lodData.accelerationStructure == accelerationStructure );
	RED_ASSERT( lodData.isCustomAccelerationStructure == true );

	lodData.accelerationStructure = accelerationStructure;
	lodData.lastUpdatedInFrame = GpuApi::FrameIndex();
	lodData.geometryInstanceFlags |= GeometryInstanceBits::DYNAMIC;
#ifndef RED_CONFIGURATION_FINAL
	lodData.geometryInstanceFlags |= GeometryInstanceBits::UPDATED;
#endif
	m_accelerationStructureUpdateNum = wasUpdated ? m_accelerationStructureUpdateNum + 1 : 0;
}
#endif // USE_RAY_TRACING

void CRenderProxy_Mesh::ConditionalInvalidateClusteredProxyData(const Bool firstTimeInvalidate)
{
	if ( IsClusteredProxyWithMultipleInstances() && m_scene )
	{
		red::DynArray < CRenderFrameCameraStorage::SCameraData* > allCameraData{ job::PoolJobScope() };
		m_scene->GetCameraStorage()->GetAllCameraData( allCameraData );
		for ( CRenderFrameCameraStorage::SCameraData* camData : allCameraData )
		{
			CameraCustomData_RenderProxyData& rpData = camData->GetCustomData< CameraCustomData_RenderProxyData >();
			rpData.InvalidateClusteredProxyData( GetIdInScene(), firstTimeInvalidate, m_instancesData, m_numInstances, m_lodSetup, camData->GetRenderCamera(RPl_Scene).GetPosition().AsVector3() );
		}

	}
}

Bool CRenderProxy_Mesh::CreateProxyBatchData()
{
	if ( m_mesh && m_proxyBatchDataId == UINT32_MAX )
	{
		SProxyBatchData batchData;
		batchData.SetProxyAndObjectTypeID( this, m_objectTypeID );

		if ( const RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
		{
			batchData.m_skinningData = skinningCustomData->m_skinningData.Get();

#ifdef RED_ASSERTS_ENABLED
			const Bool hasGpuComputeSkinningBuffers = skinningCustomData->m_gpuComputeSkinningBuffers.Get() != nullptr;
			const Bool hasAnyTopologyData = m_mesh->HasAnyTopologyData();
			RED_FATAL_ASSERT( hasGpuComputeSkinningBuffers == hasAnyTopologyData, "Invalid RuntimeTangentUpdate setup for RenderProxy for mesh '%hs', hasTopology: %d, hasGpuComputeSkinningBuffers:%d", m_mesh->GetMeshResourcePath().ToDebugString(), hasAnyTopologyData, hasGpuComputeSkinningBuffers );
#endif // RED_ASSERTS_ENABLED

			if ( skinningCustomData->m_skinningData )
			{
				batchData.m_gpuComputeSkinningBuffers = skinningCustomData->m_gpuComputeSkinningBuffers.Get();
			}
		}

		m_proxyBatchDataId = GetRenderer()->GetBatchDataAllocator().GetProxyData().Allocate( batchData );
	}

	return m_proxyBatchDataId != UINT32_MAX;
}

void CRenderProxy_Mesh::ReleaseProxyBatchData()
{
	GetRenderer()->GetBatchDataAllocator().GetProxyData().Release( m_proxyBatchDataId );
	m_proxyBatchDataId = UINT32_MAX;
}

void CRenderProxy_Mesh::CollectTodvisElements( SUpdateContext& ctx )
{
	RED_FATAL_ASSERT( m_scene != nullptr, "Trying to collect proxy that is not in a scene - hacky hacky rendering" );
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

	if ( ( m_shadowFlags.m_mask & ShadowFlags::GLOBAL_SHADOWS_MASK() ) == 0 )
		return;

	UpdateOncePerFrame( ctx );
	const Float distanceToCamera = ( ctx.cameraPosition - GetTranslation() ).Mag3();

	auto& collector = ctx.collector;
	const FeatureFlagsMask featureFlags = FeatureFlagsMask().Set( collector.m_baseFeatureFlag );

	const SRenderStageCluster stageCluster = SRenderStageCluster::Init( 0 );
	CollectContext collectCtx =
	{
		collector.m_renderStageCollector,
		collector,
		stageCluster,
		MCF_RenderInShadows,
		COLLECT_IsShadow,
		RDC_Scene,
		distanceToCamera,
		GetCustomData<RenderProxyCustomData_Skinning>(),
		GetCustomData<RenderProxyCustomData_MiscMeshParams>(),
		collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate ),
	};

	CollectAllLodModeInstances( ctx, collectCtx, featureFlags );
}

void CRenderProxy_Mesh::CollectCascadeShadowElements( SUpdateContext& ctx, SMergedShadowCascades& cascades, Uint32 perCascadeTestFlags )
{
	RED_FATAL_ASSERT( m_scene != nullptr, "Trying to collect proxy that is not in a scene - hacky hacky rendering" );
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

	// proxy not visible, even in shadows
	if ( !IsVisible() && !CheckMeshProxyFlag( RMPF_CastingGlobalShadowsEvenIfNotVisible ) )
		return;

	auto& collector = ctx.collector;

	if ( !CheckRenderFilters( collector.m_cameraInfo->GetRenderingMask() ) )
		return;

	ShadowFlags shadowFlags = m_shadowFlags;
#ifndef RED_CONFIGURATION_FINAL
	// In non-final build, allow for refreshing heuristics at every collect. This way we can easily tweak the heuristic to maintain the best balance between quality and performance
	if ( Config::cvForceVolumeHeuristicRefresh.Get() )
	{
		SetupShadowFlags( GetBoundingBox().CalcSize().Mag3(), GetAutoHideDistance() );
	}
#endif // RED_CONFIGURATION_FINAL

	if ( GetSpeedTreeWindSharedData() )
	{
		CSpeedTreeWindDataUpdater& windStorage = m_scene->GetCameraStorage()->GetCustomData<CSpeedTreeWindDataUpdater>();
		windStorage.RegisterVisibleMesh( this->m_mesh.Get() );
	}

	// Update stuff that should be updated once per frame
	UpdateOncePerFrame( ctx );

	// Process all cascades
	const Uint16 numCascades = cascades.m_numCascades;
	const Uint16 numFullDetailCascades = numCascades;

	FeatureFlagsMask featureFlags = FeatureFlagsMask().Set(FEATFLAG_Shadows);

	RenderBatchData commonBatchData;
	RED_ASSERT( numFullDetailCascades <= 4, "No more than 4 cascades are supported" );
#ifdef __EMSCRIPTEN__
	// [cull r4] sun-shadow cascade rules for static world casters (?cullShadow=0..3, default = ?cull preset; off by default).
	// Kept untouched: entities (devices/POIs), skinned (NPCs/player), vehicles/characters (non-static object types),
	// prefab proxies (skyline), terrain, roads, and anything with a bbox diagonal >= 30 m (buildings keep their shadows).
	const CpCull::Settings& cpCs = CpCull::Get();
	Int32 cpRule = 0; // 0 = engine, 1 = static caster (size/distance rules), 2 = foliage/clustered caster
	Float cpDiag = 0.0f;
	if ( cpCs.shadowLevel > 0 && !IsDynamic() && !GetSkinningData() && !CheckMeshProxyFlag( RMPF_IsProxyMesh ) && !CheckMeshProxyFlag( RMPF_Terrain )
		&& ( m_objectTypeID <= ROT_STATIC_LAST || m_objectTypeID == ROT_Foliage || m_objectTypeID == ROT_Grass ) && m_objectTypeID != ROT_Terrain && m_objectTypeID != ROT_Road )
	{
		cpDiag = GetBoundingBox().CalcSize().Mag3();
		const Bool foliage = CheckMeshProxyFlag( RMPF_IsClusteredProxy ) || CheckMeshProxyFlag( RMPF_GeneratedByVegetationSystem ) || m_objectTypeID == ROT_Foliage || m_objectTypeID == ROT_Grass;
		if ( foliage ) cpRule = 2;
		else if ( cpDiag < 30.0f ) cpRule = 1;
	}
	const Bool cpTooFar = cpRule == 1 && cpDiag < 8.0f && cpCs.cascMaxDist > 0.0f && GetCachedDistanceSquared() > cpCs.cascMaxDist * cpCs.cascMaxDist;
#endif
	for ( Uint16 cascadeIndex=0; cascadeIndex < numFullDetailCascades; ++cascadeIndex, perCascadeTestFlags >>= 2 )
	{
		if ( ( ( shadowFlags.m_mask >> cascadeIndex ) & 0x1 ) == 0 )
		{
			// If I am too small to go to cascade N, for sure I won't go to cascade N+1 either, so let's break completely
			break;
		}
		// Test the visibility of the fragment in this cascade
		const Uint32 visResult = perCascadeTestFlags & 3;

		// We are not in the cascade
		if ( visResult == 0 )
		{
			continue;
		}
#ifdef __EMSCRIPTEN__
		if ( cpRule != 0 )
		{
			const Bool skip = ( cpRule == 2 ) ? ( Int32( cascadeIndex ) >= cpCs.folMaxCascade )
				: ( cpTooFar || cpDiag < cpCs.cascMinDiag[ cascadeIndex & 3 ] );
			if ( skip )
			{
				if ( cpCs.counting ) CpCull::s_cascadeSkipped[ cascadeIndex & 3 ].fetch_add( 1, std::memory_order_relaxed );
				continue;
			}
		}
		if ( cpCs.counting ) CpCull::s_cascadeCollects[ cascadeIndex & 3 ].fetch_add( 1, std::memory_order_relaxed );
#endif

		const SRenderStageCluster stageCluster = SRenderStageCluster::Init( cascadeIndex );

		Uint32 collectFlags = COLLECT_IsShadow;

		const SShadowCascade& cascade = cascades.m_cascades[cascadeIndex];
		//TODO? Even in case of "Clustered Proxy" all of its instances will have the exact same distanceFromCamera saved into renderChunk.key
		const Float distanceToCamera = Max( 0.0f, cascade.m_camera.GetWorldToCamera().TransformPoint( GetTranslation() ).Z );

		CollectContext collectCtx
		{
			/* .stageCollector	 = */ collector.m_renderStageCollector,
			/* .collector		 = */ collector,
			/* .stageCluster	 = */ stageCluster,
			/* .renderMask		 = */ MCF_RenderInShadows,
			/* .collectFlags	 = */ collectFlags,
			/* .dissolveContext	 = */ RDC_ShadowMain,
			/* .distanceToCamera = */ distanceToCamera,
			/* .skinningCustomData=*/ GetCustomData<RenderProxyCustomData_Skinning>(),
			/* .miscMeshParams	 = */ GetCustomData<RenderProxyCustomData_MiscMeshParams>(),
			/* .useTangentUpdate = */ collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate ),
		};

		CollectAllLodModeInstances( ctx, collectCtx, featureFlags );
	}
}

Bool CRenderProxy_Mesh::CollectLocalShadowElements( SUpdateContext& ctx, Bool isDynamic, SRenderStageCluster stageCluster )
{
	RED_FATAL_ASSERT( m_scene != nullptr, "Trying to collect proxy that is not in a scene - hacky hacky rendering" );
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

	// Early check, can you even cast local shadow? no ? then stop here.
	if( !m_shadowFlags.m_localShadows )
		return false;

	// Proxy not visible, even in shadows
	if ( !IsVisible() && !CheckMeshProxyFlag( RMPF_CastingLocalShadowsEvenIfNotVisible ) )
		return false;

	auto& collector = ctx.collector;

	// non-final only: check if we are hidden for debug purposes
	if ( !CheckRenderFilters( collector.m_cameraInfo->GetRenderingMask() ) )
		return false;
	
	if ( GetSpeedTreeWindSharedData() )
	{
		CSpeedTreeWindDataUpdater& windStorage = m_scene->GetCameraStorage()->GetCustomData<CSpeedTreeWindDataUpdater>();
		windStorage.RegisterVisibleMesh( this->m_mesh.Get() );
	}

	// Update once-per frame stuff
	UpdateOncePerFrame( ctx );

	FeatureFlagsMask featureFlags = FeatureFlagsMask().Set(FEATFLAG_Shadows);

	//TODO? Even in case of "Clustered Proxy" all of its instances will have the exact same distanceFromCamera saved into renderChunk.key
	const Float distanceToCamera = ( ctx.cameraPosition - GetTranslation() ).Mag3();

	CollectContext collectCtx
	{
		/* .stageCollector	 = */ isDynamic ? collector.m_localShadowsCollector.m_renderStageCollectorDynamic : collector.m_localShadowsCollector.m_renderStageCollectorStatic,
		/* .collector		 = */ collector,
		/* .stageCluster	 = */ stageCluster,
		/* .renderMask		 = */ MCF_RenderInShadows,
		/* .collectFlags	 = */ (Uint32)COLLECT_IsShadow | (Uint32)COLLECT_NoDissolve,
		/* .dissolveContext	 = */ RDC_ShadowMain,
		/* .distanceToCamera = */ distanceToCamera,
		/* .skinningCustomData=*/ GetCustomData<RenderProxyCustomData_Skinning>(),
		/* .miscMeshParams	 = */ GetCustomData<RenderProxyCustomData_MiscMeshParams>(),
		/* .useTangentUpdate = */ collector.m_cameraInfo->HasFeature( RFF_RuntimeTangentUpdate ),
	};

	return CollectAllLodModeInstances( ctx, collectCtx, featureFlags );
}

const CRenderDissolveSynchronizer CRenderProxy_Mesh::GetDissolveSync() const
{
	RED_STATIC_ASSERT( 4 == sizeof( TDissolveMarker ) );
	return m_scene ? m_scene->GetDissolveSynchronizer() : CRenderDissolveSynchronizer::GetFallbackSynchronizer();
}

void CRenderProxy_Mesh::ComputeLOD( const SUpdateContext& ctx, Int8& outLOD, Bool& outImmediate )
{
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );

	// Calculate min/max LOD
	Int32 minLOD = -1;
	Int32 maxLOD = 100;

	// Because of this hack with replacedMaterial, similar one have to exist on the gameplay side in CMimicComponent::GetLodBoneNum()!
	// If you ever change condition for hack here, remember to update also condition on the gameplay side.
	// If you are removing this hack, please also remove the one in CMimicComponent::GetLodBoneNum().
	const Int8 forceLOD = ctx.forcedLOD;
	const Bool forceLod0 = Config::GGlobalRenderingSettings.forceHighestLOD || forceLOD == 0;
	if ( forceLod0 )
	{
		minLOD = 0;
		maxLOD = 0;
	}

	if ( m_hasForcedLOD )
	{
		if ( GetMaterialLodGroups().Empty() )
		{
			// hide mesh
			minLOD = maxLOD = -1;
		}
		else
		{
			minLOD = maxLOD = math::Min( (Uint32)m_forcedLODLevel, GetMaterialLodGroups().GetNumLods() - 1u );
		}
	}
#ifndef NO_EDITOR
	else
	{
		// Editor forced LOD
		if ( forceLOD != -1 )
		{
			outLOD = Min< Int8 >( forceLOD, m_lodSetup.GetNumLODs() - 1 );
			outImmediate = true;
		}
	}
#endif
	if (minLOD == maxLOD)
	{
		outLOD = minLOD;		// this is the vast majority of meshes, for most, m_hasForcedLOD is true
	}
	else
	{
#ifdef __EMSCRIPTEN__
		// [cull] ?cullLod=<x>: static world meshes pick their LOD as if x times farther (coarser earlier)
		const Float cpLodMul2 = CpCull::Get().lodMul2;
		if ( cpLodMul2 != 1.0f && !IsDynamic() && !GetSkinningData() )
		{
			outLOD = m_lodSetup.ComputeBestLOD( GetCachedDistanceSquared() * cpLodMul2 );
		}
		else
#endif
		outLOD = m_lodSetup.ComputeBestLOD( GetCachedDistanceSquared() );
	}
	outImmediate = forceLod0;
}

void CRenderProxy_Mesh::UpdateDissolveState( const SUpdateContext& ctx, Bool wasVisibleLastFrame )
{
	RED_FATAL_ASSERT( ctx.cameraData, "Update context does not have camera data. " );
	CameraCustomData_RenderProxyData& rpData = ctx.cameraData->GetCustomData<CameraCustomData_RenderProxyData>();
	CRenderDissolveState& dissolveState = rpData.GetDissolveState( this );

	// Leaving it here as some sort of idea for fast path optimization
	// if( !ctx.allowMeshDissolve )
	// {
	// 	Int8 lod = -1;
	// 	Bool immediateLodChange = false;
	// 	ComputeLOD( ctx, lod, immediateLodChange );
	// 
	// 	dissolveState.SetLodAndFlags( GetDissolveSync(), RDF_VisibleInScene, lod, true );
	// 
	// 	return;
	// }

	Bool doFade = wasVisibleLastFrame;

	if ( CheckMeshProxyFlag( RMPF_InitAsHiden ) )
	{
		ClearMeshProxyFlag( RMPF_InitAsHiden );
		if ( !doFade )
		{
			const Int32 framesAlive = GpuApi::FrameIndex() - m_attachedFrameIndex;
			// If the mesh is visible for the first time, but it was created more than 1 frame ago,
			// it means it could be invisible due to visibility checks. In this case, just show it immediately.
			if ( framesAlive < 3 )
			{
				doFade = true;
			}
		}
	}

	if ( CheckMeshProxyFlag( RMPF_ForceDissolveFadeIn ) )
	{
		if ( dissolveState.IsVisible( GetDissolveSync() ) ) // keep the flag until it starts to fade in
		{
			ClearMeshProxyFlag( RMPF_ForceDissolveFadeIn );
		}
		doFade = true;
	}
	
	if ( CheckMeshProxyFlag( RMPF_ForceInstantFadeIn ) )
	{
		ClearMeshProxyFlag( RMPF_ForceInstantFadeIn );
		ClearMeshProxyFlag( RMPF_ForceDissolveFadeIn ); // possible scenario: RMPF_ForceDissolveFadeIn was kept for 1 frame or more, but later RMPF_ForceInstantFadeIn was set
		doFade = false;
	}

	Uint32 dissolveFlags = 0;
	if ( IsVisibleInCurrentFrame() && m_isDissolveVisible )
	{
		dissolveFlags |= RDF_VisibleInScene;
	}
	if ( IsVisibleInCurrentFrame() && ( m_isDissolveVisible || CheckMeshProxyFlag( RMPF_CastingLocalShadowsEvenIfNotVisible ) || CheckMeshProxyFlag( RMPF_CastingGlobalShadowsEvenIfNotVisible ) ) )
	{
		dissolveFlags |= RDF_VisibleInShadows;
	}

	Int8 lod = -1;
	Bool immediateLodChange = false;
	ComputeLOD( ctx, lod, immediateLodChange );

#ifdef __EMSCRIPTEN__
	// [cull] ?cullFade=0: proxies whose far auto-hide we lowered hide at once instead of dissolving out; counters.
	{
		const Bool cpClamped = CheckMeshProxyFlag( ERenderMeshProxyFlags( CpCull::c_clampedFlag ) );
		const CpCull::Settings& cs = CpCull::Get();
		if ( cpClamped && !cs.fade && !( dissolveFlags & RDF_VisibleInScene ) )
		{
			doFade = false;
		}
		if ( cs.counting && !ctx.cameraData->IsOffscreen() )
		{
			CpCull::s_considered.fetch_add( 1, std::memory_order_relaxed );
			if ( cpClamped && GetCachedDistanceSquared() >= GetAutoHideDistanceSquared() )
			{
				// size rule if K * diagonal is the binding limit, else the props/foliage distance cap
				const Float diag = GetBoundingBox().CalcSize().Mag3();
				const Bool bySize = cs.sizeK > 0.0f && cs.sizeK * diag <= m_autoHideDistance * 1.001f;
				( bySize ? CpCull::s_culledSize : CpCull::s_culledDist ).fetch_add( 1, std::memory_order_relaxed );
			}
			else if ( ( IsDynamic() || GetSkinningData() ) && ( dissolveFlags & RDF_VisibleInScene ) )
			{
				CpCull::s_keptPoi.fetch_add( 1, std::memory_order_relaxed );
			}
			CpCull::NoteFrameAndMaybePrint();
		}
	}
#endif

	dissolveState.SetLodAndFlags( GetDissolveSync(), (ERenderDissolveFlags)dissolveFlags, lod, !ctx.allowMeshDissolve || immediateLodChange || !doFade );
}

void CRenderProxy_Mesh::UpdateProxyOncePerFrameImpl( const SUpdateContext& ctx, Bool wasVisibleLastFrame )
{
	// Compute basic stuff
	BaseClass::UpdateProxyOncePerFrameImpl( ctx, wasVisibleLastFrame );

	CameraCustomData_RenderProxyData& rpData = ctx.cameraData->GetCustomData<CameraCustomData_RenderProxyData>();

	if ( IsClusteredProxyWithMultipleInstances() )
	{
		ConditionalDisableClusteredProxyInstances();

		Bool instanceBucketsChanged = rpData.UpdateLodModesForClusteredProxyInstances( GetDissolveSync(), this, m_lodSetup, ctx.forcedLOD, m_instancesData, m_numInstances, ctx.cameraPosition );
		const Bool shouldUseStaticBuffer = m_offsetInStaticInstanceBuffer != UINT32_MAX;
		//TODO - might want to always grab the instanceBuckets from the main view camera for other camera datas as well.
		//Then we wouldn't have to constantly reupload the static instance buffer for every camera data whenever at least one of those cameras modified the buffer
		if ( instanceBucketsChanged || m_clusteredProxyStaticInstanceBufferCameraHash != ctx.cameraData->m_cameraHash )
		{
			m_clusteredProxyStaticInstanceBufferCameraHash = ctx.cameraData->m_cameraHash; //render graphs execute sequentially so it doesn't have to be atomic

			CRenderGeometryBatcher* geomBatcher = GetRenderer()->GetGeometryBatcher();
			Uint32 staticInstanceBufferAccumOffset = 0;
			//Reorder matrices in the static instance buffer
			CameraCustomData_RenderProxyData::ClusteredProxyData* clusteredProxyData = rpData.GetClusteredProxyData( this->GetIdInScene() );
			if ( clusteredProxyData )
			{
				clusteredProxyData->m_dynBuffReorderedTransforms.Clear();
				clusteredProxyData->m_dynBuffReorderedTransforms.Reserve( m_numInstances );
				for ( const auto& bucket : clusteredProxyData->m_instancesBuckets )
				{
					auto& dissolveStateInfo = const_cast<CameraCustomData_RenderProxyData::ClusteredProxyDissolveStateInfo&>( bucket.Value() );
					const Uint32 bucketSize = dissolveStateInfo.instancesIndices.Size();
					if ( bucketSize > 0 )
					{
						for ( Uint32 ind : dissolveStateInfo.instancesIndices )
						{
							clusteredProxyData->m_dynBuffReorderedTransforms.PushBack( m_instancesData[ind] );
						}
						if ( shouldUseStaticBuffer )
						{
							const Uint32 finalStaticInstanceBufferOffset = m_offsetInStaticInstanceBuffer + staticInstanceBufferAccumOffset;
							red::ArraySpan<const IRenderProxyInstance> matricesBucket = red::ArraySpan<const IRenderProxyInstance>( clusteredProxyData->m_dynBuffReorderedTransforms.TypedData() + staticInstanceBufferAccumOffset, bucketSize );
							geomBatcher->PushStaticInstanceBufferUpload( matricesBucket, finalStaticInstanceBufferOffset );

							dissolveStateInfo.staticInstanceBufferOffset = finalStaticInstanceBufferOffset;

							staticInstanceBufferAccumOffset += bucketSize;
						}
					}
					else
					{
						dissolveStateInfo.staticInstanceBufferOffset = UINT32_MAX;
					}
				}
			}
			
		}
	}
	else
	{
		UpdateDissolveState( ctx, wasVisibleLastFrame );
	}
}

Bool CRenderProxy_Mesh::IsAllowedForEnvProbes() const
{
	// ace_ibl_fix: pimp this, it doesn't include everything it should (for instance geralt's swords, and I bet some other shit also)
	return BaseClass::IsAllowedForEnvProbes() && !CheckMeshProxyFlag( RMPF_IsCharacterProxy );
}

static const TRenderPtr<CRenderSkinningData> c_emptySkinningData;
static const TRenderPtr<CRenderFloatTrackData> c_emptyFloatTrackData;

const TRenderPtr<CRenderSkinningData>& CRenderProxy_Mesh::GetSkinningData() const
{
	if ( RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
	{
		return skinningCustomData->m_skinningData;
	}
	return c_emptySkinningData;
}

const TRenderPtr<CRenderFloatTrackData>& CRenderProxy_Mesh::GetFloatTrackData() const
{
	if ( RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
	{
		return skinningCustomData->m_floatTrackData;
	}
	return c_emptyFloatTrackData;
}

Bool CRenderProxy_Mesh::HasGpuComputeSkinningBuffers() const
{
	if ( RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
	{
		return skinningCustomData->m_gpuComputeSkinningBuffers.Get() != nullptr;
	}
	return false;
}

void CRenderProxy_Mesh::SetSkinningData( const TRenderPtr<IRenderObject>& skinningData )
{
	RED_FATAL_ASSERT( skinningData );

	RED_ASSERT( m_numInstances <= 1, "Skinning Data is not supported when render proxy has multiple instances." );

	RenderProxyCustomData_Skinning* skinningCustomData = AcquireCustomData<RenderProxyCustomData_Skinning>();
	RED_FATAL_ASSERT( skinningCustomData );

	if ( skinningCustomData->m_skinningData != skinningData )
	{
		skinningCustomData->m_skinningData = red::StaticCast<CRenderSkinningData>( skinningData );

		// recreate proxy batch data
		ReleaseProxyBatchData();
		CreateProxyBatchData();
	}
}

void CRenderProxy_Mesh::SetFloatTrackData( const TRenderPtr<IRenderObject>& floatTrackData )
{
	RED_FATAL_ASSERT( floatTrackData );

	RenderProxyCustomData_Skinning* skinningCustomData = AcquireCustomData<RenderProxyCustomData_Skinning>();
	RED_FATAL_ASSERT( skinningCustomData );

	if ( skinningCustomData->m_floatTrackData != floatTrackData )
	{
		skinningCustomData->m_floatTrackData = red::StaticCast<CRenderFloatTrackData>( floatTrackData );
	}

	// Set new skinning data
	skinningCustomData->m_floatTrackData->AdvanceRead();
}

void CRenderProxy_Mesh::SetDismembermentData( const RenderDismCullObjectCB* cbufferData )
{
	//PC_SCOPE( SetDismData );

	struct Helper
	{
		static void Destroy( DismembermentDataPtr&& data )
		{
			if( data )
			{
				data->Destroy();
				data->SetData( nullptr );
			}
			data = nullptr;
		}
	};

	if ( !cbufferData )
	{
		RenderProxyCustomData_MiscMeshParams* data = GetCustomData<RenderProxyCustomData_MiscMeshParams>();
		if ( data )
		{
			Helper::Destroy( std::move( data->m_dismembermentData ) );
		}
	}
	else
	{
		RenderProxyCustomData_MiscMeshParams* data = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
		RED_FATAL_ASSERT( data );
		const Uint32 numCullObjects = RenderDismGetCount( *cbufferData );
		if ( numCullObjects > 0 )
		{
			if( !data->m_dismembermentData )
			{
				data->m_dismembermentData = red::CreateUniquePtr< RenderDismembermentData, rend::PoolRenderProxy_CustomData >();
			}

			if ( data->m_dismembermentData->m_cbuffer == GpuApi::BufferRef::Null() )
			{
				data->m_dismembermentData->Create();
			}
			data->m_dismembermentData->SetData( cbufferData );
		}
		else
		{
			Helper::Destroy( std::move( data->m_dismembermentData ) );
		}
	}
}

void CRenderProxy_Mesh::AttachToScene( CRenderSceneEx* scene )
{
	BaseClass::AttachToScene( scene );

	m_attachedFrameIndex = GpuApi::FrameIndex();
#ifdef __EMSCRIPTEN__
	CpMeshCollectProbe( CPMC_Attach, 2u, GetSkinningData() ? true : false, unsigned( m_renderMask ), unsigned( GetMaterialLodGroups().GetNumLods() ) );
#endif

	ConditionalAttachSpeedTreeWindSimulation( scene );

	ConditionalInvalidateLights( GetBoundingBox(), scene, *this );
}

void CRenderProxy_Mesh::ConditionalAttachSpeedTreeWindSimulation( CRenderSceneEx* scene )
{
	if ( m_mesh && m_mesh->GetWindData() )
	{
		RenderProxyCustomData_MiscMeshParams* miscMeshParams = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
		RED_FATAL_ASSERT( miscMeshParams );

		this->m_drawableFlags |= RPF_Dynamic; // forcing speedtree meshes to be dynamic ( to make local shadow maps update each frame )
		CSpeedTreeWindDataUpdater& windStorage = scene->GetCameraStorage()->GetCustomData<CSpeedTreeWindDataUpdater>();
		RED_ASSERT( miscMeshParams->m_speedTreeWindSimulation == nullptr, "data was already assigned." );

		miscMeshParams->m_speedTreeWindSimulation = windStorage.RegisterWindSimulation( m_mesh.Get() );
		RED_FATAL_ASSERT( miscMeshParams->m_speedTreeWindSimulation != nullptr, "Failed to register windData" );
	}
}

void CRenderProxy_Mesh::ConditionalDetachSpeedTreeWindSimulation( CRenderSceneEx* scene )
{
	if( m_mesh && m_mesh->GetWindData() )
	{
		CSpeedTreeWindDataUpdater& windStorage = scene->GetCameraStorage()->GetCustomData<CSpeedTreeWindDataUpdater>();

		RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>();
		RED_FATAL_ASSERT( miscMeshParams );
		RED_FATAL_ASSERT( miscMeshParams->m_speedTreeWindSimulation != nullptr, "Mesh didn't have a registered windData?" );

		windStorage.UnregisterWindSimulation( m_mesh.Get() );
		miscMeshParams->m_speedTreeWindSimulation = nullptr;
	}
	else
	{
		// Note: wrapping around ifdef, because GetCustomData has side effect
#ifdef RED_ASSERTS_ENABLED
		if( const RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
		{
			RED_FATAL_ASSERT( miscMeshParams->m_speedTreeWindSimulation == nullptr, "Expected no registree for windData" );
		}
#endif // RED_ASSERTS_ENABLED
	}
}

void CRenderProxy_Mesh::MarkTeleport()
{
	// clear velocity
	if ( m_motionData )
	{
		// setting to 1 because this value will be set to 2 before very first proxy collection
		// and it will prevent from rendering to velocity buffer (see ProxyMotionData::ShouldRenderMotionVectors).
		// if in the next frame proxy will be relinked then lifetime will be set to 0 -> update will increment it to 1
		// and proxy will be collected to velocity buffer.
		m_motionData->SetLifeTime( 1 );
	}

	// clear skinning velocity
	if ( const RenderProxyCustomData_Skinning* data = GetCustomData< RenderProxyCustomData_Skinning >() )
	{
		if ( data->m_skinningData )
		{
			data->m_skinningData->IgnorePreviousFrameData( true );
		}
	}
}

void CRenderProxy_Mesh::PrecachePSOs()
{
	if ( m_mesh == nullptr )
	{
		return;
	}

	RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>();
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>();

	const Bool hasComputeSkinningBuffers = skinningCustomData && skinningCustomData->m_gpuComputeSkinningBuffers;
	const Bool hasDismemberment = miscMeshParams && miscMeshParams->m_dismembermentData;

	for ( Int32 lodIndex = 0; lodIndex < GetNumLods(); ++lodIndex )
	{
		const CRenderMesh::MeshLodGroup& meshLodGroup = GetLodGroups()[ lodIndex ];

		for ( Uint32 chunkIndexInLOD = 0; chunkIndexInLOD < meshLodGroup.GetChunkCount(); ++chunkIndexInLOD )
		{
			RED_SCOPE_SHARED_LOCK( m_materialLodGroupsFlag );

			const SRenderMeshChunkInfo* chunk = meshLodGroup.GetChunk( chunkIndexInLOD );
			RED_ASSERT( chunk );
			const RenderMaterialInfo& renderMaterialInfo = m_materialLodGroups.GetMaterialEntryForChunk( lodIndex, chunkIndexInLOD );
			RED_ASSERT( renderMaterialInfo.m_materialPtr );

			GpuApi::VertexLayoutRef chunkLayoutRef = m_mesh->GetChunkVertexLayout( chunk->m_chunkIndex );

			const Bool vfSupportsDiscards = DoesMaterialVertexFactorySupportDiscards( chunk->m_vertexFactory );
			const Bool vfSupportsDismemberment = DoesMaterialVertexFactorySupportDismemberment( chunk->m_vertexFactory );
			const Bool isTangentUpdate = true; // TODO
			const Bool allowTangentUpdate = isTangentUpdate && ( lodIndex == 0 );

			const SRenderStageContext renderStageContextSolid = GetMeshChunkRenderStageContext(
				chunk->m_elementFlags, chunk->m_vertexFactory, *renderMaterialInfo.m_materialPtr, DISSOLVEMODE_Opaque,
				hasComputeSkinningBuffers, allowTangentUpdate, hasDismemberment );

			renderMaterialInfo.m_materialPtr->CreateDefaultPSO( renderStageContextSolid, chunkLayoutRef );

			// precache dissolved material only when the vertex factory supports it
			if ( vfSupportsDiscards )
			{
				SRenderStageContext renderStageContextDissolve = GetMeshChunkRenderStageContext(
					chunk->m_elementFlags, chunk->m_vertexFactory, *renderMaterialInfo.m_materialPtr, DISSOLVEMODE_FadeIn,
					hasComputeSkinningBuffers, allowTangentUpdate, hasDismemberment );
				renderMaterialInfo.m_materialPtr->CreateDefaultPSO( renderStageContextDissolve, chunkLayoutRef );
			}

			// precache dismemberment material only when the vertex factory supports it
			if ( vfSupportsDismemberment )
			{
				SRenderStageContext renderStageContextDismembermentSolid = GetMeshChunkRenderStageContext(
					chunk->m_elementFlags, chunk->m_vertexFactory, *renderMaterialInfo.m_materialPtr, DISSOLVEMODE_Opaque,
					hasComputeSkinningBuffers, allowTangentUpdate, true );
				renderMaterialInfo.m_materialPtr->CreateDefaultPSO( renderStageContextDismembermentSolid, chunkLayoutRef );

				// precache dismemberment material for dissolve
				if ( vfSupportsDiscards )
				{
					SRenderStageContext renderStageContextDismembermentDissolve = GetMeshChunkRenderStageContext(
						chunk->m_elementFlags, chunk->m_vertexFactory, *renderMaterialInfo.m_materialPtr, DISSOLVEMODE_FadeIn,
						hasComputeSkinningBuffers, allowTangentUpdate, true );
					renderMaterialInfo.m_materialPtr->CreateDefaultPSO( renderStageContextDismembermentDissolve, chunkLayoutRef );
				}
			}
		}
	}
}

void CRenderProxy_Mesh::DetachFromScene( CRenderSceneEx* scene, const red::DynArray<CRenderFrameCameraStorage::SCameraData*>& camData )
{
	m_motionData.Reset();

	ConditionalInvalidateLights( GetBoundingBox(), scene, *this );
	
	ConditionalDetachSpeedTreeWindSimulation( scene );

	if ( RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>() )
	{
		skinningCustomData->m_skinningData = nullptr;
	}

	SetDismembermentData( nullptr );

	// Pass to base class
	BaseClass::DetachFromScene( scene, camData );

#ifdef USE_RAY_TRACING
	RayTracingRelease();
#endif
}

void CRenderProxy_Mesh::UpdateMotionDataMatrix()
{
	if ( !m_scene )
	{
		return;
	}

	const Bool isNewbie = !m_motionData;
	if ( !m_motionData )
	{
		// can return null if there are way too much relink request in 1 frame
		m_motionData = m_scene->GetMotionRegistry().AcquireProxyMotionData( this );
	}

	if ( m_motionData )
	{
		m_motionData->SetInstanceMatrix( m_instanceMatrix );
		if ( isNewbie )
		{
			// setting to 1 because this value will be set to 2 before very first proxy collection
			// and it will prevent from rendering newbie to velocity buffer (see ProxyMotionData::ShouldRenderMotionVectors).
			// if in the next frame proxy will be relinked then lifetime will be set to 0 -> update will increment it to 1
			// and proxy will be collected to velocity buffer.
			m_motionData->SetLifeTime( 1 );
		}
	}
}

void CRenderProxy_Mesh::ReleaseMotionData()
{
	// it looks like memory leak but it's not :)
	m_motionData.ReleaseOwnership();
}

Bool CRenderProxy_Mesh::Relink( const Box& boundingBox, const rend::RenderProxyTransform& localToWorld )
{
	UpdateMotionDataMatrix();

	Bool boxChanged = GetBoundingBox() != boundingBox;

	if ( boxChanged )
	{
		// Use union of before and after bounding boxes in order to cover cases when 
		// we have significant fps drop and object disappeared from the shadowmap and didn't trigger it to re-draw.
		Box boxesUnion( GetBoundingBox() );
		boxesUnion.AddBox( boundingBox );

		ConditionalInvalidateLights( boxesUnion, m_scene, *this );
	}

	const Bool visGridMoveNeeded = BaseClass::Relink( boundingBox, localToWorld );
	return visGridMoveNeeded;
}

void CRenderProxy_Mesh::RelinkBBoxOnly( const Box& boundingBox )
{
	Bool boxChanged = GetBoundingBox() != boundingBox;

	BaseClass::RelinkBBoxOnly( boundingBox );

	if ( boxChanged )
	{
		// Use union of before and after bounding boxes in order to cover cases when 
		// we have significant fps drop and object disappeared from the shadowmap and didn't trigger it to re-draw.
		Box boxesUnion( GetBoundingBox() );
		boxesUnion.AddBox( boundingBox );

		ConditionalInvalidateLights( boxesUnion, m_scene, *this );
	}
}

void CRenderProxy_Mesh::RelinkTransformOnly( const rend::RenderProxyTransform& localToWorld )
{
	UpdateMotionDataMatrix();

	BaseClass::RelinkTransformOnly( localToWorld );
}

Bool CRenderProxy_Mesh::RelinkInstances( const Box& boundingBox, red::ArraySpan<const rend::RenderProxyTransform> instances, Bool forceDynamicInstanceBuffer )
{
	rend::RenderProxyTransform localToWorld = rend::RenderProxyTransform::IDENTITY();
	localToWorld.SetPosition( boundingBox.CalcCenter() );

	const Uint32 oldInstancesSize = m_numInstances;
	m_numInstances = instances.Size();
	const Bool isClusteredProxy = IsClusteredProxyWithMultipleInstances();
	Bool useStaticInstanceBuffer = ( m_numInstances > c_staticInstanceBufferTreshold ) && !CheckMeshProxyFlag( RMPF_HasAnySkinnedChunk ) && !forceDynamicInstanceBuffer;

	CRenderGeometryBatcher* geometryBatcher = GetRenderer()->GetGeometryBatcher();
	//TODO - figure out the spikes of static instance buffer allocation right after teleporting to some location in night city
	//These spikes might result in falling back to dynamic instance buffer for Clustered Proxies which is not good. Might be only in editor?
	if ( m_offsetInStaticInstanceBuffer != UINT32_MAX && ( !useStaticInstanceBuffer || oldInstancesSize != m_numInstances ) )
	{
		geometryBatcher->FreeStaticInstance( m_offsetInStaticInstanceBuffer );
		m_offsetInStaticInstanceBuffer = UINT32_MAX;
	}
		
	if ( useStaticInstanceBuffer && m_offsetInStaticInstanceBuffer == UINT32_MAX )
	{
		m_offsetInStaticInstanceBuffer = geometryBatcher->AllocateStaticInstanceBuffer( instances );
		if ( m_offsetInStaticInstanceBuffer == UINT32_MAX )
		{
			// static instance buffer allocation failed, fallback to dynamic buffer
			useStaticInstanceBuffer = false;
		}
	}

	// clear the old instances buffer and allocate a new one
	const Bool firstTimeClusteredProxyRelink = m_instancesData == nullptr;
	const Bool useInstanceData = !useStaticInstanceBuffer || isClusteredProxy;
	if ( m_numInstances != oldInstancesSize )
	{
		if ( m_instancesData )
		{
			RED_DELETE_ARRAY( m_instancesData, oldInstancesSize, rend::PoolRenderProxy_InstanceData );
			m_instancesData = nullptr;
		}
	}

	if ( m_numInstances > 0 && useInstanceData )
	{
		if ( !m_instancesData )
		{
			m_instancesData = RED_NEW_ARRAY( SRenderProxyInstance, m_numInstances, rend::PoolRenderProxy_InstanceData );
		}

		FillInstances( m_instancesData, instances );
	}

#ifdef USE_RAY_TRACING
	if ( GpuApi::GetCapabilities().rayTracingSupport )
	{
		if ( m_numInstances != oldInstancesSize )
		{
			if ( m_instancesDataCopy )
			{
				RED_DELETE_ARRAY( m_instancesDataCopy, oldInstancesSize, rend::PoolRenderProxy_InstanceData );
				m_instancesDataCopy = nullptr;
			}
		}

		if ( m_numInstances > 0 && !useInstanceData )
		{
			if ( !m_instancesDataCopy )
			{
				m_instancesDataCopy = RED_NEW_ARRAY( SRenderProxyInstance, m_numInstances, rend::PoolRenderProxy_InstanceData );
			}

			FillInstances( m_instancesDataCopy, instances );
		}
	}
#endif

	ConditionalInvalidateClusteredProxyData( firstTimeClusteredProxyRelink );

 	return Relink( boundingBox, localToWorld );
}

void CRenderProxy_Mesh::ScheduleDisabledClusteredProxyInstances( const red::BitSet64Dynamic& clusteredProxyInstances )
{
	RED_ASSERT( CheckMeshProxyFlag( RMPF_IsClusteredProxy ), "ScheduleDisabledClusteredProxyInstances should be invoked only for clustered proxies." );

	RED_SCOPE_LOCK( m_clusteredProxyDisableInstancesLock ); //TODO - find a better way to make sure no data gets changed while ConditionalDisableClusteredProxyInstances gets executed

	//proxy might not be attached to scene yet so let's schedule the update for the next UpdateOncePerFrame
	RenderProxyCustomData_ClusteredProxyDisabledInstances* data = AcquireCustomData<RenderProxyCustomData_ClusteredProxyDisabledInstances>();
	data->m_clusteredProxyDisabledInstances = std::move( clusteredProxyInstances );
	data->m_disabledInstancesCount = data->m_clusteredProxyDisabledInstances.PopulationCount();//cache the number of set bits
	data->m_updateNeeded = true;
}

void CRenderProxy_Mesh::ConditionalDisableClusteredProxyInstances()
{
	RED_SCOPE_LOCK( m_clusteredProxyDisableInstancesLock );

	RenderProxyCustomData_ClusteredProxyDisabledInstances* data = GetCustomData<RenderProxyCustomData_ClusteredProxyDisabledInstances>();

	// TODO - apart from fixing the buckets in camera data we could also shrink the instance buffer allocated on GPU but it would be best to do so based on already fractured instances
	// which cannot revert back to the original instance unless node gets recreated.
	if ( data && data->m_updateNeeded )
	{
		red::DynArray < CRenderFrameCameraStorage::SCameraData* > allCameraData{ job::PoolJobScope() };
		m_scene->GetCameraStorage()->GetAllCameraData( allCameraData );
		for ( CRenderFrameCameraStorage::SCameraData* camData : allCameraData )
		{
			CameraCustomData_RenderProxyData& rpData = camData->GetCustomData< CameraCustomData_RenderProxyData >();
			rpData.DisableClusteredProxyInstances( GetDissolveSync(), GetIdInScene(), m_numInstances, data->m_clusteredProxyDisabledInstances );
		}

		data->m_updateNeeded = false;
	}
}

Int32 CRenderProxy_Mesh::GetLODForDynamicDecal() const
{
	if ( m_hasForcedLOD && m_forcedLODLevel >= 0 && m_forcedLODLevel < GetNumLods() )
	{
		return (Int32)m_forcedLODLevel;
	}
	const CRenderMesh::MeshLodGroups& lodGroups = GetLodGroups();
	for ( Uint32 i = 0; i < lodGroups.Size(); ++i )
	{
		if ( lodGroups[ i ].GetChunkCount() > 0 )
		{
			return (Int32)i;
		}
	}
	return -1;
}

void CRenderProxy_Mesh::CreateDynamicDecalChunks( const SRenderNodeImplContext& rctx, const TRenderPtr< CRenderDynamicDecal >& decal )
{
	if ( CheckMeshProxyFlag( RMPF_SkipDynamicDecals ) )
	{
		// Skip decal creation on this mesh
		return;
	}

	Int32 lodToUse = GetLODForDynamicDecal();

	// Can't create a decal on an empty mesh!
	if ( lodToUse < 0 || lodToUse >= std::numeric_limits< Uint8 >::max() )
	{
		RED_LOG_WARNING( "Renderer: No non-empty LOD found. Can't create a decal mesh" );
		return;
	}

	TRenderPtr< CRenderSkinningData > skinningData;
	if ( const RenderProxyCustomData_Skinning* data = GetCustomData< RenderProxyCustomData_Skinning >() )
	{
		skinningData = data->m_skinningData;
	}

	DynArray< Uint16 > chunkIndexInLOD { rend::PoolDecals() };

	// Now create a decal element for each chunk in the resulting decal mesh.
	TRenderPtr< CRenderDecalMesh > decalMesh = CRenderDecalMesh::Create( rctx, ToDynamicDecalTarget(), decal, m_meshChunkMask, m_mesh, (Uint8)lodToUse, GetLocalToWorld(), skinningData, chunkIndexInLOD );
	if ( !decalMesh )
	{
		return;
	}
	
	// Now create a decal element for each chunk in the resulting decal mesh.
	const Uint32 numDecalChunks = decalMesh->GetNumChunks();
	{
		RED_SCOPE_LOCK( m_dynamicDecalChunksLock );
		m_dynamicDecalChunks.Reserve( m_dynamicDecalChunks.Size() + numDecalChunks );
	}

	RED_ASSERT( numDecalChunks == chunkIndexInLOD.Size() );

	for ( Uint32 i = 0; i < numDecalChunks; ++i )
	{
		TRenderPtr< CRenderProxy_Mesh > thisMesh = TRenderPtr< CRenderProxy_Mesh >::MakePtr( this );
		TRenderPtr< CRenderDynamicDecalChunk_Mesh > newChunk( RED_NEW( CRenderDynamicDecalChunk_Mesh )( decal, thisMesh, decalMesh, chunkIndexInLOD[ i ], i ) );

		RED_SCOPE_LOCK( m_dynamicDecalChunksLock );
		m_dynamicDecalChunks.PushBack( newChunk );
	}
}

static const CRenderMesh::MeshLodGroups s_dummyLodGroups;

const CRenderMesh::MeshLodGroups& CRenderProxy_Mesh::GetLodGroups() const 
{ 
	return m_mesh ? m_mesh->GetLodGroups() : s_dummyLodGroups;
}

const CRenderLODSetup& CRenderProxy_Mesh::GetLodSetup() const
{
	return m_lodSetup;
}

Int32 CRenderProxy_Mesh::GetNumLods() const 
{ 
	RED_FATAL_ASSERT( m_mesh, "RenderProxyMesh - proxy without RenderMesh. Make sure it was initialized correctly." );
	return m_mesh ? (Int32)m_mesh->GetNumLODs() : 0; 
}

void CRenderProxy_Mesh::SetDissolveVisible( Bool isVisible, Uint8 force )
{
	if ( isVisible )
	{
		const FadeInForcingMode forcingMode = static_cast< FadeInForcingMode > ( force );
		if ( forcingMode == FadeInForcingMode::ForceFade )
		{
			// force dissolve fade in, even if it's the first frame we render the mesh
			SetMeshProxyFlag( RMPF_ForceDissolveFadeIn, true );
		}
		else if ( forcingMode == FadeInForcingMode::ForceInstant )
		{
			// force instant fade in, even if it's already on screen
			SetMeshProxyFlag( RMPF_ForceInstantFadeIn, true );
		}
		// else FadeInForcingMode::None, so do nothing
	}

	m_isDissolveVisible = isVisible;
}

void CRenderProxy_Mesh::SetForcedLOD( Int8 forcedLOD )
{
	RED_FATAL_ASSERT( (Int32)forcedLOD < (Int32)CRenderLODSetup::MAX_LODS, "Invalid forced LOD: %d", (Int32)forcedLOD );

	if ( forcedLOD >= 0 )
	{
		m_hasForcedLOD = 1;
		m_forcedLODLevel = forcedLOD;
	}
	else
	{
		m_hasForcedLOD = 0;
	}
}

void CRenderProxy_Mesh::SetLODLevelScales( Uint32 LODLevelScales )
{
	if( m_mesh != nullptr )
	{
		CreateLodGroups( LODLevelScales );
	}
}

const CSpeedTreeWindSimulation* CRenderProxy_Mesh::GetSpeedTreeWindSharedData() const
{
	if ( const RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
	{
		return miscMeshParams->m_speedTreeWindSimulation.Get();
	}
	return nullptr;
}

static SurfaceInfo GetSurfaceForChunk( const TRenderPtr<CRenderMesh>& renderMesh, const TRenderPtr<IRenderResource>& materialSetup, Uint16 chunkIndex )
{
	SurfaceInfo surface;
	surface.id = SurfaceID::Null;
	if ( chunkIndex < renderMesh->GetNumChunks() )
	{
		const CRenderMaterialSetup* mainRenderMaterialSetup = static_cast<const CRenderMaterialSetup*>( materialSetup.Get() );
		const CName materialName = mainRenderMaterialSetup->GetChunkMaterialName( chunkIndex );

		/// @todo DP: why this happens? render blog for cylinder.w2mesh seems to have more chuinks than the mesh. Commenting out to unblock, I need Lukasz Kadej for proper fix...

		//RED_FATAL_ASSERT( mainRenderMaterialSetup->GetNumChunks() >= renderMesh->GetChunks().Size(),
		//                  "Number of chunks in the render material setup (%u) has to be greater or equal to the number of chunks in the render mesh (%u)",
		//                  mainRenderMaterialSetup->GetNumChunks(), renderMesh->GetChunks().Size() );

		res::ResourcePath meshPath;
#ifndef RED_CONFIGURATION_FINAL
		meshPath = renderMesh->GetMeshResourcePath();
#endif

		SurfaceCache& surfaceCache = GetRenderer()->GetSurfaceCache();
		surface = surfaceCache.CreateSurface( materialName, mainRenderMaterialSetup->GetMaterialSetupEntry( materialName ), meshPath );

#ifndef RED_CONFIGURATION_FINAL
		if ( surface.materialInfo.m_materialPtr != nullptr )
		{
			EMaterialVertexFactory mvf = EMaterialVertexFactory( renderMesh->GetChunks()[chunkIndex].m_vertexFactory );
			if ( !surface.materialInfo.m_materialPtr->SupportsVertexFactory( mvf ) )
			{
				const char* mvfName = "<unknown mvf>";
				RED_ASSERT(::GetTypeObject<EMaterialVertexFactory>(), "Failed to find EMaterialVertexFactory rtti type");
				RED_ASSERT(::GetTypeObject<EMaterialVertexFactory>()->GetType() == RT_Enum, "EMaterialVertexFactory is not rtti enum type");
				auto* mvfType = static_cast<const rtti::EnumType*>(::GetTypeObject<EMaterialVertexFactory>());
				CName name;
				if ( mvfType->FindName( static_cast<Int64>(mvf), name ) )
				{
					mvfName = name.AsChar();
				}

				RED_DATA_ERROR_BEGIN( red::LoggerCategory_Rendering, surface.materialInfo.m_materialPtr->SupportsVertexFactory( mvf ),
					"Material '%s' uses vertex factory '%s' which isn't supported by the material template '%.*s'",
					materialName.AsChar(), mvfName, surface.materialInfo.m_materialPtr->GetDisplayableName().Length(), surface.materialInfo.m_materialPtr->GetDisplayableName().Data() );
				RED_DATA_ERROR_ACTION_SHOW_ASSET( meshPath.ToDebugString() );
				RED_DATA_ERROR_END();
			}
		}
		else
		{
			RED_LOG_ERROR( "Mesh '%hs' does not have valid material '%hs' on chunk %d", renderMesh->GetMeshResourcePath().ToDebugString(), mainRenderMaterialSetup->GetChunkMaterialName( chunkIndex ).AsChar(), chunkIndex );
		}
#endif
	}
	return surface;
}

MaterialLodGroups::MaterialLodGroups()
	: m_chunks( nullptr )
	, m_numLods( 0 )
	, m_totalNumChunks( 0 )
{ }

MaterialLodGroups::~MaterialLodGroups()
{
	Clear();
}

void MaterialLodGroups::Apply( const TRenderPtr<CRenderMesh>& renderMesh, const TRenderPtr<IRenderResource>& materialSetup )
{
	RED_FATAL_ASSERT( renderMesh != nullptr, "CRenderMesh must be already set for this proxy in order to apply material setup." );

	Clear();

	Uint32 numLODs = renderMesh->GetNumLODs();
	m_numLods = numLODs;

	m_totalNumChunks = 0;
	for ( Uint32 lodIdx = 0; lodIdx < numLODs; ++lodIdx )
	{
		const CRenderMesh::MeshLodGroup& lodGroup = renderMesh->GetLodGroups()[ lodIdx ];
		m_totalNumChunks += lodGroup.GetChunkCount();
	}

	if ( m_totalNumChunks > 0 )
	{
		// Keep one allocation and no object rewrites
		m_chunks = RED_NEW_ARRAY( Chunk, m_totalNumChunks, rend::PoolRenderProxyInternals_Mesh );

		Uint32 chunkOffset = 0;
		for ( Uint32 lodIdx = 0; lodIdx < numLODs; ++lodIdx )
		{
			// Create rendering elements for each chunk defined at given LOD
			const CRenderMesh::MeshLodGroup& lodGroup = renderMesh->GetLodGroups()[ lodIdx ];
			const Uint32 numChunks = lodGroup.GetChunkCount();

			m_lods[ lodIdx ].firstChunk = chunkOffset;
			m_lods[ lodIdx ].numChunks = numChunks;

			for ( Uint32 i = 0; i < numChunks; ++i )
			{
				const SRenderMeshChunkInfo* meshChunkInfo = lodGroup.GetChunk( i );
				const Uint16 chunkIndex = meshChunkInfo->m_chunkIndex;

				SurfaceInfo surfaceInfo = GetSurfaceForChunk( renderMesh, materialSetup, chunkIndex );

				Chunk& chunk = m_chunks[ chunkOffset ];
				chunk.materialInfo = std::move( surfaceInfo.materialInfo );
				chunk.id = surfaceInfo.id;
				chunk.elementFlags = meshChunkInfo->m_elementFlags;
				chunk.chunkIndex = meshChunkInfo->m_chunkIndex;
				chunk.numTriangles = meshChunkInfo->m_numTriangles;
				chunk.renderMask = meshChunkInfo->m_renderMask;
				chunk.vertexFactory = meshChunkInfo->m_vertexFactory;

				if ( chunk.materialInfo.m_materialPtr == nullptr || chunk.materialInfo.m_materialParameters == nullptr )
				{
#ifndef RED_CONFIGURATION_FINAL
					if ( chunkIndex < renderMesh->GetNumChunks() )
					{
						const CRenderMaterialSetup* mainRenderMaterialSetup = static_cast<const CRenderMaterialSetup*>( materialSetup.Get() );
						const CName materialName = mainRenderMaterialSetup->GetChunkMaterialName( chunkIndex );

						RED_DATA_ERROR_BEGIN( red::LoggerCategory_Rendering, false, "Failed to get render material for '%s'. Check if mesh has valid material assignments.", materialName.AsChar() );
						if ( renderMesh->GetMeshResourcePath().IsValid() )
						{
							RED_DATA_ERROR_ACTION_SHOW_ASSET( renderMesh->GetMeshResourcePath().ToDebugString() );
						}
						RED_DATA_ERROR_END();
					}
#endif
#ifdef __EMSCRIPTEN__
					CpFallbackMaterialProbe( "lod-groups", renderMesh.Get(), unsigned( chunkIndex ), lodIdx, unsigned( meshChunkInfo->m_numTriangles ) );
#endif
					// Move Fallback
					auto renderMaterial = GetRenderer()->GetEngineHandler()->GetFallbackMaterial();

					chunk.materialInfo.m_materialParameters = red::StaticCast<CRenderMaterialParameters>( renderMaterial );
					chunk.materialInfo.m_materialPtr = red::StaticCast<CRenderMaterial>( renderMaterial );
				}

				chunkOffset++;
			}
		}

		RED_FATAL_ASSERT( chunkOffset == m_totalNumChunks );
	}
}

void MaterialLodGroups::ApplyMaterialParameterOverride( const TRenderPtr<CRenderMesh>& renderMesh, const MaterialLodGroups& originalGroups, const SMaterialSetupParameters& params )
{
	RED_ASSERT( originalGroups.GetNumLods() == GetNumLods(), "originalGroups size (%u) should match materialLodGroups (%u)", originalGroups.GetNumLods(), GetNumLods() );
	RED_ASSERT( originalGroups.m_totalNumChunks == m_totalNumChunks, "originalGroup chunks num (%u) should match activeGroup (%u)", originalGroups.m_totalNumChunks, m_totalNumChunks );

	SurfaceCache& surfaceCache = GetRenderer()->GetSurfaceCache();

	for ( Uint32 i = 0; i < originalGroups.m_totalNumChunks; ++i )
	{
		SurfaceID baseID = originalGroups.m_chunks[i].id;
		SurfaceInfo surfInfo = surfaceCache.CreateSurface( baseID, params );
		if ( surfInfo.materialInfo.m_materialPtr == nullptr || surfInfo.materialInfo.m_materialParameters == nullptr )
		{
#ifndef RED_CONFIGURATION_FINAL
			RED_ASSERT( renderMesh != nullptr, "m_mesh was null in CRenderProxy_Mesh::SetMaterialParameterOverrides" );
			RED_LOG_ERROR( "Renderer: SurfaceCache returned null material for mesh: %hs, chunk: %d. Fallbacking.", renderMesh->GetMeshResourcePath().ToDebugString(), i );
#endif
#ifdef __EMSCRIPTEN__
			CpFallbackMaterialProbe( "param-override", renderMesh.Get(), i, 0u, 0u );
#endif
			// Move Fallback
			auto renderMaterial = GetRenderer()->GetEngineHandler()->GetFallbackMaterial();

			surfInfo.materialInfo.m_materialParameters = red::StaticCast< CRenderMaterialParameters >( renderMaterial );
			surfInfo.materialInfo.m_materialPtr = red::StaticCast< CRenderMaterial >( renderMaterial );
		}

		{
			RED_SCOPE_LOCK( m_chunkMaterialInfoAccessLock );
			Chunk& chunk = m_chunks[ i ];
			chunk.id = surfInfo.id;
			chunk.materialInfo = std::move( surfInfo.materialInfo );
		}
	}
}

MaterialLodGroups& MaterialLodGroups::operator = ( const MaterialLodGroups& other )
{
	Clear();

	m_numLods = other.m_numLods;
	m_totalNumChunks = other.m_totalNumChunks;
	m_lods = other.m_lods;

	// copy chunks
	if ( other.m_chunks )
	{
		m_chunks = RED_NEW_ARRAY( Chunk, m_totalNumChunks, rend::PoolRenderProxyInternals_Mesh );

		for ( Uint32 i = 0; i < m_totalNumChunks; ++i )
		{
			m_chunks[ i ] = other.m_chunks[ i ];
		}
	}

	return *this;
}

void MaterialLodGroups::Clear()
{
	if ( m_chunks )
	{
		RED_FATAL_ASSERT( m_totalNumChunks > 0 );
		RED_DELETE_ARRAY( m_chunks, m_totalNumChunks, rend::PoolRenderProxyInternals_Mesh );
		m_chunks = nullptr;
	}

	m_totalNumChunks = 0;
	m_numLods = 0;

	for ( LodGroup& lodGroup : m_lods )
	{
		lodGroup.firstChunk = 0;
		lodGroup.numChunks = 0;
	}
}

void CRenderProxy_Mesh::ApplyMaterialSetup( const TRenderPtr<IRenderResource>& materialSetup )
{
	RED_SCOPE_LOCK( m_materialLodGroupsFlag );

	RED_FATAL_ASSERT( m_mesh != nullptr, "CRenderMesh must be already set for this proxy in order to apply material setup." );

	m_materialLodGroups.Apply( m_mesh, materialSetup );
}

void CRenderProxy_Mesh::SetMeshChunkMask( Uint64 mask )
{
	if (m_meshChunkMask != mask)
	{
		m_meshChunkMask = mask;

#ifdef USE_RAY_TRACING
		RayTracingRelease();
#endif // USE_RAY_TRACING
	}
}

void CRenderProxy_Mesh::SetIndexBufferOverride( const TRenderPtr<IRenderResource>& ibOverride )
{
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( miscMeshParams );

	miscMeshParams->m_indexBufferOverride = red::StaticCast<CRenderIndexBuffer>( ibOverride );

#ifdef USE_RAY_TRACING
	RayTracingRelease();
#endif // USE_RAY_TRACING
}

void CRenderProxy_Mesh::SetMaterialOverlay( const TRenderPtr< IRenderResource >& material, const TRenderPtr< IRenderResource >& materialParam, bool overrideMaterial )
{
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( miscMeshParams );

	miscMeshParams->m_materialOverlayIsOverride = overrideMaterial;

	RenderMaterialInfo& materialOverlay = miscMeshParams->m_materialOverlay;
	materialOverlay.m_materialPtr = red::StaticCast<CRenderMaterial>( material );
	if ( materialParam )
	{
		materialOverlay.m_materialParameters = red::StaticCast<CRenderMaterialParameters>( materialParam );
	}
	else
	{
		materialOverlay.m_materialParameters = materialOverlay.m_materialPtr;
	}

	// Precache PSOs for this new material.
	if ( materialOverlay.m_materialPtr )
	{
		if ( m_mesh != nullptr )
		{
			RenderProxyCustomData_Skinning* skinningCustomData = GetCustomData<RenderProxyCustomData_Skinning>();
			RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>();

			const Bool hasComputeSkinningBuffers = skinningCustomData && skinningCustomData->m_gpuComputeSkinningBuffers;
			const Bool hasDismemberment = miscMeshParams && miscMeshParams->m_dismembermentData;

			for ( Int32 lodIndex = 0; lodIndex < GetNumLods(); ++lodIndex )
			{
				const CRenderMesh::MeshLodGroup& meshLodGroup = GetLodGroups()[ lodIndex ];

				for ( Uint32 chunkIndexInLOD = 0; chunkIndexInLOD < meshLodGroup.GetChunkCount(); ++chunkIndexInLOD )
				{
					const SRenderMeshChunkInfo* chunk = meshLodGroup.GetChunk( chunkIndexInLOD );
					RED_ASSERT( chunk );

					GpuApi::VertexLayoutRef chunkLayoutRef = m_mesh->GetChunkVertexLayout( chunk->m_chunkIndex );

					const Bool isTangentUpdate = true; // TODO

					const SRenderStageContext renderStageContextSolid = GetMeshChunkRenderStageContext(
						chunk->m_elementFlags, chunk->m_vertexFactory, *materialOverlay.m_materialPtr, DISSOLVEMODE_Opaque,
						hasComputeSkinningBuffers, isTangentUpdate && (lodIndex == 0), hasDismemberment );
					materialOverlay.m_materialPtr->CreateDefaultPSO( renderStageContextSolid, chunkLayoutRef );

					// precache dissolved material only when the vertex factory supports it
					if ( DoesMaterialVertexFactorySupportDiscards( chunk->m_vertexFactory ) )
					{
						SRenderStageContext renderStageContextDissolve = GetMeshChunkRenderStageContext(
							chunk->m_elementFlags, chunk->m_vertexFactory, *materialOverlay.m_materialPtr, DISSOLVEMODE_FadeIn,
							hasComputeSkinningBuffers, isTangentUpdate && (lodIndex == 0), hasDismemberment );
						materialOverlay.m_materialPtr->CreateDefaultPSO( renderStageContextDissolve, chunkLayoutRef );
					}
				}
			}
		}
	}
}

void CRenderProxy_Mesh::UpdateMaterialParameterEffectContext( const SMaterialParameterEffectContextUpdate& context )
{
	RenderProxyCustomData_MaterialParams* data = AcquireCustomData<RenderProxyCustomData_MaterialParams>();
	RED_FATAL_ASSERT( data );

	if( context.m_paramMask & ECMP_CustomParam0 )
		data->m_params[0] = context.m_parameters[0];
	if( context.m_paramMask & ECMP_CustomParam1 )
		data->m_params[1] = context.m_parameters[1];
	if( context.m_paramMask & ECMP_CustomParam2 )
		data->m_params[2] = context.m_parameters[2];
	if( context.m_paramMask & ECMP_CustomParam3 )
		data->m_params[3] = context.m_parameters[3];
	if (context.m_paramMask & ECMP_CustomParam4)
		data->m_params[4] = context.m_parameters[4];
	if (context.m_paramMask & ECMP_CustomParam5)
		data->m_params[5] = context.m_parameters[5];
	if (context.m_paramMask & ECMP_CustomParam6)
		data->m_params[6] = context.m_parameters[6];
}

Vector4 CRenderProxy_Mesh::GetCustomMaterialParameter( Uint32 index ) const
{
	RED_FATAL_ASSERT( index < RenderProxyCustomData_MaterialParams::NumParams, "Invalid material parameter index" );

	if ( const RenderProxyCustomData_MaterialParams* data = GetCustomData<RenderProxyCustomData_MaterialParams>() )
	{
		return data->m_params[ index ];
	}

	return Vector4::ZEROS();
}

void CRenderProxy_Mesh::UpdateDestructionRegionsEffectContext( red::DynArray<Float>&& context)
{
	if ( const RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
	{
		if ( !miscMeshParams->m_destructionRegions.isNull() )
		{
			GpuApi::AddRef( miscMeshParams->m_destructionRegions );
			GetRenderer()->UploadDestructionRegionsBuffer( { miscMeshParams->m_destructionRegions, std::move( context ) } );
		}
	}
}

void CRenderProxy_Mesh::UpdateClothMeshData(Uint8 chunkIndex, red::ArraySpan<Vector3> positions, Box bounds)
{
    const auto& concrete_type = red::StaticCast<RenderMeshCloth>(m_mesh);
    concrete_type->UpdateAndMarkDirty(chunkIndex, positions, bounds);
}

GpuApi::BufferRef CRenderProxy_Mesh::GetDestructionRegionsDataBuffer() const
{
	if ( RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
	{
		return miscMeshParams->m_destructionRegions;
	}
	return GpuApi::BufferRef();
}

const RenderMeshProxyVehicleParams CRenderProxy_Mesh::GetVehicleParams() const
{
	if ( RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
	{
		return miscMeshParams->m_vehicleParams;
	}
	return RenderMeshProxyVehicleParams();
}

void CRenderProxy_Mesh::SetVehicleParams( const SVehicleParamsUpdate context )
{
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( miscMeshParams );

	miscMeshParams->m_vehicleParams = {
		context.m_meshPivotInGridSpace,
		context.m_dimensions
	};
}

Bool CRenderProxy_Mesh::HasDismembermentData() const
{
	if ( RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>() )
	{
		return miscMeshParams->m_dismembermentData;
	}
	return false;
}

const RenderDismembermentData& CRenderProxy_Mesh::GetDismembermentData() const
{
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( miscMeshParams );
	return *( miscMeshParams->m_dismembermentData );
}

Bool CRenderProxy_Mesh::HasRainData() const
{
	if (RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>())
	{
		return miscMeshParams->m_rainData;
	}
	return false;
}

const RenderRainData& CRenderProxy_Mesh::GetRainData() const
{
	RenderProxyCustomData_MiscMeshParams* miscMeshParams = GetCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( miscMeshParams );
	return *( miscMeshParams->m_rainData );
}

void CRenderProxy_Mesh::SetRainData( const RenderRainData& rainData )
{

	RenderProxyCustomData_MiscMeshParams* data = AcquireCustomData<RenderProxyCustomData_MiscMeshParams>();
	RED_FATAL_ASSERT( data );

	if (!data->m_rainData)
	{
		data->m_rainData = red::CreateUniquePtr< RenderRainData, rend::PoolRenderProxy_CustomData >();
	}

	data->m_rainData->m_rainIntensity = rainData.m_rainIntensity;
	data->m_rainData->m_rainMoisture = rainData.m_rainMoisture;
}

Bool CRenderProxy_Mesh::CanCastGlobalShadows() const
{
	return ( m_shadowFlags.m_mask & ShadowFlags::GLOBAL_SHADOWS_MASK() ) != 0;
}

Bool CRenderProxy_Mesh::CanCastLocalShadows() const 
{ 
	return m_shadowFlags.m_localShadows; 
}


void CRenderProxy_Mesh::SetCastLocalShadows( Bool canCastLocalShadows )
{
	IRenderProxyDrawable::SetCastLocalShadows( canCastLocalShadows );
	m_shadowFlags.m_localShadows = canCastLocalShadows;
}

void CRenderProxy_Mesh::SetCastLocalShadowsEvenIfNotVisible( Bool canCastShadowsEvenIfNotVisible )
{
	SetMeshProxyFlag( RMPF_CastingLocalShadowsEvenIfNotVisible, canCastShadowsEvenIfNotVisible );
}

void CRenderProxy_Mesh::SetMaterialParameterOverrides( const SMaterialSetupParameters& params )
{
	RED_SCOPE_LOCK( m_materialLodGroupsFlag );

	RenderProxyCustomData_OriginalLodGroups* originalLodGroupsData = AcquireCustomData<RenderProxyCustomData_OriginalLodGroups>();
	RED_FATAL_ASSERT( originalLodGroupsData );

	MaterialLodGroups& originalMaterialLodGroups = originalLodGroupsData->m_lodGroups;

	if ( originalLodGroupsData->m_lodGroups.Empty() )
	{
		originalMaterialLodGroups = m_materialLodGroups;
	}

	m_materialLodGroups.ApplyMaterialParameterOverride( m_mesh, originalMaterialLodGroups, params );
}

void CRenderProxy_Mesh::ClearMaterialParameterOverrides()
{
	RED_SCOPE_LOCK( m_materialLodGroupsFlag );

	if ( RenderProxyCustomData_OriginalLodGroups* originalLodGroupsData = GetCustomData<RenderProxyCustomData_OriginalLodGroups>() )
	{
		MaterialLodGroups& originalMaterialLodGroups = originalLodGroupsData->m_lodGroups;
		if ( !originalMaterialLodGroups.Empty() )
		{
			RED_ASSERT( originalMaterialLodGroups.GetNumLods() == m_materialLodGroups.GetNumLods(), "originalMaterialLodGroups size (%u) should match materialLodGroups (%u)", originalMaterialLodGroups.GetNumLods(), m_materialLodGroups.GetNumLods() );
			m_materialLodGroups = originalMaterialLodGroups;
			originalMaterialLodGroups.Clear();
		}
	}
}

Bool CRenderProxy_Mesh::IsVisibleForCurrentTOD( Float timeOfDay ) const
{
	// NOTE: The mask marks invisibility, so bit NOT set means 'visible'.
	const Uint16 todShift = TimeOfDayVisibilityResource::HourToBit( timeOfDay );
	return ( ( m_timeOfDayVisibilityMask & ( 1u << todShift ) ) == 0 );
}
