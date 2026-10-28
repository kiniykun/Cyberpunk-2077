/**
* Copyright (c) 2017-2018 CD Projekt Red. All Rights Reserved.
*/
#ifndef REFLECTION_PROBE_MANAGER_H
#define REFLECTION_PROBE_MANAGER_H

#include "renderFrameCamera.h"
#include "../../renderData/include/miniVec.h"
#include "../../renderData/include/envProbeResource.h"

class CRenderProxy_ReflectionProbe;
class CReflectionProbeStage;

namespace EnvManager
{
	struct CubemapSlot
	{
		CubemapSlot();

		~CubemapSlot();

		// - - - - - - - - - - - - - - - - - - - - -

		Bool IsValid() const;

		// - - - - - - - - - - - - - - - - - - - - -

		GpuApi::Texture						m_textureData;
		res::ResourceTokenHandle			m_loadingToken;

		Uint64								m_dataHash{ 0 };
		Uint16								m_refCount{ 1 };

		Bool								m_haveSkyData : 1;
		Float								m_faceDepth[6];
	};

	struct ProbeSlot
	{
		CReflectionProbeStage*				m_processing;
		IRenderScene*						m_scene;
		Uint64								m_dataHash;
		EngineTime							m_lastRelightTime;
		math::Vector3						m_position;
		math::Vector3						m_size;
		math::Vector3						m_refereneOffset;
		math::Vector3						m_edgeScale;
		math::Quaternion					m_rotation;
		EngineTime							m_lastBlendTime;
		EngineTime							m_firstBlendTime;
		Box									m_bbox;
		Float								m_emissiveScale;
		Float								m_skyScale;
		Float								m_blendSpeed;
		Vector3								m_fogColor;
		Float								m_fogDensity;
		Int8								m_brightnessEVClamp;
		envUtils::NeighborMode				m_neighborMode : 8;
		RenderSceneLayerMask				m_sceneLayerMask;
		Uint16								m_lightChannels;
		Uint16								m_volumeChannels;
		Uint8								m_priority;
		Bool								m_fullRelightAtOnce : 1;	// Instead of updating across multiple frames, update all at once.
		Bool								m_isGlobalProbe : 1;
		Bool								m_isBoxProjection : 1;
		Bool								m_ready : 1;
		Bool								m_allInShadow : 1;
		Bool								m_hideSkyColor : 1;
		Bool								m_noFadeBlend : 1;
		Bool								m_subScene : 1;
		Bool								m_volFogAmbient : 1;
		Bool								m_movableXY : 1;
		Bool								m_movableZ : 1;
		Bool								m_interior : 1;
		Bool								m_streamed : 1;

		// - - - - - - - - - - - - - - - - - - - - -

		ProbeSlot();

		// - - - - - - - - - - - - - - - - - - - - -

		RED_FORCE_INLINE Bool IsFullRelightNeeded() const { return !m_ready || m_fullRelightAtOnce; }

		RED_FORCE_INLINE Bool IsMovable() const { return m_movableXY || m_movableZ; }

		// - - - - - - - - - - - - - - - - - - - - -

		// Reset slot to some initial state
		void Reset();

		// - - - - - - - - - - - - - - - - - - - - -

	};

	class ProbeId
	{
	public:
		static ProbeId Invalid();
		static ProbeId FromProbe( const CRenderProxy_ReflectionProbe& probe );
		static ProbeId GlobalFallbackProbe();

		red::THash32 CalcHash() const;

	private:
		friend Bool operator==( ProbeId lhs, ProbeId rhs );
		friend Bool operator!=( ProbeId lhs, ProbeId rhs );
		friend Bool operator<( ProbeId lhs, ProbeId rhs );

		ProbeId( MemUint val );
		ProbeId( const CRenderProxy_ReflectionProbe& probe );

		MemUint m_val;
	};
}

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//


class CEnvProbeAtlas
{
	RED_USE_MEMORY_POOL( rend::PoolReflectionProbes );

public:

	enum EFormat
	{
		eRGB,
		eCOMPRESSED
	};

private:

	Uint32 						m_size;
	Uint32						m_lastVolumeHash;

	GpuApi::Texture 			m_texture;
	GpuApi::Buffer				m_envScrapBufferIrr;
	GpuApi::Buffer				m_envBlendedBufferIrr;
	GpuApi::Buffer				m_envLuminance;

#ifdef USE_RAY_TRACING
	GpuApi::Buffer				m_envScrapNoSkyBufferIrr;
	GpuApi::Buffer				m_envBlendedNoSkyBufferIrr;
#endif

	Uint16 						m_numMipmaps;
	EFormat						m_format;

	// Per probe.
	struct SEntry
	{
		EngineTime				m_lastRenderTime;
		Float					m_blendScale;
		EnvManager::ProbeId		m_probeId;

		SEntry()
			: m_lastRenderTime( EngineTime::ZERO )
			, m_blendScale( -1.0f )
			, m_probeId( EnvManager::ProbeId::Invalid() )
		{}

	};

	red::DynArray<SEntry>			m_slots{ rend::PoolReflectionProbes() };

public:

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	CEnvProbeAtlas( EFormat format, Uint32 numSlices, Uint32 faceSize, Uint16 mips = 1 );

	~CEnvProbeAtlas();

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	inline Uint32	GetNumFaces() const { return 1; }

	inline Uint32	GetNumSlices() const { return m_slots.Size(); }

	inline mini::uvec2 GetSize() const { return mini::uvec2( m_size * 2, m_size ); }

	inline Uint32	GetNumMips() const { return m_numMipmaps; }

	inline GpuApi::TextureRef	GetTexture() const { return m_texture; }

	inline GpuApi::BufferRef	GetIrradianceSH_Internal() const { return m_envScrapBufferIrr; }
	inline GpuApi::BufferRef	GetIrradianceSH() const { return m_envBlendedBufferIrr; }

#ifdef USE_RAY_TRACING
	inline GpuApi::BufferRef	GetIrradianceNoSkySH_Internal() const { return m_envScrapNoSkyBufferIrr; }
	inline GpuApi::BufferRef	GetIrradianceNoSkySH() const { return m_envBlendedNoSkyBufferIrr; }
#endif

	inline GpuApi::BufferRef	GetLuminance() const { return m_envLuminance; }

	inline Uint32	GetTextureExtent() const { return 1 << (m_numMipmaps - 1); }

	inline Uint32	GetTextureResolution( Uint32 baseRes ) const { return baseRes + 2 * GetTextureExtent(); }

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	inline Bool		IsSlotActive( Uint32 idx ) const
	{
		return m_slots[idx].m_probeId != EnvManager::ProbeId::Invalid();
	}

	inline EngineTime	GetLastRenderTime( Uint32 idx ) const
	{
		return m_slots[idx].m_lastRenderTime;
	}

	inline Int32	FindSlotID( EnvManager::ProbeId probeId ) const
	{
		Int32 ret = -1;
		for( Uint32 idx = 1; idx < GetNumSlices(); ++idx )
		{
			if( m_slots[idx].m_probeId == probeId ){ ret = idx; break; }
		}
		return ret;
	}

	inline Int32	FindSlotID( EnvManager::ProbeId probeId, EngineTime now )
	{
		Int32 ret = -1;
		for( Uint32 idx = 1; idx < GetNumSlices(); ++idx )
		{
			if( m_slots[idx].m_probeId == probeId )
			{
				ret = idx; 
				break; 
			}
		}
		return ret;
	}

	// We should get both Probe* and dataHash as args.
	inline Int32	FindSlotID_Alloc( EnvManager::ProbeId probeId, Bool* allocated = nullptr )
	{
		Int32 ret = FindSlotID( probeId );
		if( ret < 0 ) // if slot not found then find free slot and use it
		{
			for( Uint32 idx = 1; idx < GetNumSlices(); ++idx )
			{
				if( m_slots[idx].m_probeId == EnvManager::ProbeId::Invalid() )
				{
					if(allocated) *allocated = true;

					m_slots[idx].m_probeId = probeId;
					ret = idx;
					break;
				}
			}
		}
		return ret;
	}

	inline void		FreeSlot( EnvManager::ProbeId probeId )
	{
		for( Uint32 idx = 1; idx < GetNumSlices(); ++idx )
		{
			if( m_slots[idx].m_probeId == probeId )
			{
				m_slots[idx] = SEntry();
				break;
			}
		}
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	Bool			Render( const SRenderNodeImplContext& rctx, GpuApi::TextureRef intermediate, GpuApi::TextureRef cube, const math::Vector3& boxSize, EngineTime lastCubeRenderTime, Uint32 atlasID, Uint8* mipLvl = nullptr, Bool flipZ = false );

	Bool			BlendOver( const SRenderNodeImplContext& rctx, GpuApi::TextureRef intermediate, Float dt, Uint32 atlasID = 0 );

	Float			MipToRoughness( Uint32 mip ) const;

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

protected:


	void			Generate();
};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

class CEnvProbeRelighter
{
	RED_USE_MEMORY_POOL( rend::PoolReflectionProbes );

public:
	static const Float			FULL_RANGE_ENVMAP_SCALE;
	static const Float			REDUCED_RANGE_ENVMAP_SCALE;

private:
	static constexpr Uint32		MAX_LIGHTS_PER_PROBE = 64u;		//< Max lights to contribute when relighting a single probe

	GpuApi::Texture			m_litCubemap;
	GpuApi::Texture			m_luminance;
	GpuApi::Texture			m_skyCubemap;
	GpuApi::Texture			m_skyParabolid;
	GpuApi::Texture			m_hiResSkyCubemap;
	GpuApi::Texture			m_skyLUT;
	Uint8					m_side;
	Uint16					m_size;

	bool					m_useHiResSkyCubemap;
	Float					m_skyRadianceScale;

	GpuApi::Buffer			m_localLights[6];		//< Local lights that fit in the bounding box of our probe
	Uint32					m_localLightBufferIdx;

public:

	CEnvProbeRelighter( Uint32 size, bool needsHiResSkyTexture );

	~CEnvProbeRelighter();

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	void							PrepareLiTexture( Bool useSkyChannel );

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	inline GpuApi::TextureRef		GetLitTexture() const { return m_litCubemap; }

	inline GpuApi::TextureRef		GetLuminance() const { return m_luminance; }

	inline GpuApi::TextureRef		GetSkyTexture() const { return m_useHiResSkyCubemap ? m_hiResSkyCubemap : m_skyCubemap ; }

	inline GpuApi::TextureRef		GetSkyParaboloidTexture() const { return m_skyParabolid ; }

	inline GpuApi::TextureRef		GetSkyLUT() const { return m_skyLUT; }

	bool							IsHiResSkyCubemap() const { return m_useHiResSkyCubemap; }

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	struct Neighbors
	{
		Uint8				atlasID;
		math::Quaternion	rotation;
		math::Vector3		size;
	};

	Bool							RelightProbe( const SRenderNodeImplContext& rctx, const GpuApi::TextureRef probeData, const Neighbors* neighbors, const EnvManager::ProbeSlot& slot,
												  GpuApi::TextureRef envProbe = GpuApi::TextureRef::Null(), 
												  GpuApi::BufferRef envLuminance = GpuApi::BufferRef::Null(), 
												  GpuApi::BufferRef irradianceSH = GpuApi::BufferRef::Null() );

	void							GenerateMipmap();

	void							UseExtendedDynamicRangeSky( bool enable );

	Float							GetEnvmapRadianceScale() const { return m_skyRadianceScale; }

private:

	Uint32							CollectLocalLights( const SRenderNodeImplContext& rctx, const EnvManager::ProbeSlot& slot );

	Float							GetSkyIrradianceScale( const SRenderNodeImplContext& rctx ) const;
};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

class CReflectionProbeStage
{

	RED_USE_MEMORY_POOL( rend::PoolReflectionProbes );

public:

	enum EStage
	{
		eIDLE,
		eRELIGHT_PROBE,
		eGENERATE_MIPMAP,
		eRENDER_PROBE,
		eRENDER_GLOBAL_PROBE,
		eBLEND_OVER_PROBE,
		eFULL_PREWARM
	};

	GpuApi::Texture	 			m_intermediate;


	// Probe update stage related stuff below. A single probe will be undergoing an update process during a number of frames.
	EStage						m_stage;				//< What stage of the update are we in right now?
	EnvManager::ProbeId			m_stageProbeId;			//< Id of a probe being updated.
	Uint8						m_stageConvolution;		//< ?
	Uint8						m_atlasID;				//< Id of a CEnvProbeAtlas slot associated with probe being updated.
	Uint8						m_inQueueTimer;
	Bool						m_isGlobalProbe : 1;
	Bool						m_isSubscene : 1;
	Float						m_blendOverSpeed;

public:

	CReflectionProbeStage( mini::uvec2 size, Uint32 numMips );

	~CReflectionProbeStage();

	// - - - - - - - - - - - - - - - - - - - - - - - - - -

	inline GpuApi::TextureRef	GetIntermediate() const { return m_intermediate; }

	void						Release();

	// - - - - - - - - - - - - - - - - - - - - - - - - - -


};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

class CReflectionProbeManager
{
	RED_USE_MEMORY_POOL( rend::PoolReflectionProbes );
public:

	static constexpr Uint32					PROBE_SIZE				= 128;

	static constexpr Uint32					MAX_NUM_PROBES			= 32u;
	static constexpr Uint32					TILE_SIZE				= 32u;
	static constexpr Uint32					NUM_ENV_PER_TILE		= 16u;
	static constexpr Uint32					NUM_ENV_IND_PER_TILE	= NUM_ENV_PER_TILE + 1u;

private:

	red::DynArray< CRenderProxy_ReflectionProbe* >				m_localProbes{ rend::PoolReflectionProbes() };		// Synchronized using m_probesLock.
	red::DynArray< CRenderProxy_ReflectionProbe* >				m_globalProbes{ rend::PoolReflectionProbes() };		// Synchronized using m_probesLock.
	red::RWSpinLock												m_probesLock;

	red::Map< EnvManager::ProbeId, EnvManager::ProbeSlot >		m_probeSlots{ rend::PoolReflectionProbes() };		// Synchronized using m_slotsLock.
	red::Map< Uint64, EnvManager::CubemapSlot >					m_cubemapSlots{ rend::PoolReflectionProbes() };		// Synchronized using m_slotsLock.
	mutable red::RWSpinLock										m_slotsLock;

	red::Atomic<Uint32>											m_numRunningLoads;
	
	using TStreamingGroup = std::tuple<CRenderSceneEx*, RenderSceneLayer, Bool>;
	red::StaticArray< TStreamingGroup, 16 >						m_streamingGroups;
	red::RWSpinLock												m_streamingLock;

	red::Atomic<Bool>											m_lastFrameHadSubScene, m_currFrameHasSubScene;


	Int32														m_overflow;

	SFrameTracker												m_updateTracker;

public:

	CReflectionProbeManager();

	~CReflectionProbeManager();

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

	RED_FORCE_INLINE const red::Map< EnvManager::ProbeId, EnvManager::ProbeSlot >& GetProbeSlots() const { return m_probeSlots; }

	RED_FORCE_INLINE Uint32				GetNumGlobalProbes() const { return m_globalProbes.Size(); }

	RED_FORCE_INLINE Uint32				GetNumLocalProbes() const { return m_localProbes.Size(); }

	RED_FORCE_INLINE const EnvManager::ProbeSlot*	GetSlot( EnvManager::ProbeId probeId ) const
	{
		auto it = m_probeSlots.Find( probeId );
		return it != m_probeSlots.End() ? &it.Value() : nullptr;
	}

	RED_FORCE_INLINE Int32				WasOverflowLastFrame() { return m_overflow; }

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

	void		RegisterCubemap( CRenderProxy_ReflectionProbe& probe );

	void		UnregisterCubemap( CRenderProxy_ReflectionProbe& probe );

	void		RegisterProbe( CRenderProxy_ReflectionProbe& probe );

	void		UnregisterProbe( CRenderProxy_ReflectionProbe& probe );

	struct RetgisterData
	{
		Uint64	dataHash;
		Bool	haveSkyData;
		Float	faceDepth[6];
	};

