/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "visibilityQuerySingleFrustum.h"
#include "visibilityScene.h"
#include "visibilityResults.h"
#include "visibilityDebugDrawBuffer.h"
#include "../../redJobs2/include/jobRunner.h"
#include "../../redSystem/include/stopWatch.h"
#include "../../redJobs2/include/jobBuilder.h"

// [cull r5] web-port overlay: engine software occlusion stays exactly as shipped; this unit only adds
//   ?occl=0      skip building the main-camera occlusion buffer (A/B switch; the engine default is ON),
//   ?occlprobe=1 per-2-second 'CP2077 occl probe' lines: per main-view frame considered / frustumCulled / occluded /
//                visible targets (plus BVH-node rejects), and the sun-shadow cascade query counters from
//                visibilityQueryMultiFrustum.cpp (overall-frustum / global-occlusion / cascade-frustum / receiver
//                occlusion rejects, per-cascade passes, rasterizer availability).
// Switches are read once by CpCull::Get() (renderProxyMesh.cpp); until then (-1) behaviour is native.
#include <atomic>
#include <cstdio>
#include <cstring> // [skperf] round 48
#include <emscripten/emscripten.h>

extern std::atomic<int> g_cp2077OcclMode;
extern std::atomic<int> g_cp2077OcclProbe;

namespace CpOccl
{
	enum Counter : unsigned
	{
		// main view (queries that requested occlusion)
		M_Queries, M_BufValid, M_BufEmpty, M_Skipped, M_Considered, M_FrustumOut, M_Occluded, M_Visible, M_NodeOccluded,
		// other single-frustum queries (no occlusion requested: mirrors, offscreen, debug)
		O_Queries, O_Considered, O_Visible,
		// sun-shadow cascades (QueryMultiFrustum)
		S_Queries, S_NoRast, S_NoGlobal, S_Considered, S_OverallOut, S_GlobalOccl, S_CascOut, S_RecvOccl, S_Visible, S_CascPasses,
		S_Casc0, S_Casc1, S_Casc2, S_Casc3,
		C_Count
	};
	std::atomic<unsigned long long> s_counts[ C_Count ];
	std::atomic<double> s_lastPrint{ 0.0 };

	bool ProbeOn() { return g_cp2077OcclProbe.load( std::memory_order_relaxed ) > 0; }
	bool OcclusionOff() { return g_cp2077OcclMode.load( std::memory_order_relaxed ) == 0; }
	void Add( unsigned c, unsigned long long n ) { if ( n ) s_counts[ c ].fetch_add( n, std::memory_order_relaxed ); }

	void MaybePrint()
	{
		const double now = emscripten_get_now();
		double last = s_lastPrint.load( std::memory_order_relaxed );
		if ( last == 0.0 )
		{
			s_lastPrint.compare_exchange_strong( last, now );
			return;
		}
		if ( now - last < 2000.0 || !s_lastPrint.compare_exchange_strong( last, now ) )
		{
			return;
		}
		unsigned long long v[ C_Count ];
		for ( unsigned i = 0; i < C_Count; ++i ) v[ i ] = s_counts[ i ].exchange( 0, std::memory_order_relaxed );
		const double mf = v[ M_Queries ] ? double( v[ M_Queries ] ) : 1.0;
		const double sf = v[ S_Queries ] ? double( v[ S_Queries ] ) : 1.0;
		const double of = v[ O_Queries ] ? double( v[ O_Queries ] ) : 1.0;
		const double mc = v[ M_Considered ] ? double( v[ M_Considered ] ) : 1.0;
		std::fprintf( stderr, "CP2077 occl probe: secs=%.1f occl=%d main queries=%llu bufValid=%llu bufEmpty=%llu skipped=%llu | perFrame considered=%.0f frustumCulled=%.0f occluded=%.0f (%.1f%% of considered) visible=%.0f nodeOccluded=%.0f | other queries=%llu perQuery considered=%.0f visible=%.0f\n",
			( now - last ) / 1000.0, OcclusionOff() ? 0 : 1, v[ M_Queries ], v[ M_BufValid ], v[ M_BufEmpty ], v[ M_Skipped ],
			v[ M_Considered ] / mf, v[ M_FrustumOut ] / mf, v[ M_Occluded ] / mf, 100.0 * double( v[ M_Occluded ] ) / mc, v[ M_Visible ] / mf, v[ M_NodeOccluded ] / mf,
			v[ O_Queries ], v[ O_Considered ] / of, v[ O_Visible ] / of );
		std::fprintf( stderr, "CP2077 occl probe shadow: queries=%llu noRecvRasterizer=%llu noGlobalRasterizer=%llu | perFrame considered=%.0f overallFrustumCulled=%.0f globalOccluded=%.0f cascadeFrustumCulled=%.0f receiverOccluded=%.0f visible=%.0f cascadePasses=%.0f (c0=%.0f c1=%.0f c2=%.0f c3=%.0f)\n",
			v[ S_Queries ], v[ S_NoRast ], v[ S_NoGlobal ], v[ S_Considered ] / sf, v[ S_OverallOut ] / sf, v[ S_GlobalOccl ] / sf, v[ S_CascOut ] / sf, v[ S_RecvOccl ] / sf,
			v[ S_Visible ] / sf, v[ S_CascPasses ] / sf, v[ S_Casc0 ] / sf, v[ S_Casc1 ] / sf, v[ S_Casc2 ] / sf, v[ S_Casc3 ] / sf );
	}
}

// [skperf] round 48 mirrorCull (opt-in ?mirrorCull=<mrad>, see renderMaterialModifiers.cpp): renderCollector.cpp
// registers the vis camera position of each mirror camera's query; BVHDoTest of a query whose camera matches drops
// leaf objects whose angular radius (box half-diagonal / distance) is below the angle. Lock-free slots: a racing
// update can only make one BVHDoTest call miss the match (that call then culls nothing extra).
namespace CpMirrorCull
{
	// [skperf] round 50: a slot is one mirror query camera (vis camera position) with its mirrorCull angle (0 = none)
	// and the mirror culling box (mirrorFrustum: objects not overlapping the box are dropped, as the box query did).
	struct Slot { std::atomic<Uint32> valid{ 0 }, x{ 0 }, y{ 0 }, z{ 0 }, angle{ 0 }, box[ 6 ] = {}; };
	Slot s_slots[ 8 ];
	std::atomic<Uint32> s_next{ 0 };
	std::atomic<Int32> s_any{ 0 };
	std::atomic<unsigned long long> s_tested{ 0 }, s_culled{ 0 }, s_boxCulled{ 0 }, s_queries{ 0 };
	std::atomic<double> s_lastPrint{ 0.0 };
	inline Uint32 Bits( Float f ) { Uint32 u; std::memcpy( &u, &f, 4 ); return u; }
	inline Float FromBits( Uint32 u ) { Float f; std::memcpy( &f, &u, 4 ); return f; }
	struct Match { Bool found = false; Float angle = 0.f; Float box[ 6 ] = {}; };
	Match Find( Float x, Float y, Float z )
	{
		Match m;
		if ( s_any.load( std::memory_order_relaxed ) == 0 ) return m;
		const Uint32 bx = Bits( x ), by = Bits( y ), bz = Bits( z );
		for ( const Slot& s : s_slots )
		{
			if ( !s.valid.load( std::memory_order_acquire ) ) continue;
			if ( s.x.load( std::memory_order_relaxed ) != bx || s.y.load( std::memory_order_relaxed ) != by || s.z.load( std::memory_order_relaxed ) != bz ) continue;
			m.angle = FromBits( s.angle.load( std::memory_order_relaxed ) );
			for ( Uint32 i = 0; i < 6; ++i ) m.box[ i ] = FromBits( s.box[ i ].load( std::memory_order_relaxed ) );
			if ( !s.valid.load( std::memory_order_acquire ) ) return Match(); // rewritten meanwhile: no extra culling this call
			m.found = true;
			return m;
		}
		return m;
	}
	void MaybePrint()
	{
		const double now = emscripten_get_now();
		double last = s_lastPrint.load( std::memory_order_relaxed );
		if ( last == 0.0 ) { s_lastPrint.compare_exchange_strong( last, now ); return; }
		if ( now - last < 2000.0 || !s_lastPrint.compare_exchange_strong( last, now ) ) return;
		const unsigned long long q = s_queries.exchange( 0 ), t = s_tested.exchange( 0 ), c = s_culled.exchange( 0 ), b = s_boxCulled.exchange( 0 );
		std::fprintf( stderr, "CP2077 skperf mirrorFrustum: secs=%.1f mirrorQueryCalls=%llu objects in frustum=%llu outsideBox=%llu culled small=%llu (kept %.1f%%)\n",
			( now - last ) / 1000.0, q, t, b, c, t ? 100.0 * double( t - b - c ) / double( t ) : 0.0 );
	}
}
void CpMirrorCullRegisterQuery( Float x, Float y, Float z, Float angle, const Float* boxMinMax )
{
	using namespace CpMirrorCull;
	const Uint32 bx = Bits( x ), by = Bits( y ), bz = Bits( z ), ba = Bits( angle );
	Slot* slot = nullptr;
	for ( Slot& s : s_slots )
		if ( s.valid.load( std::memory_order_relaxed ) && s.x.load( std::memory_order_relaxed ) == bx && s.y.load( std::memory_order_relaxed ) == by && s.z.load( std::memory_order_relaxed ) == bz ) { slot = &s; break; }
	if ( !slot ) slot = &s_slots[ s_next.fetch_add( 1, std::memory_order_relaxed ) & 7 ];
	slot->valid.store( 0, std::memory_order_release );
	slot->x.store( bx, std::memory_order_relaxed ); slot->y.store( by, std::memory_order_relaxed ); slot->z.store( bz, std::memory_order_relaxed );
	slot->angle.store( ba, std::memory_order_relaxed );
	for ( Uint32 i = 0; i < 6; ++i ) slot->box[ i ].store( Bits( boxMinMax[ i ] ), std::memory_order_relaxed );
	slot->valid.store( 1, std::memory_order_release );
	s_any.store( 1, std::memory_order_relaxed );
}


// #TODO: Put these into the QueryParams or something? Might be nice to have the size scale with screen resolution.
namespace Config
{
	TConfigVar<Int32> cvMinOcclusionWidth( "Occlusion", "MinBufferWidth", 256, eConsoleVarFlag_Developer );
	TConfigVar<Int32> cvMinOcclusionHeight( "Occlusion", "MinBufferHeight", 128, eConsoleVarFlag_Developer );
}

namespace vis
{

	VIS_IMPL_QUERY_BASE_PARAMS( QuerySingleFrustum, SQueryParams_SingleFrustum );


	void QuerySingleFrustum::Init( const QueryContext& context, const SQueryParams_SingleFrustum& params )
	{
		QueryBase::Init( context, params );
		m_params = params;

		// #TODO: maybe use planes from camera directly, instead of going through CFrustum. Probably going to want some
		// custom tests anyways eventually, for doing multiple objects in parallel with simd stuff.
		m_frustum.SetCamera( m_params.m_camera, params.m_ignoreNearPlane );
	}

	Bool QuerySingleFrustum::AreParamsCompatible( const SQueryParams_SingleFrustum& params ) const
	{
		// Compatible if both are using occlusion, or both are not using occlusion. But we don't want to add occlusion
		// to a query that didn't already have it; or use a query that has it allocated when it's not requred (and then
		// end up getting a query that does want occlusion, but we don't have one anymore and need to create a new one).
		return m_params.m_useOcclusion == params.m_useOcclusion;
	}


	void QuerySingleFrustum::Clear()
	{
		m_occlusionBuffer.Clear();
	}


	void QuerySingleFrustum::Prepare()
	{
		RED_ASSERT(!m_params.m_useOcclusion, "Occlusion only supported by CounterChain version");
		if ( CpOccl::ProbeOn() ) CpOccl::Add( m_params.m_useOcclusion ? CpOccl::M_Queries : CpOccl::O_Queries, 1 );
		QueryBase::Prepare();
	}

	void QuerySingleFrustum::Prepare( job::Builder& builder )
	{
		const bool cpProbe = CpOccl::ProbeOn();
		if ( cpProbe ) CpOccl::Add( m_params.m_useOcclusion ? CpOccl::M_Queries : CpOccl::O_Queries, 1 );
		if ( m_params.m_useOcclusion && CpOccl::OcclusionOff() )
		{
			// [cull r5] ?occl=0: no occlusion buffer (IsValid() stays false -> frustum only; the collector then
			// passes no receiver rasterizer to the shadow cascades)
			if ( cpProbe ) CpOccl::Add( CpOccl::M_Skipped, 1 );
		}
		else if ( m_params.m_useOcclusion )
		{
			OcclusionBuffer::InitInfo occBufferInit;
			occBufferInit.m_camera = m_params.m_occlusionCamera;
			occBufferInit.m_width = Max<Uint32>( m_params.m_occlusionWidth, Config::cvMinOcclusionWidth.Get() );
			occBufferInit.m_height = Max<Uint32>( m_params.m_occlusionHeight, Config::cvMinOcclusionHeight.Get() );
			occBufferInit.m_occluders = &m_context.m_visScene->GetOccluderCollection();
			occBufferInit.m_collectDebugGeometry_Occluders = m_debugSettings.OccludersUsed;
			occBufferInit.m_collectDebugGeometry_RasterizedTriangles = m_debugSettings.OccludersRasterized;
			occBufferInit.m_collectDebugGeometry_Sectors = m_debugSettings.SectorsUsed;

#ifndef RED_CONFIGURATION_FINAL
			VisibilityStatsArray& stats = m_visibilitySystemStats;
			occBufferInit.m_cachedDebugMeshInfo = m_params.m_cachedDebugMeshesInfo;

#elif defined USE_PROFILER
			VisibilityStatsArray& stats = m_visibilitySystemStats;
#else
			VisibilityStatsArray stats{ red::PoolDebug() };
#endif

			if ( !m_occlusionBuffer.Prepare( stats, occBufferInit, builder ) )
			{
				RED_LOG_SPAM("Visibility: Failed to prepare occlusion buffer. Occlusion will not be used.");
			}
			if ( cpProbe ) CpOccl::Add( m_occlusionBuffer.IsValid() ? CpOccl::M_BufValid : CpOccl::M_BufEmpty, 1 );

		}

		builder.DispatchJob("Vis/BasePrepare", [this]( const job::RunContext& )
		{
			// Call non-job base function. Don't want to call the CounterChain version, since that'll just call into us
			// again, and we'll prepare twice.
			QueryBase::Prepare();
		});
	}


	void QuerySingleFrustum::CollectDebugQuery( DebugDrawBuffer& drawBuffer ) const
	{
		QueryBase::CollectDebugQuery( drawBuffer );

		if ( m_occlusionBuffer.IsValid() )
		{
			drawBuffer.Append( const_cast<OcclusionBuffer&>(m_occlusionBuffer).GetDebugDrawBuffer() );

			if ( m_debugSettings.OcclusionBuffer )
			{
				const Uint32 width = m_occlusionBuffer.GetDepthBufferWidth();
				const Uint32 height = m_occlusionBuffer.GetDepthBufferHeight();
				rast::DepthType* imageData = static_cast<rast::DepthType*>( drawBuffer.AllocateExtraData( width * height * sizeof(rast::DepthType) ) );
				m_occlusionBuffer.GetDepthBuffer( imageData );
				drawBuffer.AddImage( String::Printf("Occlusion_%u", m_viewId), imageData, width, height, IDebugDrawer::ImageFormat::R16_UNORM );
			}

			if ( m_debugSettings.OcclusionBufferRoughMin || m_debugSettings.OcclusionBufferRoughMax )
			{
				const rast::TileMinMaxDepth* srcData = m_occlusionBuffer.GetRoughDepthBufferPtr();
				const Uint32 width = m_occlusionBuffer.GetRoughDepthBufferWidth();
				const Uint32 height = m_occlusionBuffer.GetRoughDepthBufferHeight();
				rast::DepthType* imageData = (rast::DepthType*)drawBuffer.AllocateExtraData( width * height * sizeof( rast::DepthType ) );
				for ( Uint32 i = 0; i < width * height; ++i )
				{
					imageData[i] = m_debugSettings.OcclusionBufferRoughMin ? srcData[i].min : srcData[i].max;
				}
				drawBuffer.AddImage( String::Printf( "OcclusionRough_%u", m_viewId ), imageData, width, height, IDebugDrawer::ImageFormat::R16_UNORM );
			}
			if ( m_debugSettings.OcclusionBufferInternal )
			{
				const rast::DepthType* srcData = m_occlusionBuffer.GetInternalDepthBufferPtr();
				const Uint32 width = m_occlusionBuffer.GetInternalWidth();
				const Uint32 height = m_occlusionBuffer.GetInternalHeight();
				void* imageData = drawBuffer.AllocateExtraData( width * height * sizeof( rast::DepthType ) );
				red::Memcpy( imageData, srcData, width * height * sizeof( rast::DepthType ) );
				drawBuffer.AddImage( String::Printf("OcclusionInternal_%u", m_viewId), imageData, width, height, IDebugDrawer::ImageFormat::R16_UNORM );
			}
		}
	}


	void QuerySingleFrustum::DoCollectDebugQueryBounds( DebugDrawBuffer& drawBuffer ) const
	{
		drawBuffer.AddQueryFrustum( m_params.m_camera.GetScreenToWorld() );
	}


	TargetVisibilityMask QuerySingleFrustum::GetBoxVisibilityMask( const Box& boundingBox, const EFrustumResult parentFrustumResult ) const
	{
		EFrustumResult ret = parentFrustumResult;

		if ( ret == FR_Intersecting )
		{
			ret = m_frustum.TestBox( boundingBox );
		}

		if ( ret != EFrustumResult::FR_Outside && m_occlusionBuffer.IsValid() )
		{
			ret = m_occlusionBuffer.TestBox( boundingBox, ret, rast::IRasterizer::DepthTestType::Approx ).m_frustumResult;
			if ( ret == EFrustumResult::FR_Outside && CpOccl::ProbeOn() ) CpOccl::Add( CpOccl::M_NodeOccluded, 1 );
		}

		return ret;
	}

	void QuerySingleFrustum::BVHDoTest( const red::ArraySpan<const MainSceneAccelerationStructure::NodeData>& targets, const Bool doFrustumTest, vis::QueryResults& visResults ) const
	{
		Uint32 numVisibleTargets = 0;
		Uint32 numTestedTargets = 0;

		Uint32 numOccBufVisibilityTests = 0;
		Uint32 numOccBufTilesTests = 0;
		Uint32 numOccBufEarlyOuts = 0;
		Uint32 numOccBufClippedTests = 0;
		Uint32 numOccBufPassedLateVisibilityTests = 0;

		// Local copy to make sure accesses are local.
		const CameraFrustumTest frustum = m_frustum;
		const QueryMask queryMask = m_params.m_queryMask;

		const Bool enableOcclusion = m_occlusionBuffer.IsValid();
		Uint32 cpFrustumOut = 0, cpOccluded = 0; // [cull r5] probe counters (flushed once per call, only with ?occlprobe=1)
		// [skperf] round 48 mirrorCull: > 0 only for a registered mirror camera query with the switch on
		const auto& cpCamPos = m_params.m_camera.GetPosition();
		const CpMirrorCull::Match cpMirror = CpMirrorCull::Find( cpCamPos.X, cpCamPos.Y, cpCamPos.Z ); // [skperf] round 50
		const Float cpMirrorAngleSqr = cpMirror.angle * cpMirror.angle;
		Uint32 cpMirrorTested = 0, cpMirrorCulled = 0, cpMirrorBoxCulled = 0;

		const Uint32 totalTargetsCount = targets.Size();
		for ( Uint32 i = 0; i < totalTargetsCount; ++i )
		{
			const MainSceneAccelerationStructure::NodeData& objData = targets[i];
			if ( ( objData.m_inclusionMask & queryMask.m_mask ) == 0 )
			{
				continue;
			}
			if ( ( objData.m_todvisMask & m_params.m_queryMaskTimeOfDay ) != 0 )
			{
				continue;
			}

			++numTestedTargets;

			const Box box = Box( objData.m_boxMin, objData.m_boxMax );

			VisibilityResult visResult( FR_Inside );
			if ( doFrustumTest )
			{
				visResult.m_frustumResult = frustum.TestBox( box );
			}
			cpFrustumOut += ( visResult.m_frustumResult == FR_Outside ) ? 1u : 0u;

			if ( cpMirror.found && visResult.m_frustumResult != FR_Outside ) // [skperf] round 50 mirrorFrustum box / round 48 mirrorCull size
			{
				++cpMirrorTested;
				const Float* b = cpMirror.box;
				if ( objData.m_boxMax.X < b[ 0 ] || objData.m_boxMin.X > b[ 3 ] || objData.m_boxMax.Y < b[ 1 ] || objData.m_boxMin.Y > b[ 4 ] ||
					objData.m_boxMax.Z < b[ 2 ] || objData.m_boxMin.Z > b[ 5 ] )
				{
					visResult.m_frustumResult = FR_Outside; ++cpMirrorBoxCulled; // outside the mirror culling box: the box query never returned it
				}
				else if ( cpMirror.angle > 0.f )
				{
					const Float hx = ( objData.m_boxMax.X - objData.m_boxMin.X ) * 0.5f, hy = ( objData.m_boxMax.Y - objData.m_boxMin.Y ) * 0.5f, hz = ( objData.m_boxMax.Z - objData.m_boxMin.Z ) * 0.5f;
					const Float dx = objData.m_boxMin.X + hx - cpCamPos.X, dy = objData.m_boxMin.Y + hy - cpCamPos.Y, dz = objData.m_boxMin.Z + hz - cpCamPos.Z;
					const Float r2 = hx * hx + hy * hy + hz * hz, d2 = dx * dx + dy * dy + dz * dz;
					if ( d2 > r2 && r2 < cpMirrorAngleSqr * d2 ) { visResult.m_frustumResult = FR_Outside; ++cpMirrorCulled; }
				}
			}

			if ( enableOcclusion && visResult.m_frustumResult != FR_Outside )
			{
				visResult = m_occlusionBuffer.TestBox( box, visResult.m_frustumResult, rast::IRasterizer::DepthTestType::Approx );
				cpOccluded += ( visResult.m_frustumResult == FR_Outside ) ? 1u : 0u;

#ifndef RED_CONFIGURATION_FINAL
				numOccBufVisibilityTests += visResult.m_stat.m_numOccBufVisibilityTests;
				numOccBufTilesTests += visResult.m_stat.m_numOccBufTilesTests;
				numOccBufEarlyOuts += visResult.m_stat.m_numOccBufEarlyOuts;
				numOccBufClippedTests += visResult.m_stat.m_numOccBufClippedTests;
				numOccBufPassedLateVisibilityTests += visResult.m_stat.m_numOccBufPassedLateVisibilityTests;
#endif
			}

			if ( visResult.m_frustumResult != FR_Outside )
			{
#ifdef RED_CONFIGURATION_FINAL
				visResults.AddVisibleTarget<vis::QueryResultsVisMaskInclusion::VMI_NO>( objData.m_userData, visResult.m_frustumResult );
#else
				visResults.AddVisibleTarget<vis::QueryResultsVisMaskInclusion::VMI_YES>( objData.m_userData, visResult.m_frustumResult );
				if ( m_params.m_debugSettings.VisibleTargetBounds )
				{
					visResults.AddDebugVisibleBox( box );
				}
#endif
				++numVisibleTargets;
			}
		}

		if ( cpMirror.found ) // [skperf] round 48/50: line every ~2 s while a mirror renders through its frustum query
		{
			CpMirrorCull::s_queries.fetch_add( 1, std::memory_order_relaxed );
			CpMirrorCull::s_tested.fetch_add( cpMirrorTested, std::memory_order_relaxed );
			CpMirrorCull::s_culled.fetch_add( cpMirrorCulled, std::memory_order_relaxed );
			CpMirrorCull::s_boxCulled.fetch_add( cpMirrorBoxCulled, std::memory_order_relaxed );
			CpMirrorCull::MaybePrint();
		}

		if ( CpOccl::ProbeOn() )
		{
			if ( m_params.m_useOcclusion )
			{
				CpOccl::Add( CpOccl::M_Considered, numTestedTargets );
				CpOccl::Add( CpOccl::M_FrustumOut, cpFrustumOut );
				CpOccl::Add( CpOccl::M_Occluded, cpOccluded );
				CpOccl::Add( CpOccl::M_Visible, numVisibleTargets );
			}
			else
			{
				CpOccl::Add( CpOccl::O_Considered, numTestedTargets );
				CpOccl::Add( CpOccl::O_Visible, numVisibleTargets );
			}
			CpOccl::MaybePrint();
		}

#ifndef RED_CONFIGURATION_FINAL
		visResults.m_stats.m_numObjectsTested += numTestedTargets;
		visResults.m_stats.m_numObjectsVisible += numVisibleTargets;

		visResults.m_stats.m_singleFrustumNumTotalTargets += totalTargetsCount;
		visResults.m_stats.m_singleFrustumNumTestedTargets += numTestedTargets;
		visResults.m_stats.m_singleFrustumNumPassedTargets += numVisibleTargets;

		visResults.m_stats.m_numOccBufVisibilityTests += numOccBufVisibilityTests;
		visResults.m_stats.m_numOccBufTilesTests += numOccBufTilesTests;
		visResults.m_stats.m_numOccBufEarlyOuts += numOccBufEarlyOuts;
		visResults.m_stats.m_numOccBufClippedTests += numOccBufClippedTests;
		visResults.m_stats.m_numOccBufPassedLateVisibilityTests += numOccBufPassedLateVisibilityTests;
#endif

	}

}
