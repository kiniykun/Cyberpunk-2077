/*
* Copyright (c) 2016-2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "triggerCondition.h"
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include <mutex>
#include <string>
#include <vector>
#include "triggerCondition_FulfillInfo.h"
#include "questNodeFailed.h"

#include "../../triggers/include/triggerActivator.h"
#include "../../worldEntities/include/triggerActivatorComponent.h"
#include "../../world/include/gameInstance.h"
#include "../../world/include/worldTriggerAreaNodeInstance.h"
#include "../gameCore/include/gameRttiExtensions.h"
#include "../gameCore/include/gameEngineExtensions.h"
#include "../gameCore/include/entityStub.h"
#include "../gameCore/include/entityStubSystemInterface.h"

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif


RTTI_BEGIN_TYPE_IN_NAMESPACE( TriggerCondition, quest );
	RTTI_PARENT_TYPE( Condition );
	RTTI_DESCRIPTIVE_NAME( TriggerCondition );
	RTTI_PROPERTY( m_type ).instanceEditable();
	RTTI_PROPERTY( m_triggerAreaRef ).instanceEditable();
	RTTI_PROPERTY( m_activatorRef ).instanceEditable().customEditor( "scnbPerformerPuppetRefSelector" );
	RTTI_PROPERTY( m_isPlayerActivator ).instanceEditable();
RTTI_END_TYPE();

namespace quest
{
	TriggerCondition::TriggerCondition()
		: m_type( TriggerConditionType::IsInside )
		, m_isPlayerActivator( true )
	{}

	String TriggerCondition::GetFriendlyAcronym() const
	{
		return "Trigger";
	}

	String TriggerCondition::GetDescription() const
	{
		auto type = game::rttiExtensions::EnumToString( RED_NAME_CONSTEXPR_NOREG( "questTriggerConditionType" ), static_cast< Int64 >( m_type ) );
		return type.AsChar();
	}

	red::SharedPtr< EventListenerWrapper > TriggerCondition::CreateEventListener( NodeExecutionContext& executionContext ) const
	{
		return red::CreateSharedPtr< TriggerListenerWrapper >( executionContext, m_type, m_triggerAreaRef, m_activatorRef, m_isPlayerActivator );
	}

	// CP2077 web [nomadplay]: tracing is on for ?start=nomadquest (or &questlog=1|all), off elsewhere (&questlog=0 forces off)
	static bool NpLogOn()
	{
		static int s_on = -1;
		if ( s_on < 0 )
		{
			s_on = 0;
#ifdef __EMSCRIPTEN__
			s_on = MAIN_THREAD_EM_ASM_INT( { try { const q = new URLSearchParams( location.search ); const v = q.get( 'questlog' ); if ( v === '0' ) return 0; return ( v !== null || q.get( 'start' ) === 'nomadquest' ) ? 1 : 0; } catch ( e ) { return 0; } } );
#endif
		}
		return s_on > 0;
	}

	Bool TriggerCondition::OnIsFulfilledImmediately( const NodeExecutionContext& executionContext ) const
	{
		TriggerObjectPtr triggerObjectPtr;

		// check immediate conditions
		if ( m_type >= TriggerConditionType::IsInside )
		{
			THandle< game::Object > activator = nullptr;

			world::ResolvedNodeRef areaNodeRef = m_triggerAreaRef.Resolve( world::GlobalNodeID::GetRoot() );
			const world::NodeInstancePtr areaNodeInstance = areaNodeRef.GetNodeInstance( executionContext.GetNodeInstanceRegistry() );

			auto getResultByTriggerType =	[ this ]( Bool isInteracting )
											{
												if ( m_type == TriggerConditionType::IsInside || m_type == TriggerConditionType::AllInsideMP )
												{
													return isInteracting;
												}
												else if ( m_type == TriggerConditionType::IsOutside || m_type == TriggerConditionType::AllOutsideMP )
												{
													return !isInteracting;
												}

												return false;
											};


			// [nomadplay] log each trigger ref once (and again when its streamed state changes): instance present, type, position
			if ( NpLogOn() )
			{
				static std::mutex s_npTrigMutex;
				static std::vector< std::pair< std::string, Int32 > > s_npTrigSeen;
				const std::string ref = areaNodeRef.ToDebugString().AsChar();
				const THandle< world::TriggerAreaNodeInstance > tni = areaNodeInstance ? Cast< world::TriggerAreaNodeInstance >( areaNodeInstance ) : THandle< world::TriggerAreaNodeInstance >();
				const Int32 state = !areaNodeInstance ? 0 : ( !tni ? 1 : ( !tni->GetTriggerObject() ? 2 : 3 ) );
				std::lock_guard< std::mutex > lock( s_npTrigMutex );
				Bool found = false;
				for ( auto& e : s_npTrigSeen )
				{
					if ( e.first == ref )
					{
						found = true;
						if ( e.second != state )
						{
							e.second = state;
							found = false;
						}
						break;
					}
				}
				if ( !found )
				{
					if ( s_npTrigSeen.size() < 4096 )
					{
						s_npTrigSeen.emplace_back( ref, state );
					}
					if ( state == 3 )
					{
						const Vector4 tp = tni->GetTriggerObject()->GetPosition().ToVector();
						std::fprintf( stderr, "[nomadplay] trigger %s type=%d instance=streamed at (%.1f, %.1f, %.1f)\n", ref.c_str(), Int32( m_type ), tp.X, tp.Y, tp.Z );
					}
					else
					{
						std::fprintf( stderr, "[nomadplay] trigger %s type=%d state=%s\n", ref.c_str(), Int32( m_type ), state == 0 ? "NO NODE INSTANCE (not streamed / missing)" : ( state == 1 ? "not a TriggerAreaNodeInstance" : "no TriggerObject" ) );
					}
				}
			}

			if ( !areaNodeInstance )
			{
				// no error, an instance can be streamed out
				return getResultByTriggerType( false );
			}

			const THandle< world::TriggerAreaNodeInstance > triggerNodeInstance = Cast< world::TriggerAreaNodeInstance >( areaNodeInstance );
			if ( !triggerNodeInstance )
			{
				QUEST_NODE_FAILED( executionContext, "Trigger Condition failed, node instance '%hs' is not TriggerAreaNodeInstance", areaNodeRef.ToDebugString().AsChar() );
				return false;
			}

			const TriggerObjectPtr& triggerObject = triggerNodeInstance->GetTriggerObject();
			if ( !triggerObject )
			{
				QUEST_NODE_FAILED( executionContext, "Trigger Condition failed, cannot get TriggerObject from TriggerAreaNodeInstance '%hs'", areaNodeRef.ToDebugString().AsChar() );
				GAME_DATA_ERROR( false, Quests, "Trigger %hs seems not to have any listener instance, it will not work", areaNodeRef.ToDebugString().AsChar() );
				return false;
			}

			if ( m_isPlayerActivator )
			{
				// get trigger area position
				const Vector3 areaPosition = triggerObject->GetPosition().ToVector();

				// get the closest player
				activator = RuntimeHelper::FindPlayerObject( executionContext, false, areaPosition );
			}
			else
			{
				// Specific client player does not count as "m_isPlayerActivator"
				const Bool isMultiplayer = executionContext.GetGameInstance().GetRuntimeInfo().IsMultiplayer();
				if ( isMultiplayer )
				{
					activator = RuntimeHelper::GetMultiplayerObject( executionContext, m_activatorRef );
				}
				if ( !activator )
				{
					activator = RuntimeHelper::GetPuppetOrSimpleObject( executionContext, m_activatorRef );
				}
			}

			if ( activator )
			{
				auto FindInteractableTriggerActivatorComponent = [ &, triggerObject, activator ]( const red::DynArray< THandle< ent::TriggerActivatorComponent > >& components )
				{
					if ( triggerObject )
					{
						// go through all activator components and use the first one our trigger can interact with
						// (TriggerObject::CanInteractWith method internally checks trigger channels compatibility)

						for ( auto&& component : components )
						{
							if ( auto triggerActivator = component->GetActivator() )
							{
								if ( triggerObject->CanInteractWith( *triggerActivator ) )
								{
									return component;
								}
							}
						}
					}

					// fall back to the old method if we don't know which trigger channels we should look for
					return game::engineExt::FindComponentByType< ent::TriggerActivatorComponent >( activator.Get() );
				};

				red::DynArray< THandle< ent::TriggerActivatorComponent > > components{ red::PoolFrame() };
				game::engineExt::FindComponentsByType< ent::TriggerActivatorComponent >( activator.Get(), components );

				if ( components.Empty() )
				{
					QUEST_NODE_FAILED( executionContext, "Trigger Condition failed, activator does not have TriggerActivatorComponent" );
					GAME_DATA_ERROR( false, Quests, "Activator doesn't have TriggerActivatorComponent" );
					return false;
				}

				auto triggerActivatorComponent = FindInteractableTriggerActivatorComponent( components );
				if ( !triggerActivatorComponent )
				{
					QUEST_NODE_FAILED( executionContext, "Trigger Condition failed, activator is no longer available (its owner has possibly detached)" );
					return false;
				}

				return getResultByTriggerType( triggerActivatorComponent->IsInteracting( m_triggerAreaRef ) );
			}
			else // stub support
			{
				ent::EntityID entityId = RuntimeHelper::GetCharacterOrSimpleObjectEntityID( executionContext, m_activatorRef, m_isPlayerActivator );
				if ( entityId.IsDefined() )
				{
					if ( const game::EntityStub* stub = executionContext.GetEntityStubSystem()->FindStub( entityId ) )
					{
						return getResultByTriggerType( triggerObject->TestPoint( stub->GetEntityWorldPositionAsVector3() ) );
					}
				}
			}
		}

		// otherwise return false to create listener
		return false;
	}

	void TriggerCondition::OnValidate( NodeValidationContext& validationContext, const NodeDefinition& node ) const
	{
		QUEST_VALIDATION_CHILD_MISSING_OR_INVALID_PROPERTY( m_isPlayerActivator || m_activatorRef.IsDefined(), GET_PROPERTY_NAME( m_activatorRef ) );
		QUEST_VALIDATION_CHILD_MISSING_OR_INVALID_PROPERTY( m_triggerAreaRef.IsDefined(), GET_PROPERTY_NAME( m_triggerAreaRef ) );
		QUEST_VALIDATION_CHILD_MISSING_OR_INVALID_PROPERTY( m_type >= TriggerConditionType::Entered && m_type <= TriggerConditionType::IsOutside, GET_PROPERTY_NAME( m_type ) );
	}

	Bool TriggerCondition::IsImmediate() const
	{
		switch ( m_type )
		{
		case TriggerConditionType::Entered:
		case TriggerConditionType::Exited:
			return false;
		case TriggerConditionType::IsInside:
		case TriggerConditionType::IsOutside:
		case TriggerConditionType::AllInsideMP:
		case TriggerConditionType::AllOutsideMP:
			return true;
		}

		return true;
	}

	Bool TriggerCondition::IsFinal() const
	{
		return false;
	}

	void TriggerCondition::GetCustomEditableProperties( rtti::EditableProperties& outRootProperties ) const
	{
		Condition::GetCustomEditableProperties( outRootProperties );

		if( m_isPlayerActivator )
		{
			// m_activatorRef is not needed if m_isPlayerActivator is true
			outRootProperties.RemoveIf( []( const rtti::ClassEditablePropertyInfo& info )
			{
				return info.m_name == RED_NAME_CONSTEXPR_NOREG( "activatorRef" );
			} );
		}
	}

	bool TriggerCondition::ftGetFulfillInfo( FulfillInfoArray& fulfillInfo ) const
	{
		red::SharedPtr<TriggerCondition_FulfillInfo> result =
			red::CreateSharedPtr<TriggerCondition_FulfillInfo>();

		result->AddInfo("class", "trigger");

		result->AddInfo(
			"type",
			game::rttiExtensions::EnumToString(
				RED_NAME_CONSTEXPR("questTriggerConditionType"),
				static_cast<Int64>(m_type))
				.AsChar());

		result->AddInfo(
			"activator",
			(m_isPlayerActivator ? "player" : "not_player"));

		// get trigger area position
		world::ResolvedNodeRef areaNodeRef =
			m_triggerAreaRef.Resolve(world::GlobalNodeID::GetRoot());

		result->AddInfo(
			"areaNodeRef",
			areaNodeRef.ToDebugString());

		result->NodeRefToResolve(areaNodeRef.GetRef());

		fulfillInfo.AddFulfillInfo(std::move(result));

		return true;
	}
}

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif
