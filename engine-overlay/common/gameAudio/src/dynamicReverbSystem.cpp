/*
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "dynamicReverbSystem.h"
#include "audioMaterialHelpers.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../soundMetadata/include/acousticsMetadata.h"
#include "../../engine/include/baseEngine.h"
#include "../../gameSpatialQueries/include/hedgehogQuery.h"
#include "../../sound/include/soundSystem.h"
#include "../../sound/include/scriptAudioInterface.h"
#include "../../sound/include/acousticsSystem.h"
#include "../../sound/include/reflectionEmitter.h"
#include "../../sound/include/audioCustomEntityIdGenerator.h"
#include "../../sound/include/audioInterfaceBuilder.h"
#include "../../sound/include/emitterInterfaceBuilder.h"
#include "../../sound/include/audioEventFactory.h"
#ifdef RED_USE_IMGUI
#include "../../imgui/include/redImGui.h"
#endif
#include "../soundMetadata/include/audioMetadataManager.h"
#include "../sound/include/dynamicReverbInterface.h"
#include "../sound/include/audioDirectorSystem.h"
#include "../sound/include/audioHelpers.h"


static rend::DebugFilter DF_AudioDrawReverbDistances( "Audio/DrawReverbDistances" );
static rend::DebugFilter DF_AudioDrawReverbDebug( "Audio/DrawReverbDebug" );
static rend::DebugFilter DF_AudioDrawReflectionsUI( "Audio/ReflectionsUI" );
static rend::DebugFilter DF_RenderAcousticsDebug( "Audio/Acoustics" );
static rend::DebugFilter DF_AudioDebugListenerRoom( "Audio/DrawListenerRoomRoots" );
static CName s_reflectionVolumeParam = RED_NAME_CONSTEXPR( "game_reflection_volume" );
static CName s_reflectionLowPassParam = RED_NAME_CONSTEXPR( "game_reflection_lowpass" );
static CName s_reflectionHighPassParam = RED_NAME_CONSTEXPR( "game_reflection_highpass" );

namespace world
{
	const Float DynamicReverbSystem::c_reverbDirAngle = 360.f / Float( c_numReverbDir );
	const Float DynamicReverbSystem::c_reverbVerticalDirAngle = 180.f / Float( audio::DynamicReverbSettings::c_numVerticalReverbDir + 2 ); // constant doesn't include up/down

	void DynamicReverbSystem::Update()
	{
		UpdateAcousticEnvironment();
		UpdateListenerRoomDetector();
	}

	void DynamicReverbSystem::UpdateListenerRoomDetector()
	{
		if( m_hedgehogProvider && m_hedgehogProvider->GetHedgehogQuery() )
		{
			const ::character::HedgehogQuery* hedgehog = m_hedgehogProvider->GetHedgehogQuery();
			if( hedgehog )
			{
				Vector3 centerPosition = hedgehog->GetOriginTransform().GetPosition3();
				const Quaternion& orientation = hedgehog->GetOriginTransform().GetOrientation();

				m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Center, centerPosition );
				const Uint32 forwardSliceIndex = hedgehog->GetSliceIndexForYawAngle( .0f - orientation.GetYaw() );
				const Uint32 rightSliceIndex = hedgehog->GetSliceIndexForYawAngle( 90.0f - orientation.GetYaw() );
				const Uint32 backSliceIndex = hedgehog->GetSliceIndexForYawAngle( 180.0f - orientation.GetYaw() );
				const Uint32 leftSliceIndex = hedgehog->GetSliceIndexForYawAngle( 270.0f - orientation.GetYaw() );
				const Uint32 planeIndex = hedgehog->GetPlaneIndexForPitchAngle( .0f );
				const ::character::HedgehogQuery::RawResults& rawResults = hedgehog->GetRawResults();
				const ::character::HedgehogQuery::PlaneRawResults& planeRawResults = rawResults[ planeIndex ];

				{
					const physics::AsyncQueryTokenPtr& result = planeRawResults[ forwardSliceIndex ];
					Vector3 direction = orientation.GetForward3();
					Float distance = 10.0f;
					if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
					{
						distance = result->m_result.m_distance * .7f;
					}
					m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Front, centerPosition + direction * distance );
				}
				{
					const physics::AsyncQueryTokenPtr& result = planeRawResults[ rightSliceIndex ];
					Vector3 direction = orientation.GetRight3();
					Float distance = 10.0f;
					if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
					{
						distance = result->m_result.m_distance * .7f;
					}
					m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Right, centerPosition + direction * distance );
				}
				{
					const physics::AsyncQueryTokenPtr& result = planeRawResults[ backSliceIndex ];
					Vector3 direction = -orientation.GetForward3();
					Float distance = 10.0f;
					if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
					{
						distance = result->m_result.m_distance * .7f;
					}
					m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Back, centerPosition + direction * distance );
				}
				{
					const physics::AsyncQueryTokenPtr& result = planeRawResults[ leftSliceIndex ];
					Vector3 direction = -orientation.GetRight3();
					Float distance = 10.0f;
					if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
					{
						distance = result->m_result.m_distance * .7f;
					}
					m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Left, centerPosition + direction * distance );
				}
				{
					const physics::AsyncQueryTokenPtr& result = hedgehog->GetUpRawResults();
					Vector3 direction = Vector3::EZ();
					Float distance = 10.0f;
					if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
					{
						distance = result->m_result.m_distance * .7f;
					}
					m_listenerRoomDetector.UpdateDirection( ListenerRoomDetector::Direction::Top, centerPosition + direction * distance );
				}
			}
		}
	}

	void DynamicReverbSystem::UpdateSmoothReverbDistanceForAmbients()
	{
		RED_STATIC_ASSERT( c_numReverbDir == 36 );
		Float smoothReverbDistance[ 36 ];
		for( Uint32 i = 0; i < c_numReverbDir; ++i )
		{
			smoothReverbDistance[ i ] = m_reverbDistance[ i ][ 3 ];
		}

		Uint32 index = 0;
		for( Uint32 i = 0; i < 144; ++i )
		{
			Uint32 prev = ( index + 35 ) % 36;
			Uint32 next = ( index + 1) % 36;
			smoothReverbDistance[ index ] = ( smoothReverbDistance[ prev ] + smoothReverbDistance[ next ] ) * .3f + smoothReverbDistance[ index ] * .4f;
			index = ( index + 11 ) % 36;
		}
		GEngine->GetSoundSystem()->GetAcousticsSystem()->SetSmoothReverbDistances( smoothReverbDistance );
	}

	void DynamicReverbSystem::OnRenderDebug( rend::IDebugDrawer& dd )
	{
		rend::FragmentBuilder builder( dd );
		if( dd.GetMask().Test( DF_AudioDebugListenerRoom ) )
		{
			m_listenerRoomDetector.OnRenderDebug( builder );
		}

		if( dd.GetMask().Test( DF_AudioDrawReverbDebug ) )
		{

			const Uint32 ySize = 25;
			const Uint32 xSize = 15;
			Uint32 yPos = 450;
			Uint32 xZero = 600;
			Uint32 xPos = xZero;
			builder.EnableTextBackground( Color::DARK_GREEN() );

			const audio::DynamicReverbSettings* reverbSettings = GEngine->GetSoundSystem()->GetReverbSettings();

			if( reverbSettings )
			{
				if( reverbSettings->m_reverbType == audio::DynamicReverbType::Dynamic )
				{
					RenderDynamicReverbInfo( builder, xPos, yPos, ySize, xSize, reverbSettings );
				}
				else if( reverbSettings->m_reverbType == audio::DynamicReverbType::StaticSmall )
				{
					RenderStaticSmallReverbInfo( builder, xPos, yPos, ySize, xSize, reverbSettings );
				}
				else if( reverbSettings->m_reverbType == audio::DynamicReverbType::EnvironmentSmallStaticMedium )
				{
					RenderEnviroSmallStaticMediumReverbInfo( builder, xPos, yPos, ySize, xSize, reverbSettings );
				}
			}
		}

		if( dd.GetMask().Test( DF_AudioDrawReverbDistances ) )
		{
			builder.DisableTextBackground();
			Vector4 listenerPos = GEngine->GetSoundSystem()->GetListenerPosition();
			Vector3 startPos = Vector3( listenerPos.X, listenerPos.Y, listenerPos.Z ) + Vector3( 0.f, 0.f, 0.1f );
			for( int i = 0; i < c_numReverbDir; i++ )
			{
				for( Int32 j = 0; j < audio::DynamicReverbSettings::c_numVerticalReverbDir; j++ )
				{
					Color col = Color::LIGHT_BLUE();

					Vector3 direction = m_reverbDir[ i ].dir[ j ];

					// FrontLeft
					if( m_reverbChannels[ i ] == audio::FrontLeftReverb )
					{
						col = Color::LIGHT_BLUE();
					}
					// FrontRight
					else if( m_reverbChannels[ i ] == audio::FrontRightReverb )
					{
						col = Color::LIGHT_GRAY();
					}
					// BackLeft
					else if( m_reverbChannels[ i ] == audio::BackLeftReverb )
					{
						col = Color::LIGHT_GREEN();
					}
					// BackRight
					else if( m_reverbChannels[ i ] == audio::BackRightReverb )
					{
						col = Color::LIGHT_RED();
					}

					Vector3 endPos = startPos + direction * m_reverbDistance[ i ][ j ];
					builder.SetColor( col );
					builder.AddWorldTextf( endPos, 0, 0, "%f", m_reverbDistance[ i ][ j ] );
				}
			}
		}
		for( AudioReflectionSystem& system : m_reflectionSystems )
		{
			system.OnRenderDebug( *this, builder );
		}
	}

	static CName s_refletionEmitternames[] = {
		RED_NAME_CONSTEXPR( "front_left_reflection" ),
		RED_NAME_CONSTEXPR( "front_right_reflection" ),
		RED_NAME_CONSTEXPR( "back_left_reflection" ),
		RED_NAME_CONSTEXPR( "back_right_reflection" ),
		RED_NAME_CONSTEXPR( "up_reflection" )};

	DynamicReverbSystem::DynamicReverbSystem()
		: m_closestWeightedDistance( 0.f )
	{
		m_reverbDir.Resize( c_numReverbDir );
		for( int i = 0; i < c_numReverbDir; i++ )
		{
			m_reverbDir[ i ].dir.Resize( audio::DynamicReverbSettings::c_numVerticalReverbDir );
		}
		m_constantPresets = ::audio::AcousticConstantsPreset::Get();

		red::DynArray< const ::audio::ReflectionEmitterSettings* > metadataObjects = {::audio::PoolAudioMetadata()};

		const ::audio::ReflectionEmitterSettings* defaultReflectionMetadata = ::audio::GMetadataManager->GetMetadataObject<::audio::ReflectionEmitterSettings >( RED_NAME_CONSTEXPR( "default_reflection" ) );
		const ::audio::ReflectionEmitterSettings* voReflectionMetadata = ::audio::GMetadataManager->GetMetadataObject<::audio::ReflectionEmitterSettings >( RED_NAME_CONSTEXPR( "player_vo_reflection" ) );
		const ::audio::ReflectionEmitterSettings* locomotionReflectionMetadata = ::audio::GMetadataManager->GetMetadataObject<::audio::ReflectionEmitterSettings >( RED_NAME_CONSTEXPR( "player_locomotion_reflection" ) );
		if( defaultReflectionMetadata )
		{
			AudioReflectionSystem& reflectionSystem = m_reflectionSystems.EmplaceBack();
			reflectionSystem.Init( *this, defaultReflectionMetadata );
		}
		if( voReflectionMetadata )
		{
			AudioReflectionSystem& reflectionSystem = m_reflectionSystems.EmplaceBack();
			reflectionSystem.Init( *this, voReflectionMetadata );
		}
		if( locomotionReflectionMetadata )
		{
			AudioReflectionSystem& reflectionSystem = m_reflectionSystems.EmplaceBack();
			reflectionSystem.Init( *this, locomotionReflectionMetadata );
		}

		m_reverbChannels.Resize( c_numReverbDir );

		if( auto soundSystem = GEngine->GetSoundSystem() ) // HACK: We shouldn't even create IGameAudioSystem on the server
		{
			GEngine->GetSoundSystem()->ApplyReverbSettings( audio::DynamicReverbSettings::c_defaultReverb );

			m_currentReverb = audio::DynamicReverbSettings::c_defaultReverb;

			const audio::DynamicReverbSettings* reverbSettings = GEngine->GetSoundSystem()->GetReverbSettings();

			if( reverbSettings )
			{
				m_reverbLevelsCrossFade.Init( audio::soundCurvePoint( reverbSettings->m_crossover1.m_dist, 1.f ), reverbSettings->m_crossover1.m_fade, audio::soundCurvePoint( reverbSettings->m_crossover2.m_dist, 1.f ), reverbSettings->m_crossover2.m_fade );
			}
		}

		InitReverbBaseDirections();
	}

	void DynamicReverbSystem::SetHedgehogProvider( const ::character::HedgehogProvider& provider )
	{
		if( m_hedgehogProvider && m_hedgehogProvider->GetHedgehogQuery() )
		{
			return;
		}
		m_hedgehogProvider = provider.MakeCopy();
	}

	::audio::ReverbChannel DynamicReverbSystem::GetReverbChannel( Float angle )
	{
		Float cosAng = MCos( angle ), sinAng = MSin( angle );
		//FrontLeft
		if( cosAng <= 0.f && sinAng > 0.f )
		{
			return audio::ReverbChannel::BackLeftReverb;
		}
		//FrontRight
		else if( cosAng > 0.f && sinAng > 0.f )
		{
			return audio::ReverbChannel::FrontLeftReverb;
		}
		//BackLeft
		else if( cosAng <= 0.f && sinAng <= 0.f )
		{
			return audio::ReverbChannel::BackRightReverb;
		}
		//BackRight
		else if( cosAng > 0.f && sinAng <= 0.f )
		{
			return audio::ReverbChannel::FrontRightReverb;
		}

		return audio::ReverbChannel::FrontLeftReverb;
	}

	const character::HedgehogQuery* DynamicReverbSystem::GetHedgehog()
	{
		if( !m_hedgehogProvider )
		{
			return nullptr;
		}
		return m_hedgehogProvider->GetHedgehogQuery();
	}

	Bool DynamicReverbSystem::InHedgehogRange( const Vector3& worldPosition ) const
	{
		if( !m_hedgehogProvider )
		{
			return false;
		}

		const character::HedgehogQuery* hedgehog = m_hedgehogProvider->GetHedgehogQuery();
		Uint32 centerSliceIndex, centerPlaneIndex;
		if( !hedgehog->GetIndicesForWorldPosition( worldPosition, centerPlaneIndex, centerSliceIndex ) )
		{
			return true;
		}
		Uint32 startSliceIndex = ( centerSliceIndex + character::HedgehogQuery::k_numberOfSlices - 1 ) % character::HedgehogQuery::k_numberOfSlices;
		Float nearestDistance = 1000.f;

		for( Int32 ix = 0; ix < 3; ++ix )
		{
			Uint32 x = ( ix + startSliceIndex ) % character::HedgehogQuery::k_numberOfSlices;
			Float distance;
			Bool hit = hedgehog->GetHitDistance( centerPlaneIndex, x, distance );
			if( hit && nearestDistance > distance )
			{
				nearestDistance = distance;
			}
		}

		Float sourceDistance = ( worldPosition - GEngine->GetSoundSystem()->GetListenerPosition().AsVector3() ).Mag();
		return nearestDistance > sourceDistance;
	}

	class AcousticsInterface : public ::audio::acoustics::DynamicReverbInterface
	{
		RED_USE_MEMORY_POOL(::audio::PoolAudioSystems );

	public:
		AcousticsInterface( DynamicReverbSystem& dynamicReverbSystem );
		virtual Bool InHedgehogRange( const Vector3& position ) const override;
		virtual Bool IsInPlayersRoom( const Vector3& position ) const override;
		virtual Bool FillEvironmentDescription(::audio::acoustics::EnvironmentDescription& outDesc ) const override;

	private:
		DynamicReverbSystem& m_dynamicReverbSystem;
	};

	Bool AcousticsInterface::InHedgehogRange( const Vector3& position ) const
	{
		return m_dynamicReverbSystem.InHedgehogRange( position );
	}

	Bool AcousticsInterface::IsInPlayersRoom( const Vector3& position ) const
	{
		return m_dynamicReverbSystem.GetListenerRoomDetector().TestPosition( position );
	}

	Bool AcousticsInterface::FillEvironmentDescription(::audio::acoustics::EnvironmentDescription& outDesc ) const
	{
		outDesc.m_outdoorness = GEngine->GetSoundSystem()->GetAcousticsSystem()->GetOutdoorness(); //m_dynamicReverbSystem.GetListenerRoomDetector().GetMaxOutDoorness();

		const ::character::HedgehogQuery* hedgehog = m_dynamicReverbSystem.GetHedgehog();
		if( hedgehog )
		{
			Float planeDistances [ ::character::HedgehogQuery::k_numberOfSlices ];
			const Float c_fallbackPlaneDistance = 100.0f;
			hedgehog->GetPlaneDistances( ( (::character::HedgehogQuery::k_numberOfPlanes - 1 ) >> 1 ), planeDistances, c_fallbackPlaneDistance );
			outDesc.m_avgHorizontalDistance = .0f;
			for( Float planeDistance : planeDistances )
			{
				outDesc.m_avgHorizontalDistance += planeDistance;
			}
			outDesc.m_avgHorizontalDistance /= ::character::HedgehogQuery::k_numberOfSlices;

			hedgehog->GetPlaneDistances( 4, planeDistances, c_fallbackPlaneDistance );
			outDesc.m_avgElevatedDistance = .0f;

			for( Float planeDistance : planeDistances )
			{
				outDesc.m_avgElevatedDistance += planeDistance;
			}

			outDesc.m_avgElevatedDistance /= ::character::HedgehogQuery::k_numberOfSlices;
			outDesc.m_ceilingDistance = hedgehog->GetUpDistance( c_fallbackPlaneDistance );
			outDesc.m_altitude = GEngine->GetSoundSystem()->GetListenerPosition().Z;
			outDesc.m_relativeAltitude = hedgehog->CalcRelativeAltitude( outDesc.m_altitude );


			outDesc.m_inBadlands = GEngine->GetSoundSystem()->GetAcousticsSystem()->IsInBadlandsEnvironment();
			outDesc.m_inBadlandsWide = GEngine->GetSoundSystem()->GetAcousticsSystem()->IsInBadlandsWideEnvironment();
		}
		return true;
	}

	AcousticsInterface::AcousticsInterface( DynamicReverbSystem& dynamicReverbSystem )
		: m_dynamicReverbSystem( dynamicReverbSystem ){};

	red::UniquePtr< audio::acoustics::DynamicReverbInterface, audio::PoolAudioReverb > DynamicReverbSystem::GetAcousticsInterface()
	{
		return red::CreateUniquePtr< AcousticsInterface, ::audio::PoolAudioReverb >( *this );
	}

	const world::ListenerRoomDetector& DynamicReverbSystem::GetListenerRoomDetector() const
	{
		return m_listenerRoomDetector;
	}

	void DynamicReverbSystem::UpdateGlobalCeilingCoverage() const
	{
		if( !m_hedgehogProvider )
		{
			return;
		}

		const ::character::HedgehogQuery* hedgehog = m_hedgehogProvider->GetHedgehogQuery();

		if( !hedgehog || !m_constantPresets )
		{
			return;
		}

		const Float c_maxCeilingDistance = m_constantPresets->m_ambExteriorCeilingMaxDistance;
		const Float c_minCeilingDistance = m_constantPresets->m_ambExteriorCeilingMinDistance;

		const Float c_ceilingDistanceInvertedRange = 1.0f / ( c_maxCeilingDistance - c_minCeilingDistance );
		Uint32 count = 0;
		Float sum = .0f;

		Float highCheckSum = .0f;

		const physics::AsyncQueryTokenPtr& result = hedgehog->GetUpRawResults();
		const physics::AsyncQueryTokenPtr& farResult = hedgehog->GetFarUpRawResults();

		if( result && result->DidHit() && result->m_result.m_distance < c_maxCeilingDistance )
		{
			++count;
			sum += result->m_result.m_distance;
		}

		if( farResult && farResult->DidHit() && farResult->m_result.m_distance < 110.0f )
		{
			highCheckSum += 20.0f;
		}


		::character::HedgehogQuery::PlaneRawResults planeResults;
		hedgehog->GetPlaneRawReults(::character::HedgehogQuery::k_numberOfPlanes - 1, planeResults );
		for( physics::AsyncQueryTokenPtr& result : planeResults )
		{
			if( result && result->DidHit() && result->m_result.m_distance < c_maxCeilingDistance && result->m_result.m_worldNormal.Z < -.3f )
			{
				++count;
				sum += result->m_result.m_distance;
			}

			if( result && result->DidHit() && result->m_result.m_distance < 110.0f )
			{
				highCheckSum += Min( result->m_result.m_distance / c_minCeilingDistance, 1.0f );
			}

		}

		Float output = .0f;
		Float highOutput = .0f;

		if( count > 3 )
		{
			output = ( count - 3.0f ) / 34.0f;
			Float avg = sum / count;

			Float avgFactor = 1.0f - Clamp( ( avg - c_minCeilingDistance ) * c_ceilingDistanceInvertedRange, .0f, 1.0f );
			output *= avgFactor;
		}

		highOutput = Clamp( ( highCheckSum - 3.0f ) / 53.0f, .0f, 1.0f );

		output = Min( GEngine->GetSoundSystem()->GetAcousticsSystem()->GetOutdoorness(), output );
		highOutput = Min( GEngine->GetSoundSystem()->GetAcousticsSystem()->GetOutdoorness(), highOutput );
		static const CName c_rtpcCeilingCoverage = RED_NAME_CONSTEXPR( "amb_exterior_ceiling" );
		static const CName c_rtpcCeilingFarCoverage = RED_NAME_CONSTEXPR( "amb_exterior_far_ceiling" );

		GEngine->GetSoundSystem()->SetGlobalParameter( c_rtpcCeilingCoverage, output );
		GEngine->GetSoundSystem()->SetGlobalParameter( c_rtpcCeilingFarCoverage, highOutput );
	}

	void DynamicReverbSystem::OverrideReflectionVariant( audio::ReflectionVariant variant )
	{
		for( auto& reflectionSystem : m_reflectionSystems )
		{
			reflectionSystem.OverrideReflectionVariant( variant );
		}
	}

	void DynamicReverbSystem::ResetReflectionVariant()
	{
		for( auto& reflectionSystem : m_reflectionSystems )
		{
			reflectionSystem.ResetReflectionVariant();
		}
	}

	void DynamicReverbSystem::RenderDynamicReverbInfo( rend::FragmentBuilder& builder, Uint32 xPos, Uint32 yPos, const Uint32 ySize, const Uint32 xSize, const ::audio::DynamicReverbSettings* reverbSettings )
	{
		builder.AddFlatTextf( xPos, yPos, "Active Reverb Metadata: %s", m_currentReverb.AsChar() );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Reverb alogorithm: Environment Sphere with Layered Reverb" );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Closest distance: %f", m_closestWeightedDistance );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "%s (small): FL: %f, FR : %f, BL : %f, BR : %f",
							  reverbSettings->m_smallReverb.AsChar(),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontLeftReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontRightReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackLeftReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackRightReverb, audio::ReverbSize::SmallReverb ) );
		yPos += xSize;
		builder.AddFlatTextf( xPos, yPos, "%s (medum): FL: %f, FR : %f, BL : %f, BR : %f",
							  reverbSettings->m_mediumReverb.AsChar(),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontLeftReverb, audio::ReverbSize::MediumReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontRightReverb, audio::ReverbSize::MediumReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackLeftReverb, audio::ReverbSize::MediumReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackRightReverb, audio::ReverbSize::MediumReverb ) );
		yPos += xSize;
		builder.AddFlatTextf( xPos, yPos, "%s (large): FL: %f, FR : %f, BL : %f, BR : %f",
							  reverbSettings->m_largeReverb.AsChar(),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontLeftReverb, audio::ReverbSize::LargeReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontRightReverb, audio::ReverbSize::LargeReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackLeftReverb, audio::ReverbSize::LargeReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackRightReverb, audio::ReverbSize::LargeReverb ) );
	}

	void DynamicReverbSystem::RenderEnviroSmallStaticMediumReverbInfo( rend::FragmentBuilder& builder, Uint32 xPos, Uint32 yPos, const Uint32 ySize, const Uint32 xSize, const ::audio::DynamicReverbSettings* reverbSettings )
	{
		builder.AddFlatTextf( xPos, yPos, "Active Reverb Metadata: %s", m_currentReverb.AsChar() );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Reverb alogorithm: Environment Small, Static Medium" );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Closest distance: %f", m_closestWeightedDistance );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Environment: %s: FL: %f, FR : %f, BL : %f, BR : %f",
							  reverbSettings->m_smallReverb.AsChar(),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontLeftReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::FrontRightReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackLeftReverb, audio::ReverbSize::SmallReverb ),
							  CalculateReverbFactorForChannel( audio::ReverbChannel::BackRightReverb, audio::ReverbSize::SmallReverb ) );
		yPos += xSize;
		builder.AddFlatTextf( xPos, yPos, "Static: %s", reverbSettings->m_mediumReverb.AsChar() );
	}

	void DynamicReverbSystem::RenderStaticSmallReverbInfo( rend::FragmentBuilder& builder, Uint32 xPos, Uint32 yPos, const Uint32 ySize, const Uint32 xSize, const ::audio::DynamicReverbSettings* reverbSettings )
	{
		builder.EnableTextBackground( Color::DARK_GREEN() );
		builder.AddFlatTextf( xPos, yPos, "Active Reverb Metadata: %s", m_currentReverb.AsChar() );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "Reverb alogorithm: Simple Static Reverb" );
		yPos += ySize;
		builder.AddFlatTextf( xPos, yPos, "%s", reverbSettings->m_smallReverb.AsChar() );
	}


	Vector3 DynamicReverbSystem::Get2DListenerDir()
	{
		Vector3 listenerDir( GEngine->GetSoundSystem()->GetListenerDirection().X, GEngine->GetSoundSystem()->GetListenerDirection().Y, 0.f );
		listenerDir.Normalize();
		return listenerDir;
	}

	void DynamicReverbSystem::GetListenerQuat( Quaternion& quat ) const
	{
		Vector4 pos, up, front, side;

		GEngine->GetSoundSystem()->GetListenerVectors( pos, up, front );
		side = Vector4::Cross( front, up );
		side.Normalize3();
		if( m_hedgehogProvider )
		{
			const ::character::HedgehogQuery* hedgehogQuery = m_hedgehogProvider->GetHedgehogQuery();
			if( hedgehogQuery )
			{
				// keeping the up direction from the entity instead of the listener

				up = hedgehogQuery->GetOriginTransform().GetOrientation().GetZAxis4();
				up.Normalize3();

				side = Vector4::Cross( front, up );
				side.Normalize3();
				front = Vector4::Cross( up, side );
				front.Normalize3();
			}
		}

		side.W = .0f;
		front.W = .0f;
		up.W = .0f;

		Matrix listenerOrientationMatrix( side, front, up, Vector4::ZERO_3D_POINT() );
		quat = listenerOrientationMatrix.ToQuat();
		quat.Normalize();
	}

	void DynamicReverbSystem::UpdateAcousticEnvironment()
	{
		ScopedProfilerChannel scopedChannel( PBC_AUDIO );

		AUDIO_PC_SCOPE_FUNC();

		audio::SoundSystem* soundSystem = GEngine->GetSoundSystem();
		if( !soundSystem )
			return; // HACK: We shouldn't even create IGameAudioSystem on the server

		RED_SCOPE_LOCK( m_updateMutex );
		UpdateReverbDirections();
		UpdateReverbDistances();
		UpdateSmoothReverbDistanceForAmbients();
		PushReverbMetricsToEngine();

		for( AudioReflectionSystem& system : m_reflectionSystems )
		{
			system.UpdateReflections( *this );
		}

		UpdateCeilingDistanceRtpc();
		UpdateGlobalCeilingCoverage();
	}

	void DynamicReverbSystem::UpdateCeilingDistanceRtpc()
	{
		bool updistanceCaptured = false;
		if( const character::HedgehogQuery* hedgehog = GetHedgehog() )
		{
			const physics::AsyncQueryTokenPtr& result = hedgehog->GetUpRawResults();
			if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
			{
				GEngine->GetSoundSystem()->SetGlobalParameter( RED_NAME_CONSTEXPR( "g_ceiling_distance" ), result->m_result.m_distance );
				updistanceCaptured = true;
			}
		}
		if( !updistanceCaptured )
		{
			GEngine->GetSoundSystem()->SetGlobalParameter( RED_NAME_CONSTEXPR( "g_ceiling_distance" ), 100.0f );
		}
	}

	void DynamicReverbSystem::PushReverbMetricsToEngine()
	{
		for( Uint32 reverbSize = 0; reverbSize < audio::NumReverbSizes; reverbSize++ )
		{
			for( Uint32 reverbChannel = 0; reverbChannel < audio::NumReverbChannels; reverbChannel++ )
			{
				Float reverbFactor = CalculateReverbFactorForChannel( static_cast< audio::ReverbChannel >( reverbChannel ), static_cast< audio::ReverbSize >( reverbSize ) );
				GEngine->GetSoundSystem()->SetReverbFactor( static_cast< audio::ReverbSize >( reverbSize ), static_cast< audio::ReverbChannel >( reverbChannel ), reverbFactor );
			}
		}

		for( Uint32 reverbChannel = 0; reverbChannel < audio::NumReverbChannels; reverbChannel++ )
		{
			GEngine->GetSoundSystem()->SetAcousticWeightedDistance( static_cast< audio::ReverbChannel >( reverbChannel ), m_cachedWeightedDistances[ reverbChannel ] );
		}
	}

	void DynamicReverbSystem::UpdateReverbDistances()
	{
		const audio::DynamicReverbSettings* reverbSettings = GEngine->GetSoundSystem()->GetReverbSettings();

		if( !reverbSettings )
		{
			return;
		}
		if( reverbSettings->m_name != m_currentReverb )
		{
			m_reverbLevelsCrossFade.Init( audio::soundCurvePoint( reverbSettings->m_crossover1.m_dist, 1.f ), reverbSettings->m_crossover1.m_fade, audio::soundCurvePoint( reverbSettings->m_crossover2.m_dist, 1.f ), reverbSettings->m_crossover2.m_fade );
			m_currentReverb = reverbSettings->m_name;
		}

		Vector4 listenerPos = GEngine->GetSoundSystem()->GetListenerPosition();

		if( m_hedgehogProvider )
		{
			const ::character::HedgehogQuery* hedgehog = m_hedgehogProvider->GetHedgehogQuery();

			if( !hedgehog )
			{
				return;
			}

			for( Uint32 iYaw = 0; iYaw != c_numReverbDir; ++iYaw )
				for( Uint32 iPitch = 0; iPitch != ::audio::DynamicReverbSettings::c_numVerticalReverbDir; ++iPitch )
				{
					Float outPitch = ( (Int32)iPitch - 3 ) * c_reverbVerticalDirAngle;
					Float outYaw = iYaw * c_reverbDirAngle;

					hedgehog->GetHitDistance( outPitch,  outYaw, m_reverbDistance[ iYaw ][ iPitch ] );
				}

			UpdateAcousticsGraphOffset();

			Float closestDistanceAnyDirection[ audio::DynamicReverbSettings::c_numVerticalReverbDir ];
			Float closestDistanceSum = 0.f;

			for( int j = 0; j < audio::DynamicReverbSettings::c_numVerticalReverbDir; j++ )
			{
				Float closestDistance = reverbSettings->m_maxDistance;

				for( int i = 0; i < c_numReverbDir; i++ )
				{
					Float newDistance = m_reverbDistance[ i ][ j ];

					if( newDistance < closestDistance )
					{
						closestDistance = newDistance;
					}
				}

				closestDistanceAnyDirection[ j ] = closestDistance;
			}

			for( int i = 0; i < audio::DynamicReverbSettings::c_numVerticalReverbDir; i++ )
			{
				closestDistanceSum += closestDistanceAnyDirection[ i ] * reverbSettings->m_verticalWeightings[ i ];
			}

			GEngine->GetSoundSystem()->SetGlobalParameter( RED_NAME_CONSTEXPR( "closest_reverb_distance" ), closestDistanceSum );
			m_closestWeightedDistance = closestDistanceSum;

			for( Uint32 i = 0; i < audio::NumReverbChannels; i++ )
			{
				m_cachedWeightedDistances[ i ] = CalculateWeightedDistanceForChannel( (audio::ReverbChannel)i );
			}
		}
	}

	void DynamicReverbSystem::UpdateAcousticsGraphOffset()
	{
		constexpr Float acousticOffsetInfluenceDistance = 1.4f;
		Float acousticMaxInfluence = .0f;
		Vector3 acousticOffsetSum( .0f, .0f, .0f );
		const Uint32 acousticsCorrectionIPiutchIndex = ( audio::DynamicReverbSettings::c_numVerticalReverbDir >> 1 );

		for( Uint32 iYaw = 0; iYaw != c_numReverbDir; ++iYaw )
		{
			const Vector3& worldSpaceDir = m_reverbDir[ iYaw ].dir[ acousticsCorrectionIPiutchIndex ];

			if( m_reverbDistance[ iYaw ][ acousticsCorrectionIPiutchIndex ] < acousticOffsetInfluenceDistance )
			{
				Float influence = acousticOffsetInfluenceDistance - m_reverbDistance[ iYaw ][ acousticsCorrectionIPiutchIndex ];
				acousticMaxInfluence = Max( influence, acousticMaxInfluence );
				acousticOffsetSum -= worldSpaceDir * influence;
			}
		}
		Vector3 acousticsOffset( .0f, .0f, .0f );

		if( acousticMaxInfluence > .01f )
		{
			Float length = acousticOffsetSum.Mag();
			if( length > .0001f && length < 1e6f)
			{
				acousticsOffset = acousticOffsetSum * ( acousticMaxInfluence / length );
			}
			else
			{
				return;
			}
		}

		GEngine->GetSoundSystem()->GetAcousticsSystem()->SetListenerOffset( acousticsOffset );
	}
	void AudioReflectionSystem::UpdateReflections( DynamicReverbSystem& reverbSystem )
	{
		if( !reverbSystem.m_hedgehogProvider || !reverbSystem.m_hedgehogProvider->GetHedgehogQuery() || ! GEngine->GetSoundSystem() || m_reflectionSettings->m_reflectionEvent == CName::NONE() )
		{
			return;
		}

		Bool inVehicle = GEngine->GetSoundSystem()->GetDirectorSystem()->GetGameState().GetPlayerIsInAVehicle();

		if( inVehicle )
		{
			OverrideReflectionVariant(::audio::ReflectionVariant::LocalSpaceSideDirections );
		}
		else
		{
			ResetReflectionVariant();
		}

		m_frontDirection = reverbSystem.m_hedgehogProvider->GetHedgehogQuery()->GetOriginTransform().GetForward();
		m_listenerPosition = reverbSystem.m_hedgehogProvider->GetHedgehogQuery()->GetOriginTransform().GetPosition3();
		DeduceRoomSize( reverbSystem );

		if( m_usedReflectionVariant == ::audio::ReflectionVariant::FindingMaximumFaceAlignemnt ||  m_usedReflectionVariant == ::audio::ReflectionVariant::FindingMaximumFaceAligment2Sides )
		{
			UpdateMaximumFaceAlignentReflectionPositions( reverbSystem );
		}
		else if( m_usedReflectionVariant == ::audio::ReflectionVariant::LocalSpaceSideDirections )
		{
			UpdateSideReflectionPositions( reverbSystem );
		}
		else
		{
			UpdateFourDirectionReflectionPositions( reverbSystem, ( m_usedReflectionVariant == ::audio::ReflectionVariant::WorldSpaceFixedDrections ) );
		}

		UpdateUpReflection( reverbSystem );
	}

	void AudioReflectionSystem::UpdateUpReflection( DynamicReverbSystem& reverbSystem )
	{
		Bool upReflectionSet = false;
		const ::character::HedgehogQuery* hedgehog = reverbSystem.m_hedgehogProvider->GetHedgehogQuery();

		if( m_reflectionSettings->m_upReflectionEnabled )
		{
			if( hedgehog )
			{
				const physics::AsyncQueryTokenPtr& result = hedgehog->GetUpRawResults();

				if( result && result->DidHit() && result->m_result.m_distance < 100.0f )
				{
					SetReflectionPosition( m_upReflectionIndex, result->m_result.m_worldPosition );
					upReflectionSet = true;
				}
			}
		}

		if( !upReflectionSet )
		{
			m_emitters[ m_upReflectionIndex ] = nullptr;
		}
	}

	void AudioReflectionSystem::SetReflectionPosition( Uint32 index, const Vector3& position, Float gain )
	{

		m_cachedPositions[ index ] = position;
		if( !m_emitters[ index ] )
		{
			::audio::ReflectionEmitterContext emitterContext( s_refletionEmitternames[ index ], m_entityId );
			emitterContext.m_immediatePositionUpdate = ( index == m_upReflectionIndex );

			m_emitters[ index ] = ::audio::EmitterInterfaceBuilder( &emitterContext ).AquireEmitterInterface();
		}
		if( gain < .001f )
		{
			m_emitters[ index ] = nullptr;
			return;
		}

		// normalizing manually to avoid weird numeric error
		Vector3 listenerDir = m_listenerPosition - position;
		Float len = listenerDir.Mag();
		if (!(len > .001f && len < 1.e5f))
		{
			return;
		}
		Float oolen = 1.0f / len;
		listenerDir *= oolen;

		Float frontDot = m_frontDirection.Dot( listenerDir );
		Float cutoff = ( frontDot - ( 1.0f - m_reflectionSettings->m_backReflectionCutoffSpread ) ) / Max( m_reflectionSettings->m_backReflectionCutoffSoftness, .01f );
		cutoff = Clamp( cutoff, .0f, 1.0f );
		cutoff *= m_reflectionSettings->m_backReflectionCutoffStrength;

		if( cutoff > .99f )
		{
			m_emitters[ index ] = nullptr;
		}
		else
		{
			m_emitters[ index ]->SetTransform( WorldTransform( position ) );
			for( CName emitterName : s_refletionEmitternames )
			{
				::audio::SetSoundParameter( m_entityId, s_reflectionVolumeParam, gain * ( 1.0f - cutoff ), emitterName );
				// ^^ could be ooptimized by caching but needs to be reset everytime emitter changes (scary)- implement caching when it's necessary
			}
		}
	}

	void AudioReflectionSystem::DisableReflection( Uint32 index )
	{
		m_emitters[ index ] = nullptr;
	}

	void AudioReflectionSystem::DeduceRoomSize( DynamicReverbSystem& reverbSystem )
	{
		static constexpr Float c_ceilingIndoorDistance = 8.0f;
		if( !m_reflectionSettings->m_shortReflectionIndoors )
		{
			m_currentReflectionDistance = m_reflectionSettings->m_farReflectionDistance;
			return;
		}

		const ::character::HedgehogQuery* hedgehog = reverbSystem.m_hedgehogProvider->GetHedgehogQuery();

		if( !hedgehog )
		{
			return;
		}

		Uint32 outdoorCounter = 0;

		for( Uint32 i = 0; i < ::character::HedgehogQuery::k_numberOfSlices; ++i )
		{
			Float distance = 100.0f;
			hedgehog->GetHitDistance( 6, i, distance );
			if( distance >= c_ceilingIndoorDistance )
			{
				++outdoorCounter;
			}
		}

		Bool outdoor = ( outdoorCounter > (::character::HedgehogQuery::k_numberOfSlices >> 2 ) );
		m_currentReflectionDistance = outdoor ? m_reflectionSettings->m_farReflectionDistance : m_reflectionSettings->m_nearReflectionDistance;
	}

	void AudioReflectionSystem::UpdateFourDirectionReflectionPositions( DynamicReverbSystem& reverbSystem, Bool isWorldSpace )
	{
		if( !reverbSystem.m_hedgehogProvider )
		{
			return;
		}

		const ::character::HedgehogQuery* hedgehog = reverbSystem.m_hedgehogProvider->GetHedgehogQuery();

		if( !hedgehog )
		{
			return;
		}
		Float hedgehogYaw = isWorldSpace ? hedgehog->GetOriginTransform().GetYaw() : .0f;

		std::function< Bool( physics::AsyncQueryToken& ) > criterion = [this]( physics::AsyncQueryToken& token ) {
			return token.DidHit() && token.m_result.m_distance < m_currentReflectionDistance && token.m_result.m_worldNormal.Z < .707f && ( m_listenerPosition - token.m_result.m_worldPosition ).Dot( token.m_result.m_worldNormal ) > m_reflectionSettings->m_minimumFaceAlignement * token.m_result.m_distance;
		};

		physics::AsyncQueryToken* resToken;

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 45.0f - hedgehogYaw, criterion, resToken ) )
		{
			SetReflectionPosition( audio::FrontLeftReverb, resToken->m_result.m_worldPosition );
		}

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 315.f - hedgehogYaw, criterion, resToken ) )
		{
			SetReflectionPosition( audio::FrontRightReverb, resToken->m_result.m_worldPosition );
		}

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 135.f - hedgehogYaw, criterion, resToken ) )
		{
			SetReflectionPosition( audio::BackLeftReverb, resToken->m_result.m_worldPosition );
		}

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 225.f - hedgehogYaw, criterion, resToken ) )
		{
			SetReflectionPosition( audio::BackRightReverb, resToken->m_result.m_worldPosition );
		}
	}

	void AudioReflectionSystem::UpdateSideReflectionPositions( DynamicReverbSystem& reverbSystem )
	{
		if( !reverbSystem.m_hedgehogProvider || ! GEngine->GetSoundSystem() )
		{
			return;
		}

		const ::character::HedgehogQuery* hedgehog = reverbSystem.m_hedgehogProvider->GetHedgehogQuery();

		if( !hedgehog )
		{
			return;
		}
		Float hedgehogYaw = hedgehog->GetOriginTransform().GetYaw();

		std::function< Bool( physics::AsyncQueryToken& ) > criterion = [this]( physics::AsyncQueryToken& token ) {
			return token.DidHit() && token.m_result.m_distance < m_currentReflectionDistance && token.m_result.m_worldNormal.Z < 0.707f && ( m_listenerPosition - token.m_result.m_worldPosition ).Dot( token.m_result.m_worldNormal ) > .1f * token.m_result.m_distance;
		};

		physics::AsyncQueryToken* resToken;

		Vector3 listenerPosition = GEngine->GetSoundSystem()->GetListenerPosition();

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 90.0f - hedgehogYaw, criterion, resToken ) )
		{
			Vector3 pos = resToken->m_result.m_worldPosition;
			Vector3 pos2 = pos - listenerPosition;
			pos2 = listenerPosition + pos2 + pos2;
			SetReflectionPosition( audio::FrontLeftReverb, pos );
			SetReflectionPosition( audio::BackRightReverb, pos2 );
		}

		if( hedgehog->GetFittingNeighbour( m_reflectionSettings->m_fixedRaycastPitch, 270.f - hedgehogYaw, criterion, resToken ) )
		{
			Vector3 pos = resToken->m_result.m_worldPosition;
			Vector3 pos2 = pos - listenerPosition;
			pos2 = listenerPosition + pos2 + pos2;
			SetReflectionPosition( audio::FrontRightReverb, pos );
			SetReflectionPosition( audio::BackRightReverb, pos2 );
		}
	}

	void AudioReflectionSystem::OnRenderDebug( DynamicReverbSystem& reverbSystems, rend::FragmentBuilder& builder )
	{

		if( reverbSystems.m_hedgehogProvider && m_usedReflectionVariant == ::audio::ReflectionVariant::FindingMaximumFaceAlignemnt && m_renderFacingAlignment )
		{
			builder.EnableOverlay( true );
			builder.SetColor( Color::LIGHT_BLUE() );
			const ::character::HedgehogQuery* hedgehog = reverbSystems.m_hedgehogProvider->GetHedgehogQuery();
			if( !hedgehog )
			{
				return;
			}
			Vector3 startPos = hedgehog->GetOriginTransform().GetPosition3();
			const ::character::HedgehogQuery::RawResults& rawResults = hedgehog->GetRawResults();
			for( int i = 0; i < DynamicReverbSystem::c_numReverbDir; i++ )
			{
				for( Int32 j = 0; j < audio::DynamicReverbSettings::c_numVerticalReverbDir; j++ )
				{
					const physics::AsyncQueryTokenPtr& res = rawResults[ j ][ i ];

					if( res && res->DidHit() )
					{
						Vector3 dir = ( res->m_result.m_worldPosition - startPos ) / res->m_result.m_distance;
						Float alignment = res->m_result.m_worldNormal.Dot( dir );
						alignment *= -alignment * alignment;
						alignment = Max( alignment, .0f );
						Vector3 direction = reverbSystems.m_reverbDir[ i ].dir[ j ];
						Vector3 endPos = startPos + dir * 10.0f;
						builder.AddWireBox( endPos - Vector3( .5f ) * alignment, endPos + Vector3( .5f ) * alignment );
						builder.AddLine( endPos, endPos + res->m_result.m_worldNormal );
					}
				}
			}


			builder.EnableOverlay( false );
		}
	}

	void AudioReflectionSystem::OverrideReflectionVariant( audio::ReflectionVariant variant )
	{
		m_usedReflectionVariant = variant;
	}

	void AudioReflectionSystem::ResetReflectionVariant()
	{
		m_usedReflectionVariant = m_reflectionSettings->m_reflectionVariant;
	}

	void AudioReflectionSystem::UpdateMaximumFaceAlignentReflectionPositions( DynamicReverbSystem& reverbSystems )
	{

		const ::character::HedgehogQuery* hedgehog = reverbSystems.m_hedgehogProvider->GetHedgehogQuery();

		if( !hedgehog )
		{
			return;
		}

		const ::character::HedgehogQuery::RawResults& rawResults = hedgehog->GetRawResults();

		struct Candidate
		{
			Float m_distance;
			Vector3 m_position;
			const ::audio::ReflectionMaterialSettings* m_materialSettings;
			Candidate() = default;
			Candidate( Float distance, Vector3 position, const ::audio::ReflectionMaterialSettings* materialSettings )
				: m_distance( distance )
				, m_position( position )
				, m_materialSettings( materialSettings )
			{}
			Bool operator<( const Candidate& other ) const
			{
				return m_distance < other.m_distance;
			}
		};

		//typedef std::pair< Float, Vector3 > Candidate;
		typedef red::DynArray< Candidate > CandidateList;
		CandidateList candidates{red::PoolFrame()};
		candidates.Reserve(::character::HedgehogQuery::k_numberOfPlanes * ::character::HedgehogQuery::k_numberOfSlices );

		for( const ::character::HedgehogQuery::PlaneRawResults& planeRawResult : rawResults )
		{
			for( const physics::AsyncQueryTokenPtr& tokenPtr : planeRawResult )
			{
				if( tokenPtr && tokenPtr->DidHit() )
				{
					physics::TraceResult& traceResult = tokenPtr->m_result;
					if( traceResult.m_distance > m_currentReflectionDistance || traceResult.m_worldNormal.Z > .707f )
					{
						continue;
					}
					Vector3 position = traceResult.m_worldPosition;
					Float faceAlignment = ( m_listenerPosition - position ).Dot( traceResult.m_worldNormal ) / traceResult.m_distance;
					if( faceAlignment > .5f )
					{
						const ::audio::ReflectionMaterialSettings* materialSettings = ::audio::helpers::FindReflectionParams( traceResult.m_proxyId, traceResult.m_material );
						candidates.EmplaceBack( faceAlignment, position, materialSettings );
					}
				}
			}
		}

		Vector3 newPositions[ 4 ];
		const ::audio::ReflectionMaterialSettings* newMaterialSettings[ 4 ];

		for( Uint32 i = 0; i < m_upReflectionIndex; ++i )
		{
			CandidateList::iterator maxIterator = std::max_element( candidates.Begin(), candidates.End() );
			Vector3 maxPosition = maxIterator->m_position;
			Vector3 maxPosDiff = maxPosition - m_listenerPosition;
			Float maxPosDist = maxPosDiff.SquareMag();
			Float influenceDistanceInv = Max( .01f, 1.0f / ( maxPosition - m_listenerPosition ).SquareMag() );

			for( Candidate& candidate : candidates )
			{
				Float distSqr = ( candidate.m_position - maxPosition ).SquareMag();

				if( maxPosDiff.Dot( candidate.m_position - m_listenerPosition ) > .0f )
				{
					candidate.m_distance *= Min( distSqr * influenceDistanceInv, 1.0f );
				}
			}

			newPositions[ i ] = maxPosition;
			newMaterialSettings[ i ] = maxIterator->m_materialSettings;
		}
		Bool taken[] = {false, false, false, false};
		for( Uint32 i = 0; i < m_upReflectionIndex; ++i )
		{
			Float minDistSqr = 1.e10f;
			Uint32 nearestIndex = 0;
			for( Uint32 j = 0; j < m_upReflectionIndex; ++j )
			{
				if( taken[ j ] )
				{
					continue;
				}
				Float distance = ( m_cachedPositions[ i ] - newPositions[ j ] ).SquareMag();

				if( !taken[ j ] && minDistSqr > distance )
				{
					nearestIndex = j;
					minDistSqr = distance;
				}
			}
			taken[ nearestIndex ] = true;
			Float gain = 1.0f;

			Float newHighPass = 0.0f;
			Float newLowPass = 0.0f;
			if( newMaterialSettings[ nearestIndex ] )
			{
				const ::audio::ReflectionMaterialSettings* settings = newMaterialSettings[ nearestIndex ];
				gain = settings->m_gain;
				newHighPass = settings->m_highPass;
				newLowPass = settings->m_lowPass;
			}
			SetReflectionPosition( i, newPositions[ nearestIndex ], gain );
			if( m_emitters[ i ] )
			{
				Uint32 emitterId = m_emitters[ i ]->GetId();
				m_interface->PostEvent(::audio::CreateSetParameterEvent( emitterId, s_reflectionHighPassParam, newHighPass ) );
				m_interface->PostEvent(::audio::CreateSetParameterEvent( emitterId, s_reflectionLowPassParam, newLowPass ) );
			}
		}
	}

	void DynamicReverbSystem::UpdateReverbDirections()
	{
		Quaternion listenerQuat;
		GetListenerQuat( listenerQuat );

		const Float startingAngle = 0.f;

		for( int iYaw = 0; iYaw < c_numReverbDir; iYaw++ )
			for( int iPitch = 0; iPitch < audio::DynamicReverbSettings::c_numVerticalReverbDir; iPitch++ )
			{
				m_reverbDir[ iYaw ].dir[ iPitch ] = listenerQuat.Transform( m_reverbBaseDir[ iYaw ].dir[ iPitch ] );
			}
	}

	void DynamicReverbSystem::InitReverbBaseDirections()
	{
		m_reverbBaseDir.Resize( c_numReverbDir );
		for( int i = 0; i < c_numReverbDir; i++ )
		{
			m_reverbBaseDir[ i ].dir.Resize( audio::DynamicReverbSettings::c_numVerticalReverbDir );
		}

		auto verticalRotate = []( const Vector4& startDir, Float rotAng ) {
			Float z = MTan( DEG2RAD( rotAng ) ); // adjacent is a normalzed vector // *startDir.Mag2();
			Vector4 rotDir = startDir;
			rotDir.Z = z;
			rotDir.Normalize3();
			return rotDir;
		};

		const Float startingAngle = 0.f;
		Float dirAngle = startingAngle;
		Vector3 lastDir( .0f, 1.0f, .0f );

		for( int horizontalIndex = 0; horizontalIndex < c_numReverbDir; horizontalIndex++ )
		{
			Vector4 baseDir = ::audio::SoundUtils::RotateNormalizedVector( lastDir, DEG2RAD( Float( c_reverbDirAngle ) ) );
			lastDir = baseDir;

			// We don't test directly up/down here (would get tested multiple times)...can add that in later if needed
			// Start with the down most angle
			Float verticalAng = -1.f * c_reverbVerticalDirAngle * ( ( audio::DynamicReverbSettings::c_numVerticalReverbDir - 1 ) / 2 );

			for( int verticalIndex = 0; verticalIndex < audio::DynamicReverbSettings::c_numVerticalReverbDir; verticalIndex++ )
			{
				m_reverbBaseDir[ horizontalIndex ].dir[ verticalIndex ] = verticalRotate( baseDir, verticalAng );
				verticalAng += c_reverbVerticalDirAngle;
			}

			dirAngle += DEG2RAD( Float( c_reverbDirAngle ) );
			m_reverbChannels[ horizontalIndex ] = GetReverbChannel( dirAngle );
		}
	}

	Float DynamicReverbSystem::CalculateWeightedDistanceForChannel( audio::ReverbChannel channel )
	{
		return CalculateReverbFactorForChannel( channel, audio::ReverbSize::AllReverbSizes );
	}

	Float DynamicReverbSystem::CalculateReverbFactorForChannel( audio::ReverbChannel channel, audio::ReverbSize reverbSize )
	{
		if( channel != audio::ReverbChannel::FrontLeftReverb && channel != audio::ReverbChannel::FrontRightReverb && channel != audio::ReverbChannel::BackLeftReverb && channel != audio::ReverbChannel::BackRightReverb )
		{
			return 0.f;
		}

		const audio::DynamicReverbSettings* reverbSettings = GEngine->GetSoundSystem()->GetReverbSettings();

		if( reverbSettings == nullptr )
		{
			return 0.f;
		}

		Float distanceFactor = 0.f;

		Uint32 reverbDistanceCount = 0;

		auto getDistance = [&reverbDistanceCount, this]( audio::ReverbChannel channel, Int32 dir, Int32 verticalDir ) {
			Vector3 direction = m_reverbDir[ dir ].dir[ verticalDir ];
			Float dist = 0.f;

			if( channel == m_reverbChannels[ dir ] )
			{
				if( verticalDir == 0 )
				{
					reverbDistanceCount++;
				}
				dist = m_reverbDistance[ dir ][ verticalDir ];
			}

			return dist;
		};

		for( Int32 i = 0; i < c_numReverbDir; i++ )
		{
			for( Int32 j = 0; j < audio::DynamicReverbSettings::c_numVerticalReverbDir; j++ )
			{
				Float distance = getDistance( channel, i, j );
				if( distance > 0.f )
				{
					switch( reverbSize )
					{
					case audio::ReverbSize::SmallReverb:
						distanceFactor += m_reverbLevelsCrossFade.GetSection1().CalculateYVal( distance ) * reverbSettings->m_verticalWeightings[ j ];
						break;
					case audio::ReverbSize::MediumReverb:
						distanceFactor += m_reverbLevelsCrossFade.GetSection2().CalculateYVal( distance ) * reverbSettings->m_verticalWeightings[ j ];
						break;
					case audio::ReverbSize::LargeReverb:
						distanceFactor += m_reverbLevelsCrossFade.GetSection3().CalculateYVal( distance ) * reverbSettings->m_verticalWeightings[ j ];
						break;
					case audio::ReverbSize::AllReverbSizes:
						distanceFactor += distance * reverbSettings->m_verticalWeightings[ j ];
						break;
					case audio::ReverbSize::InvalidReverbSize:
					default:
						break;
					}
				}
			}
		}

		if( reverbDistanceCount != 0 )
		{
			distanceFactor /= reverbDistanceCount;
		}
		return distanceFactor;
	}

	void AudioReflectionSystem::Init( DynamicReverbSystem& reverbSystem, const ::audio::ReflectionEmitterSettings* reflectionSettings )
	{
		m_reflectionSettings = reflectionSettings;
		if( m_reflectionSettings->m_reflectionEvent == CName::NONE() )
		{
			return;
		}
		
		m_usedReflectionVariant = m_reflectionSettings->m_reflectionVariant;
		if( m_usedReflectionVariant == ::audio::ReflectionVariant::FindingMaximumFaceAligment2Sides )
		{
			m_upReflectionIndex = 2;
			m_numEmitters = 3;
		}
		::audio::AudioInterfaceContext audioContext( audio::CustomEntityType::Reflection );
		::audio::AudioInterfaceBuilder interfaceBuilder( &audioContext );
		m_interface = interfaceBuilder.AcquireAudioInterface();
		m_entityId = audioContext.GetId();

		red::Memzero( reverbSystem.m_cachedWeightedDistances, sizeof( Float ) * ::audio::NumReverbChannels );

		for( int i = 0; i < (int)m_numEmitters; i++ )
		{
			::audio::ReflectionEmitterContext emitterContext( s_refletionEmitternames[ i ], m_entityId );
			red::DynArray< CName > emitterDecotatorNames{::audio::PoolAudioSystems()};
			emitterDecotatorNames.PushBack( reflectionSettings->m_name );
			emitterContext.m_emitterMetadataNames = &emitterDecotatorNames;
			emitterContext.m_reflectionMetadataName = reflectionSettings->m_name;
			emitterContext.m_immediatePositionUpdate = ( i == m_upReflectionIndex );
			m_emitters[ i ] = ::audio::EmitterInterfaceBuilder( &emitterContext ).AquireEmitterInterface();
		}
	}

	void ListenerRoomDetector::UpdateDirection( Direction direction, const Vector3& sourcePosition )
	{
		::audio::acoustics::IntCoords* output = &( m_roomPositions[ static_cast< Uint32 >( direction ) ] );

		::audio::acoustics::Node* roomNode = GEngine->GetSoundSystem()->GetAcousticsSystem()->FindRootNodeByPosition( sourcePosition );
		if( roomNode )
		{
			*output = roomNode->GetCoords();
		}
		else
		{
			*output = ::audio::acoustics::IntCoords( GEngine->GetSoundSystem()->GetListenerPosition().AsVector3() );
		}
	}

	Bool ListenerRoomDetector::TestPosition( const Vector3& position ) const
	{
		::audio::acoustics::Node* node = GEngine->GetSoundSystem()->GetAcousticsSystem()->FindRootNodeByPosition( position );
		if( !node )
		{
			return false;
		}
		const ::audio::acoustics::IntCoords& nodeCoords = node->GetCoords();
		for( const ::audio::acoustics::IntCoords& coords : m_roomPositions )
		{
			if( coords == nodeCoords )
			{
				return true;
			}
		}
		return false;
	}

	Float ListenerRoomDetector::GetMaxOutDoorness() const
	{
		Float maxOutDoorness = .0f;
		for( const ::audio::acoustics::IntCoords& coords : m_roomPositions )
		{
			::audio::acoustics::Node* node = GEngine->GetSoundSystem()->GetAcousticsSystem()->FindNodeByPosition( coords );
			if( node )
			{
				maxOutDoorness = Max( node->GetOutdoorness(), maxOutDoorness );
			}
		}
		return maxOutDoorness;
	}

	void ListenerRoomDetector::OnRenderDebug( rend::FragmentBuilder& builder )
	{
		for(::audio::acoustics::IntCoords& listenerRoomPositions : m_roomPositions )
		{
			builder.SetColor( Color::BLUE() );
			builder.AddWireSphere( listenerRoomPositions.ToVector3(), .5f );
		}
	}
}
