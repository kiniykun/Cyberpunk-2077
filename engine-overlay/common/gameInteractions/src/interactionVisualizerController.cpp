// Copyright (c) 2020 CD Projekt Red. All Rights Reserved.

#include "build.h"
#include "interactionVisualizerController.h"

#include "interactionActivator.h"
#include "interactionGroupedVisualizerInterface.h"
#include "interactionGuide.h"
#include "interactionHotSpot.h"
#include "interactionUtilVisitors.h"
#include "interactionVisualizerCache.h"
#include "interactionVisualizerFamily.h"
#include "interactionVisualizerGrouper.h"

#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/gameObject.h"
#include "../../gameCore/include/inputHandle.h"
#include "../../gameCore/include/prereqInterface.h"
#include "../../worldBlackboardSystem/include/blackboardSystemInterface.h"

#if defined( __EMSCRIPTEN__ )
#include <cstdio>
#include <cstring>
extern "C" void cp2077_touch_set_prompts( int total, int dialog );   // [touch] gameInput/src/inputManager.cpp
#endif


namespace game { namespace interactions { namespace vis {

	////////////////////////////////////////////////////////////////////////////////////
	//
	VisualizerInstanceState::VisualizerInstanceState( const CActivator& activator, const IVisualizerDefinition& definition, const CHotSpot& hotSpot, const CHotSpotLayerDefinition& layerDef, Cache::Entry&& entry )
		: m_definitionClassName{ definition.GetFamilyName() }
		, m_hotSpot{ SharedFromPtr( &hotSpot ) }
		, m_layerDef{ HandleFromPtr( &layerDef ) }
		, m_entry{ std::move( entry ) }
		, m_logic{ &m_entry.Get() }
		, m_visualizer{ &m_logic->GetVisualizer() }
	{
		m_logic->SetupInstance( LogicInitializationContext{ hotSpot, activator, HotSpotNodeID{ hotSpot.GetHotSpotID(), GetLinkedLayersDefinition().GetNodeID() }, HandleFromPtr( &definition ) } );
	}

	VisualizerInstanceState::~VisualizerInstanceState()
	{
		if( m_logic )
		{
			m_logic->ClearInstance();
		}
	}

	VisualizerInstanceState::VisualizerInstanceState( VisualizerInstanceState&& other )
		: m_definitionClassName{ other.m_definitionClassName }
		, m_hotSpot{ std::move( other.m_hotSpot ) }
		, m_layerDef{ std::move(other.m_layerDef)}
		, m_entry{ std::move( other.m_entry ) }
		, m_logic{ other.m_logic }
		, m_visualizer{ other.m_visualizer }
		
	{
		other.m_logic = nullptr;
		other.m_visualizer = nullptr;
	}

	VisualizerInstanceState& VisualizerInstanceState::operator=( VisualizerInstanceState&& other )
	{
		if( this != &other )
		{
			m_definitionClassName = other.m_definitionClassName;
			m_hotSpot = std::move( other.m_hotSpot );
			m_layerDef = std::move( other.m_layerDef );
			m_entry = std::move( other.m_entry );
			m_logic = other.m_logic;
			m_visualizer = other.m_visualizer;

			other.m_logic = nullptr;
			other.m_visualizer = nullptr;
		}

		return *this;
	}

	const CLinkedLayersDefinition& VisualizerInstanceState::GetLinkedLayersDefinition() const
	{
		return m_layerDef->GetParentDefinition();
	}


	////////////////////////////////////////////////////////////////////////////////////
	//
	Controller::Controller( const IGameInstance& gameInstance, CActivator& activator, Cache& cache, OrbSystem& orbSystem )
		: m_gameInstance{ gameInstance }
		, m_activator{ activator }
		, m_visCache{ cache }
		, m_visGrouper{ red::CreateUniquePtr< Grouper >( gameInstance, *this, activator, orbSystem ) }
	{
		RegisterInputListening();

		m_visualizerFamilies.Reserve( 8 );
		m_visualizers.Reserve( 8 );

		m_interactionsBB = m_gameInstance.GetSystem< game::IBlackboardSystem >().Get( BLACKBOARD_INDEX( UIInteractions ) );

		InitVisualizerFamilies();
	}

	Controller::~Controller()
	{
		red::DynArray< HotSpotNodeID > keys = m_visualizers.GetKeys();
		ProcessRemovedVisualizers( keys );

		m_visGrouper.Reset();
		m_visualizerFamilies.Clear();

		UnregisterInputListening();
	}

	void Controller::UpdateVisualizerPrereqs()
	{
		if( m_globalActivationPrereq )
		{
			m_globalActivationPrereqResult = m_globalActivationPrereq->IsFulfilled( &m_gameInstance, m_activator.GetObject().ToHandle() );
		}
		else
		{
			m_globalActivationPrereqResult = true;
		}

		IterateVisualizerFamilies( [ this ]( FamilyBase& family ) {
			family.UpdatePrereq( m_gameInstance, m_activator );
		} );
	}

	void Controller::Tick( Float dt )
	{
		PC_SCOPE_FUNC();

		IterateVisualizerFamilies( [ dt ]( FamilyBase& family )
		{
			family.UpdateFamily( dt );
		} );

		DirtyOutput output = ProcessDirtyVisualizers();

		if( !output.m_removableVisualizers.Empty() )
		{
			ProcessRemovedVisualizers( output.m_removableVisualizers );
		}

		if( output.m_tryHighlightTopVisualizer )
		{
			m_visGrouper->HighlightTopVisualizer();
		}

		if( output.m_refreshPresenters )
		{
			ProcessPresenters();
		}

		m_visGrouper->Update( dt );
	}

	void Controller::OnActivationChanged( Bool isActivated, const CHotSpot& hotSpot, const CHotSpotLayerDefinition& layerDef )
	{
		if( VisualizerInstanceState* instance = GetOrCreateInstance( hotSpot, layerDef ) )
		{
			instance->m_logic->SetLayerActive( layerDef, isActivated );
		}
	}

	void Controller::OnModifiedHotSpot( const CHotSpot& hotSpot, red::ArraySpan< const LayerLocalID > activeLayers )
	{
		// need to check if a visualizer that belongs to the modified hotspot is created.
		// if so, then let's recreate it
		// TODO_MW: NEXT_ITR => only recreate when needed
		for( auto itVis : m_visualizers )
		{
			const HotSpotNodeID& linkedLayersId = itVis.Key();
			if( linkedLayersId.GetHotSpotID() == hotSpot.GetHotSpotID() )
			{
				VisualizerInstanceState& instance = itVis.Value();
				THotSpotLayerDefinitionPtr layerDefPtr = instance.m_layerDef;
				const CLinkedLayersDefinition& linkedLayersDef = instance.GetLinkedLayersDefinition();
				THandle< const IVisualizerDefinition > hotSpotVisualizerDefinition = hotSpot.GetVisualizerDefinition( linkedLayersDef );
				if( !hotSpotVisualizerDefinition )
				{
					// linked layers has no visualizer assigned to it after the modification. Remove it
					RemoveInstance( linkedLayersId );
				}
				else
				{
					THandle< const IVisualizerDefinition > currentVisualizerDefinition = HandleFromPtr< const IVisualizerDefinition >( &instance.m_visualizer->GetDefinition() );
					if( hotSpotVisualizerDefinition->IsExactlyA( currentVisualizerDefinition->GetClass() ) )
					{
						// swap visualizers of the same type
						if( currentVisualizerDefinition != hotSpotVisualizerDefinition )
						{
							// definitions differ, modify
							LogicModificationContext ctx{ hotSpot, m_activator, currentVisualizerDefinition, hotSpotVisualizerDefinition };
							instance.m_logic->ModifyInstance( ctx );
						}
					}
					else
					{
						// swap visualizers of two different types
						// this involves removing the previous visualizer
						// and then creating a visualizer of the new type, activating its layers and updating its choice list

						// get choice list from the previous visualizer
						red::DynArray< Choice > currentChoiceList{ red::PoolFrame{} };
						instance.m_logic->GetChoiceList( currentChoiceList );

						// remove the previous visualizer
						RemoveInstance( linkedLayersId );

						// find all the active layers for the new visualizer
						red::DynArray< const CHotSpotLayerDefinition* > activeLayerDefs{ red::PoolFrame{} };
						activeLayerDefs.Reserve( activeLayers.Size() );
						for( const LayerLocalID& layerLocalId : activeLayers )
						{
							if( layerLocalId != layerDefPtr->GetLayerLocalID() )
							{
								if( const CHotSpotLayerDefinition* layerDef = linkedLayersDef.FindLayerDefinition( layerLocalId ) )
								{
									activeLayerDefs.PushBack( layerDef );
								}
							}
						}

						// create the new visualizer, set initial active layer
						VisualizerInstanceState& newVisualizerInstance = CreateInstance( *hotSpotVisualizerDefinition, hotSpot, linkedLayersDef, *layerDefPtr );
						newVisualizerInstance.m_logic->SetLayerActive( *layerDefPtr, true );

						// set other active layers on the new visualizer
						for( const CHotSpotLayerDefinition* anotherDef : activeLayerDefs )
						{
							newVisualizerInstance.m_logic->SetLayerActive( *anotherDef, true );
						}

						// update choice list on the new visualizer
						newVisualizerInstance.m_logic->UpdateChoiceList( currentChoiceList );
					}
				}
			}
		}

		// The visualizers could need to be resorted after modifications. 
		// Store a dirty flag on controller in this case rather than having to mark all the visualizers as dirty
		m_visualizersResorted = m_visGrouper->SortVisualizersIfRequired();

	}

	TLinkedLayersDefinitionPtr Controller::GetLinkedLayersDefinition( const IVisualizerLogicInterface& visLogic ) const
	{
		if( const VisualizerInstanceState* instance = GetInstance( visLogic ) )
		{
			return HandleFromPtr( &instance->GetLinkedLayersDefinition() );
		}

		return nullptr;
	}

	THotSpotLayerDefinitionPtr Controller::GetInputLayerDefinition( const IVisualizerLogicInterface& visLogic ) const
	{
		if( const VisualizerInstanceState* instance = GetInstance( visLogic ) )
		{
			return instance->m_layerDef;
		}

		return nullptr;
	}

	const CHotSpot* Controller::GetHotSpot( const IVisualizerLogicInterface & visLogic ) const
	{
		if( const VisualizerInstanceState* instance = GetInstance( visLogic ) )
		{
			if( THotSpotSharedPtr hotSpot = instance->m_hotSpot.Lock() )
			{
				return hotSpot.Get();
			}
		}

		return nullptr;
	}

	Bool Controller::IsVisualizerPrereqTestSuccess( CName visFamilyName ) const
	{
		if( m_globalActivationPrereqResult )
		{
			return m_visualizerFamilies[ visFamilyName ]->GetPrereqTestResult();
		}
		else
		{
			return false;
		}
	}

	void Controller::SetChoices( const HotSpotNodeID& hotSpotLinkedLayersId, const red::DynArray< Choice >& choices )
	{
		if( VisualizerInstanceState* instance = m_visualizers.FindPtr( hotSpotLinkedLayersId ) )
		{
			instance->m_logic->UpdateChoiceList( choices );
		}
	}

	void Controller::ResetChoices( const HotSpotNodeID& hotSpotLinkedLayersId )
	{
		if( VisualizerInstanceState* instance = m_visualizers.FindPtr( hotSpotLinkedLayersId ) )
		{
			instance->m_logic->UpdateChoiceList( { PoolGMPL_Interactions{} } );
		}
	}

