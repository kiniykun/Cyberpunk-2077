/**
* Copyright (C)2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "../inkCore/include/inkSystem.h"
#include "../inkCore/include/inputKeyIconManager.h"
#include "../inkCore/include/widgetControllerUtils.h"
#include "../inkCore/include/compoundWidget.h"
#include "../worldUserInterface/include/inputHintEvents.h"
#include "inputHintController.h"
#include "inputHintManagerController.h"
#include "inputHintGroupController.h"
#include "gameUISystem.h"
#include "uiSaveData.h"

#if defined( __EMSCRIPTEN__ )
// [touch] gameInput/src/inputManager.cpp: nonzero while the page's touch overlay is on. Keyboard hint lists
// ("Switch Camera [Q] / Exit [F] / Radio [R]", skip hints...) mean nothing on a phone, so this controller's root widget
// is hidden then. Interaction prompts / dialogue choices are a different system (interaction visualizers) and stay.
#include <cstdio>
extern "C" int cp2077_touch_active();
#endif

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( InputHintManagerGameController, game, ui );
	RTTI_SCRIPT_ALIAS( "InputHintManagerGameController" );
	RTTI_PARENT_TYPE( WidgetGameController );
	RED_EVENT_CONNECTOR( OnUpdateInputHint );
	RED_EVENT_CONNECTOR( OnLogTutorialHintActionEvent );
	RED_EVENT_CONNECTOR( OnAddInputGroupEvent );
	RED_EVENT_CONNECTOR( OnDeleteInputGroupEvent );
	RED_EVENT_CONNECTOR( OnDeleteInputHintBySourceEvent );
	RTTI_PROPERTY( m_hintContainerId ).editable();
	RTTI_PROPERTY( m_baseGroupContainer ).editable();
	RTTI_PROPERTY( m_groupsContainer ).editable();
	RTTI_PROPERTY( m_hintLibRef ).editable();
	RTTI_PROPERTY( m_groupLibRef ).editable();
RTTI_END_TYPE();

namespace
{
	static const Uint32 cMaxInputHintsLimit = 30;
	static const Uint32 cMaxKeysCombinationLimit = 5;

	game::ui::KeysCombinationHash GetKeysCombinationHash( const red::DynArray<ink::InputKeyData>& keyCombination, ink::InputHintHoldIndicationType holdIndicationType )
	{
		game::ui::KeysCombinationHash hash = 0;

		for( const ink::InputKeyData& key : keyCombination )
		{
			hash = red::CombineHashes32( hash, key.GetRawInputKeyID() );
			hash = red::CombineHashes32( hash, key.IsHoldInput() ? 0 : 1 );
		}

		return hash;
	}
}

namespace game
{
	namespace ui
	{
		struct HintsQueueSortingPredicate
		{
			Bool operator()( const InputHintData& a, const InputHintData& b )
			{
				return a.m_queuePriority < b.m_queuePriority;
			}
		};

		InputHintManagerGameController::InputHintManagerGameController()
			: m_lastUsedInputDevice( ::input::EInputDevice::INVALID )
		{
		}

		InputHintManagerGameController::~InputHintManagerGameController() = default;

		void InputHintManagerGameController::Initialize( const InitializeContext& initContext )
		{
			TBaseClass::Initialize( initContext );

			m_id = m_hintContainerId.GetHash();

			const ink::InputKeyIconManager& inputKeyIconManager = GetInkSystem().GetInputKeyIconManager();
			m_lastUsedInputDevice = inputKeyIconManager.GetActiveInputDevice();

			const auto& weakSelf = WeakHandleFromThis<InputHintManagerGameController>();
			const red::FixedSizeFunction< void( const ::input::EInputDevice, const ::input::EInputDevice ) > event =
				[ weakSelf ]( const ::input::EInputDevice lastInputDevice, const ::input::EInputDevice activeInputDevice )
			{
				if ( const auto& self = weakSelf.ToHandle() )
				{
					self->OnDeviceChanged( activeInputDevice );
				}
			};

			m_eventToken = inputKeyIconManager.OnInputDeviceChanged += event;

			m_useLocalHints = ink::IsLocalWidgetLibraryRef( this, m_hintLibRef );
			m_useLocalGroups = ink::IsLocalWidgetLibraryRef( this, m_groupLibRef );

			GameSystemUI *gameSystemUI = m_gameService->GetSystem<GameSystemUI>();
			gameSystemUI->RegisterSaveRequester( this );

			Load();

			THandle<game::ui::InputHintInitializedEvent> initEvent = CreateHandle<game::ui::InputHintInitializedEvent>( m_hintContainerId );
			QueueBroadcastEvent( initEvent );
		}

		void InputHintManagerGameController::Uninitialize( const UninitializeContext& uninitContext )
		{
			TBaseClass::Uninitialize( uninitContext );

			const ink::InputKeyIconManager& inputKeyIconManager = GetInkSystem().GetInputKeyIconManager();
			inputKeyIconManager.OnInputDeviceChanged -= m_eventToken;

			GameSystemUI *gameSystemUI = m_gameService->GetSystem<GameSystemUI>();
			gameSystemUI->UnRegisterSaveRequester( this );
		}

		Bool InputHintManagerGameController::NeedsUpdate() const
		{
#if defined( __EMSCRIPTEN__ )
			if( cp2077_touch_active() )
			{
				if( const THandle< ink::Widget > root = GetWidget() )
				{
					if( root->GetVisibleFlag() ) return true;   // [touch] hide it (Update)
				}
			}
#endif
			return m_isDirty;
		}

		void InputHintManagerGameController::Update( const UpdateContext& updateContext )
		{
			if( m_isDirty )
			{
				UpdateView();
				m_isDirty = false;
			}
#if defined( __EMSCRIPTEN__ )
			// [touch] Only ever hides (never forces visible), so without the page flag nothing changes.
			if( cp2077_touch_active() )
			{
				if( const THandle< ink::Widget > root = GetWidget() )
				{
					if( root->GetVisibleFlag() )
					{
						root->SetVisible( false );
						static int s_logs = 0;
						if( s_logs < 4 ) { ++s_logs; std::fprintf( stderr, "CP2077 touch: input hint widgets hidden\n" ); }
					}
				}
			}
#endif
		}

		void InputHintManagerGameController::OnUpdateInputHint( const UpdateInputHintEvent& evt )
		{
			if ( !IsValid() )
			{
				return;
			}

			if ( m_hintContainerId != evt.m_targetHintContainer )
			{
				return;
			}

			if ( evt.m_show )
			{
				AddHint( evt.m_data );
			}
			else
			{
				RemoveHint( evt.m_data );
			}
		}

		void InputHintManagerGameController::OnLogTutorialHintActionEvent( const LogTutorialHintActionEvent& evt )
		{
			if ( !IsValid() || evt.m_actionName.Empty() )
			{
				return;
			}

			if ( !m_tutorialActions.KeyExist( evt.m_actionName ) )
			{
				m_tutorialActions.Insert( evt.m_actionName, 0 );
			}

			m_tutorialActions[evt.m_actionName]++;

			UpdateTutorialHints( evt.m_actionName, m_tutorialActions[evt.m_actionName] );
		}

		void InputHintManagerGameController::OnAddInputGroupEvent( const AddInputGroupEvent& evt )
		{
			if (!IsValid())
			{
				return;
			}

			if( m_hintContainerId != evt.m_targetHintContainer )
			{
				return;
			}

			if( WeakHandle<ink::CompoundWidget>* group = m_groupWidgets.FindPtr( evt.m_groupId ) )
			{
				RED_ASSERT( false, "Group %hs already exist", evt.m_groupId.AsChar() );
			}

			auto onLoaded = [ weakSelf = WeakHandleFromThis<InputHintManagerGameController>(), &evt ]( const THandle<ink::Widget>& spawnedWidget, const THandle<ink::IWidgetController>& spawnedController, const THandle<ISerializable>& userData )
			{
				auto handleSelf = weakSelf.ToHandle();
				if( !handleSelf )
				{
					return;
				}

				if( spawnedWidget )
				{
					handleSelf->m_groupWidgets.Insert( evt.m_groupId, Cast<ink::CompoundWidget>( spawnedWidget ) );

					if( const THandle< InputHintGroupController > groupController = Cast<InputHintGroupController>( spawnedWidget->GetLogicController() ) )
					{
						groupController->SetData( evt.m_data );
					}
					else
					{
						RED_DATA_ERROR( red::LoggerCategory_UserInterface, spawnedController != nullptr, "Can't find 'InputHintGroupController' attached to the widget '%hs'", evt.m_groupId.AsChar() );
					}
				}
				else
				{
					RED_DATA_ERROR( red::LoggerCategory_UserInterface, spawnedWidget != nullptr, "Can't spawn input hint group widget for '%hs'", evt.m_groupId.AsChar() );
				}

				handleSelf->m_isDirty = true;
			};
			
			if( m_useLocalGroups )
			{
				AsyncSpawnFromLocal( m_hintLibRef.m_widgetItem, m_groupsContainer.Get(), onLoaded );
			}
			else
			{
				AsyncSpawnFromExternal( m_hintLibRef.m_widgetLibrary.GetPath(), m_hintLibRef.m_widgetItem, m_groupsContainer.Get(), onLoaded );
			}
		}

		void InputHintManagerGameController::OnDeleteInputGroupEvent( const DeleteInputGroupEvent& evt )
		{
			if (!IsValid())
			{
				return;
			}

			if( m_hintContainerId != evt.m_targetHintContainer )
			{
				return;
			}

			WeakHandle<ink::CompoundWidget>* group = m_groupWidgets.FindPtr( evt.m_groupId );
			THandle< ink::CompoundWidget > groupsContainer = m_groupsContainer.GetSafe< ink::CompoundWidget >();

			if( group && groupsContainer )
			{
				groupsContainer->RemoveChild( *group );
				m_groupWidgets.Remove( evt.m_groupId );
			}
			else
			{
				RED_ASSERT( false, "Group %hs doesn't exist", evt.m_groupId.AsChar() );
			}

			m_isDirty = true;
		}

		void InputHintManagerGameController::OnDeleteInputHintBySourceEvent( const DeleteInputHintBySourceEvent& evt )
		{
			if( !IsValid() )
			{
				return;
			}

			if( m_hintContainerId != evt.m_targetHintContainer )
			{
				return;
			}
			
			RemoveInputHintsBySource( evt.m_source );
		}


		void InputHintManagerGameController::OnDeviceChanged( ::input::EInputDevice deviceId )
		{
			m_lastUsedInputDevice = deviceId;
			
			red::DynArray< InputHintData > dataList{ red::PoolFrame() };
			dataList.Reserve( cMaxInputHintsLimit );

			for( const auto& it : m_queue )
			{
				dataList.PushBack( it.Value() );
			}

			m_queue.Clear();
			m_keyCombinations.Clear();

			for( const InputHintData& dataItem : dataList )
			{
				AddHint( dataItem );
			}
			
			ResetView();
		}

		void InputHintManagerGameController::AddHint( const InputHintData& hintData )
		{
			red::DynArray< ink::InputKeyData > keys{ ink::PoolInk_Controllers() };

			GetInkSystem().GetActionToInputKeyDataList( hintData.m_action, keys, m_lastUsedInputDevice );

			KeysCombinationHash inputHash = GetKeysCombinationHash( keys, hintData.m_holdIndicationType );
			HintsQueue* queue = m_queue.FindPtr( inputHash );

			if ( !HintApplicable( hintData ) )
			{
				return;
			}

			if( queue )
			{
				// [action] + [source id] is a unique key for input hints, duplicates are ignored
				const auto it = std::find_if( queue->Begin(), queue->End(), [ &hintData ]
				( const InputHintData& data ) { return data.m_source == data.m_source && data.m_action == data.m_action; } );
				if( it == queue->End() )
				{
					queue->PushBack( hintData );
				}
			}
			else
			{
				m_queue.Insert( inputHash, { { hintData }, { ink::PoolInk_Controllers() } } );
				m_keyCombinations.Insert( inputHash, keys );
			}

			m_isDirty = true;
		}

		void InputHintManagerGameController::RemoveHint( const InputHintData& hintData )
		{
			red::DynArray< ink::InputKeyData > keys{ red::PoolFrame() };
			keys.Reserve( cMaxKeysCombinationLimit );

			GetInkSystem().GetActionToInputKeyDataList( hintData.m_action, keys, m_lastUsedInputDevice );
			KeysCombinationHash inputHash = GetKeysCombinationHash( keys, hintData.m_holdIndicationType );
			HintsQueue* queue = m_queue.FindPtr( inputHash );

			if( queue )
			{
				queue->RemoveReorder( hintData );

				if( queue->Size() <= 0 )
				{
					m_queue.Remove( inputHash );
					m_keyCombinations.Remove( inputHash );
				}
			}

			m_isDirty = true;
		}

		void InputHintManagerGameController::RemoveInputHintsBySource( const CName& source )
		{
			auto queueIt = m_queue.Begin();

			while( queueIt != m_queue.End() )
			{
				HintsQueue& queue = queueIt.Value();
				red::FixedSizeFunction<Bool( const InputHintData& )> pred =
					[ &source ]( const InputHintData& data ) { return data.m_source == source; };
				queue.Remove( std::remove_if( queue.Begin(), queue.End(), pred ), queue.End() );

				if( queue.Size() <= 0 )
				{
					m_keyCombinations.Remove( queueIt.Key() );

					const auto res = m_queue.Remove( queueIt );
					if( res.IsSuccessful() )
					{
						queueIt = res.Iterator();
					}
				}
				else
				{
					++queueIt;
				}
			}

			m_isDirty = true;
		}

		void InputHintManagerGameController::UpdateView()
		{
			red::DynArray< KeysCombinationHash > usedCombinations{ red::PoolFrame() };
			usedCombinations.Reserve( m_hintWidgets.Size() );
			m_hintWidgets.GetKeys( usedCombinations );

			for( const auto& it : m_queue )
			{
				const HintsQueue& curQueue = it.Value();
				const KeysCombinationHash& curCombinationHash = it.Key();

				if( !curQueue.Empty() )
				{
					const InputHintData& hintData = curQueue.Front();

					if( WeakHandle<ink::Widget>* widget = m_hintWidgets.FindPtr( curCombinationHash ) )
					{
						THandle<ink::Widget> widgetHandle = widget->ToHandle();
						RED_ASSERT( widgetHandle, "Invalid inpunt hint widget" );

						UpdateHintWidgetViewData( widgetHandle, hintData.m_localizedLabel, hintData.m_action, hintData.m_enableHoldAnimation );
						UpdateHintWidgetGroup( widgetHandle, hintData.m_groupId );

						usedCombinations.Remove( curCombinationHash );
					}
					else
					{
						SpawnNewHintWidget( curCombinationHash, hintData );
					}
				}
			}

			for( const auto& it : usedCombinations )
			{
				THandle<ink::Widget> widget = m_hintWidgets.FindPtr( it )->ToHandle();
				RED_ASSERT( widget, "Invalid inpunt hint widget" );
				
				if( THandle<ink::CompoundWidget> parentWidget = Cast<ink::CompoundWidget>( widget->GetParentWidget() ) )
				{
					parentWidget->RemoveChild( widget );
					m_hintWidgets.Remove( it );
				}

				if( m_pendingWidgets.KeyExist( it ) )
				{
					m_pendingWidgets.Remove( it );
				}
			}
		}

		void InputHintManagerGameController::ResetView()
		{
			red::DynArray< WeakHandle<ink::Widget>> widgetList{ red::PoolFrame() };
			widgetList.Reserve( m_hintWidgets.Size() );
			m_hintWidgets.GetValues( widgetList );
			m_hintWidgets.Clear();
			m_pendingWidgets.Clear();

			for( const auto& it : m_queue )
			{
				const HintsQueue& curQueue = it.Value();
				const KeysCombinationHash& curCombinationHash = it.Key();

				if( !curQueue.Empty() )
				{
					const InputHintData& hintData = curQueue.Back();

					if( widgetList.Size() )
					{
						WeakHandle<ink::Widget> widget = widgetList.PopBack();
						InitializeHintWidget( widget, curCombinationHash );
						UpdateHintWidgetViewData( widget, hintData.m_localizedLabel, hintData.m_action, hintData.m_enableHoldAnimation );
						UpdateHintWidgetGroup( widget, hintData.m_groupId );
					}
					else
					{
						SpawnNewHintWidget( curCombinationHash, hintData );
					}

				}
			}
			
			for( const WeakHandle<ink::Widget>& widget : widgetList )
			{
				if( const THandle<ink::Widget> widgetHandle = widget.ToHandle() )
				{
					if( THandle<ink::CompoundWidget> parentWidget = Cast<ink::CompoundWidget>( widgetHandle->GetParentWidget() ) )
					{
						parentWidget->RemoveChild( widget );
					}
				}
			}

		}

		void InputHintManagerGameController::UpdateTutorialHints( const CName& tutorialAction, Int32 currentCount )
		{
			red::DynArray< InputHintData > hintsToRemove{ ink::PoolInk_Controllers() };

			for ( auto& queue : m_queue.Values() ) // unfortunate naming, m_queue is a map of queues
			{
				for ( auto& hint : queue )
				{
					if ( !HintApplicable( hint ) )
					{
						hintsToRemove.PushBack( hint );
					}
				}
			}

			for ( auto& hint : hintsToRemove )
			{
				RemoveHint( hint );
			}

			if ( hintsToRemove.Size() )
			{
				m_isDirty = true;
			}
		}

		bool InputHintManagerGameController::HintApplicable( const InputHintData& data )
		{
			if ( !data.m_tutorialAction.Empty() && data.m_tutorialActionCount > 0 )
			{
				Int32 actionsPerformed;

				if ( m_tutorialActions.Find( data.m_tutorialAction, actionsPerformed ) )
				{
					if ( actionsPerformed >= data.m_tutorialActionCount )
					{
						return false;
					}
				}
			}

			return true;
		}

		void InputHintManagerGameController::Load()
		{
			GameSystemUI *gameSystemUI = m_gameService->GetSystem<GameSystemUI>();
			if( THandle<InputHintManagerGameControllerData> savedData = Cast< InputHintManagerGameControllerData>( gameSystemUI->GetSavedData( UIDataType::HintManager, m_id ) ) )
			{
				for( Uint32 i = 0; i < savedData->m_inputHintsData.Size(); i++ )
				{
					AddHint( savedData->m_inputHintsData[ i ] );
				}

				for ( const auto& tutAction : savedData->m_tutorialActions )
				{
					m_tutorialActions[ tutAction.m_actionName ] = tutAction.m_occurances;
				}
			}		
			
			m_isDirty = true;
		}

		void InputHintManagerGameController::Save( THandle< BaseUIData > &outData )
		{
			THandle< InputHintManagerGameControllerData > data = CreateHandle< InputHintManagerGameControllerData>();
			data->SetID( m_id );
			data->m_inputHintsData.Reserve( m_queue.Size() );

			for( const auto& it : m_queue )
			{
				data->m_inputHintsData.PushBack( it.Value() );

			}

			for ( const auto& tutAction : m_tutorialActions )
			{
				data->m_tutorialActions.PushBack( { tutAction.Key(), tutAction.Value() } );
			}

			outData = data;
		}

		void InputHintManagerGameController::SpawnNewHintWidget( const KeysCombinationHash& combinationHash, const InputHintData& data )
		{
			if( m_pendingWidgets.KeyExist( combinationHash ) )
			{
				// already requested, but not loaded yet
				m_pendingWidgets[ combinationHash ] = data;
				return;
			}

			if( m_hintWidgets.KeyExist( combinationHash ) )
			{
				RED_ASSERT( false, "Widget for current input keys combination is already created" );
			}
			
			WeakHandle<ink::Widget> containerWidget;

			if( m_groupWidgets.KeyExist( data.m_groupId ) )
			{
				containerWidget = GetGroupContainer( m_groupWidgets[ data.m_groupId ] );
			}
			else
			{
				containerWidget = m_baseGroupContainer.Get();
			}

			if( auto containerHandle = containerWidget.ToHandle() )
			{
				const String& label = data.m_localizedLabel;
				const CName& action = data.m_action;
				const Bool enableHoldAnimation = data.m_enableHoldAnimation;

				auto onLoaded = [ weakSelf = WeakHandleFromThis<InputHintManagerGameController>(), label, action, enableHoldAnimation, combinationHash ]
								( const THandle<ink::Widget>& spawnedWidget, const THandle< ink::IWidgetController >& spawnedController, const THandle<ISerializable>& userData )
				{
					auto handleSelf = weakSelf.ToHandle();
					if( !handleSelf )
					{
						return;
					}

					if( spawnedWidget )
					{
						const Bool hasPendingData = handleSelf->m_pendingWidgets.KeyExist( combinationHash );

						if( !handleSelf->m_keyCombinations.KeyExist( combinationHash ) || !hasPendingData )
						{
							// the widget is finally loaded, but no longer needed, removing

							if( hasPendingData )
							{
								handleSelf->m_pendingWidgets.Remove( combinationHash );
							}

							if( auto parentWidget = Cast<ink::CompoundWidget>( spawnedWidget->GetParentWidget() ) )
							{
								parentWidget->RemoveChild( spawnedWidget );
								return;
							}
						}

						const InputHintData& data = handleSelf->m_pendingWidgets[ combinationHash ];
						handleSelf->InitializeHintWidget( spawnedWidget, combinationHash );
						handleSelf->UpdateHintWidgetViewData( spawnedWidget, data.m_localizedLabel, data.m_action, data.m_enableHoldAnimation );
						handleSelf->m_pendingWidgets.RemoveValue( data );
					}
					else
					{
						RED_DATA_ERROR( red::LoggerCategory_UserInterface, spawnedWidget != nullptr, "Can't spawn input hint for '%hs'", action.AsChar() );
					}
				};

				if( m_useLocalHints )
				{
					AsyncSpawnFromLocal( m_hintLibRef.m_widgetItem, containerHandle, onLoaded );
				}
				else
				{
					AsyncSpawnFromExternal( m_hintLibRef.m_widgetLibrary.GetPath(), m_hintLibRef.m_widgetItem, containerHandle, onLoaded );
				}

				m_pendingWidgets[ combinationHash ] = data;
			}
		}

		void InputHintManagerGameController::UpdateHintWidgetViewData( const THandle<ink::Widget>& hintWidget, const String& localizedLabel, const CName& action, Bool isHold /*= true*/ )
		{
			if( THandle<InputHintController> hintCtrl = hintWidget->GetLogicController<InputHintController>() )
			{
				hintCtrl->SetLabel( localizedLabel );
				if( isHold )
				{ 
					hintCtrl->SetHoldAction( action );
				}
				hintCtrl->Refresh();
			}
			else
			{
				RED_ASSERT( false, "InputHintController should be attached to the widget" );
			}
		}

		void InputHintManagerGameController::UpdateHintWidgetGroup( const THandle<ink::Widget>& hintWidget, const CName& groupId )
		{
			if( THandle<InputHintController> hintCtrl = hintWidget->GetLogicController<InputHintController>() )
			{
				RED_SCOPE_LOCK( m_groupsLock );

				WeakHandle<ink::CompoundWidget>* widget = m_groupWidgets.FindPtr( groupId );

				if( widget != nullptr )
				{
					if( THandle<ink::CompoundWidget> groupWidget = widget->ToHandle() )
					{
						groupWidget = GetGroupContainer( groupWidget );

						if( hintWidget->GetParentWidget() != groupWidget )
						{
							hintWidget->Reparent( groupWidget );
						}
					}
				}
			}
			else
			{
				RED_ASSERT( false, "InputHintController should be attached to the widget" );
			}
		}

		void InputHintManagerGameController::InitializeHintWidget( const THandle<ink::Widget>& hintWidget, const KeysCombinationHash& combinationHash )
		{
			{				
				RED_SCOPE_LOCK( m_hintsLock );
				m_hintWidgets.Insert( combinationHash, hintWidget );
			}

			if( auto hintController = hintWidget->GetLogicController<InputHintController>() )
			{
				KeysCombination* combination = m_keyCombinations.FindPtr( combinationHash );
				RED_ASSERT( combination, "Keys combination doesn't exist" );
				hintController->SetInputKeyCombination( *combination );
			}
			else
			{
				RED_ASSERT( false, "InputHintController should be attached to the widget" );
			}
		}

		THandle<ink::CompoundWidget> InputHintManagerGameController::GetGroupContainer( const THandle<ink::CompoundWidget>& groupWidget ) const
		{
			if( THandle<InputHintGroupController> groupController = groupWidget->GetLogicController<InputHintGroupController>() )
			{
				const ink::CompoundWidgetReference& groupContainerRef = groupController->GetContainerRef();
				const THandle< ink::Widget > groupContainer = groupContainerRef.Get();
				if( groupContainer )
				{
					return Cast<ink::CompoundWidget>( groupContainer );
				}
			}

			// use root widget if sub container is not defined
			return groupWidget;
		}

		Bool InputHintManagerGameController::IsValid() const
		{
			return GetWidget() != nullptr;
		}

	}	// namespace ui
}	// namespace game

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif