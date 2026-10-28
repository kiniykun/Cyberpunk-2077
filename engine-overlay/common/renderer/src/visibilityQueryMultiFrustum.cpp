/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "visibilityQueryMultiFrustum.h"
#include "visibilityScene.h"
#include "visibilityResults.h"
#include "visibilityDebugDrawBuffer.h"
#include "shadowCascade.h"

#include "../../visibility/include/visRasterizerSSEThreaded.h"
#include "../../redSystem/include/stopWatch.h"
#include "../../renderData/include/meshEnum.h"
#include "../../visibility/include/visRasterizerSSEThreaded.h"

// [cull r5] web-port overlay (sun-shadow cascade query). Native logic unchanged; adds
//   ?occl=0          no occlusion in the cascade query (receiver test vs main-camera buffer + distant-shadow global occlusion),
//   ?occlShadow=0|1|2 override Occlusion/MultiFrustumOcclusionMode (0 off, 1 approx = engine default, 2 full test),
//   ?occlprobe=1     counters printed with the main-view line by visibilityQuerySingleFrustum.cpp.
#include <atomic>
extern std::atomic<int> g_cp2077OcclMode;
extern std::atomic<int> g_cp2077OcclShadowMode;
namespace CpOccl
{
	enum Counter : unsigned
	{
		M_Queries, M_BufValid, M_BufEmpty, M_Skipped, M_Considered, M_FrustumOut, M_Occluded, M_Visible, M_NodeOccluded,
		O_Queries, O_Considered, O_Visible,
		S_Queries, S_NoRast, S_NoGlobal, S_Considered, S_OverallOut, S_GlobalOccl, S_CascOut, S_RecvOccl, S_Visible, S_CascPasses,
		S_Casc0, S_Casc1, S_Casc2, S_Casc3,
		C_Count
	};
	bool ProbeOn();
	bool OcclusionOff();
	void Add( unsigned c, unsigned long long n );
	void MaybePrint();
}

namespace Config
{
	// 0 - disabled, 1 - approx, 2 - full
	TConfigVar<Int32> cvOcclusionMode( "Occlusion", "MultiFrustumOcclusionMode", 1, 0, 2, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvUseGlobalOcclusion( "Occlusion", "MultiFrustumUseGlobalOcclusion", true, eConsoleVarFlag_Developer );
}

namespace vis
{

	VIS_IMPL_QUERY_BASE_PARAMS( QueryMultiFrustum, SQueryParams_MultiFrustum );

	static void ComputeOrientedBoundingBox(
		const math::Box& inBox, const math::Vector4& x, const math::Vector4& y, const math::Vector4& z,
		math::Vector4& outCenter, math::Vector4& outExtents )
	{
		math::Vector4 minRange, maxRange;

		// TODO DPPS and BLENDVPS instruction are pretty slow, maybe there's a better way to do it?
		minRange.X = Vector4::Dot3( _mm_blendv_ps( inBox.Min, inBox.Max, x ), x );
		maxRange.X = Vector4::Dot3( _mm_blendv_ps( inBox.Max, inBox.Min, x ), x );
		minRange.Y = Vector4::Dot3( _mm_blendv_ps( inBox.Min, inBox.Max, y ), y );
		maxRange.Y = Vector4::Dot3( _mm_blendv_ps( inBox.Max, inBox.Min, y ), y );
		minRange.Z = Vector4::Dot3( _mm_blendv_ps( inBox.Min, inBox.Max, z ), z );
		maxRange.Z = Vector4::Dot3( _mm_blendv_ps( inBox.Max, inBox.Min, z ), z );

		outExtents = ( maxRange - minRange ) * 0.5f;
		const Vector4 center = ( maxRange + minRange ) * 0.5f;
		outCenter = x * center.X + y * center.Y + z * center.Z;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


	// #TODO: Depth Buffer visualization could be moved out to be reused elsewhere. Showing the camera occlusion buffer
	// this way doesn't make sense until we can detach the render camera from the occlusion camera, so just keeping it
	// here until it's needed somewhere else.
	const Uint32 c_depthBufferDebugPatchWidth = 127;
	const Uint32 c_depthBufferDebugPatchHeight = 127;
	static_assert((c_depthBufferDebugPatchWidth+1)*(c_depthBufferDebugPatchHeight+1)*4 <= 65536, "Patch too big for 16-bit indices");

	static Bool InitDepthBufferIndices( red::DynArray<Uint16>& dbSolidIndices, red::DynArray<Uint16>& dbWireIndices )
	{
		dbSolidIndices.Reserve( (c_depthBufferDebugPatchWidth-1)*(c_depthBufferDebugPatchHeight-1)*6 );
		dbWireIndices.Reserve( c_depthBufferDebugPatchWidth*(c_depthBufferDebugPatchHeight-1)*2 + c_depthBufferDebugPatchHeight*(c_depthBufferDebugPatchWidth-1)*2 );

		for ( Uint32 patchY = 0; patchY < c_depthBufferDebugPatchHeight; ++patchY )
		{
			for ( Uint32 patchX = 0; patchX < c_depthBufferDebugPatchWidth; ++patchX )
			{
				const Uint32 pxIndex = patchX + patchY*(c_depthBufferDebugPatchWidth+1);

				// One square per pixel
				dbWireIndices.PushBack( pxIndex*4 + 0 ); dbWireIndices.PushBack( pxIndex*4 + 1 );
				dbWireIndices.PushBack( pxIndex*4 + 1 ); dbWireIndices.PushBack( pxIndex*4 + 3 );
				dbWireIndices.PushBack( pxIndex*4 + 3 ); dbWireIndices.PushBack( pxIndex*4 + 2 );
				dbWireIndices.PushBack( pxIndex*4 + 2 ); dbWireIndices.PushBack( pxIndex*4 + 0 );

				dbSolidIndices.PushBack( pxIndex*4 + 0 ); dbSolidIndices.PushBack( pxIndex*4 + 1 ); dbSolidIndices.PushBack( pxIndex*4 + 2 );
				dbSolidIndices.PushBack( pxIndex*4 + 2 ); dbSolidIndices.PushBack( pxIndex*4 + 1 ); dbSolidIndices.PushBack( pxIndex*4 + 3 );

				// Edges to + adjacent pixels
				const Uint32 pxXIndex = (patchX+1) + patchY*(c_depthBufferDebugPatchWidth+1);
				const Uint32 pxYIndex = patchX + (patchY+1)*(c_depthBufferDebugPatchWidth+1);
				const Uint32 pxXYIndex = (patchX+1) + (patchY+1)*(c_depthBufferDebugPatchWidth+1);
#if 0
				// Edges connecting adjacent pixels add quite a bit of noise... can we do something to make this easier
				// to see?
				// #TODO: Combine the +,+ edges into one, covering whatever the biggest span is.
				dbWireIndices.PushBack( pxIndex*4 + 1 ); dbWireIndices.PushBack( pxXIndex*4 + 0 );
				dbWireIndices.PushBack( pxIndex*4 + 3 ); dbWireIndices.PushBack( pxXIndex*4 + 2 );

				dbWireIndices.PushBack( pxIndex*4 + 2 ); dbWireIndices.PushBack( pxYIndex*4 + 0 );
				dbWireIndices.PushBack( pxIndex*4 + 3 ); dbWireIndices.PushBack( pxYIndex*4 + 1 );

				dbWireIndices.PushBack( pxIndex*4 + 3 ); dbWireIndices.PushBack( pxXYIndex*4 + 0 );
#endif
				dbSolidIndices.PushBack( pxIndex*4 + 3 ); dbSolidIndices.PushBack( pxIndex*4 + 1 ); dbSolidIndices.PushBack( pxXIndex*4 + 2 );
				dbSolidIndices.PushBack( pxXIndex*4 + 2 ); dbSolidIndices.PushBack( pxIndex*4 + 1 ); dbSolidIndices.PushBack( pxXIndex*4 + 0 );

				dbSolidIndices.PushBack( pxIndex*4 + 2 ); dbSolidIndices.PushBack( pxIndex*4 + 3 ); dbSolidIndices.PushBack( pxYIndex*4 + 0 );
				dbSolidIndices.PushBack( pxYIndex*4 + 0 ); dbSolidIndices.PushBack( pxIndex*4 + 3 ); dbSolidIndices.PushBack( pxYIndex*4 + 1 );
			}
		}

		return true;
	}

	static void AddDepthBuffer( DebugDrawBuffer& drawBuffer, const rast::DepthType* depthBuffer, Uint32 width, Uint32 height, const math::Matrix& viewportToWorld, DebugDrawBuffer::MeshType meshType )
	{
		static thread_local red::DynArray<math::Vector4> dbVertices { rend::PoolRenderDebug() };
		static thread_local red::DynArray<Uint16> dbSolidIndices { rend::PoolRenderDebug() };
		static thread_local red::DynArray<Uint16> dbWireIndices { rend::PoolRenderDebug() };
		static thread_local Bool dbIndicesInited = InitDepthBufferIndices( dbSolidIndices, dbWireIndices );

		dbVertices.Resize( (c_depthBufferDebugPatchWidth+1)*(c_depthBufferDebugPatchHeight+1)*4 );

		for ( Uint32 roughY = 0; roughY < height; roughY += c_depthBufferDebugPatchHeight )
		{
			for ( Uint32 roughX = 0; roughX < width; roughX += c_depthBufferDebugPatchWidth )
			{
				const Uint32 thisWidth = Min( c_depthBufferDebugPatchWidth+1, width - roughX );
				const Uint32 thisHeight = Min( c_depthBufferDebugPatchHeight+1, height - roughY );

				RED_ASSERT( thisWidth > 0 && thisHeight > 0 );

				for ( Uint32 patchY = 0; patchY < thisHeight; ++patchY )
				{
					for ( Uint32 patchX = 0; patchX < thisWidth; ++patchX )
					{
						const Uint32 x = roughX + patchX;
						const Uint32 y = roughY + patchY;

						const Uint32 pxIndex = patchX + patchY*(c_depthBufferDebugPatchWidth+1);

						const Float depth = depthBuffer[x+y*width] / (Float)UINT16_MAX;

						math::Vector4 viewportP;
						viewportP.X = (x + 0.5f);
						viewportP.Y = (y + 0.5f);
						viewportP.Z = depth;
						viewportP.W = 1.0f;
						dbVertices[pxIndex*4 + 0] = viewportP + math::Vector4( -0.5f, -0.5f, 0.0f, 0.0f );
						dbVertices[pxIndex*4 + 1] = viewportP + math::Vector4( 0.5f, -0.5f, 0.0f, 0.0f );
						dbVertices[pxIndex*4 + 2] = viewportP + math::Vector4( -0.5f, 0.5f, 0.0f, 0.0f );
						dbVertices[pxIndex*4 + 3] = viewportP + math::Vector4( 0.5f, 0.5f, 0.0f, 0.0f );
					}

					for ( Uint32 patchX = thisWidth; patchX < c_depthBufferDebugPatchWidth+1; ++patchX )
					{
						const Uint32 pxIndexRightmost = (thisWidth-1) + patchY*(c_depthBufferDebugPatchWidth+1);
						const Uint32 pxIndex = patchX + patchY*(c_depthBufferDebugPatchWidth+1);
						dbVertices[pxIndex*4 + 0] = dbVertices[pxIndexRightmost*4 + 1];
						dbVertices[pxIndex*4 + 1] = dbVertices[pxIndexRightmost*4 + 1];
						dbVertices[pxIndex*4 + 2] = dbVertices[pxIndexRightmost*4 + 3];
						dbVertices[pxIndex*4 + 3] = dbVertices[pxIndexRightmost*4 + 3];
					}
				}
				for ( Uint32 patchY = thisHeight; patchY < c_depthBufferDebugPatchHeight+1; ++patchY )
				{
					for ( Uint32 patchX = 0; patchX < c_depthBufferDebugPatchWidth+1; ++patchX )
					{
						const Uint32 pxIndexBottommost = patchX + (thisHeight-1)*(c_depthBufferDebugPatchWidth+1);
						const Uint32 pxIndex = patchX + patchY*(c_depthBufferDebugPatchWidth+1);
						dbVertices[pxIndex*4 + 0] = dbVertices[pxIndexBottommost*4 + 2];
						dbVertices[pxIndex*4 + 1] = dbVertices[pxIndexBottommost*4 + 3];
						dbVertices[pxIndex*4 + 2] = dbVertices[pxIndexBottommost*4 + 2];
						dbVertices[pxIndex*4 + 3] = dbVertices[pxIndexBottommost*4 + 3];
					}
				}

				drawBuffer.AddMesh( meshType, dbVertices, dbSolidIndices, dbWireIndices, viewportToWorld, false );
			}
		}
	}


	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


	void QueryMultiFrustum::Init( const QueryContext& context, const SQueryParams_MultiFrustum& params )
	{
		QueryBase::Init( context, params );
		m_params = params;

		if ( CpOccl::ProbeOn() )
		{
			const bool off = CpOccl::OcclusionOff();
			CpOccl::Add( CpOccl::S_Queries, 1 );
			CpOccl::Add( CpOccl::S_NoRast, ( off || m_params.m_rasterizer == nullptr ) ? 1 : 0 );
			CpOccl::Add( CpOccl::S_NoGlobal, ( off || m_params.m_globalOcclusionRasterizer == nullptr ) ? 1 : 0 );
		}

		if ( m_params.m_numFrustums == 0 )
		{
			RED_LOG_WARNING( "Visibility: MultiFrustum query has 0 frustums. Will not test anything." );
		}
		if ( m_params.m_numFrustums > MAX_CASCADES )
		{
			RED_LOG_ERROR( "Visibility: MultiFrustum query has %u frustums! Will be clamped to 4", m_params.m_numFrustums );
			m_params.m_numFrustums = MAX_CASCADES;
		}

		for ( Uint32 i = 0; i < m_params.m_numFrustums; ++i )
		{
			m_frustumTests[i].SetPlanes( m_params.m_cullingPlanes[i].TypedData(), m_params.m_cullingPlanes[i].Size() );
		}

		m_overallFrustumTest.SetPlanes( m_params.m_overallCullingPlanes.TypedData(), m_params.m_overallCullingPlanes.Size() );
	}

	Bool QueryMultiFrustum::AreParamsCompatible( const SQueryParams_MultiFrustum& params ) const
	{
		return true;
	}


	void QueryMultiFrustum::Clear()
	{
		// Nothing to do.
	}


	void QueryMultiFrustum::CollectDebugQuery( DebugDrawBuffer& drawBuffer ) const
	{
		QueryBase::CollectDebugQuery( drawBuffer );

		if ( m_params.m_globalOcclusionRasterizer )
		{
			if ( m_params.m_debugSettings.OcclusionBufferRoughWorld )
			{
				const rast::TileMinMaxDepth* depthBuffer = m_params.m_globalOcclusionRasterizer->GetTileDepths();
				const Uint32 width = m_params.m_globalOcclusionRasterizer->GetRoughDepthBufferWidth();
				const Uint32 height = m_params.m_globalOcclusionRasterizer->GetRoughDepthBufferHeight();

				math::Matrix tileToViewport = math::Matrix::IDENTITY();
				tileToViewport.SetScale( math::Vector4( Float( rast::RasterizerSSEBase::TILE_SIZE ), Float( rast::RasterizerSSEBase::TILE_SIZE ), 1.0f, 1.0f ) );

				const math::Matrix worldToViewport = m_params.m_globalOcclusionRasterizer->GetWorldToViewport();
				const math::Matrix viewportToWorld = tileToViewport * worldToViewport.FullInverted();

				red::DynArray< rast::DepthType > tempBuffer{ red::PoolDebug() };
				tempBuffer.Resize( width * height );
				for ( Uint32 i = 0; i < width * height; ++i )
				{
					tempBuffer[i] = depthBuffer[i].max;
				}

				AddDepthBuffer( drawBuffer, tempBuffer.TypedData(), width, height, viewportToWorld, DebugDrawBuffer::MeshType::OccluderBroadphase );

				drawBuffer.AddImage( String::Printf( "OcclusionRough_%u", m_viewId ), depthBuffer, width, height, IDebugDrawer::ImageFormat::R16_UNORM );
			}

			if ( m_params.m_debugSettings.OcclusionBufferFineWorld )
			{
				const Uint32 width = m_params.m_globalOcclusionRasterizer->GetDepthBufferWidth();
				const Uint32 height = m_params.m_globalOcclusionRasterizer->GetDepthBufferHeight();
				static thread_local red::DynArray<rast::DepthType> depthBuffer { rend::PoolRenderDebug() };
				depthBuffer.Resize( width*height );
				m_params.m_globalOcclusionRasterizer->GetDebugDepthBuffer( depthBuffer.TypedData() );

				const math::Matrix worldToViewport = m_params.m_globalOcclusionRasterizer->GetWorldToViewport();
				const math::Matrix viewportToWorld = worldToViewport.FullInverted();

				AddDepthBuffer( drawBuffer, depthBuffer.TypedData(), width, height, viewportToWorld, DebugDrawBuffer::MeshType::OccluderFillphase );

				drawBuffer.AddImage( String::Printf( "Occlusion_%u", m_viewId ), depthBuffer.TypedData(), width, height, IDebugDrawer::ImageFormat::R16_UNORM );
			}

		}
	}

	void QueryMultiFrustum::DoCollectDebugQueryBounds( DebugDrawBuffer& drawBuffer ) const
	{
		const Uint32 numFrustums = m_params.m_numFrustums;
		RED_ASSERT( numFrustums <= MAX_CASCADES, "Invalid number of frustums: %u!", numFrustums );
		for ( Uint32 i = 0; i < numFrustums; ++i )
		{
			// TODO what about this? we don't have a frustum, but 8 random planes...
			//Matrix screenToWorld = m_params.m_worldToScreen[i].FullInverted();
			//drawBuffer.AddQueryFrustum( screenToWorld );
		}
	}


	TargetVisibilityMask QueryMultiFrustum::GetBoxVisibilityMask( const Box& boundingBox, const EFrustumResult parentFrustumResult ) const
	{
		//TODO - handle the parentFrustumResult correctly here if we want to handle multiFrustum queries in BVH

		EFrustumResult frustumResult = m_overallFrustumTest.TestBox( boundingBox );

		//if ( frustumResult != EFrustumResult::FR_Outside &&  Config::cvUseGlobalOcclusion.Get() && m_params.m_globalOcclusionRasterizer != nullptr )
		//{
		//	OcclusionBufferTestResult occRes = m_params.m_globalOcclusionRasterizer->IsVisible( rast::IRasterizer::DepthTestType::CascadeGlobalOcclusion, boundingBox ).m_result;
		//	frustumResult = occRes == OcclusionBufferTestResult::Occluded ? EFrustumResult::FR_Outside : frustumResult;
		//}

		return frustumResult;

	}

	void QueryMultiFrustum::BVHDoTest( const red::ArraySpan<const MainSceneAccelerationStructure::NodeData>& targets, const Bool doFrustumTest, vis::QueryResults& results ) const
	{
		PC_SCOPE( QueryMultiFrustum );

		// Stuff for stats
		Uint32 numTargets = 0;
		Uint32 numTargetsPassedTodVis = 0;
		Uint32 numVisibleTargets = 0;
		Uint32 numTestedTargets = 0;
		Uint32 numTestedTargetsWithIncorrectBoundingBox = 0;
		Uint32 numTargetsPassedOverallFrustumCulling = 0;
		Uint32 numTargetsPassedShadowOcclusion = 0;
		Uint32 numTargetsPassedFrustumCulling = 0;
		Uint32 numFrustumsTested = 0;
		Uint32 numFrustumsPassed = 0;

		// [cull r5] ?occlShadow overrides the mode, ?occl=0 disables both occlusion tests here
		const Int32 cpShadowMode = g_cp2077OcclShadowMode.load( std::memory_order_relaxed );
		const Bool cpOcclOff = CpOccl::OcclusionOff();
		const Int32 occlusionMode = cpOcclOff ? 0 : ( cpShadowMode >= 0 ? cpShadowMode : Config::cvOcclusionMode.Get() );
		const vis::rast::RasterizerSSEThreaded* rasterizer = occlusionMode > 0 ? m_params.m_rasterizer : nullptr;
		const vis::rast::RasterizerSSEThreaded* globalOcclusionRasterizer = ( !cpOcclOff && Config::cvUseGlobalOcclusion.Get() ) ? m_params.m_globalOcclusionRasterizer : nullptr;
		Uint32 cpGlobalOccl = 0, cpRecvOccl = 0;
		Uint32 cpCasc[ 4 ] = { 0, 0, 0, 0 };

		// precompute box extrusion length for occlusion culling 
		const Float nearPlaneDist = Config::cvFrontCullingPlaneDistance.Get();
		const Float halfExtrudeLength = ( nearPlaneDist / 2.0f ) + m_params.m_maxVerticalSize; // vertical size is half cascade box height

		const Uint32 numFrustums = m_params.m_numFrustums;
		RED_ASSERT( numFrustums <= MAX_CASCADES, "Invalid number of frustums: %u!", numFrustums );
		if ( numFrustums > 0 )
		{
			// Local copy to make sure accesses are local.
			QueryMask queryMasks[MAX_CASCADES];
			QueryMask combinedQueryMask;

			for ( Uint32 i = 0; i < numFrustums; ++i )
			{
				queryMasks[i] = m_params.m_queryMasks[i];
				combinedQueryMask.m_mask |= queryMasks[i].m_mask;
			}

#ifndef RED_CONFIGURATION_FINAL
			// Allow enforcing visibility checks across all cascade no matter what, to allow for easy realtime tuning of heuristics
			const Bool forceVolumeHeuristicRefresh = Config::cvForceVolumeHeuristicRefresh.Get();
			QueryMask allCascadesMask;
			allCascadesMask.m_shadows.m_mask = ShadowFlags::CASCADE_SHADOWS_MASK();
#endif
			numTargets = targets.Size();

			const Uint32 prefetchLookahead = 8;

			// prefetch initial targets - in case of BVH traversal it's a bit faster without prefetches
			//for ( Uint32 target_i = 0; target_i < Min( prefetchLookahead, numTargets ); ++target_i )
			//{
			//	const MainSceneAccelerationStructure::NodeData& nextTargetInfo = targets[target_i];
			//	_mm_prefetch( (const char*)&nextTargetInfo, _MM_HINT_T0 );
			//}

			for ( Uint32 target_i = 0; target_i < numTargets; ++target_i )
			{
				// prefetch next target info - in case of BVH traversal it's a bit faster without prefetches
				//if ( target_i + prefetchLookahead < numTargets )
				//{
				//	const MainSceneAccelerationStructure::NodeData& nextTargetInfo = targets[target_i + prefetchLookahead];
				//	_mm_prefetch( (const char*)&nextTargetInfo, _MM_HINT_T0 );
				//}

				const MainSceneAccelerationStructure::NodeData& nodeData = targets[target_i];

				//  Check visibility in the current time of the day. m_params should have encoded current time of day in
				//	and group info should contain its .todvis mask. If anything is missing (set to 0) the check passes.
				{
					const Bool todvisMismatch = ( ( nodeData.m_todvisMask & m_params.m_queryMaskTimeOfDay ) != 0 );
					if ( todvisMismatch )
					{
						continue;
					}
					numTargetsPassedTodVis++;
				}

				// If the object doesn't match the combined renderMask filter, then it won't match any individual
				// filters, and we can skip it.
				QueryMask inclusionMask = nodeData.m_inclusionMask;
				{
#ifndef RED_CONFIGURATION_FINAL
					if ( forceVolumeHeuristicRefresh )
					{
						// in case of forcing real-time heuristics values, we need to check all the cascades
						// proxies will be filtered on collection
						inclusionMask.m_mask |= allCascadesMask.m_mask; //TODO - prevent in non final
					}
#endif

					if ( ( inclusionMask.m_mask & combinedQueryMask.m_mask ) == 0 )
					{
						continue;
					}
					numTestedTargets++;
				}

				const Box box = Box( nodeData.m_boxMin, nodeData.m_boxMax );
#ifndef RED_CONFIGURATION_FINAL
				if ( !( box.IsOk() && ( Vector3( box.Min.X, box.Min.Y, box.Min.Z ) - Vector3( RED_FLT_MAX, RED_FLT_MAX, RED_FLT_MAX ) ).Mag() > 1e-6 ) )
				{
					numTestedTargetsWithIncorrectBoundingBox++;
				}
#endif

				// Perform fast check again overall frustum containing all sub-frustums
				{
					if ( doFrustumTest && !m_overallFrustumTest.TestBoxSimple( box ) ) //TODO
					{
						continue;
					}
					numTargetsPassedOverallFrustumCulling++;
				}

				if ( globalOcclusionRasterizer )
				{
					if ( m_params.m_globalOcclusionRasterizer->IsVisible( rast::IRasterizer::DepthTestType::CascadeGlobalOcclusion, box ).m_result == OcclusionBufferTestResult::Occluded )
					{
						++cpGlobalOccl;
						continue;
					}
					numTargetsPassedShadowOcclusion++;
				}

				// Perform per-frustum culling
				Uint8 visMask = 0;
				{
					for ( Uint32 frustum_i = 0; frustum_i < MAX_CASCADES; ++frustum_i )
					{
						if ( ( inclusionMask.m_mask & queryMasks[frustum_i].m_mask ) != 0 )
						{
							const EFrustumResult result = m_frustumTests[frustum_i].TestBoxSimple( box ) ? FR_Intersecting : FR_Outside;

							visMask |= ( result << ( frustum_i * 2 ) );

							numFrustumsTested++;
							numFrustumsPassed += result != FR_Outside ? 1 : 0;
							cpCasc[ frustum_i & 3 ] += result != FR_Outside ? 1u : 0u;
						}
					}

					if ( visMask == 0 )
					{
						// not visible in any frustum
						continue;
					}
					numTargetsPassedFrustumCulling++;
				}

				// Perform occlusion culling against main-camera occlusion buffer
				if ( rasterizer )
				{
					math::Vector4 center, size;
					ComputeOrientedBoundingBox( box, m_params.m_shadowRight, m_params.m_shadowDir, m_params.m_shadowUp, center, size );

					// extrude toward the sun
					size.Y += halfExtrudeLength;
					center += m_params.m_shadowDir * halfExtrudeLength;

					math::Vector4 x = m_params.m_shadowRight * size.X;
					math::Vector4 y = m_params.m_shadowDir * size.Y;
					math::Vector4 z = m_params.m_shadowUp * size.Z;

					const Bool occlusionTestResult =
						occlusionMode > 1 ?
						rasterizer->TestOrientedBoxFull( center, x, y, z ) :
						rasterizer->TestOrientedBoxApprox( center, x, y, z );

					if ( !occlusionTestResult )
					{
						// shadow is occluded
						++cpRecvOccl;
						continue;
					}
				}

				results.AddVisibleTarget<vis::QueryResultsVisMaskInclusion::VMI_YES>( nodeData.m_userData, visMask );
#ifndef RED_CONFIGURATION_FINAL
				if ( m_params.m_debugSettings.VisibleTargetBounds )
				{
					results.AddDebugVisibleBox( box );
				}
#endif
				++numVisibleTargets;
			}
		}

		if ( CpOccl::ProbeOn() )
		{
			CpOccl::Add( CpOccl::S_Considered, numTestedTargets );
			CpOccl::Add( CpOccl::S_OverallOut, numTestedTargets - numTargetsPassedOverallFrustumCulling );
			CpOccl::Add( CpOccl::S_GlobalOccl, cpGlobalOccl );
			CpOccl::Add( CpOccl::S_CascOut, ( numTargetsPassedOverallFrustumCulling - cpGlobalOccl ) - numTargetsPassedFrustumCulling );
			CpOccl::Add( CpOccl::S_RecvOccl, cpRecvOccl );
			CpOccl::Add( CpOccl::S_Visible, numVisibleTargets );
			CpOccl::Add( CpOccl::S_CascPasses, numFrustumsPassed );
			CpOccl::Add( CpOccl::S_Casc0, cpCasc[ 0 ] );
			CpOccl::Add( CpOccl::S_Casc1, cpCasc[ 1 ] );
			CpOccl::Add( CpOccl::S_Casc2, cpCasc[ 2 ] );
			CpOccl::Add( CpOccl::S_Casc3, cpCasc[ 3 ] );
			CpOccl::MaybePrint();
		}

#ifndef RED_CONFIGURATION_FINAL
		results.m_stats.m_numObjectsTested += numTestedTargets;
		results.m_stats.m_numObjectsVisible += numVisibleTargets;

		results.m_stats.m_multiFrustumNumTargetsTotal += numTargets;
		results.m_stats.m_multiFrustumNumTargetsPassedTodVis += numTargetsPassedTodVis;
		results.m_stats.m_multiFrustumNumTargetsTested += numTestedTargets;
		results.m_stats.m_multiFrustumNumTargetsPassedOverallFrusumCulling += numTargetsPassedOverallFrustumCulling;
		results.m_stats.m_multiFrustumNumTargetsPassedShadowOcclusion += numTargetsPassedShadowOcclusion;
		results.m_stats.m_multiFrustumNumTargetsPassedFrusumCulling += numTargetsPassedFrustumCulling;
		results.m_stats.m_multiFrustumNumTargetsVisible += numVisibleTargets;

		results.m_stats.m_multiFrustumNumFrustumsTested += numFrustumsTested;
		results.m_stats.m_multiFrustumNumFrustumsPassed += numFrustumsPassed;
#endif
	}

}
