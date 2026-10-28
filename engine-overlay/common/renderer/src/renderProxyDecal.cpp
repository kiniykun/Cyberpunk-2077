/**
* Copyright (c) 2012 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "renderCollector.h"
#include "renderMesh.h"
#include "renderDecalMesh.h"
#include "renderProxyDecal.h"
#include "renderShaderPair.h"
#include "renderTexture.h"
#include "renderScene.h"
#include "../../renderData/include/dynamicDecal.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/effectContextUpdateData.h"
#include "../../renderData/include/decalParams.h"
#include "renderMaterial.h"
#ifdef __EMSCRIPTEN__
// [cull] (renderProxyMesh.cpp overlay): far auto-hide limit for a static world decal; returns orig when unchanged.
extern "C" float cp2077_cull_decal_limit( float orig, float diag, int isEntityDecal, const char* materialName, unsigned materialNameLength );
#endif

#define MIN_DEPTH_FADE_MUL 1.0f
#define MAX_DEPTH_FADE_MUL 8.0f
#define STATIC_DEPTH_FADE_MUL 1024.0f		// Big enough that it shouldn't make static decals fade out when that wasn't intended.

namespace priv
{
	// regular objectType is held in its proper place, additional spots are reserved for batchDataIDs for decals with AllStatic and AllDynamic render modes
	static red::FixedArray< TBatchDataId, ERenderObjectType::ROT_Max+2 > s_batchDataId;
}

namespace Config
{
	TConfigVar< Float > cvDecalLookingAngleScalingFactor( "Rendering", "DecalLookingAngleScalingFactor", 0.1f, 0.0f, 1.0f, eConsoleVarFlag_Developer );
}

static SRenderStageContext decalRenderStageContext( MVF_Decal );

CRenderProxy_Decal::CRenderProxy_Decal( const RenderProxyInitInfo& info ) 
	: IRenderProxyDrawable( info )
	, m_alpha( 1.0f )
	, m_scale( 1.0f )
	, m_emissiveScale( 1.0f )
	, m_normalThreshold( 0 )
	, m_roughnessScale( 1 )
	, m_useVerticalProjection( false )
	, m_horizontalFlip( false )
	, m_verticalFlip( false )
	, m_isStretchingEnabled( false )
	, m_isNormalTresholdEnabled( true )
	, m_normalsBlendingMode( RenderDecalNormalsBlendingMode::AlphaBlending )
	, m_atlasVector( .5f ,.5f , .5f , .5f )
	, m_cachedSubUVClip( 0.0f, 0.0f, 1.0f, 1.0f )
	, m_tangent( Vector3::EX() )
	, m_bitangent( Vector3::EY() )
	, m_up( Vector4::EZ() )
	, m_Zdepthfade( STATIC_DEPTH_FADE_MUL )
	, m_orderNo( 0 )
	, m_surfaceType( ERenderObjectType::ROT_Static )
	, m_frameOverride( -1 )
	, m_decalRenderMode( EDecalRenderMode::DRM_AllStatic )
	, m_shouldCollectWithRayTracing( true )
{
	RED_FATAL_ASSERT( info.m_componentData && RPT_SSDecal == info.m_componentData->m_type, "Expects appropriate init data" );

	Vector4 effectScale = Vector4::ONES();

	if ( info.m_componentData && RPT_SSDecal == info.m_componentData->m_type )
	{
		const RenderProxyDecalComponentInitData * component = static_cast< const RenderProxyDecalComponentInitData* >( info.m_componentData );
		
		RED_ASSERT( component );
		if ( component->m_materialRenderResource )
		{
			m_staticMaterialInfo.m_materialPtr			= red::StaticCast< CRenderMaterial >( component->m_materialRenderResource );
			m_staticMaterialInfo.m_materialParameters	= red::StaticCast< CRenderMaterialParameters >( component->m_materialRenderResource );
		}
		else
		{
			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Rendering, "Creating RenderProxy_Decal with no material, expect problems." );
		}

		RED_FATAL_ASSERT( component->m_orderNo < ( 1 << (Uint32)BATCHMASK_PRIORITY_NUMBITS ), "Invalid decal order: %u, max is %u", (Uint32)component->m_orderNo, (Uint32)( ( 1 << (Uint32)BATCHMASK_PRIORITY_NUMBITS ) - 1 ) );

		m_autoHideDistance			= component->m_autoHideDistanceFar;

		RED_ASSERT( m_autoHideDistance > 0.f, "Invalid autohide for CRenderProxy_Decal: %1.3f", m_autoHideDistance );
#ifdef __EMSCRIPTEN__
		{
			// [cull] static decals already fade out over the last 20% of their auto-hide (CollectElements): lowering it
			// gives the same distance fade earlier. Entity-attached decals and emissive/sign materials are kept.
			red::StringView cpMat;
			if ( m_staticMaterialInfo.m_materialPtr ) cpMat = m_staticMaterialInfo.m_materialPtr->GetDisplayableName();
			m_autoHideDistance = cp2077_cull_decal_limit( m_autoHideDistance, component->m_boundingBox.CalcSize().Mag3(), component->m_isDynamicGeometryComponent ? 1 : 0,
				cpMat.Length() ? cpMat.Data() : nullptr, unsigned( cpMat.Length() ) );
		}
#endif

		m_horizontalFlip			= component->m_horizontalFlip;
		m_verticalFlip				= component->m_verticalFlip;
		m_alpha						= component->m_alpha;
		m_normalThreshold			= Float16Compressor::Compress( component->m_isNormalTresholdEnabled ? component->m_normalThreshold : -1.0f );
		m_roughnessScale			= Float16Compressor::Compress( component->m_roughnessScale );
		m_diffuseColorScale			= component->m_diffuseColorScale;
		m_orderNo					= component->m_orderNo;
		m_surfaceType				= component->m_surfaceType;
		effectScale					= component->m_effectScale;
		m_isStretchingEnabled		= component->m_isStretchingEnabled;
		m_isNormalTresholdEnabled	= component->m_isNormalTresholdEnabled;
		m_normalsBlendingMode		= component->m_normalsBlendingMode;
		m_frameOverride				= component->m_frameOverride;
		m_decalRenderMode			= component->m_decalRenderMode;
		m_shouldCollectWithRayTracing= component->m_shouldCollectWithRayTracing;
	}

	m_initialTransform = rend::RenderProxyTransform::SLOW_FromMatrix( GetLocalToWorld() );

	CacheProjectionMatrix( m_initialTransform.GetScale() * effectScale );

	{
		SProxyBatchData batchData;
		batchData.SetProxy( this );
		m_proxyBatchDataId = GetRenderer()->GetBatchDataAllocator().GetProxyData().Allocate( batchData );
	}

#ifdef PRECACHE_PSO
	PrecachePSOs();
#endif // PRECACHE_PSO
}

CRenderProxy_Decal::CRenderProxy_Decal( const SDynamicDecalInitInfo& decalInfo )
	: BaseClass( RenderProxyInitInfo() )
	, m_alpha( 1.0f )
	, m_scale( 1.0f )
	, m_emissiveScale( 1.0f )
	, m_horizontalFlip( false )
	, m_verticalFlip( false )
	, m_isNormalTresholdEnabled( true )
	, m_atlasVector( decalInfo.m_atlasVector )
	, m_cachedSubUVClip( 0.0f, 0.0f, 1.0f, 1.0f )
	, m_tangent( Vector3::EX() )
	, m_bitangent( Vector3::EY() )
	, m_up( Vector3::EZ() )
	, m_Zdepthfade( math::Lerp( MAX_DEPTH_FADE_MUL, MIN_DEPTH_FADE_MUL, decalInfo.m_depthFadePower ) )
	, m_cachedProjection( Matrix::ZEROS() )
	, m_frameOverride( -1 )
	, m_shouldCollectWithRayTracing( true )
{
	Matrix templ2w = decalInfo.GetDecalToWorldMatrix();
	// Scale up in this case, because the SS decal mesh has vertices in range [-0.5, 0.5], instead of [-1,1].
	// Swap X/Z because SS decals project along Z, while dyn decals project along Y.
	Matrix finall2w;
	finall2w.SetRow( 0, templ2w.GetRow( 0 )*2.f );
	finall2w.SetRow( 1, templ2w.GetRow( 2 )*2.f );
	finall2w.SetRow( 2, -templ2w.GetRow( 1 )*2.f );
	finall2w.SetRow( 3, templ2w.GetRow( 3 ) );

	SetInstanceMatrix( finall2w );

	SetBoundingBox( decalInfo.GetWorldBounds() );

	m_normalThreshold = Float16Compressor::Compress( decalInfo.m_normalThreshold );
	m_roughnessScale = Float16Compressor::Compress( decalInfo.m_roughnessScale );

	// SS decals use a hard threshold, but dynamic decals fade out. We'll just pick the mid-point of the fading as our threshold.
	// m_normalThreshold = /*MAcos*/( Clamp( 0.5f / decalInfo.m_normalFadeScale - decalInfo.m_normalFadeBias, -1.0f, 1.0f ) );
	if ( decalInfo.m_staticMaterialInfo.m_material )
	{
		m_staticMaterialInfo.m_materialParameters	= red::StaticCast< CRenderMaterialParameters >( decalInfo.m_staticMaterialInfo.m_material );
		m_staticMaterialInfo.m_materialPtr			= red::StaticCast< CRenderMaterial >( decalInfo.m_staticMaterialInfo.m_material );
	}
	else
	{
		RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Rendering, "Creating RenderProxy_Decal with no static material info, expect problems." );
	}

	m_autoHideDistance = decalInfo.m_autoHideDistance;
	RED_ASSERT( m_autoHideDistance > 0.f, "Invalid autohide for CRenderProxy_Decal: %1.3f", m_autoHideDistance );

	m_initialTransform = rend::RenderProxyTransform::SLOW_FromMatrix( GetLocalToWorld() );

	CacheProjectionMatrix( m_initialTransform.GetScale() );

	{
		SProxyBatchData batchData;
		batchData.SetProxy( this );
		m_proxyBatchDataId = GetRenderer()->GetBatchDataAllocator().GetProxyData().Allocate( batchData );
	}

#ifdef PRECACHE_PSO
	PrecachePSOs();
#endif // PRECACHE_PSO
}

CRenderProxy_Decal::~CRenderProxy_Decal()
{
	GetRenderer()->GetBatchDataAllocator().GetProxyData().Release( m_proxyBatchDataId );
}

void CRenderProxy_Decal::PrecachePSOs()
{
	if ( m_staticMaterialInfo.IsValid() )
	{
		static GpuApi::VertexLayoutRef decalBoxVertexLayoutRef = CRenderDecalMesh::GetDecalBoxVertexLayout();		
		m_staticMaterialInfo.m_materialPtr->CreateDefaultPSO( decalRenderStageContext, decalBoxVertexLayoutRef );
	}
}

void CRenderProxy_Decal::UpdateEffectContext( const SDecalEffectContextUpdate& update )
{
	SetAlpha( update.m_alpha );
	if ( update.m_updateScale )
	{
		SetEffectScale( update.m_scale );	
	}
	if ( update.m_updateEmissiveScale )
	{
		m_emissiveScale = update.m_emissiveScale;
	}
}

void CRenderProxy_Decal::SetEffectScale( Vector4 scale )
{
	rend::RenderProxyTransform newLocalToWorld = m_initialTransform;
	newLocalToWorld.scale *= scale;
	const Box worldBox = newLocalToWorld.TransformBox( Box( Vector4::ZEROS(), 0.5f ) );
	Relink( worldBox, newLocalToWorld );
}

TBatchDataId CRenderProxy_Decal::GetBindBatchDataIndex( CGeometryBatchDataAllocator &allocator )
{
	Uint32 batchDataIdx = 0;
	ERenderObjectType surfaceType = ROT_Max;
	switch ( GetRenderMode() )
	{
		case EDecalRenderMode::DRM_AllStatic:	
			surfaceType = ROT_STATIC_LAST;
			batchDataIdx = ROT_Max;
			break;
		case EDecalRenderMode::DRM_AllDynamic:	
			surfaceType	= ROT_DYNAMIC_FIRST;
			batchDataIdx = ROT_Max + 1;
			break;
		case EDecalRenderMode::DRM_ObjectType:
			surfaceType = m_surfaceType;
			batchDataIdx = m_surfaceType;
			break;
		default:
			RED_FATAL( "Invalid decal render mode: %u", (Uint32)GetRenderMode() );
	}

	SGeometryBatchData batchData;
	batchData.m_type = GT_Decal;
	batchData.m_decalData.m_surfaceType = surfaceType;

	TBatchDataId& batchDataId = priv::s_batchDataId[ batchDataIdx ];

	if ( allocator.DataInitRequest( batchDataId, batchData, false ) )
	{
		const SGeometryBatchData& batchData = allocator.GetData( batchDataId.GetValue() );
		RED_FATAL_ASSERT( batchData.m_type == GT_Decal );
		RED_FATAL_ASSERT( batchData.m_decalData.m_surfaceType == surfaceType );
	}

	return batchDataId;
}