	Bool Controller::GetChoice( const HotSpotNodeID& hotSpotLinkedLayersId, Choice& outChoice ) const
	{
		if( const VisualizerInstanceState* instance = m_visualizers.FindPtr( hotSpotLinkedLayersId ) )
		{
			if( instance->m_visualizer->IsFocusGranted() )
			{
				return instance->m_visualizer->GetChoice( outChoice );
			}
		}

		return false;
	}

	void Controller::SetGlobalActivationPrereq( const THandle< IPrereq >& prereq )
	{
		m_globalActivationPrereq = prereq;
	}

	void Controller::Dbg_Refresh()
	{
		if( m_visGrouper )
		{
			m_visGrouper->Dbg_Refresh();
		}
	}

	void Controller::OnAction( const input::ListenerAction& action, input::ListenerActionConsumer& consumer )
	{
		if( const VisualizerInstanceState* activeInstance = FindActiveInstance() )
		{
			if( activeInstance->m_logic->IsA< IGroupedVisualizerLogic >() )
			{
				HandleActionResult( *activeInstance, m_visGrouper->HandleInput( action ), action, consumer );
			}
			else
			{
				HandleActionResult( *activeInstance, activeInstance->m_logic->ProcessAction( action ), action, consumer );
			}
		}
		else
		{
			ActionResult result = m_visGrouper->HandleInput( action );
			if( result.IsValid() )
			{
				consumer.Consume();
			}
		}
	}

	void Controller::InitVisualizerFamilies()
	{
		static const rtti::ClassType* c_familyBaseType = GetRttiSystem().FindClass( ::GetTypeName< FamilyBase >() );

		red::DynArray< const rtti::ClassType* > derivedClasses{ red::PoolFrame{} };
		GetRttiSystem().EnumDerivedClasses( c_familyBaseType, derivedClasses );

		for( const rtti::ClassType* cls : derivedClasses )
		{
			if( !cls->IsAbstract() )
			{
				auto familyHandle = red::UniquePtr< FamilyBase >( cls->CreateObject< FamilyBase >() );
				familyHandle->Init( m_gameInstance, m_activator );
				m_visualizerFamilies.Emplace(
					familyHandle->GetFamilyName(),
					std::move( familyHandle )
				);
			}
		}
	}

	const VisualizerInstanceState* Controller::GetInstance( const IVisualizerLogicInterface& logicInterface ) const
	{
		if( const VisualizerInstanceState* instance = m_visualizers.FindPtr( logicInterface.GetVisualizer().GetLinkedLayersID() ) )
		{
			return instance;
		}

		return nullptr;
	}

	const VisualizerInstanceState* Controller::GetInstance( const CHotSpot& hotSpot, const CHotSpotLayerDefinition& layerDef ) const
	{
		const CLinkedLayersDefinition& linkedLayersDef = layerDef.GetParentDefinition();
		const HotSpotNodeID linkedLayersId{ hotSpot.GetHotSpotID(), linkedLayersDef.GetNodeID() };
		if( const VisualizerInstanceState* instance = m_visualizers.FindPtr( linkedLayersId ) )
		{
			return instance;
		}
		else
		{
			return nullptr;
		}
	}

	VisualizerInstanceState* Controller::GetOrCreateInstance( const CHotSpot& hotSpot, const CHotSpotLayerDefinition& layerDef )
	{
		const CLinkedLayersDefinition& linkedLayersDef = layerDef.GetParentDefinition();
		const THandle< IVisualizerDefinition >& visDef = hotSpot.GetVisualizerDefinition( linkedLayersDef );
		if( visDef )
		{
			return &CreateInstance( *visDef, hotSpot, linkedLayersDef, layerDef );
		}
		else
		{
			return nullptr;
		}
	}

