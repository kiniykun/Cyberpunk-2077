#include "build.h"

#include "advertSystem.h"
#include "audioEmitter.h"
#include "soundSystem.h"
#include "acousticsSystem.h"
#include "audioEmitterDecorator.h"
#include "../../redMath/include/random.h"
#include "../../soundMetadata/include/advertMetadata.h"
#include "../../soundEngine/include/sound.h"
#include "../../soundEngine/include/soundBuilder.h"
#include "../../soundEngine/include/positionDataStream.h"
#include "../../soundEngine/include/soundObjects.h"

#if ENABLE_AUDIO_DEBUG
#include "../../renderData/include/drawer.h"
#include "../../imgui/include/redImGui.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#endif
#include "singleObjectEmitter.h"
#include "audioCustomEntityIdGenerator.h"

namespace audio
{
	struct AdvertPoint
	{
		RED_USE_MEMORY_POOL( PoolAudioDecorators );
		Vector3 m_position;
		Float m_distance;
		Uint32 m_id;
		AdvertPoint() = default;
		AdvertPoint(const Vector3& position, Uint32 m_id);
		~AdvertPoint();
		Bool operator< (const AdvertPoint& other) const;
	};

	//////////////////////////////////////////////////////////////////////////

	typedef red::DynArray< AdvertPoint > AdvertPointArray;

	class AdvertPositionStrategy : public ApplySoundPositionStrategy
	{
		RTTI_DECLARE_TYPE( AdvertPositionStrategy );
	public:

		struct PositionMapping
		{
			SoundId m_soundId;
			Vector3 m_worldPos;
			Float m_delay = .0f;
		};

		virtual void ApplyPosition( SoundObject* soundObject ) override;
		virtual Float GetClosestDistance(const Vector3& distancePos, const PositionEntry* ) const override;
		Float CalcDistance();
		void UpdatePosition( Uint32 advertId, const Vector3& newPosition );
		virtual Vector3 GetNearestPosition( const PositionEntry* positionEntry ) const override;

	private:

		void CalcPositionMappings( const AdvertSoundMetadata* currentMetadata );
		void CalcPointDistances();

		AdvertPointArray m_positions = {PoolAudioDecorators()};
		red::StaticArray< PositionMapping, 4 > m_positionMapping;
		AudioEmitter* m_parentEmitter = nullptr;
		friend class AdvertTagEmitter;
	};

	//////////////////////////////////////////////////////////////////////////

	class AdvertTagEmitter : public AudioEmitter
	{
		RED_USE_MEMORY_POOL( PoolAudioDecorators );

	public:

		AdvertTagEmitter(const AdvertMetadata* metadata);
		void Tick(Float deltaTime);
		void AddPoint(const Vector3& position, Uint32 id);
		void RemovePoint(Uint32 id);
		virtual void SetTransform(const WorldTransform &transform) override;
		virtual ApplySoundPositionStrategy* GetSoundPositionStrategy() override;
		virtual Uint64 GetEntityId() const override
		{
			return m_advertEntityId;
		}
		
		Uint32 CountStreamedInEmitters() const;
		Float GetSilenceTime() const;
		Bool HasFiinishedPlaying() const;
		Bool TestOutdoorness() const;
		void UpdatePosition( Uint32 advertId, const Vector3& newPosition );

		
#if ENABLE_AUDIO_DEBUG
		void SkipWaiting();
		virtual void RenderDebug( rend::IDebugDrawer& dd, const EmitterDebugContext& context, rend::FragmentBuilder& builder ) override;
		void RenderDebug( rend::IDebugDrawer& dd );
#endif
	private:

		const AdvertMetadata* m_metadata;
		const AdvertSoundMetadata* m_currentMetadata;
		Uint64 m_advertEntityId = 0;
		AdvertPositionStrategy m_positionStrategy;
		math::Random m_randomGenerator;
		red::LightMutex m_addPointMutex;
		Float m_silenceTimer;
		Float m_playingTime;
	};

	//////////////////////////////////////////////////////////////////////////

	class AdvertDecorator : public EmitterDecorator
	{
	public:
		DEFINE_AUDIO_DECORATOR_GET_NAME( AdvertDecorator );

		AdvertDecorator( const Vector3& position, CName tagName );
		virtual ~AdvertDecorator();
		virtual void Update( AudioEmitter* emitter, Float dt ) override;
	private:

		Vector3 m_position;
		CName m_tagName;
		Uint32 m_advertId;
	};

	//////////////////////////////////////////////////////////////////////////

	AdvertPoint::AdvertPoint( const Vector3& position, Uint32 id )
		: m_position( position )
		, m_distance( .0f )
		, m_id( id )
	{
	}

	AdvertPoint::~AdvertPoint()
	{
	}

	Bool AdvertPoint::operator < ( const AdvertPoint& other ) const
	{
		//we rely on partial sort copy to give nearest points so the lower the distance the higher the "value"
		return m_distance > other.m_distance;
	}

	//////////////////////////////////////////////////////////////////////////

	void AdvertPositionStrategy::ApplyPosition( SoundObject* soundObject )
	{
		for( PositionMapping& mapping : m_positionMapping )
		{
			if( m_parentEmitter->GetSoundObject( mapping.m_soundId ) == soundObject )
			{ 
				GSoundEngine->SetPosition( soundObject->m_id, mapping.m_worldPos, Vector4::FRONT(), Vector4::UP() );
				return;
			}
		}
	}

	Float AdvertPositionStrategy::GetClosestDistance( const Vector3& distancePos, const PositionEntry* ) const
	{
		Float retVal = RED_FLT_MAX;
		for( const PositionMapping& mapping : m_positionMapping )
		{
			retVal = Min( ( mapping.m_worldPos - distancePos ).Mag(), retVal );
		}
		return retVal;
	}

	Float AdvertPositionStrategy::CalcDistance()
	{
		if( m_positions.Size() == 0 )
		{
			return RED_FLT_MAX;
		}

		CalcPointDistances();
		return std::max_element( m_positions.Begin(), m_positions.End() )->m_distance;
	}

	void AdvertPositionStrategy::UpdatePosition( Uint32 advertId, const Vector3& newPosition )
	{
		for( AdvertPoint& point : m_positions )
		{
			if( point.m_id == advertId )
			{
				point.m_position = newPosition;
			}
		}
	}

	Vector3 AdvertPositionStrategy::GetNearestPosition( const PositionEntry* ) const
	{
		return m_positionMapping[ 0 ].m_worldPos;
	}

	void AdvertPositionStrategy::CalcPositionMappings( const AdvertSoundMetadata* currentMetadata )
	{
		AdvertPoint nearestPoints[ 4 ];
		Int32 resultPointCount = Min( 4, static_cast< Int32 >( m_positions.Size() ) );

		if( ! resultPointCount )
		{
			return;
		}

		CalcPointDistances();
		std::partial_sort_copy( m_positions.Begin(), m_positions.End(), nearestPoints, nearestPoints + resultPointCount );
		m_positionMapping.Clear();

		for( Int32 i = resultPointCount - 1; i >= 0; --i )
		{
			AdvertPoint& sourcePoint = nearestPoints[ i ];
			PositionMapping& mapping = m_positionMapping.EmplaceBack();
			mapping.m_worldPos = sourcePoint.m_position;
			mapping.m_delay = currentMetadata->GetDelay( i, sourcePoint.m_distance );
		}
	}

	void AdvertPositionStrategy::CalcPointDistances()
	{
		Vector3 listenerPosition = GSoundEngine->GetListenerPosition().AsVector3();

		for( AdvertPoint& point : m_positions )
		{
			point.m_distance = ( listenerPosition - point.m_position ).Mag();
		}
	}

	//////////////////////////////////////////////////////////////////////////

	AdvertTagEmitter::AdvertTagEmitter( const AdvertMetadata* metadata )
		: AudioEmitter()
		, m_metadata( metadata )
		, m_randomGenerator()
		, m_silenceTimer( .0f )
		, m_playingTime( .0f )
	{
		
		m_currentMetadata = m_metadata->GetRandomAdvert( m_randomGenerator );
		m_silenceTimer = m_randomGenerator.Get( metadata->GetMinSilenceTime(), metadata->GetMaxSilenceTime() );
		m_positionStrategy.m_parentEmitter = this;
		if( !m_currentMetadata )
		{
			AUDIO_DEBUG_DATA_ERROR( ::audio::DebugLogCategory::General, "Advert doesn't have any advert sounds attached. Fix the AdvertMetadata named %s", metadata->m_name.AsChar() );
		}
		static const CName advertTagEmitterName = RED_NAME_CONSTEXPR( "advertTagEmitter" );
		m_advertEntityId = CustomIdGenerator().GetId( CustomEntityType::Advert, advertTagEmitterName );
		
	}

	void AdvertTagEmitter::Tick( Float deltaTime )
	{
		if( ! m_currentMetadata )
		{
			return;
		}

		if( m_playingTime > .0f )
		{
			m_playingTime += deltaTime;
			Uint32 audioEventIndex = 0;

			for( AdvertPositionStrategy::PositionMapping& mapping : m_positionStrategy.m_positionMapping )
			{
				Float prevDelay = mapping.m_delay;
				mapping.m_delay -= deltaTime;

				if( prevDelay > .0f && mapping.m_delay <= .0f )
				{
					SoundBuilder soundBuilder( m_currentMetadata->GetAudioEvent( audioEventIndex ) );
					PlaySound( soundBuilder );
					mapping.m_soundId = soundBuilder.GetSoundId();
				}
				++audioEventIndex;
			}
			
			if( HasFiinishedPlaying() )
			{
				m_playingTime = .0f;
			}
		}
		else
		{
			m_silenceTimer -= deltaTime;

			if( m_silenceTimer <= .0f )
			{
				m_currentMetadata = m_metadata->GetRandomAdvert( m_randomGenerator );
				m_silenceTimer = m_randomGenerator.Get( m_metadata->GetMinSilenceTime(), m_metadata->GetMaxSilenceTime() );
				
				if( ! GSoundSystem->GetAcousticsSystem() )
				{
					return;
				}

				

				if( ! TestOutdoorness() )
				{
					return;
				}
				
				if( m_metadata->m_minDistance != 0.0f && m_metadata->m_minDistance < m_positionStrategy.CalcDistance() )
				{
					return;
				}

				m_playingTime += deltaTime;
				m_positionStrategy.CalcPositionMappings( m_currentMetadata );
			}
		}
	}

	void AdvertTagEmitter::AddPoint( const Vector3& position, Uint32 id )
	{
		RED_SCOPE_LOCK( m_addPointMutex );
		m_positionStrategy.m_positions.EmplaceBack( position, id );
	}

	void AdvertTagEmitter::RemovePoint(Uint32 id)
	{
		AdvertPointArray& positions = m_positionStrategy.m_positions;

		AdvertPointArray::iterator toRemove = std::find_if( begin( positions ), end( positions ), [ id ]( const AdvertPoint& point )
		{
			return point.m_id == id;
		});

		if( toRemove != end( positions ) )
		{
			positions.RemoveReorder( toRemove );
		}
	}

	void AdvertTagEmitter::SetTransform( const WorldTransform& )
	{
	}

	ApplySoundPositionStrategy* AdvertTagEmitter::GetSoundPositionStrategy()
	{
		return &m_positionStrategy;
	}

	Uint32 AdvertTagEmitter::CountStreamedInEmitters() const
	{
		return m_positionStrategy.m_positions.Size();
	}
	
	Float AdvertTagEmitter::GetSilenceTime() const
	{
		return m_silenceTimer;
	}

	Bool AdvertTagEmitter::HasFiinishedPlaying() const
	{
		if( IsPlaying() )
		{
			return false;
		}
		for( const AdvertPositionStrategy::PositionMapping& mapping : m_positionStrategy.m_positionMapping )
		{
			if( mapping.m_delay > .0f )
			{
				return false;
			}
		}
		return true;
	}

	Bool AdvertTagEmitter::TestOutdoorness( ) const
	{
		const Float outdoorness = GSoundSystem->GetAcousticsSystem()->GetOutdoorness();
		return ( m_metadata->m_filter != AdvertIndoorFilter::OnlyIndoor || outdoorness <= .45f ) && 
			( m_metadata->m_filter != AdvertIndoorFilter::OnlyOutdoor || outdoorness >= .55f );

	}

	void AdvertTagEmitter::UpdatePosition( Uint32 advertId, const Vector3& newPosition )
	{
		m_positionStrategy.UpdatePosition( advertId, newPosition );
	}

#if ENABLE_AUDIO_DEBUG

	void AdvertTagEmitter::SkipWaiting()
	{
		m_silenceTimer = .0f;
	}

	void AdvertTagEmitter::RenderDebug( rend::IDebugDrawer &dd )
	{
		rend::FragmentBuilder builder( dd );
		const Bool isPlaying = ! HasFiinishedPlaying();
		builder.SetColor( isPlaying ? Color::WHITE() : Color::GREEN() );
		if( isPlaying )
		{
			builder.EnableOverlay( true );
		}
		for( AdvertPoint& point : m_positionStrategy.m_positions )
		{
			builder.AddWireBox( point.m_position - Vector3( .2f ), point.m_position + Vector3( .2f ) );
		}
	}

	void AdvertTagEmitter::RenderDebug( rend::IDebugDrawer& dd, const EmitterDebugContext& context, rend::FragmentBuilder& builder )
	{
		RenderDebug( dd );
	}

#endif

	//////////////////////////////////////////////////////////////////////////

	AdvertDecorator::AdvertDecorator( const Vector3& position, CName tagName )
	{
		m_position = position;
		m_advertId = GSoundSystem->GetAdvertSystem()->RegisterTag( position, tagName );
		m_tagName = tagName;
	}


	AdvertDecorator::~AdvertDecorator()
	{
		AdvertSystem* sys = GSoundSystem->GetAdvertSystem();
		if( sys )
		{
			sys->DeregisterTag( m_advertId, m_tagName );
		}
		
	}

	void AdvertDecorator::Update( AudioEmitter* emitter, Float dt )
	{
		Vector3 newPosition;
		const PositionEntry* positionEntry = emitter->GetPositionEntry();

		if( positionEntry )
		{
			newPosition = positionEntry->m_position.AsVector3();
			if( newPosition != m_position )
			{
				m_position = newPosition;
				GSoundSystem->GetAdvertSystem()->UpdatePosition( m_tagName, m_advertId, m_position );
			}
		}

		if( GSoundSystem->GetAdvertSystem()->InReregisterState() )
		{
			m_advertId = GSoundSystem->GetAdvertSystem()->RegisterTag( m_position, m_tagName );
		}
	}

	//////////////////////////////////////////////////////////////////////////

	AdvertSystem::AdvertSystem()
		: m_idGenerator( 0 )
		, m_tags{ PoolAudioDecorators() }
	{
		BuildFromMetadata();
	}

	AdvertSystem::~AdvertSystem()
	{
	}

	Uint32 AdvertSystem::RegisterTag( const Vector3& position, CName tagName )
	{
		AdvertTagMap::iterator tagRef = m_tags.Find( tagName );

		if( tagRef == m_tags.End() )
		{
			return c_invalidId;
		}

		Uint32 id = m_idGenerator.Increment();
		AdvertTagEmitter* tagPoints = tagRef.Value().Get();
		tagPoints->AddPoint( position, id );

		return id;
	}
	
	void AdvertSystem::DeregisterTag( Uint32 id, CName tagName )
	{
		if( id == c_invalidId )
		{
			return;
		}
		AdvertTagMap::iterator tagRef = m_tags.Find( tagName );

		if( tagRef == end( m_tags ) )
		{
			return;
		}
		tagRef.Value()->RemovePoint( id );
		
	}

	void AdvertSystem::UpdatePosition( CName tagName, Uint32 advertId, const Vector3& newPosition )
	{
		red::UniquePtr< AdvertTagEmitter >* emitter = m_tags.FindPtr( tagName );
		if( ! emitter ) 
		{
			return;
		}
		( *emitter )->UpdatePosition( advertId, newPosition );
	}

	void AdvertSystem::Tick(Float deltaTime)
	{
		switch ( m_reloadMetadataState )
		{
			case ReloadMetadataState::Done:
			{
				for ( AdvertTagMap::ElementType tag : m_tags )
				{
					tag.Value()->Tick( deltaTime );
				}
				break;
			}
			case ReloadMetadataState::Requested:
			{
				Bool foundActive = false;
				for ( AdvertTagMap::iterator tag = m_tags.Begin(); tag != m_tags.End(); ++tag )
				{
					if ( tag.Value()->IsActive() )
					{
						foundActive = true;
						break;
					}
				}

				if ( !foundActive )
				{
					m_reloadMetadataState = ReloadMetadataState::InProgress;
				}
				break;
			}
			case ReloadMetadataState::InProgress:
			{
				m_tags.Clear();
				BuildFromMetadata();
				m_reloadMetadataState = ReloadMetadataState::ReRegistering;
				break;
			}
			case ReloadMetadataState::ReRegistering:
			{
				m_reloadMetadataState = ReloadMetadataState::Done;
				break;
			}
			default:
			{
				break;
			}
		}
	}