	Bool		RegisterCubemap( const RetgisterData& regData, const THandle< IRenderResourceBlob >& blob, res::ResourcePath path );

	Bool		RegisterCubemap_LEGACY( const RetgisterData& regData, const DataBuffer& data );

	Bool		ReloadCubemap( const TResAsyncRef<CReflectionProbeDataResource>& resourceRef, Uint64 dataHash );

	// OnRelinkProbe is used to update the probe's bbox on the manager side. Manager needs that bbox, so it can figure out what lights should contribute to a relight.
	void		OnRelinkProbe( CRenderProxy_ReflectionProbe& probe );

	void		InvalidateProbes( CRenderSceneEx* scene );

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

	void		RenderSkyScattering( const struct SRenderNodeImplContext& rctx );

	void		RenderDebug( const struct SRenderNodeImplContext& rctx );

	Bool		TickStreaming( const SRenderNodeImplContext& rctx );

	void		ResolveStreaming();

	void		Tick( const struct SRenderNodeImplContext& rctx );

	void		CullProbes( const struct SRenderNodeImplContext& rctx );

	void		PrepareDummyData( const struct SRenderNodeImplContext& rctx );

	void		CleanupStage( CReflectionProbeStage* stage, Bool useLock );

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

	void		BuildSH( const SRenderNodeImplContext& rctx, const GpuApi::TextureRef texture, const GpuApi::TextureRef textureLuma, Uint32 targetSlot, const Vector3& probeSize, Bool includeDownSide = true, Bool placeOnFinalBuffer = false );

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

private:

	const EnvManager::ProbeSlot* IsUsable( EnvManager::ProbeId probeId, Bool mustBeReady = false ) const;

	void SetStreamingState( Uint64 dataHash, Uint8 newState );

	void SetStreamingState( Uint64 dataHash, Uint8 newState, Uint8 comparand );

	struct ProbeSearchResult
	{
		EnvManager::ProbeId		id{ EnvManager::ProbeId::Invalid() };
		math::Quaternion		rotation{ math::Quaternion::IDENTITY() };
		math::Vector3			position{ math::Vector3::ZEROS() };
		math::Vector3			size{ math::Vector3::ONES() };
	};

	ProbeSearchResult	FindClosestProbe( const EnvManager::ProbeSlot& slot, Bool mustBeInside, Float zRange = FLT_MAX ) const;

	ProbeSearchResult	FindClosestProbeInView( const EnvManager::ProbeSlot& slot, Uint32 face ) const;

	// - - - - - - - - - - - - - - - - - - - - - - - - - 

	struct ProcesContext
	{
		class SceneCustomData_ReflectionAtlas& sceneData;

		CEnvProbeRelighter* probeRelighter = nullptr;
		CReflectionProbeStage* stage = nullptr;
		CRenderSceneEx* currScene = nullptr;

		Bool isUnderRenderReadyQuery = false;
		Bool processingAnyGlobalProbe = false;
		Bool processingCurrentSubscene = false;
		Bool isSubScene = false;

		RenderSceneLayer currentSceneLayer{ RenderSceneLayer::Default };

		Float timeDT{ 1/30.0f };
		Float now{ 0.0f };
		Uint8 numIterations{ 0 };
		Uint8 iteration{ 0 };
		Uint8 numFullPrewarm{ 0 };

		EnvManager::ProbeId closestGlobalProbeId{ EnvManager::ProbeId::Invalid() };
		red::StaticArray< CReflectionProbeStage*, 8 >	idleStages;
	};

	Bool ProcessSingleStage_Relight( const SRenderNodeImplContext& rctx, ProcesContext& ctx, Bool fullRelight = false );

