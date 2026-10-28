///**
//* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
//*/
//

#include "build.h"

#include "projectile.h"
#include "projectileController.h"
#include "projectileEvents.h"
#include "projectileTrajectory.h"
#include "projectileParabolicTrajectory.h"
#include "projectileDebug.h"
#include "puppet.h"
#include "attack.h"
#include "statsSystem.h"

#include "../../gameCore/include/gameObject.h"
#include "../../gameCore/include/gameTimeSystemInterface.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../redMath/include/random.h"
#include "../../worldEntities/include/entityBinding.h"
#include "../../worldEntities/include/slotComponent.h"
#include "../../worldEntities/include/positionProvider.h"
#include "../../worldEntities/include/orientationProvider.h"
#include "../../redReflection/include/rttiFunctionCalling.h"
#include "../worldPhysics/include/runtimeSystemPhysics.h"
#include "../worldEntities/include/visualComponent.h"
#include "../world/include/worldNodeInstance.h"
#include "../worldEntities/include/velocityProvider.h"
#include "../gameCore/include/gameVisionModeSystemInterface.h"
#include "../world/include/gameInstance.h"
#include "../redReflection/include/mathUtils.h"
#include "../../common/gameCore/include/gameRttiExtensions.h"
#include "../../gameCore/include/photoModeSystemInterface.h"
#include <cstdio>
// [tkfix] coreplay hit probes (logging only), defined in scriptableSystemsContainer.cpp; on for ?sandbox / ?coreplay=1
extern volatile int g_coreplayHitProbe;
extern volatile int g_coreplayHitBudget[ 3 ];
extern volatile unsigned long long g_coreplayHitRing[ 8 ];
extern volatile int g_coreplayHitRingPos;
static inline bool CpHitProbeTake( int which ) { return g_coreplayHitProbe && __atomic_sub_fetch( &g_coreplayHitBudget[ which ], 1, __ATOMIC_RELAXED ) >= 0; }



#ifdef RED_USE_IMGUI
	#include "../../imgui/include/redImGui.h"
	#include "../renderDraw/include/renderDebugDrawHistorySystemInterface.h"
	#include "../redContainers/include/string/stringBuilder.h"
#endif


#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( CollisionEvaluator, game, projectile );
	RTTI_PARENT_TYPE( IScriptable );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( ScriptCollisionEvaluator, game, projectile );
	RTTI_PARENT_TYPE( game::projectile::CollisionEvaluator );
RTTI_END_TYPE();

RTTI_BEGIN_TYPE_IN_NAMESPACE( CollisionEvaluatorParams, game, projectile );
	RTTI_PARENT_TYPE( IScriptable );
	RTTI_SCRIPT_ALIAS( "CollisionEvaluatorParams" );
	RTTI_PROPERTY( m_target );
	RTTI_PROPERTY( m_isPiercableSurface );
	RTTI_PROPERTY( m_angle );
	RTTI_PROPERTY( m_numBounces );
	RTTI_PROPERTY( m_position );
	RTTI_PROPERTY( m_projectilePenetration );
	RTTI_PROPERTY( m_isTechPiercing );
RTTI_END_TYPE();

namespace
{
	Bool DEBUG_STEP_FRAME_ON = false;
	Int32 DEBUG_STEP_FRAME_FPS = false;

	constexpr Float c_minDistanceStopPostionLerp = 0.05f;
	constexpr Float c_minAngleInRadiansStopOrientationLerp = DEG2RAD( 5.0f );
	constexpr Float c_orientationLerpingMultiplier = 5.0f;
	

	const CName c_collisionEvaluatorFunctionName = RED_NAME_CONSTEXPR( "EvaluateCollision;gameprojectileOnCollisionActionCollisionEvaluatorParams" );
}

namespace game
{
	namespace projectile
	{
		// 
		// CollisionEvaluator / ScriptCollisionEvaluator
		// 

		OnCollisionAction CollisionEvaluator::Evaluate( const OnCollisionAction defaultOnCollisionAction, THandle<CollisionEvaluatorParams>& evalParams)
		{
			return defaultOnCollisionAction;
		}

		OnCollisionAction ScriptCollisionEvaluator::Evaluate( const OnCollisionAction defaultOnCollisionAction, THandle<CollisionEvaluatorParams>& evalParams )
		{
			OnCollisionAction onCollisionAction = defaultOnCollisionAction;
 			rtti::CallFunctionRet( this, rtti::RawFunctionName( c_collisionEvaluatorFunctionName ), onCollisionAction, defaultOnCollisionAction, evalParams );
			return onCollisionAction;
		}

		//
		// Controller
		//

		const Float DEFAULT_ENERGY_LOSS_FACTOR = 1.f;
		const Uint32 MAX_RECURRENT_CALLS = 5;

		Controller::Controller()
		{
			Reset();
		}

		Controller::~Controller()
		{
		}

		void Controller::Initialize( projectile::Component* owner, const physics::QueryFilter& data, const PreviewDisplay::InitContext& previewInitContext )
		{
			ent::Entity* ownerEntity = owner->GetEntity();
			
			world::RuntimeSystemPhysics* physicsSystem = ownerEntity->GetSceneSystem<world::RuntimeSystemPhysics>();
			game::ITimeSystem* timeSystem = ownerEntity->GetGameSystem< game::ITimeSystem >();
			m_photomodeSystem = WeakHandleFromPtr(ownerEntity->GetGameSystem<game::IPhotoModeSystem>());

			m_collisionResolver.Initialize( physicsSystem, timeSystem, ownerEntity, WeakHandleFromPtr( owner ), data, owner->GetCollisionsFilterClosest() );
			m_preview.Initialize( physicsSystem, timeSystem, ownerEntity, data );
			m_previewDisplay.Initialize( previewInitContext );
			m_explosionTime = previewInitContext.m_explosionTime;
		}

		void Controller::Uninitialize()
		{
			m_collisionResolver.Uninitialize();
			m_preview.Uninitialize();
			ClearPreview();
			Reset();
		}

		Bool Controller::ShouldTickMovement() const
		{
			if ( !m_wasLaunched || IsTrajectoryEmpty() )
			{
				return false;
			}

			return true;
		}

		void Controller::OnTickPreQuery( Component* owner, Float deltaTime, const tick::Info& tickInfo )
		{
#ifdef PROJECTILES_DEBUG
			if ( DEBUG_STEP_FRAME_ON )
			{
				if ( m_debugFrameStepTick )
				{
					deltaTime = 1.0f / ( Float ) DEBUG_STEP_FRAME_FPS;
					m_debugFrameStepTick = false;
				}
				else
				{
					return;
				}
			}
#endif

			if ( !m_wasLaunched )
			{
				return;
			}

			if( !m_hasMovementStarted )
			{
				// Get updated data from the launch position/orientation providers so that projectile movement starts at the intended launch position (e.g., the muzzle)
				SetInitialTransforms( owner );
			}

			THandle< TickEvent > tickEvent = CreateHandle< TickEvent >();
			tickEvent->m_deltaTime = deltaTime;
			
			if( owner->GetEntity()->HasExternalTransformDriver() )
			{
				// In the first tick after ScheduleBindExternalTransformDriver() it can happen that GetLocalToWorld() will return local offset instead of world position due to tick order
				// In this case pick hit position as the last one and then zero it.
				if( m_attachmentPositionFix.IsZero() )
				{
					tickEvent->m_position = owner->GetLocalToWorld().GetPosition().AsVector3();
				}
				else
				{
					tickEvent->m_position = m_attachmentPositionFix;
					m_attachmentPositionFix = Vector3::ZEROS();
				}
				m_currentTransform = tickEvent->m_position;
			}
			else
			{
				tickEvent->m_position = m_currentTransform.GetPosition();
			}
			

			owner->QueueEntityEvent( tickEvent );

			//No movement
			if ( IsTrajectoryEmpty() )
			{
				return;
			}

			ResolveTrajectoriesUpdate( owner, deltaTime, tickInfo );
		}

		void Controller::OnTickPostQuery( Component* owner, Float deltaTime, projectile::HitResults& hitResults, Uint32 recurrentCalls /*= 0*/, Bool hasBounced /*= false*/ )
		{
			if ( !ShouldTickMovement() )
			{
				return;
			}
			
			if ( recurrentCalls == 0 )
			{
				// we don't want tick it multiple times per frame
				m_proxyCooldownMap.Tick( deltaTime );
			}

			Vector3 currentPosition = m_currentTransform.GetPosition3();
			Vector3 moveVector = m_desiredTransform.GetPosition3() - currentPosition;

			OnCollisionAction onCollisionAction = owner->GetOnCollisionAction();

 			Bool hit = false;

			if ( !hitResults.Empty() )
			{
				if( recurrentCalls == 0  )
				{
					owner->PrepareForHit();
				}

				THandle<CollisionEvaluator> collisionEvaluator = owner->GetCollisionEvaluator();

				// we need to go through all of them and figure out which one we should use for handling bouncing / attachment
				// we'll choose the first, unless there's another with higher priority (ie stopping)
				HitResult chosenResult;
				CName chosenMaterialName;
				THandle<ent::IPlacedComponent> chosenPlacedComponent;
				WorldTransform chosenWorldTransform;
				ent::TransformBindingContext chosenTransformBindingContext;
				THandle<ent::Entity> chosenEntity;

				// the reason we can't just take m_velocity is because when it bounces off, it may have a velocity that is going against the current movement delta
				Vector3 forward = !moveVector.IsAlmostZero() ? moveVector.Normalized() : m_velocity.Normalized();

				auto hitEvent = CreateHandle< HitEvent >();

				for ( Int32 i = 0, limit = hitResults.Size(); i != limit; ++i )
				{
					const HitResult& result = hitResults[i];

					// check if Entity or INodeInstance
					void* resultProxy = result.m_hitObject.ToHandle().Get();
					if ( resultProxy == nullptr )
					{
						resultProxy = result.m_hitNodeInstance.ToHandle().Get();
					}
					
					// Put immediately on cooldown to avoid multiple collision events on same projectile in the same frame. 
					if ( m_proxyCooldownMap.CheckIsInCooldownAndUpdate( resultProxy, EProxyCooldownPhase::InCooldown ) )
					{
						// was this object already hit and in cooldown? ignore it
						continue;
					}

					Float hitDotProduct = result.m_traceResult.m_worldNormal.Dot( forward );

					if ( hitDotProduct > 0.0f )
					{
						// ignore if the normal and forward are "in the same direction", because specially with the sweeps you can get situations
						// where you "move it back" but because of imprecisions it will still be inside the object it collided with, and if we'd
						// adjust to the normal + radius there's visible popping in some situations
 						continue;
					}

					if ( m_numBounces == 0 && recurrentCalls == 0 && CpHitProbeTake( 1 ) )
					{
						// [tkfix] coreplay impact probe (logging only): what the projectile hit first, and where
						THandle< ent::Entity > cpHit = result.m_hitObject.ToHandle();
						const Vector3& cpPos = result.m_traceResult.m_worldPosition;
						std::fprintf( stderr, "[coreplay] PROJECTILE IMPACT: hit %s (%s) at (%.2f, %.2f, %.2f) material %s | projectile %s\n",
							cpHit ? cpHit->GetEntityID().ToDebugString().AsChar() : ( result.m_hitNodeInstance.ToHandle() ? "world node" : "none" ),
							cpHit ? cpHit->GetClass()->GetName().AsChar() : "-", cpPos.X, cpPos.Y, cpPos.Z, result.m_traceResult.m_material.AsChar(),
							owner->GetEntity() ? owner->GetEntity()->GetEntityID().ToDebugString().AsChar() : "-" );
					}

				// get the penetration value


					Bool isTechPiercing = (m_numBounces == 0) && attackutils::IsChargedTechWeaponAttack( m_weapon, m_weaponCharge );

					CName materialName = result.m_traceResult.m_material;
					THandle<ent::IPlacedComponent> placedComponent; // can we find a component to stick to?
					WorldTransform worldTransform; // transform to bring the projectile to local space if needed

					THandle<ent::Entity> hitEntity = result.m_hitObject.ToHandle();

					// binding in case we should stick to something
					ent::TransformBindingContext transformBindingContext;

					if ( collisionEvaluator.Get() != nullptr || onCollisionAction == OnCollisionAction::StopAndStick || onCollisionAction == OnCollisionAction::StopAndStickPerpendicular )
					{
						if ( hitEntity.Get() != nullptr )
						{
							// did it hit something that represents geometry?
							auto& hitShapes = result.m_hitRepresentationResult.m_hitShapes;

							for ( int i = 0, numShapes = hitShapes.Size(); i != numShapes; ++i )
							{
								auto& hitShape = hitShapes[i];

								THandle<ent::SlotComponent> slotComponent;
								ent::SlotId slotId;

								// @note #NA this is done here because in certain cases the hitShapes don't seem to have slotName (e.g. VTEK)
								materialName = hitShape.m_physicsMaterial;

								if ( !hitShape.m_slotName.Empty() && hitEntity->FindFirstSlotWithName( hitShape.m_slotName, slotComponent, slotId ) )
								{
									// if it's slot, we need to pass the slotName in the binding
									transformBindingContext.slotName = hitShape.m_slotName;
									placedComponent = slotComponent;

									// get transform from slotId
									slotComponent->GetTransformWS( slotId, worldTransform );

									break;
								}
							}
						}

						Float hitAngle = 90.0f - MathUtils::VectorUtils::GetAngleDegBetweenVectors( -forward, result.m_traceResult.m_worldNormal );

						if ( collisionEvaluator.Get() != nullptr )
						{
							Float attackPenetration = m_weaponAttackPenetration;
							
							if( isTechPiercing )
							{
								Float distLaunchToHitSq = m_launchPosition.DistanceSquaredTo( result.m_hitPosition );
								
								attackPenetration = RED_FLT_MAX;
								if( !m_hasPeircedTechSurface )
								{
									m_hasPeircedTechSurface = true;
									m_firstTechPiercePos = result.m_hitPosition;

								}
								else if( m_techTargetDistanceSq > 0.0f && m_techTargetDistanceSq > distLaunchToHitSq )
								{
									//Do nothing. Keep tech piercing until we reach target distance
								}
								else if( m_firstTechPiercePos.DistanceSquaredTo( result.m_hitPosition ) > attackutils::c_techPierceMaxDistanceSq )
								{
									isTechPiercing = false;
								}
								
								
							}
							const CName materialName = result.m_traceResult.m_material;
							Bool isPiercable = game::attackutils::IsPiercable( isTechPiercing, attackPenetration, materialName );

							
							THandle<CollisionEvaluatorParams> evalParams = CreateHandle<CollisionEvaluatorParams>();
							evalParams->m_target = Cast<game::Object>( hitEntity );
							evalParams->m_isPiercableSurface = isPiercable;
							evalParams->m_angle = hitAngle;
							evalParams->m_numBounces = m_numBounces;
							evalParams->m_position = result.m_hitPosition;
							evalParams->m_isTechPiercing = isTechPiercing;
							evalParams->m_projectilePenetration = game::rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "physicsMaterialTagProjectilePenetration" ), static_cast< Int64 >( physics::GetMaterialResource( materialName ).GetTags().m_projectilePenetration ) );
							onCollisionAction = collisionEvaluator->Evaluate( onCollisionAction, evalParams );
							
#ifdef RED_USE_IMGUI
							//TODO consolidate with projectile system debug info if this ends up being useful to a lot of people
							rend::IDebugDrawHistorySystem* debugHistorySystem = rend::IDebugDrawHistorySystem::GetDebugDrawHistorySystem();
							ent::Entity* ownerEntity = owner->GetOwner().ToHandle() ? owner->GetOwner().ToHandle().Get() : nullptr;

							if( debugHistorySystem && ownerEntity )
							{
								String entityName = hitEntity ? hitEntity->GetDebugName() : "";
								Vector3 collisionNormal = result.m_traceResult.m_worldNormal;

								debugHistorySystem->AddDebugDrawEntry( [onCollisionAction, forward, collisionNormal, isTechPiercing, evalParams, materialName, entityName]( rend::IDebugDrawer& dd )
								{
									rend::FragmentBuilder builder( dd );

									static float arrowLength = 0.33f;

									builder.SetColor( Color::YELLOW() );
									builder.AddArrow( evalParams->m_position, evalParams->m_position - forward * arrowLength );
									builder.SetColor( Color::BLUE() );
									builder.AddArrow( evalParams->m_position, evalParams->m_position + collisionNormal * arrowLength );


									builder.SetColor( Color::GREEN() );

									String collisionAction( "None" );
									switch( onCollisionAction )
									{
									case OnCollisionAction::Stop: collisionAction = String( "Stop" ); builder.SetColor( Color::RED() ); break;
									case OnCollisionAction::Bounce: collisionAction = String( "Bounce" ); break;
									case OnCollisionAction::StopAndStick: collisionAction = String( "StopAndStick" ); break;
									case OnCollisionAction::StopAndStickPerpendicular: collisionAction = String( "StopAndStickPerpendicular" ); break;
									case OnCollisionAction::Pierce: collisionAction = String( "Pierce" ); break;
									default: break;
									}

									builder.AddWireSphere( evalParams->m_position, 0.03f );
									builder.SetColor( Color::WHITE() );


									red::StringBuilder<red::String> stringBuilder;
									stringBuilder.Append( entityName );
									stringBuilder.Appendf( "\nResult: %s", collisionAction.AsChar() );
									stringBuilder.Appendf( "\nMaterial: %s", materialName.AsChar() );
									stringBuilder.Appendf( "\nAngle: %2.2f", evalParams->m_angle );
									stringBuilder.Appendf( "\nPiercable: %s %s", evalParams->m_isPiercableSurface ? "Yes" : "No",
															isTechPiercing ? "[TechPiercing]" : "" );
									stringBuilder.Appendf( "\nBounces: %d", evalParams->m_numBounces );

									builder.AddWorldText( evalParams->m_position, 0, 0, stringBuilder.AsChar() );
								}, "ProjectileCollisionEvaluatorResults", ownerEntity, &rend::IDebugDrawHistorySystem::DF_DebugDrawHistoryPlayer );
							}
							
#endif
						}
					}

					HitInstance hitInstance;

					hitInstance.m_traceResult = result.m_traceResult;
					hitInstance.m_traceResult.m_material = materialName;
					hitInstance.m_position = result.m_hitPosition;
					hitInstance.m_projectilePosition = m_currentTransform.GetPosition() + moveVector.Normalized() * result.m_distanceTravelled;
					hitInstance.m_projectileSourcePosition = m_currentTransform.GetPosition();
					hitInstance.m_forward = forward;
					hitInstance.m_velocity = m_velocity;
					hitInstance.m_hitObject = result.m_hitObject;
					hitInstance.m_hitWeakspot = result.m_hitWeakspot;
					hitInstance.m_hitRepresentationResult = result.m_hitRepresentationResult;
					hitInstance.m_numRicochetBounces = m_numBounces;

					hitEvent->m_hitInstances.PushBack( hitInstance );

					Bool shouldStop = onCollisionAction == OnCollisionAction::Stop || 
									  onCollisionAction == OnCollisionAction::StopAndStick || 
									  onCollisionAction == OnCollisionAction::StopAndStickPerpendicular ||
									  onCollisionAction == OnCollisionAction::StopAndStickPerpendicular ||
									  onCollisionAction == OnCollisionAction::Bounce;

					if ( !hit || shouldStop )
					{
						hit = true;
						chosenResult = result; // choose the first one by default, or replace when it should stop
						chosenMaterialName = materialName;
						chosenPlacedComponent = placedComponent;
						chosenWorldTransform = worldTransform;
						chosenTransformBindingContext = transformBindingContext;
						chosenEntity = hitEntity;

						if ( shouldStop )
						{
							break;
						}
					}
				}

				if ( !hitEvent->m_hitInstances.Empty() )
				{
					owner->QueueEntityEvent(hitEvent);
				}

				hitResults.Clear();
				// decide which direction it is facing
				m_desiredTransform.SetOrientation( Quaternion::IDENTITY().BuildFromDirectionVector( onCollisionAction == OnCollisionAction::StopAndStickPerpendicular ? chosenResult.m_traceResult.m_worldNormal : forward ) );
				
				if ( hit )
				{
					Vector3 adjustedHitPosition;
					if ( owner->GetUseSweepCollision() )
					{
						adjustedHitPosition = currentPosition + forward * chosenResult.m_distanceTravelled; // so that the object doesn't move around
					}
					else
					{
						adjustedHitPosition = chosenResult.m_hitPosition;
					}

					if( onCollisionAction == OnCollisionAction::Bounce )
					{
						ResolveBounceCollision( owner, chosenResult.m_hitObject, hitResults, adjustedHitPosition, chosenResult.m_traceResult.m_worldNormal, m_currentTransform, recurrentCalls, hasBounced );
						
					}
					else if( onCollisionAction == OnCollisionAction::Pierce )
					{
						// Do nothing we will pass through...
					}
					else if ( onCollisionAction == OnCollisionAction::Stop )
					{
						// just set position
						m_desiredTransform.SetPosition( adjustedHitPosition );
					}
					else if ( onCollisionAction == OnCollisionAction::StopAndStick || onCollisionAction == OnCollisionAction::StopAndStickPerpendicular )
					{
						// set position
						m_desiredTransform.SetPosition( adjustedHitPosition );

						// couldn't find any slot
						if ( chosenPlacedComponent.Get() == nullptr )
						{
							// check component
							chosenPlacedComponent = Cast<ent::IPlacedComponent>( chosenResult.m_hitComponent );

							// @note we can't do it just bellow since it may have been determined in the code above if it was a slot)
							if ( chosenPlacedComponent.Get() != nullptr )
							{
								chosenWorldTransform = chosenPlacedComponent->GetLocalToWorld();
							}
						}

						if ( chosenPlacedComponent.Get() != nullptr )
						{
							owner->GetEntity()->ScheduleBindExternalTransformDriver( chosenPlacedComponent, chosenTransformBindingContext );
						}
						else if ( chosenEntity.Get() != nullptr ) // if everything else failed, fallback to entity (or will stay in world)
						{
							owner->GetEntity()->ScheduleBindExternalTransformDriver( chosenEntity.Get(), chosenTransformBindingContext );

							chosenWorldTransform = chosenEntity->GetWorldTransform();
						}

						// bring transform to local space
						m_desiredTransform = chosenWorldTransform.TransformInv( m_desiredTransform );
						m_attachmentPositionFix = adjustedHitPosition;
					}
				}
			}

			ApplyDesiredTransform( deltaTime, owner, m_desiredTransform );
			m_rotationController.Tick( deltaTime );

			if ( hit && (!m_hasEnergy || onCollisionAction == OnCollisionAction::Stop || onCollisionAction == OnCollisionAction::StopAndStick || onCollisionAction == OnCollisionAction::StopAndStickPerpendicular) )
			{
				ClearTrajectories( owner );
			}
		}

		void Controller::OnTickPostCamera( Component* owner, Float deltaTime )
		{
			bool isPhotomodeActive = false;
			
			if (auto photomode = m_photomodeSystem.ToHandle())
			{
				isPhotomodeActive = photomode->IsPhotoModeActive();
			}
			
			if ( owner->IsDisplayingPreview() && !isPhotomodeActive )
			{
				// preview
				red::DynArray< Vector3 > trajectoryPointsOverTime{ red::PoolFrame() };
				red::DynArray< Vector3 > bouncePoints{ red::PoolFrame() };
				red::DynArray< Vector3 > bounceNormals{ red::PoolFrame() };
				m_preview.GetPreviewTrajectory( *this, trajectoryPointsOverTime, bouncePoints, bounceNormals, m_explosionTime );
				if( !trajectoryPointsOverTime.Empty() )
				{
					// distribute points uniformly
					Float segmentSize = m_previewSegmentSize();
					red::DynArray< Vector3 > trajectoryPoints{ red::PoolFrame() };
					trajectoryPoints.Reserve( trajectoryPointsOverTime.Size() );

					Float leftover = 0.0f;
					Float currentDist = 0.0f;
					Float lastDist = 0.0f;
					Bool stopped = false;
					
					for( Uint32 i = 0; i < trajectoryPointsOverTime.Size() - 1; ++i )
					{
						auto& p1 = trajectoryPointsOverTime[i];
						auto& p2 = trajectoryPointsOverTime[i + 1];
						Vector3 segment = p2 - p1;
						Float mag = segment.Mag();
						if( mag < 0.001f )
						{
							stopped = true;
						}

						Float nextDist = lastDist + mag;

						while( currentDist + segmentSize <= nextDist )
						{
							currentDist += segmentSize;
							trajectoryPoints.PushBack( p1 + segment.Normalized() * (currentDist - lastDist) );
						}

						leftover = nextDist - currentDist;
						lastDist = nextDist;

						if( i == trajectoryPointsOverTime.Size() - 2 || stopped )
						{
							trajectoryPoints.PushBack( p2 );
						}

						if( stopped )
						{
							break;
						}
					}

					m_previewDisplay.UpdatePoints( trajectoryPoints, bouncePoints, bounceNormals, m_preview.GetAimUpVector() );
				}
			}
			else
			{
				ClearPreview();
			}
		}

		Bool Controller::RequestRaycast( physics::RaycastParams& outRycastParams )
		{
			if ( !ShouldTickMovement() )
			{
				return false;
			}

			m_collisionResolver.CreateRaycastParams( m_currentTransform.GetPosition(), m_desiredTransform.GetPosition(), outRycastParams );
			return true;
		}

		Bool Controller::RequestSweep( const Float radius, physics::SweepParams& outSweepParams )
		{
			if ( !ShouldTickMovement() )
			{
				return false;
			}

			m_collisionResolver.CreateSweepParams( m_currentTransform.GetPosition(), m_desiredTransform.GetPosition(), radius, outSweepParams );
			return true;
		}

		void Controller::SetPhysicsRaycastResult( const physics::AsyncQueryTokenN& token, projectile::HitResults& hitResults )
		{
			m_collisionResolver.SetPhysicsRaycastResult( token, hitResults );
		}

		void Controller::SetPhysicsSweepResult( const physics::AsyncQueryTokenN& token, projectile::HitResults& hitResults )
		{
			m_collisionResolver.SetPhysicsSweepResult( token, hitResults );
		}

#ifdef PROJECTILES_DEBUG
		void Controller::OnRenderDebug( rend::IDebugDrawer& dd, const debug::Filters& debugFilters ) const
		{
			if( debugFilters.m_transform )
			{
				rend::FragmentBuilder builder( dd );
				game::debug::DrawAxis( builder, m_currentTransform.GetPosition3(), m_currentTransform.GetForward(), m_currentTransform.GetRight(), m_currentTransform.GetUp() );

				if( m_trajectory != nullptr )
				{
					m_trajectory->OnRenderDebug( *this, dd );
				}
			}

			if( debugFilters.m_visualTransform )
			{
				rend::FragmentBuilder builder( dd );
				game::debug::DrawAxis( builder, m_visualTransform.GetPosition3(), m_visualTransform.GetForward(), m_visualTransform.GetRight(), m_visualTransform.GetUp() );
			}

			DEBUG_STEP_FRAME_ON = debugFilters.m_frameStep;
			DEBUG_STEP_FRAME_FPS = debugFilters.m_frameStepFPS;

			if( debugFilters.m_trajectory )
			{
				if ( m_trajectoryDBG )
				{
					m_trajectoryDBG->DrawSteps( dd );
				}
			}
			
			if ( debugFilters.m_debugTransforms )
			{
				m_previewDisplay.OnRenderDebug( dd );
			}
			

		}

		void Controller::ToImGui( const Component* owner ) const
		{
			if ( ImGui::CollapsingHeader( "Controller" ) )
			{
				debug::ImGuiInspectorSection section;
				section.Entry( "Current position", m_currentTransform.GetPosition() );
				section.Entry( "Current orientation", m_currentTransform.GetOrientation() );
				section.Entry( "Desired position", m_desiredTransform.GetPosition() );
				section.Entry( "Desired orientation", m_desiredTransform.GetOrientation() );
				section.Entry( "Visual position", m_visualTransform.GetPosition() );
				section.Entry( "Visual orientation", m_visualTransform.GetOrientation() );
				if ( owner )
				{
					section.Entry( "Entity position", owner->GetEntity()->GetWorldPosition().AsVector3() );
					section.Entry( "Entity orientation", owner->GetEntity()->GetWorldOrientation() );
				}

				section.Entry( "Velocity vector", m_velocity );
				section.Entry( "Energy loss factor", m_energyLossFactor );
				section.Entry( "Has energy", m_hasEnergy );
			}

			if ( ImGui::CollapsingHeader( "Trajectory", ImGuiTreeNodeFlags_DefaultOpen ) )
			{
				debug::ImGuiInspectorSection section;

				if ( m_trajectory )
				{
					section.Entry( "Name", m_trajectory->GetDebugName() );

					m_trajectory->ToImGui( *this );
				}
			}
		}
#endif

		void Controller::SetUp( const SetUpEvent& evt, Component* owner )
		{
			Reset();

			if ( evt.m_trajectoryParams )
			{
				red::UniquePtr<Trajectory> trajectory = evt.m_trajectoryParams->CreateTrajectory();
				AddTrajectory( std::move( trajectory ), owner );
			}
			m_collisionResolver.OnSetUp( evt );
			//TODO preview requires refactor
			m_preview.SetUp( Cast< Puppet >( evt.m_owner ), HandleFromPtr( owner ), m_trajectory.Get() );

			m_lerpMultiplier = evt.m_lerpMultiplier;
		}

		void Controller::Launch( Component* owner, const LaunchEvent& evt )
		{
			Float hitCooldown = -1.f;
			if( evt.m_gameEffectBlackboard )
			{
				evt.m_gameEffectBlackboard->Read( BLACKBOARD_ID( EffectData, hitCooldown ), hitCooldown );
			}
			m_proxyCooldownMap.SetCooldown( hitCooldown );

			m_RicochetData = evt.m_projectileParams.m_ricochetData;

			m_launchParams = evt.m_launchParams;

			SetInitialTransforms( owner );

			// CYB-538874
			// If we are a throwable item, it will take one more frame to actually break the slot attachment (AttachmentSlots::ProcessDetachedItems).
			// We can't call ApplyDesiredTransform now because it assumes the transform is in world space, which will cause an incorrect offset.
			if ( !evt.m_isThrowableItem )
			{
				ApplyDesiredTransform( 0.f, owner, m_currentTransform );
			}

			if( evt.m_projectileParams.m_ignoreWeaponOwnerCollision )
			{
				if( const THandle< game::Object >& ownerHandle = evt.m_owner.ToHandle() )
				{
					AddIgnoredEntity( ownerHandle->GetEntityID() );
				}
			}

			for( const ent::EntityID& entityID : evt.m_projectileParams.m_objectsToIgnoreCollisionWith )
			{
				AddIgnoredEntity( entityID );
			}

			m_wasLaunched = true;

			m_weapon = evt.m_weapon;
			m_weaponCharge = evt.m_projectileParams.m_charge;

			if( auto weapon = m_weapon.ToHandle() )
			{
				auto statsSystem = weapon->GetGameSystem<game::StatsSystem>();
				if( statsSystem )
				{
					m_weaponAttackPenetration = statsSystem->GetValue( weapon->GetEntityID(), data::StatType::AttackPenetration );
				}
			}
			
			
			//Check for tech highlighting for player
			THandle<Object> ownerEntity = evt.m_owner.ToHandle();
			QueryForTechTargets( ownerEntity );

			if ( auto projectileEntity = owner->GetEntity() )
			{
				if ( !evt.m_projectileParams.m_targetPosition.IsOk() || evt.m_projectileParams.m_targetPosition == Vector4::PLUS_MAX() )
				{
					projectileEntity->CallEvent(
						RED_NAME_CONSTEXPR( "OnShoot" ),
						CreateHandle< ShootEvent >(
							evt.m_owner,
							evt.m_weapon,
							m_currentTransform.ToMatrix(),
							m_trajectoryStartPoint,
							m_velocity,
							GetInitialOwnerVelocity( owner ),
							evt.m_projectileParams ) );
				}
				else
				{
					projectileEntity->CallEvent(
						RED_NAME_CONSTEXPR( "OnShootTarget" ),
						CreateHandle< ShootTargetEvent >(
							evt.m_owner,
							evt.m_weapon,
							m_currentTransform.ToMatrix(),
							m_trajectoryStartPoint,
							m_velocity,
							GetInitialOwnerVelocity( owner ),
							evt.m_projectileParams ) );
				}
			}
		}

		// For player using techpiercing shot.. 
		// Do an extra raycast at launch to check for highlighted targets
		// If one is found then set piercingDistance on bullet to help player hit them. 
		// So we cheat the piercing logic to help user Interface be accurate more often
		// Maybe should only be done for linear trajectories but hard to predict that as scripts can change the trajectory often
		void Controller::QueryForTechTargets( THandle<Object>& ownerEntity )
		{
			THandle<Object> weapon = m_weapon.ToHandle();
			m_techTargetDistanceSq = -1.f;

			if( weapon && ownerEntity && ownerEntity->IsPlayerControlled() )
			{
				Bool isTechWeaponShot = attackutils::IsChargedTechWeaponAttack( m_weapon, m_weaponCharge );
				if( isTechWeaponShot )
				{
					static const auto techPierceFilter = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "AI" ) );
					physics::TraceQueryBuffer results;
					world::RuntimeSystemPhysics* physicsSystem = weapon->GetSceneSystem<world::RuntimeSystemPhysics>();
					auto visionSystem = weapon->GetGame()->GetSystemPtr< IVisionModeSystem >();

					// attacks can go further but this is enough to check for highlighting
					constexpr Float c_maxTechPierceHighlightTest = 100.f;
					Vector4 endPos = m_desiredTransform.GetPosition() + m_desiredTransform.GetForward() * c_maxTechPierceHighlightTest;
					physics::RaycastParams params = physics::RaycastParams::Create( m_desiredTransform.GetPosition(), endPos, techPierceFilter ).SetUseCase( physics::QueryUseCase::Gameplay );
					physicsSystem->SyncRaycastN( params, results );

					for( physics::TraceResult& result : results )
					{
						const THandle< ent::Entity >& hitEntity = result.FindHitObject< ent::Entity >();
						if( hitEntity )
						{
							if( visionSystem->IsVisibleThroughWalls( hitEntity ) )
							{
								//Extend a bit past the hit position. We do this as we aren't going to check for hit represenation here
								static constexpr Float c_hitPosOffset = 0.5f; 
								m_techTargetDistanceSq = Max( math::Pow( result.m_distance + c_hitPosOffset, 2.f ), m_techTargetDistanceSq );
#ifdef RED_USE_IMGUI
								rend::IDebugDrawHistorySystem* debugHistorySystem = rend::IDebugDrawHistorySystem::GetDebugDrawHistorySystem();
								if( debugHistorySystem )
								{
									debugHistorySystem->AddDebugDrawEntry( [result]( rend::IDebugDrawer& dd )
									{
										rend::FragmentBuilder builder( dd );
										builder.SetColor( Color::GREEN() );
										builder.AddWireSphere( result.m_worldPosition, 0.03f );
										builder.SetColor( Color::WHITE() );
										builder.AddWorldText( result.m_worldPosition, 0, 10, red::String::Printf( "Dist: %2.2f", result.m_distance ).AsChar() );
									}, "ProjectileVisionHighlightedTarget", ownerEntity.Get(), &rend::IDebugDrawHistorySystem::DF_DebugDrawHistoryPlayer );
								}
#endif	
							}
						}
					}
				}
			}
		}

		void Controller::AddTrajectory( red::UniquePtr<Trajectory>&& trajectory, Component* owner )
		{
			m_trajectory.Reset();
			m_trajectory = std::move( trajectory );
		}

		void Controller::SetDesiredTransform( const Transform& transform )
		{
			m_desiredTransform = transform;
		}

		void Controller::ClearTrajectories(const Component* owner)
		{
			m_trajectory.Reset();
			m_desiredTransform = m_currentTransform;
			m_projectileUpVector = Vector3::EZ();
			m_trajectoryStartPoint = m_desiredTransform.GetPosition3();
			m_launchPosition = m_desiredTransform.GetPosition3();
			m_velocity = Vector3::ZEROS();
			m_rotationController.Reset();
		}

		void Controller::AddAxisRotation( const Vector3& axis, const Float value )
		{
			m_rotationController.AddAxisRotation( axis, value );
		}

		void Controller::Reset()
		{
			m_trajectory.Reset();
			m_desiredTransform = Transform::IDENTITY();
			m_currentTransform = Transform::IDENTITY();
			m_projectileUpVector = Vector3::EZ();
			m_trajectoryStartPoint = Vector3::PLUS_MAX();
			m_launchPosition = Vector3::PLUS_MAX();
			m_velocity = Vector3::ZEROS();
			m_attachmentPositionFix = Vector3::ZEROS();
			m_energyLossFactor = DEFAULT_ENERGY_LOSS_FACTOR;
			m_puppetEnergyLossFactor = DEFAULT_ENERGY_LOSS_FACTOR;
			m_hasEnergy = true;
			m_lastUsedDeltaTime = 0.f;
			m_wasLaunched = false;
			m_trajectoryPrint.Reset();
			m_rotationController.Reset();
			m_proxyCooldownMap.Reset();
			m_numBounces = 0;
			m_hasPendingLockOrientationRequest = false;
			m_lockOrientation = false;
			m_spiralController.Reset();
			m_techTargetDistanceSq = -1.f;

			m_currSpiralLocalOffset = Vector3::ZEROS();

			m_weapon = nullptr;
			m_hasPeircedTechSurface = false;
			m_firstTechPiercePos = Vector4::ZEROS();
			m_weaponCharge = 0.0f;
			m_weaponAttackPenetration = 0.0f;

			m_hasMovementStarted = false;

#ifdef PROJECTILES_DEBUG
			m_trajectoryDBG = red::CreateUniquePtr< debug::TrajectoryArray >();
#endif
		}
		void Controller::SetSpiral( const SpiralParams& params )
		{
			m_spiralController.SetParams( params );
		}

		void Controller::ApplyEnergyLoss( Vector3& outVelocity, WeakHandle< ent::Entity > hitObject )
		{
			if( THandle<game::PuppetBase> hitPuppetBase = Cast<game::PuppetBase>( hitObject.ToHandle() ) )
			{
				outVelocity = outVelocity * m_puppetEnergyLossFactor;
			}
			else
			{
				outVelocity = outVelocity * m_energyLossFactor;
			}
		}

		void Controller::ResolveTrajectoriesUpdate( Component* owner, Float deltaTime, const tick::Info& tickInfo )
		{
			if( !m_trajectory ||
				!m_hasEnergy )
			{
				m_rotationController.ClearAxisRotations();
				return;
			}

			if( m_hasPendingLockOrientationRequest )
			{
				m_lockedOrientationDirection = m_desiredTransform.GetForward();
				m_hasPendingLockOrientationRequest = false;
			}

			if( !m_trajectory->IsLaunched() )
			{
				Vector3 ownerVelocity = Vector3::ZEROS();

				// If this is the first trajectory being launched, inherit the owner's velocity
				if( !m_hasMovementStarted )
				{
					ownerVelocity = GetInitialOwnerVelocity( owner );
					m_hasMovementStarted = true;
				}

				Trajectory::LaunchContext context( owner, m_trajectoryStartPoint, m_trajectoryStartPoint, m_velocity, m_desiredTransform.GetForward(), ownerVelocity, std::move( m_trajectoryPrint ) );
				m_trajectory->Launch( context, m_velocity );
			}

			if( !m_trajectoryPrint )
			{
				m_trajectoryPrint = red::CreateUniquePtr< TrajectoryPrint >();
			}

			// We have to remove the effect of the spiral before calling m_trajectory->UpdateMovement as 
			// some trajectories adjust their algorithm based on curentTransform (Ex. FollowCurve). 
			// The intent of spiral controller was to be an additive effect and not affect the base trajectory. 
			Transform contextTransform = m_currentTransform;
			Vector3 worldOffsetToRemove = contextTransform.TransformVector( m_currSpiralLocalOffset );
			contextTransform.SetPosition( contextTransform.GetPosition3() - worldOffsetToRemove );
			
			m_desiredTransform.SetPosition( m_trajectoryStartPoint );
			Trajectory::UpdateContext context( owner, contextTransform, deltaTime, m_trajectoryStartPoint );

			if ( m_trajectory->UpdateMovement( context, m_trajectoryPrint->m_offset, m_velocity ) )
			{
				m_desiredTransform = m_desiredTransform * m_trajectoryPrint->m_offset;
			}
			m_trajectoryPrint->m_velocity = m_velocity;

			{
				Vector3 targetPosition( Vector3::PLUS_MAX() );
				m_trajectory->GetTargetPosition( targetPosition );
				if( m_spiralController.Update( deltaTime, m_desiredTransform, targetPosition, m_currSpiralLocalOffset ) )
				{
					Vector3 worldOffset = m_desiredTransform.TransformVector( m_currSpiralLocalOffset );
					m_desiredTransform.SetPosition( m_desiredTransform.GetPosition3() + worldOffset );
				}
				
				Quaternion rotation;
				Vector3 direction = m_lockOrientation && !m_hasPendingLockOrientationRequest ? m_lockedOrientationDirection : m_velocity.Normalized();
				rotation.BuildFromDirectionVector( direction, m_projectileUpVector );
				
				// add offset to the mesh actually points towards velocity direction
				rotation = rotation * owner->GetRotationOffset();

				if( owner->IsAxisRotationEnabled() )
				{
					rotation *= m_rotationController.GetRotation();
				}

				// lerp it because of direction changes
				// use realtime as we want orientation to stay up to date during time dilation. 
				rotation = Quaternion::Lerp( m_desiredTransform.GetOrientation(), rotation, tickInfo.m_realTimeDelta * c_orientationLerpingMultiplier ); 

				m_desiredTransform.SetOrientation( rotation );
			}

			m_lastUsedDeltaTime = deltaTime;
		}

		void Controller::ResolveBounceCollision(Component* owner, WeakHandle< ent::Entity > hitObject, projectile::HitResults& hitResults, const Vector3& hitPos, const Vector3& hitNormal, const Transform& currentTransform, Uint32 recurrentCalls, Bool hasBounced )
		{	
			//Allow multiple hits on same object on a bounce 
			m_proxyCooldownMap.Reset();

			Vector3 currentPosition = currentTransform.GetPosition3();
			Vector3 desiredPosition = m_desiredTransform.GetPosition3();
			Vector3 moveVector = desiredPosition - currentPosition;
			Float desiredMovementLength = moveVector.Mag();
			//GAME_ASSERT( desiredMovementLength != 0.f, Projectile, "Desired position is the same as current" );
			if ( desiredMovementLength == 0.0f )
			{
				return;
			}

			if( !hasBounced )
			{
				hasBounced = true;
				m_numBounces++;
			}

			Vector3 moveVectorAfterHitPos = desiredPosition - hitPos;
			Float lengthMoveVectorAfterHitPos = moveVectorAfterHitPos.Mag();

			Float afterHitTime = m_lastUsedDeltaTime * lengthMoveVectorAfterHitPos / desiredMovementLength;
			if ( afterHitTime > m_lastUsedDeltaTime )
			{
				// this needs to be clamped because of the minimum sweep / raycast radius, which means that it could return lengthMoveVectorAfterHitPos > desiredMovementLength
				afterHitTime = m_lastUsedDeltaTime;
			}

			// keep in mind that hitPos is already adjusted to radius if it is a sweep
			m_trajectoryStartPoint = hitPos;

			// now that we hit, we start from where we are now
			m_currentTransform.SetPosition( m_trajectoryStartPoint ); // keep rotation
			m_desiredTransform = m_currentTransform;
			m_visualTransform = m_currentTransform;
			
			ApplyEnergyLoss( m_velocity, hitObject );
			
			Vector3 moveVectorUpToHitPos = currentPosition - hitPos; // is it hitting "the same spot"?

			if( m_velocity.SquareMag() < CollisionResolver::COLLISION_PRECISION_SQR || moveVectorUpToHitPos.Mag() < 0.001 )
			{
				m_hasEnergy = false;
				return;
			}
			
			Trajectory::CollisionContext context( owner, currentTransform, afterHitTime, hitPos, hitNormal, &m_RicochetData );
			
			if ( m_trajectory->UpdateCollision( context, m_trajectoryPrint->m_offset, m_velocity ) == false )
			{
				return;
			}
			
			m_desiredTransform = m_desiredTransform * m_trajectoryPrint->m_offset;
			m_lastUsedDeltaTime = afterHitTime;

#ifdef PROJECTILES_DEBUG
			if( m_trajectoryDBG )
			{
				m_trajectoryDBG->AddCollisionStep( { hitPos, hitNormal, m_desiredTransform.GetPosition() } );
			}
#endif

// 			//Synchronous collision test with recurrent call to test immediate collision after another. 
// 			//Especially important in low frame rate because the with low velocity and FPS projectile can go through colliders.
			if ( recurrentCalls < MAX_RECURRENT_CALLS )
			{
				// This is added so in recurrent call we won't start from collider surface;
				static const Float OFFSET_FACTOR = 0.01f;

				Transform currentHitTransform = Transform::IDENTITY();
				currentHitTransform = hitPos + hitNormal * OFFSET_FACTOR;

				if ( owner->GetUseSweepCollision() )
				{
					m_collisionResolver.PerformSyncSweep( currentHitTransform.GetPosition3(), m_desiredTransform.GetPosition3(), owner->GetSweepCollisionRadius(), hitResults );
				}
				else
				{
					m_collisionResolver.PerformSyncRaycast( m_desiredTransform, currentHitTransform, hitResults );
				}

				this->OnTickPostQuery( owner, afterHitTime, hitResults, ++recurrentCalls );
			}
			else
			{
				//it's stuck or it converges
				m_desiredTransform.SetPosition( m_trajectoryStartPoint );
				m_hasEnergy = false;
			}
		}
		void Controller::LockOrientation( Bool lock )
		{
			m_lockOrientation = lock;

			// If orientation lock is requested, it will be handled in ResolveTrajectoriesUpdate
			m_hasPendingLockOrientationRequest = m_lockOrientation;
		}
		
		void Controller::SetEnergyLossFactor( Float energyLossFactor, Float puppetEnergyLossFactor )
		{
			m_energyLossFactor = energyLossFactor;
			m_puppetEnergyLossFactor = puppetEnergyLossFactor;
		}
		
		Vector3 Controller::GetPrintVelocity() const
		{
			return m_trajectoryPrint ? m_trajectoryPrint->m_velocity : Vector3();
		}

		void Controller::SetWasTrajectoryStopped( Bool wasStopped )
		{
			
			if( m_trajectoryPrint )
			{
				m_trajectoryPrint->m_wasStopped = wasStopped;
			}
		}

		Vector3 Controller::GetDerivedVelocity( const VelocityParams& params, const Vector3& velocity, const Transform& transform ) const
		{
			Vector3 localX = transform.GetRight();
			Vector3 localY = transform.GetForward();
			Vector3 localZ = transform.GetUp();

			Vector3 outVelocityX = ProjectVectorWithFactor( velocity, localX, params.m_xFactor );
			Vector3 outVelocityY = ProjectVectorWithFactor( velocity, localY, params.m_yFactor );
			Vector3 outVelocityZ = ProjectVectorWithFactor( velocity, localZ, params.m_zFactor );

			return outVelocityX + outVelocityY + outVelocityZ;
		}

		void Controller::ClearPreview()
		{
			m_previewDisplay.Clear();
		}

		Vector3 Controller::ProjectVectorWithFactor( const Vector3& projectedVector, const Vector3& projectionDestination, Float factor ) const
		{
			return  projectionDestination.Normalized() * ( projectedVector.Dot( projectionDestination ) * factor / pow( projectionDestination.Mag(), 2) );
		}

		void Controller::ApplyDesiredTransform( Float deltaTime, Component* owner, const Transform& transform )
		{
			if ( m_wasLaunched )
			{
				// @NOTE This is here only if you need to debug projectile collisions! ATM there's an issue where
				// the animation sets grenade rendering plane to be weapon, but then it is never set back. This is 
				// obviously not the way to fix that issue! Witek will look at it afterwards
// 				auto components = owner->GetEntity()->GetComponents();
// 
// 				for ( const auto& component : components )
// 				{
// 					if ( auto visualComponent = Cast<ent::IVisualComponent>( component ) )
// 					{
// 						visualComponent->SetPlaneSetting( ERenderingPlane::RPl_Scene );
// 					}
// 				}

				if ( m_lerpingPosition || m_lerpingOrientation )
				{
					Vector3 position;
					Quaternion orientation;

					if ( m_lerpingPosition )
					{
						Vector3 logicPosition = transform.GetPosition3();

						// move the same delta from this frame
						Vector3 moveDelta = logicPosition - m_currentTransform.GetPosition3();
						position = m_visualTransform.GetPosition3() + moveDelta;

						// lerp the remainder
						Float positionT = math::Clamp( deltaTime * m_lerpMultiplier, 0.0f, 1.0f );
						position = Vector3::Lerp( position, logicPosition, positionT );
						
						// check if close enough
						if ( ( position - logicPosition ).IsAlmostZero(c_minDistanceStopPostionLerp) )
						{
							position = logicPosition;
							m_lerpingPosition = false;
						}
					}
					else
					{
						position = transform.GetPosition3();
					}

					if ( m_lerpingOrientation )
					{
						Quaternion logicOrientation = transform.GetOrientation();

						// rotate the same since from this frame
						Quaternion rotationDelta = logicOrientation *m_currentTransform.GetOrientation().Inverse();
						orientation = m_visualTransform.GetOrientation() * rotationDelta;

						// lerp the remainder
						Float orientationT = math::Clamp( deltaTime * m_lerpMultiplier, 0.0f, 1.0f );
						orientation = Quaternion::Lerp( orientation, logicOrientation, orientationT );

						// check if close enough to catch up
						rotationDelta = logicOrientation * orientation.Inverse();
						if( rotationDelta.GetAngle() < c_minAngleInRadiansStopOrientationLerp )
						{
							orientation = logicOrientation;
							m_lerpingOrientation = false;
						}
					}
					else
					{
						orientation = transform.GetOrientation();
					}

					m_visualTransform.Set( position, orientation );
				}
				else
				{
					m_visualTransform = transform;
				}
			}

			m_currentTransform = transform;

			owner->SetTransform( m_visualTransform );

#ifdef PROJECTILES_DEBUG
			if( m_trajectoryDBG )
			{
				m_trajectoryDBG->AddTrajectoryStep( m_currentTransform.GetPosition3(), m_velocity );
			}
#endif
		}

		void Controller::SetInitialTransforms( const Component* owner )
		{			
			Transform logicalTransform = m_currentTransform;
			
			if( m_launchParams.m_logicalPositionProvider )
			{
				logicalTransform.SetPosition( m_launchParams.m_logicalPositionProvider->CalculatePosition() );
			}
			
			if( m_launchParams.m_logicalOrientationProvider )
			{
				logicalTransform.SetOrientation( m_launchParams.m_logicalOrientationProvider->operator()() );
			}

			m_projectileUpVector = logicalTransform.GetUp();
			m_lerpingPosition = false;
			m_lerpingOrientation = false;

			switch( m_launchParams.m_launchMode )
			{
			case ELaunchMode::Default:
				m_desiredTransform = logicalTransform;
				m_visualTransform = logicalTransform;

				if( m_launchParams.m_visualPositionProvider )
				{
					m_visualTransform.SetPosition( m_launchParams.m_visualPositionProvider->CalculatePosition() );
					m_lerpingPosition = !( m_visualTransform.GetPosition3() - m_desiredTransform.GetPosition3() ).IsAlmostZero();
				}

				if( m_launchParams.m_visualOrientationProvider )
				{
					m_visualTransform.SetOrientation( m_launchParams.m_visualOrientationProvider->operator()() );
					m_lerpingOrientation = !( m_visualTransform.GetOrientation().AsVector() - m_desiredTransform.GetOrientation().AsVector() ).IsAlmostZero();
				}
				break;
			case ELaunchMode::FromLogic:
				m_desiredTransform = logicalTransform;
				m_visualTransform = logicalTransform;
				break;
			case ELaunchMode::FromVisuals:
				m_visualTransform.Set( owner->GetWorldPosition().AsVector3(), owner->GetWorldOrientation() );
				m_desiredTransform = m_visualTransform;
				m_projectileUpVector = owner->GetWorldUp();
				break;
			default:
				RED_ASSERT( false, "[ProjectileController] Invalid launch mode" );
				break;
			}

			const Vector3 currentPosition = m_desiredTransform.GetPosition3();
			m_currentTransform = m_desiredTransform;

			//cache current launch position
			// m_trajectoryStartPoint will be updated during movement
			m_trajectoryStartPoint = currentPosition;
			m_launchPosition = currentPosition;
		}

		Vector3 Controller::GetInitialOwnerVelocity( const Component* owner ) const
		{
			Vector3 ownerVelocity = Vector3::ZEROS();

			const THandle< ent::IVelocityProvider >& ownerVelocityProvider = m_launchParams.m_ownerVelocityProvider;
			if( ownerVelocityProvider )
			{
				ownerVelocity = ownerVelocityProvider->CalculateVelocity();
				ownerVelocity = ownerVelocity.IsOk() ? ownerVelocity : Vector3::ZEROS();

				if( owner->IsDerivingVelocity() )
				{
					if( const THandle< game::Object >& characterHandle = owner->GetOwner().ToHandle() )
					{
						ownerVelocity = GetDerivedVelocity( owner->GetDeriveVelocityParams(), ownerVelocity, characterHandle->GetWorldTransform()._ToXForm() );
					}
				}
			}

			return ownerVelocity;
		}
	}//projectile
}



#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