void CRenderProxy_Decal::CollectElements( SUpdateContext& ctx )
{
#ifdef USE_RAY_TRACING
	if ( Config::GGlobalRenderingSettings.allowRaytracing && !m_shouldCollectWithRayTracing )
	{
		return;
	}
#endif // USE_RAY_TRACING

	auto& collector = ctx.collector;
	if ( !collector.m_cameraInfo->GetRenderingMask().Test( rend::DF_RenderMask_Decals ) )
	{
		return;
	}

	if ( !m_staticMaterialInfo.IsValid() )
	{
		return;
	}

	// If we're totally faded out, no point in drawing.
	// Or projection matrix is not set
	if ( m_alpha <= 0.0f || m_cachedProjection[3][3] < 1.0f )
	{
		return;
	}

	if ( m_proxyBatchDataId == UINT32_MAX )
	{
		return;
	}

	const Vector4 positionToCamera = ctx.cameraPosition - GetTranslation();

	// backface culling
	if ( m_isNormalTresholdEnabled )
	{
		// shift by the decal depth so it's not disappearing too quickly at grazing angle
		const Vector4 scaledUp = GetInstanceMatrix().GetAxisZ();
		if ( GetUp().Dot3( positionToCamera + scaledUp ) < 0.0f )
		{
			return;
		}
	}

	// reduce autohide at grazing angles
	const Vector4 viewDir = positionToCamera.Normalized3();
	const Float NdotL = Abs( GetUp().Dot3( viewDir ) );
	const Float lookingAngleScaling = 1.0f - Config::cvDecalLookingAngleScalingFactor.Get() * ( 1.0f - NdotL );
	
	const Float squaredAutohide = lookingAngleScaling * lookingAngleScaling * GetAutoHideDistanceSquared();
	const Float squaredDistanceToCamera = positionToCamera.SquareMag3();
	if ( squaredDistanceToCamera > squaredAutohide )
	{
		return;
	}
	
	// compute autohide fade
	const Float autoHideRamp = 0.64f; // = 0.8f * 0.8f; // 80%
	const Float fade = 1.0f - ::Clamp( ( squaredDistanceToCamera - autoHideRamp * squaredAutohide ) / ( squaredAutohide * ( 1.0f - autoHideRamp ) ), 0.0f, 1.0f );
	if ( fade <= 0.0f )
	{
		return;
	}

	// collected, update stats
#ifndef RED_CONFIGURATION_FINAL
	ctx.collector.m_renderingStats.m_collectedDecalProxies.Increment();
#endif // RED_CONFIGURATION_FINAL

	// Update once per frame
	UpdateOncePerFrame( ctx );

	CBatchDataAllocator& batchDataAllocator = GetRenderer()->GetBatchDataAllocator();

	CRenderMaterial* material;
	CRenderMaterialParameters* materialParameters;
	m_staticMaterialInfo.GetMaterialAndParams( material, materialParameters );

	FeatureFlagsMask mask;
	mask.Set( collector.m_baseFeatureFlag );
#ifndef RED_CONFIGURATION_FINAL
	mask.Set( FEATFLAG_Overdraw );
#endif // RED_CONFIGURATION_FINAL
	
	switch ( GetRenderMode() )
	{
		case EDecalRenderMode::DRM_AllStatic:	mask.Set( FEATFLAG_DecalsOnStaticObjects );		break;
		case EDecalRenderMode::DRM_AllDynamic:	mask.Set( FEATFLAG_DecalsOnDynamicObjects );	break;
		case EDecalRenderMode::DRM_ObjectType:	/* do nothing, proper flags are set */			break;
	}

	TRenderStagePasses passes;
	material->GetRenderPassesForFeatureFlags( batchDataAllocator, mask, decalRenderStageContext, materialParameters, passes );

	const TBatchDataId geoBatchDataIndex = GetBindBatchDataIndex( batchDataAllocator.GetGeometryData() );
	if ( !geoBatchDataIndex.IsValid() )
	{
		// overflow in geometry batch data, skip the decal so no invalid chunks are created
		return;
	}

	RenderBatchData commonBatchDataMask;

	for ( const auto &pass : passes )
	{
		if ( !commonBatchDataMask.IsValid() )
		{
			Uint32 instanceOffset = batchDataAllocator.GetInstanceData().Allocate( m_instanceMatrix );
			if ( batchDataAllocator.GetInstanceData().CheckOverflowValue( instanceOffset ) )
			{
				continue;
			}
			commonBatchDataMask.Clear();
			commonBatchDataMask.SetProxy( m_proxyBatchDataId );
			commonBatchDataMask.SetInstanceOffset( instanceOffset );
			commonBatchDataMask.SetInstanceCount( 1 );
			commonBatchDataMask.SetCustomData( static_cast< Uint8 >( fade * 255.0f ) );
		}

		SRenderChunk &newChunk = collector.m_renderStageCollector.AddChunk( pass.m_stageId, SRenderStageCluster::Default() );
		newChunk.m_key = pass.m_renderChunkKey;
		newChunk.m_key.SetGeometry( geoBatchDataIndex.GetValue() );
		newChunk.m_key.SetPriority( m_orderNo );
		RED_FATAL_ASSERT( newChunk.m_key.GetDissolveMode() == DISSOLVEMODE_Opaque, "Key expected to be by default setup to opaque dissolve, and able to bitwiseOr any other dissolve mode" );
		newChunk.m_data = pass.m_renderChunkData | commonBatchDataMask;
	}
}