	VisualizerInstanceState& Controller::CreateInstance( const IVisualizerDefinition& visDef, const CHotSpot& hotSpot, const CLinkedLayersDefinition& linkedLayersDef, const CHotSpotLayerDefinition& layerDef )
	{
		const HotSpotNodeID linkedLayersId{ hotSpot.GetHotSpotID(), linkedLayersDef.GetNodeID() };
		if( VisualizerInstanceState* foundInstance = m_visualizers.FindPtr( linkedLayersId ) )
		{
			// ensure latest input layer
			if( layerDef.GetGroup() == EGroupType::EGT_default )
			{
				foundInstance->m_layerDef = HandleFromPtr( &layerDef );
			}

			return *foundInstance;
		}
		else
		{
			Cache::Entry&& cachedVisualizer{ m_visCache.CreateVisualizer( visDef ) };
			auto it = m_visualizers.Emplace( linkedLayersId, m_activator, visDef, hotSpot, layerDef, std::move( cachedVisualizer ) );
			RED_ASSERT( it.IsSuccessful() );

			VisualizerInstanceState& instance = it.Iterator().Value();
			FamilyBase* family = m_visualizerFamilies.FindPtr( visDef.GetFamilyName() )->Get();
			family->AddVisualizer( linkedLayersId, instance.m_entry.GetHandle() );

			if( auto groupedVisualizer = Cast< IGroupedVisualizerLogic >( instance.m_entry.GetHandle() ) )
			{
				m_visGrouper->AddVisualizer( groupedVisualizer );
			}

			return instance;
		}
	}

	void Controller::RemoveInstance( const HotSpotNodeID& linkedLayersId )
	{
		auto it = m_visualizers.Find( linkedLayersId );
		RED_ASSERT( it != m_visualizers.End(), "Trying to remove non-existing visualizer for %hs", linkedLayersId.ToString().AsChar() );

		VisualizerInstanceState& instance = it.Value();

		FamilyBase* family = m_visualizerFamilies.FindPtr( instance.m_definitionClassName )->Get();
		family->RemoveVisualizer( linkedLayersId );

		if( auto groupedVisualizer = Cast< IGroupedVisualizerLogic >( instance.m_entry.GetHandle() ) )
		{
			m_visGrouper->RemoveVisualizer( groupedVisualizer );
		}

		m_visualizers.Remove( it );
	}

	VisualizerInstanceState* Controller::FindActiveInstance()
	{
		VisualizerInstanceState* activeInstance = nullptr;

		IterateVisualizers( [&]( const HotSpotNodeID& linkedLayersId, VisualizerInstanceState& instance ) {
			if( instance.m_visualizer->IsActive() )
			{
				if( !activeInstance || !activeInstance->m_visualizer->IsInputLayerActive() )
				{
					activeInstance = &instance;
				}			
			}
		} );

		return activeInstance;
	}

	Controller::DirtyOutput Controller::ProcessDirtyVisualizers()
	{
		PC_SCOPE_FUNC();

		
		DirtyOutput output;
		IterateVisualizers( [ this, &output ]( const HotSpotNodeID& linkedLayersId, VisualizerInstanceState& instance ) {
			const DirtyFlag dirtyFlag = instance.m_logic->TryConsumeDirty();
			if( dirtyFlag != DirtyFlag::None )
			{
				if( ( dirtyFlag & DirtyFlag::Visual ) == DirtyFlag::Visual || m_visualizersResorted )
				{
					output.m_refreshPresenters = true;
				}
				
				if( ( dirtyFlag & DirtyFlag::Focus ) == DirtyFlag::Focus || m_visualizersResorted )
				{
					if( auto groupedVisualizer = Cast< IGroupedVisualizerLogic >( instance.m_entry.GetHandle() ) )
					{
						m_visGrouper->NotifyVisualizerChange( groupedVisualizer );
						output.m_tryHighlightTopVisualizer = true;
					}
				}

				if( instance.m_logic->IsRemovable() )
				{
					output.m_removableVisualizers.PushBack( linkedLayersId );
				}
			}
		} );

		m_visualizersResorted = false;
		output.m_tryHighlightTopVisualizer |= !output.m_removableVisualizers.Empty();
		return output;
	}