	void AdvertSystem::PostSoundUpdate()
	{
		if( m_reloadMetadataState == ReloadMetadataState::Requested )
		{
			for (AdvertTagMap::ElementType tag : m_tags)
			{
				tag.Value()->StopTagged(RED_NAME_CONSTEXPR("all"));
			}
		}
		for( AdvertTagMap::ElementType tag : m_tags )
		{
			tag.Value()->PostSoundUpdate( true );
		}
	}

	void AdvertSystem::ResetMetadata()
	{
		m_reloadMetadataState = ReloadMetadataState::Requested;
	}

	Bool AdvertSystem::InReregisterState() const
	{
		return m_reloadMetadataState == ReloadMetadataState::ReRegistering;
	}

#if ENABLE_AUDIO_DEBUG

	static rend::DebugFilter DF_AudioAdvertDebug( "Audio/AdvertDebug" );

	void AdvertSystem::RenderDebug( rend::IDebugDrawer &dd )
	{
		if( ! dd.GetMask().Test( DF_AudioAdvertDebug ) )
		{
			return;
		}
		Uint32 xCoords = 50;
		Uint32 yCoords = dd.GetHeight( ) >> 1;
		for( AdvertTagMap::ElementType tag : m_tags )
		{
			tag.Value()->RenderDebug( dd );
		}

#ifdef RED_USE_IMGUI
		ImGui::Begin("audio adverts", &m_isGuiOpened);
		ImGui::SetWindowPos( ImVec2( xCoords * 1.0f, yCoords * 1.0f ) );
		ImGui::SetWindowSize( ImVec2( 250.0f, dd.GetHeight( ) - yCoords - 20.0f ) );
		ImGui::SetWindowFontScale( 1.0f );
		ImGui::Text( "[tagname]: [streamed count] / [idle time]" );

		Uint32 lineNo = 0;	
		for( AdvertTagMap::ElementType tag : m_tags )
		{
			ImGui::Text( "%.25s:   %d", tag.Key().AsChar(), tag.Value()->CountStreamedInEmitters() );
			ImGui::SameLine();
			if( ! tag.Value()->HasFiinishedPlaying() )
			{
				ImGui::Text( " (playing)" );
			}
			else
			{
				ImGui::Text( "/  %.1fs", tag.Value()->GetSilenceTime() );

				red::StringBuilder< String > skipWaitingBuffer;
				skipWaitingBuffer.Appendf("skip Waiting##S%d", lineNo);
				ImGui::SameLine();

				if( ImGui::Button( skipWaitingBuffer.AsChar() ))
				{
					tag.Value()->SkipWaiting();
				}
			}
			++lineNo;
			
		}
		if (ImGui::Button( "reload metadata" ) )
		{
			GSoundSystem->ReloadMetadata();
		}
		ImGui::End();

#endif //RED_USE_IMGUI	
	}

#endif //ENABLE_AUDIO_DEBUG	

	void AdvertSystem::BuildFromMetadata()
	{
		red::DynArray< const AdvertMetadata* > metadataObjects{ PoolAudioDecorators() };
		GMetadataManager->GetMetadataObjects(metadataObjects);

		for (const AdvertMetadata* metadata : metadataObjects)
		{
			m_tags.Insert(metadata->m_name, red::CreateUniquePtr< AdvertTagEmitter >(metadata));
		}
	}



	//////////////////////////////////////////////////////////////////////////

	void AttachAdvertMetadata( AudioEmitter* emitter, const EmitterMetadata * metadata )
	{
		if( ! metadata )
		{
			return;
		}

		const AdvertMetadata* advertEmitterMetadata = Cast< AdvertMetadata >(metadata);
		if (!advertEmitterMetadata)
		{
			return;
		}

		const PositionEntry* positionEntry = emitter->GetPositionEntry();

		if( ! positionEntry )
		{
			return;
		}

		emitter->SetForceUpdate( true );
		emitter->AddDecorator(red::CreateUniquePtr< AdvertDecorator >( positionEntry->m_position.AsVector3(), advertEmitterMetadata->m_name ));
	}

}

RTTI_BEGIN_TYPE_IN_NAMESPACE(AdvertPositionStrategy, audio)
RTTI_PARENT_TYPE(ApplySoundPositionStrategy);
RTTI_END_TYPE();