	Bool ProcessSingleStage_GenerateMipmap( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	Bool ProcessSingleStage_RenderGlobalProbe( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	Bool ProcessSingleStage_RenderLocalProbe( const SRenderNodeImplContext& rctx, ProcesContext& ctx, Bool fullRelight = false );

	Bool ProcessSingleStage_BlendOver( const SRenderNodeImplContext& rctx, ProcesContext& ctx, Bool instantBlend = false, Bool cleanup = true );

	Bool ProcessSingleStage_FullPrewarm( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	void ProcessSingleStage( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	void ProcessIdleStages( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	void HandleUpdateRequests( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

	void PrepareProbeData( const SRenderNodeImplContext& rctx, ProcesContext& ctx );

};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

class SceneCustomData_ReflectionAtlas : public ISceneStorageCustomData
{
	RTTI_DECLARE_TYPE( SceneCustomData_ReflectionAtlas );
	DECLARE_CUSTOM_SCENE_DATA( 1 );

	friend class CReflectionProbeManager;

	red::UniquePtr<class CEnvProbeAtlas>				m_atlas[2];
	red::UniquePtr<class CEnvProbeRelighter>			m_probeRelighter;

	red::DynArray< red::UniquePtr<CReflectionProbeStage> > m_stages{ rend::PoolReflectionProbes() };
	CReflectionProbeStage*								m_relitLock;

	red::Atomic<Bool>									m_requestSkyRender;
	red::Atomic<Bool>									m_requestGlobalRender;

	EngineTime											m_lastSkyUpdate;
	mutable EngineTime									m_lastRenderReadyQuery;

	static red::Mutex											m_lock;
	static red::DynArray< SceneCustomData_ReflectionAtlas* >	m_allAtlasses;

	Bool												m_useExtendedDynamicRangeSky;
	Bool												m_allProbesUsable;

public:

	SceneCustomData_ReflectionAtlas();

	~SceneCustomData_ReflectionAtlas();

	// - - - - - - - - - - - - - - - - - - - - - - - - -

	RED_FORCE_INLINE CEnvProbeAtlas*	GetAtlas( Bool isSubScene ) const { return m_atlas[isSubScene].Get(); }

	RED_FORCE_INLINE CEnvProbeRelighter*GetRelighter() const { return m_probeRelighter.Get(); }

	RED_FORCE_INLINE const red::DynArray< red::UniquePtr<CReflectionProbeStage> >& GetStages() const { return m_stages; }

	RED_FORCE_INLINE void UseExtendedDynamicRangeSky( bool enable ) { m_useExtendedDynamicRangeSky = enable; if ( m_probeRelighter ) m_probeRelighter->UseExtendedDynamicRangeSky( enable ); }

	RED_FORCE_INLINE Bool FetchGlobalProbeRefresh() { return m_requestGlobalRender.Exchange( false ); }

	// - - - - - - - - - - - - - - - - - - - - - - - - -

	virtual void Initialize() override;

	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData ) override;

	virtual void Evict() override;

	virtual const AnsiChar* IsRenderingReady() const override;

	virtual void TickWhileLoading( Bool isFirstFrame ) override;

	virtual rend::customData::Priority	GetPriority() const override { return rend::customData::Priority::LOW; };

	// - - - - - - - - - - - - - - - - - - - - - - - - -

	void ForceRelitGlobalProbe( );

	void ForceRelitLocalProbes(  const Vector3& position, Float radius );

	// - - - - - - - - - - - - - - - - - - - - - - - - -

	template <typename LAMBDA>
	static void PerformAll( LAMBDA&& lambda )
	{
		RED_SCOPE_LOCK( m_lock );
		for( auto it : m_allAtlasses )
			lambda( it );
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - -

};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

class CameraCustomData_ReflectionProbes : public ICameraStorageCustomData
{
	RTTI_DECLARE_TYPE( CameraCustomData_ReflectionProbes );
	DECLARE_CUSTOM_CAMERA_DATA( 2 );

public:

	struct SEnvProbe
	{
		Matrix		m_screenToLocal;

		Vector3		m_position;
		Uint32		m_atlasID : 8;
		Uint32		m_volumeMask : 16;

		Vector3		m_size;
		Float		m_importance;

		Vector3		m_edgeScale;
		Uint32		m_nonBoxProjected : 1;
		Uint32		m_volFogAmbient : 1;
		Uint32		m_flags : 30;

		Vector3		m_boundingSize;
		Float		PADDING2;
	};

	struct SEnvProbeParams
	{
		Uint32		m_numTiles[2];
		Uint32		m_numProbes;
		Uint32		m_globalProbeID;
	};

	struct SEnvProbeConstant
	{
		SEnvProbeParams		params;
		SEnvProbe			probe[CReflectionProbeManager::MAX_NUM_PROBES];
		Vector4				probeIrradiance[ ( 1 + CReflectionProbeManager::MAX_NUM_PROBES ) * 6 ];	// 6 sides for all probes + sky
	};

	mini::uvec2										m_numTiles;
	mini::uvec2										m_numTilesAllocated;

	GpuApi::Buffer									m_activeProbesParams;	//< Constants for active probes - probes that are in the current frame's frustum
#ifdef USE_RAY_TRACING
	GpuApi::Buffer									m_activeProbesNoSkyParams;	//< Constants for active probes - probes that are in the current frame's frustum
#endif
	GpuApi::Buffer									m_probesIndices;		//< Active probes to all probes redirection
	GpuApi::Buffer									m_probesForwardIndices;	//< Active probes to all probes redirection
	Bool											m_indicesClearNeeded;	//< Probe indices buffer was filled, so an update OR a clear is needed next time

	red::DynArray< CRenderProxy_ReflectionProbe* >	m_activeProbes{ rend::PoolReflectionProbes() };

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - 

	CameraCustomData_ReflectionProbes();

	virtual ~CameraCustomData_ReflectionProbes();

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - 

	virtual void Initialize() override;

	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage::SCameraData& cameraData ) override;

	virtual void Evict() override;

	virtual const AnsiChar* IsRenderingReady() const override;

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - 


	RED_FORCE_INLINE const GpuApi::BufferRef	GetProbesData() const { return m_activeProbesParams; }

#ifdef USE_RAY_TRACING
	RED_FORCE_INLINE const GpuApi::BufferRef	GetProbesNoSkyData() const { return m_activeProbesNoSkyParams; }
#endif

	RED_FORCE_INLINE const GpuApi::BufferRef	GetProbesIndices() const { return m_probesIndices; }

	RED_FORCE_INLINE const GpuApi::BufferRef	GetProbesForwardIndices() const { return m_probesForwardIndices; }

	RED_FORCE_INLINE Uint32						GetIndicesBufferSize() const { return m_numTiles.x * m_numTiles.y * CReflectionProbeManager::NUM_ENV_IND_PER_TILE * sizeof( Uint32 ); }

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - 

};

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

namespace EnvManager
{
	// CubemapSlot is invalid if cubemap data has not yet loaded.
	RED_INLINE Bool CubemapSlot::IsValid() const
	{
		// RED_FATAL_ASSERT( m_refCount > 0 );
		return m_refCount > 0 && !m_textureData.isNull();
	}

	RED_INLINE ProbeId ProbeId::Invalid()
	{
		return ProbeId( -1 );
	}

	RED_INLINE ProbeId ProbeId::FromProbe( const CRenderProxy_ReflectionProbe& probe )
	{
		return ProbeId( probe );
	}

	RED_INLINE ProbeId ProbeId::GlobalFallbackProbe()
	{
		return ProbeId( 0 );
	}

	RED_INLINE ProbeId::ProbeId( MemUint val )
		: m_val( val )
	{}

	RED_INLINE ProbeId::ProbeId( const CRenderProxy_ReflectionProbe& probe )
		: m_val( reinterpret_cast< MemUint >( &probe ) )
	{}

	RED_INLINE red::THash32 ProbeId::CalcHash() const
	{
		return red::GetHash( static_cast<Uint64>( m_val ) );
	}

	RED_INLINE Bool operator==( ProbeId lhs, ProbeId rhs )
	{
		return lhs.m_val == rhs.m_val;
	}

	RED_INLINE Bool operator!=( ProbeId lhs, ProbeId rhs )
	{
		return !( lhs == rhs );
	}

	RED_INLINE Bool operator<( ProbeId lhs, ProbeId rhs )
	{
		return (lhs.m_val < rhs.m_val);
	}

}

//*********************************************************************************************************************************************//
//*********************************************************************************************************************************************//

#endif