	void Controller::ProcessRemovedVisualizers( red::ArraySpan< const HotSpotNodeID > removedVisualizers )
	{
		for( const HotSpotNodeID& linkedLayersId : removedVisualizers )
		{
			RemoveInstance( linkedLayersId );
		}
	}

	void Controller::ProcessPresenters()
	{
		red::DynArray< const IVisualizer* > presentationVisualizers{ red::PoolFrame{} };
		VisualizersInfo visualizersInfo;

#if defined( __EMSCRIPTEN__ )
		// [touch] Count the prompts each family presents (dialog family = choice hubs) for the page's touch overlay.
		int cpDialog = 0;
		IterateVisualizerFamilies( [ this, &presentationVisualizers, &visualizersInfo, &cpDialog ]( FamilyBase& family ) {
			const Uint32 before = visualizersInfo.m_visIds.Size();
			family.PresentFamily( visualizersInfo );
			const Uint32 added = visualizersInfo.m_visIds.Size() - before;
			const char* name = family.GetFamilyName().AsChar();
			if( added && name && ( std::strstr( name, "ialog" ) || std::strstr( name, "IALOG" ) ) ) cpDialog += static_cast< int >( added );
			static int s_familyLogs = 0;
			if( added && s_familyLogs < 24 ) { ++s_familyLogs; std::fprintf( stderr, "CP2077 input prompts family: %s added=%u\n", name ? name : "-", added ); }
		} );
		cp2077_touch_set_prompts( static_cast< int >( visualizersInfo.m_visIds.Size() ), cpDialog );
#else
		IterateVisualizerFamilies( [ this, &presentationVisualizers, &visualizersInfo ]( FamilyBase& family ) {
			family.PresentFamily( visualizersInfo );
		} );
#endif

		m_interactionsBB->Write( BLACKBOARD_ID( UIInteractions, VisualizersInfo ), visualizersInfo, true );
	}

	void Controller::RegisterInputListening()
	{
		if( auto object = m_activator.GetObject().ToHandle() )
		{
			if( auto inputHandle = input::LocalHandle{ *object } )
			{
				inputHandle->RegisterListener( this );
			}
		}
	}

	void Controller::UnregisterInputListening()
	{
		if( auto object = m_activator.GetObject().ToHandle() )
		{
			if( auto inputHandle = input::LocalHandle{ *object } )
			{
				inputHandle->UnregisterListener( this );
			}
		}
	}

	void Controller::HandleActionResult( const VisualizerInstanceState& instance, const ActionResult& result, const input::ListenerAction& action, input::ListenerActionConsumer& consumer )
	{
		if( result.IsValid() )
		{
			consumer.Consume();

			if( result.GetType() == ActionResult::Type::Choice )
			{
				const AttemptedChoice& attempt = result.GetAttemptedChoice();
				if( attempt.m_isSuccess )
				{
					if( const THotSpotSharedPtr hotSpot = instance.m_hotSpot.Lock() )
					{
						hotSpot->OnChoiceEvent( attempt.m_choice, action.GetType(), *instance.m_layerDef, m_activator );
					}
				}

				m_interactionsBB->Write( BLACKBOARD_ID( UIInteractions, LastAttemptedChoice ), attempt, true );
			}
			else if( result.GetType() == ActionResult::Type::Browse )
			{
				// nothing for now
			}
			else
			{
				RED_FATAL( "Unsupported ActionResult::Type" );
			}

			const ActionResult::Flags flags = result.GetFlags();
			if( enumUtils::HasFlag( flags, ActionResult::Flags::RequestLookAt ) )
			{
				if( auto groupedVisualizer = Cast< IGroupedVisualizerLogic >( instance.m_entry.GetHandle() ) )
				{
					m_visGrouper->RequestLookAt( groupedVisualizer );
				}
			}

			if( enumUtils::HasFlag( flags, ActionResult::Flags::PreventSendingReleaseInputEvent ) )
			{
				consumer.DontSendReleaseEvent();
			}
		}
	}

} } }