Bool CRenderProxy_Decal::Relink( const Box& boundingBox, const rend::RenderProxyTransform& localToWorld )
{
	const Bool visGridMoveNeeded = BaseClass::Relink( boundingBox, localToWorld );
	CacheProjectionMatrix( GetLocalToWorld().GetScale33() );
	return visGridMoveNeeded;
}

void CRenderProxy_Decal::RelinkTransformOnly( const rend::RenderProxyTransform& localToWorld )
{
	BaseClass::RelinkTransformOnly( localToWorld );
	CacheProjectionMatrix( GetLocalToWorld().GetScale33() );
}

void CRenderProxy_Decal::RelinkBBoxOnly( const Box& boundingBox )
{
	BaseClass::RelinkBBoxOnly( boundingBox );
	CacheProjectionMatrix( GetLocalToWorld().GetScale33() );
}

void CRenderProxy_Decal::CacheProjectionMatrix( const Vector4& scale )
{
	// Sometimes scale is very small (track Item curves etc). 
	if(scale.X < 1e-2f || scale.Y < 1e-2f || scale.Z < 1e-2f)
	{
		m_cachedProjection = Matrix::ZEROS();
		return;
	}

	// calculation of the decalWorldToUV matrix, idea:
	// 1) we have the world space positions of the pixels
	// 2) we have to build view matrix in a way, that camera will be on the "up" side of the box, facing towards the center
	// 3) projection matrix is the ortho matrix with width, height, near and far being the appropriate sizes of the box

	Matrix l2w = GetLocalToWorld();
	Vector4 position = GetTranslation() + l2w.GetAxisZ().Normalized3() * scale.Z / 2.0f;

	Matrix orthoCameraRotation = Matrix::IDENTITY();
	orthoCameraRotation[0] = l2w.GetAxisX().Normalized3();
	orthoCameraRotation[1] = -l2w.GetAxisZ().Normalized3();
	orthoCameraRotation[2] = l2w.GetAxisY().Normalized3();

	Float orthoW = scale.X;
	Float orthoH = scale.Y;
	Float nearPlane = 0.0f;
	Float farPlane = scale.Z;

	// world to screen from render camera
	CRenderCamera::CreationContext context;
	{
		context.position = WorldPosition( position );
		context.rotation = orthoCameraRotation.ToQuat();
		context.fov = 0.0f;
		context.aspect = orthoW / orthoH;
		context.nearPlane = nearPlane;
		context.farPlane = farPlane;
		context.zoom = orthoH;
	}
	CRenderCamera cam( context );
	Matrix wToS_RC = cam.GetWorldToScreen();

	Matrix clipSpaceToTextureSpace = Matrix::IDENTITY();
	clipSpaceToTextureSpace[0].X = m_horizontalFlip ? -m_atlasVector.X : m_atlasVector.X;
	clipSpaceToTextureSpace[1].Y = m_verticalFlip ? -m_atlasVector.Y : m_atlasVector.Y;
	clipSpaceToTextureSpace.SetTranslation( m_atlasVector.Z, m_atlasVector.W, 0.0f );

	m_cachedProjection = wToS_RC * clipSpaceToTextureSpace;

	// Cache clipping sub uv. This tells the shader the real using atlas's tile part
	// so he can cut off the pixels outside the tile. ZW is the middle if the tile , 
	// XY is the half size of the tile.
	m_cachedSubUVClip.X = m_atlasVector.Z - m_atlasVector.X;
	m_cachedSubUVClip.Y = m_atlasVector.W - m_atlasVector.Y;

	m_cachedSubUVClip.Z = m_atlasVector.Z + m_atlasVector.X;
	m_cachedSubUVClip.W = m_atlasVector.W + m_atlasVector.Y;

	m_tangent = ( m_horizontalFlip ? -orthoCameraRotation[ 0 ] : orthoCameraRotation[ 0 ] ).Normalized3();
	m_bitangent	= ( m_verticalFlip ? orthoCameraRotation[ 2 ] : -orthoCameraRotation[ 2 ] ).Normalized3();

	m_up		= -orthoCameraRotation[1];
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CRenderProxy_DynamicDecal::CRenderProxy_DynamicDecal( const RenderProxyInitInfo& info )
	: IRenderProxyDrawable( info )
	, m_spawned( false )
	, m_dynamicDecal( nullptr )
{
	RED_ASSERT( info.m_componentData, "Expects appropriate init data" );
	RED_ASSERT( RPT_DynamicDecalSpawner == info.m_componentData->m_type, "Expected RPT_DynamicDecalSpawner and got: %d", info.m_componentData->m_type );
	
	const RenderProxyDynamicDecalSpawnerComponentInitData* component = static_cast< const RenderProxyDynamicDecalSpawnerComponentInitData* >( info.m_componentData );
	RED_ASSERT( component );
	RED_ASSERT( component->m_materialRenderResource, "Dynamic decal spawner with no material provided!" );
	m_renderMaterial.m_materialPtr			= red::StaticCast< CRenderMaterial >( component->m_materialRenderResource);
	m_renderMaterial.m_materialParameters	= red::StaticCast< CRenderMaterialParameters >( component->m_materialRenderResource);
	m_initInfo								= component->m_dynamicDecalInitInfo;
	m_autoHideDistance						= component->m_autoHideDistanceFar;
	RED_ASSERT( m_autoHideDistance > 0.f, "Invalid autohide for CRenderProxy_DynamicDecal: %1.3f", m_autoHideDistance );
}

CRenderProxy_DynamicDecal::CRenderProxy_DynamicDecal( const SDynamicDecalInitInfo& decalInfo )
	: BaseClass( RenderProxyInitInfo() )
	, m_spawned( false )
	, m_dynamicDecal( nullptr )
{
	m_initInfo			= decalInfo;
	SetBoundingBox( decalInfo.GetWorldBounds() );
	RED_ASSERT( m_autoHideDistance > 0.f, "Invalid autohide for CRenderProxy_DynamicDecal: %1.3f", m_autoHideDistance );
	m_autoHideDistance	= decalInfo.m_autoHideDistance;
	if ( decalInfo.m_skinnedMaterialInfo.m_material )
	{
		m_renderMaterial.m_materialParameters	= red::StaticCast< CRenderMaterialParameters >( decalInfo.m_skinnedMaterialInfo.m_material );
		m_renderMaterial.m_materialPtr			= red::StaticCast< CRenderMaterial >( decalInfo.m_skinnedMaterialInfo.m_material );
	}
}

CRenderProxy_DynamicDecal::~CRenderProxy_DynamicDecal()
{
	if ( m_dynamicDecal )
	{
		m_dynamicDecal->OnParentProxyDestroyed();
	}
}

void CRenderProxy_DynamicDecal::SpawnDecal( CRenderSceneEx* scene )
{
	if ( m_spawned )
	{
		return;
	}
	if ( !scene )
	{
		return;
	}

	const Float worldBoundsDiagonal = m_initInfo.GetWorldBounds().CalcSize().Mag3();

	SDynamicDecalInitInfo initInfo = m_initInfo;
	initInfo.m_timeToLive						= std::numeric_limits< Float >::infinity();
	initInfo.m_projectOnSkinned					= true;
	initInfo.m_skinnedMaterialInfo.m_material	= m_renderMaterial.m_materialPtr;
	initInfo.m_parentDecal						= this;
	initInfo.m_applyInLocalSpace				= false;

	if ( TRenderPtr< IRenderResource > decalRenderResource = GetRenderer()->CreateDynamicDecal( initInfo ) )
	{
		TRenderPtr< CRenderDynamicDecal > decal = red::StaticCast< CRenderDynamicDecal >( decalRenderResource );
		m_dynamicDecal = decal;
		scene->QueueDynamicDecalSpawn( decal, false, true );
		m_spawned = true;
	}
}

void CRenderProxy_DynamicDecal::AttachToScene( CRenderSceneEx* scene )
{
	BaseClass::AttachToScene( scene );

	if ( m_initInfo.m_isDismembermentDecal )
	{
		SpawnDecal( scene );
	}
}

void CRenderProxy_DynamicDecal::CollectElements( SUpdateContext& ctx )
{
	auto& collector = ctx.collector;
	if ( CRenderSceneEx* scene = const_cast< CRenderSceneEx* >( collector.GetScene() ) )
	{
		SpawnDecal( scene );
	}
}

