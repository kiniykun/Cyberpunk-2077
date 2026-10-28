/**
* Copyright (C)2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "../../inkCore/include/widget.h"
#include "../../inkCore/include/children.h"
#include "../../inkCore/include/viewportWrapper.h"

#include "../../gameTweakDB/include/tweakDB.h"
#include "../../gameTweakDB/include/records/tweakDBUICondition.h"
#include "../../gameTweakDB/include/records/tweakDBUIElement.h"
#include "../../gameTweakDB/include/types/tweakDBUICondition.h"

#include "../../redJobs2/include/jobBuilder.h"
#include "widgetGameControllerInterface.h"
#include "uiElementManager.h"
#include <atomic> // [skperf] round 41
#include <cstdio>

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

namespace world
{
	namespace ui
	{
		static red::Atomic< Uint64 > s_registrationTokenCounter( 0 );
		static const Uint64 s_invalidToken = 0;

		world::ui::ElementManager::ElementManager()
			: m_elements( ink::PoolInk_System() )
			, m_elementsList( ink::PoolInk_System() )
		{
			const auto& records = game::data::GetTweakDB().GetRecords< game::data::UIElement_Record >();
			m_elements.Reserve( records.Size() );
			for( const auto& record : records )
			{
				m_elements.Insert( record->GetID(), Element() );
			}
			m_elementsList.Reserve( records.Size() );
		}

		void ElementManager::Update( const tick::Info& tickInfo, job::Builder& builder )
		{
			RED_SCOPE_LOCK( m_lock );
			{
				m_elementsList.Clear();
				m_elements.GetValues( m_elementsList );
			}

			const red::WeakPtr< ElementManager >& weakElementManager = SharedFromThis();

			const auto tickSingleElementFunc = [ weakElementManager, tickInfo]( Uint32 index, const job::RunContext& context )
			{
				if ( const auto& elementManager = weakElementManager.Lock() )
				{
					const Element& element = elementManager->m_elementsList[ index ];
					element.Update( tickInfo );
				}
			};

			static red::InstrumentationObject instrObj{ "UI/TickElementManager/TickElement", PBC_UI };
			builder.DispatchParallelForJob(instrObj, { m_elementsList.Size() }, tickSingleElementFunc );
		}

		void ElementManager::BindElement( const TweakDBID& elementRecordID, const THandle< IWidgetGameController >& controller )
		{
			RED_SCOPE_SHARED_LOCK( m_lock );

			if( controller == nullptr )
			{
				return;
			}

			const auto iter = m_elements.Find( elementRecordID );
			if( iter == m_elements.End() )
			{
				return;
			}

			Element& element = iter.Value();
			if( !element.m_controller.Expired() )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, "%hs is attempting to bind to element %hs which already has binding", controller->GetFriendlyName().AsChar(), elementRecordID.ToString().AsChar() );
				return;
			}

			element.m_controller = controller;
		}

		void ElementManager::UnbindElement( const TweakDBID& elementRecordID, const THandle< IWidgetGameController >& controller )
		{
			RED_SCOPE_SHARED_LOCK( m_lock );

			if( controller == nullptr )
			{
				return;
			}

			auto iter = m_elements.Find( elementRecordID );
			if( iter == m_elements.End() )
			{
				return;
			}

			Element& element = iter.Value();
			element.m_controller = nullptr;
		}

		ElementConditionToken ElementManager::RegisterListener( const TweakDBID& elementRecordID, game::data::UICondition condition, Bool expectedValue, CallbackT&& onFulfilled )
		{
			RED_SCOPE_SHARED_LOCK( m_lock );

			if( !IsUIConditionSupportedByElement( condition, elementRecordID ) )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false,
					"Attempting to register to UI Condition '%hs' that is not supported by UI Element '%hs'",
					GetUIConditionRecord( condition )->enumName().AsChar(),
					elementRecordID.ToString().AsChar() );

				return s_invalidToken;
			}

			const auto iter = m_elements.Find( elementRecordID );
			if( iter == m_elements.End() )
			{
				return s_invalidToken;
			}

			const Uint64 token = s_registrationTokenCounter.Increment();

			Element& element = iter.Value();
			Element::ListenerArray& listeners = element.m_listeners.Emplace( condition, ink::PoolInk_System() ).Iterator().Value();
			Element::Listener& listener = listeners.EmplaceBack( token, expectedValue, std::move( onFulfilled ) );

			return token;
		}

		void ElementManager::UnregisterListener( const TweakDBID& elementRecordID, game::data::UICondition condition, ElementConditionToken token )
		{
			RED_SCOPE_SHARED_LOCK( m_lock );

			if( token == s_invalidToken )
			{
				return;
			}

			const auto iter = m_elements.Find( elementRecordID );
			if( iter == m_elements.End() )
			{
				return;
			}

			Element& element = iter.Value();
			Element::ListenerArray& listeners = element.m_listeners.Find( condition ).Value();

			for( Uint32 i=0; i < listeners.Size(); ++i )
			{
				const Element::Listener& listener = listeners[i];
				if( listener.m_token == token )
				{
					listeners.RemoveAt( i );
					break;
				}
			}
		}

		Bool ElementManager::ReadCondition( const TweakDBID& elementRecordID, game::data::UICondition condition ) const
		{
			RED_SCOPE_SHARED_LOCK( m_lock );

			const auto iter = m_elements.Find( elementRecordID );
			if( iter != m_elements.End() )
			{
				return iter.Value().ReadCondition( condition );
			}
			return false;
		}
		
		//////////////////////////////////////////////////////////////////////////

		Element::Element()
			: Element( nullptr )
		{}
		Element::Element( const THandle< IWidgetGameController >& controller )
			: m_controller( controller )
			, m_listeners( ink::PoolInk_System() )
		{}

		void Element::Update( const tick::Info& tickInfo ) const
		{
			const THandle< IWidgetGameController > controller = m_controller.ToHandle(); // Optim : only promote once per tick
			for( auto iter = m_listeners.Begin(); iter != m_listeners.End(); ++iter )
			{
				game::data::UICondition condition = iter.Key();
				const Element::ListenerArray& listeners = iter.Value();

				const Bool result = ReadCondition( condition, controller );
				for( const Element::Listener& listener : listeners )
				{
					if( listener.m_expectedValue == result )
					{
						listener.m_onFulfilled();
					}
				}
			}
		}

		//////////////////////////////////////////////////////////////////////////
		// CONDITIONS

		Bool CustomCondition( game::data::UICondition condition, const THandle< IWidgetGameController >& controller )
		{
			if( controller == nullptr )
				return false;

			Bool result = false;
			if( !rtti::CallFunctionRet( controller.Get(), RED_NAME_CONSTEXPR( "ReadUICondition" ), result, condition ) )
			{
				RED_DATA_ERROR( red::LoggerCategory_UserInterface, false, 
					"Attempting to read UI Condition '%hs' that is not supported by UI Element '%hs'", 
					GetUIConditionRecord( condition )->enumName().AsChar(),
					controller->GetElementRecordID().ToString().AsChar() );
			}
			return result;
		}

		Bool IsVisible( game::data::UICondition condition, const THandle< IWidgetGameController >& controller )
		{
			if( controller == nullptr )
				return false;

			if( const auto rootWidget = controller->GetWidget() )
			{
				if( rootWidget->GetVisible() )
				{
					// Sanity check : Make sure there's at least one visible child of the root
					Bool hasVisibleChildren = false;
					const auto children = rootWidget->GetChildren();
					const Uint32 numChildren = children->Size();
					for( Uint32 i = 0; i < numChildren; ++i )
					{
						if( const auto child = children->GetChildAt( i ) )
						{
							if( child->GetVisible() )
							{
								hasVisibleChildren = true;
								break;
							}
						}
					}
					return hasVisibleChildren;
				}
			}
			return false;
		}

		using ConditionFunc = std::add_pointer<Bool( game::data::UICondition condition, const THandle< IWidgetGameController >& controller )>::type;
		ConditionFunc GetConditionFunc( game::data::UICondition condition )
		{
			// TODO_UI : Ideally we shouldn't have to find the record every time
			auto record = GetUIConditionRecord( condition );

			// [skperf] round 41 (web): a missing TweakDB record used to be dereferenced (no trap on wasm: reads near
			// address 0) and an unknown condition fell through to the compiled-out fatal assert -> nullptr, which
			// ReadCondition then called ('RuntimeError: null function', tc29 bar crash at ~312 s). Both are now nullptr
			// and ReadCondition treats them as 'condition not met'.
			if( record == nullptr )
			{
				return nullptr;
			}

			// Custom Condition
			if( record->isCustom() )
			{
				return CustomCondition;
			}

			// Generic Condition
			switch( condition )
			{
			case game::data::UICondition::Visible: return IsVisible;
			}

			RED_FATAL_ASSERT( false, "[UIElementManager::GetConditionFunc] Condition is invalid or custom" );
			return nullptr;
		}

		/* static */ Bool Element::ReadCondition( game::data::UICondition condition, const THandle< IWidgetGameController >& controller )
		{
			const ConditionFunc func = GetConditionFunc( condition );
			if( func == nullptr ) // [skperf] round 41 (web): see GetConditionFunc
			{
				static std::atomic< Uint32 > s_cpNullConditions{ 0 };
				const Uint32 n = s_cpNullConditions.fetch_add( 1, std::memory_order_relaxed ) + 1;
				if( n <= 8 || ( n & ( n - 1 ) ) == 0 )
				{
					std::fprintf( stderr, "CP2077 skperf uicond guard: unknown/unrecorded UI condition %u read as false (count=%u)\n", ( Uint32 )condition, n );
				}
				return false;
			}
			return func( condition, controller );
		}

		//
		//////////////////////////////////////////////////////////////////////////

		Bool Element::ReadCondition( game::data::UICondition condition ) const
		{			
			return ReadCondition( condition, m_controller.ToHandle() );
		}

		//////////////////////////////////////////////////////////////////////////

		Element::Listener::Listener() = default;
		Element::Listener::Listener( ElementConditionToken token, Bool expectedValue, CallbackT&& onFulfilled )
			: m_token( token )
			, m_expectedValue( expectedValue )
			, m_onFulfilled( onFulfilled )
		{}

		//////////////////////////////////////////////////////////////////////////
		// UIElement TweakDB Helpers

		THandle< game::data::UICondition_Record > GetUIConditionRecord( game::data::UICondition condition )
		{
			const auto records = game::data::GetTweakDB().GetRecords< game::data::UICondition_Record >();
			for( const auto& record : records )
			{
				const auto uiConditionRecord = Cast< game::data::UICondition_Record >( record );
				if( uiConditionRecord->type == condition )
				{
					return uiConditionRecord;
				}
			}
			return nullptr;
		}

		Bool IsUIConditionSupportedByElement( game::data::UICondition condition, const TweakDBID& elementRecordID )
		{
			const auto conditionRecord = GetUIConditionRecord( condition );
			const auto elementRecord = Cast< game::data::UIElement_Record >( game::data::GetTweakDB().Get( elementRecordID ) );
			if( !conditionRecord || !elementRecord )
				return false;

			if( !conditionRecord->isCustom() )	// Non-custom conditions are supported by all elements
				return true;

			return elementRecord->customConditionsContains( conditionRecord );
		}

	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif