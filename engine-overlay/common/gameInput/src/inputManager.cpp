#include "build.h"
#include "inputManager.h"
#include "inputManagerBackend.h"
#include "inputContextManager.h"
#include "inputBindingsViewer.h"
#include "inputCache.h"
#include "inputSensitivityMappingHandler.h"
#include "../../common/engine/include/nativeFunctionsRegistry.h"
#include "../../common/renderData/include/debugFilter.h"
#include "../../common/renderData/include/drawer.h"
#include "../../common/input/include/inputBufferedInputEvent.h"
#include "../../common/engine/include/debugConsoleTypes.h"
#include "../redConfig/include/inGameConfigSystem.h"

#if defined( __EMSCRIPTEN__ )
#include <emscripten/emscripten.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
// [touch] Publish the active player / UI input context, and (from the interaction visualizer controller) the number of
// on-screen interaction prompts, for the page's touch overlay. A small seqlocked struct in Wasm memory; the page reads
// it on demand through Module.cp2077TouchState() (registered once). No per-frame main-thread calls.
namespace
{
	struct alignas( 8 ) Cp2077TouchState
	{
		Uint32 m_seq;
		char m_player[ 64 ];
		char m_ui[ 64 ];
		Int32 m_prompts;
		Int32 m_dialog;
		Int32 m_touchActive;   // written by the page (Module.cp2077SetTouchActive / Module.cp2077TouchActive)
	};
	static_assert( offsetof( Cp2077TouchState, m_player ) == 4 && offsetof( Cp2077TouchState, m_ui ) == 68 &&
		offsetof( Cp2077TouchState, m_prompts ) == 132 && offsetof( Cp2077TouchState, m_dialog ) == 136 && offsetof( Cp2077TouchState, m_touchActive ) == 140, "touch state layout is read by touch.js" );
	Cp2077TouchState g_cpTouchState{};
	std::atomic< bool > g_cpTouchRegistered{ false };

	void CpTouchRegister()
	{
		if( g_cpTouchRegistered.exchange( true ) ) return;
		MAIN_THREAD_ASYNC_EM_ASM( {
			Module['cp2077TouchState'] = (function( at ) { return function() {
				var str = function( o ) { var t = ''; for( var i = 0; i < 64; i++ ) { var ch = HEAPU8[ o + i ]; if( !ch ) break; t += String.fromCharCode( ch ); } return t; };
				var q0 = HEAPU32[ at / 4 ];
				var r = ({ seq: q0, ctx: str( at + 4 ), ui: str( at + 68 ), prompts: HEAP32[ ( at + 132 ) / 4 ], dialog: HEAP32[ ( at + 136 ) / 4 ] });
				if( ( q0 & 1 ) || HEAPU32[ at / 4 ] !== q0 ) return null;
				return r; }; })( Number( $0 ) );
			Module['cp2077SetTouchActive'] = (function( at ) { return function( v ) { HEAP32[ ( at + 140 ) / 4 ] = v ? 1 : 0; }; })( Number( $0 ) );
			if( Module['cp2077TouchActive'] ) Module['cp2077SetTouchActive']( 1 );
		}, static_cast< double >( reinterpret_cast< uintptr_t >( &g_cpTouchState ) ) );
	}
	void CpTouchBegin() { __atomic_fetch_add( &g_cpTouchState.m_seq, 1u, __ATOMIC_SEQ_CST ); }
	void CpTouchEnd() { __atomic_fetch_add( &g_cpTouchState.m_seq, 1u, __ATOMIC_SEQ_CST ); }
	void CpTouchCopy( char* dst, const char* src )
	{
		const size_t n = src ? strnlen( src, 63 ) : 0;
		if( n ) std::memcpy( dst, src, n );
		dst[ n ] = 0;
	}
	void CpTouchPublishContexts( CName player, CName ui )
	{
		static CName s_player;
		static CName s_ui;
		static Bool s_first = true;
		CpTouchRegister();
		if( !s_first && player == s_player && ui == s_ui ) return;
		s_first = false;
		s_player = player;
		s_ui = ui;
		CpTouchBegin();
		CpTouchCopy( g_cpTouchState.m_player, player.AsChar() );
		CpTouchCopy( g_cpTouchState.m_ui, ui.AsChar() );
		CpTouchEnd();
		std::fprintf( stderr, "CP2077 input context: player=%s ui=%s\n", player.AsChar(), ui.AsChar() );
	}
}
// [touch] Nonzero while the page runs its touch overlay (mobile): the engine hides keyboard-only hint widgets.
extern "C" int cp2077_touch_active()
{
	return __atomic_load_n( &g_cpTouchState.m_touchActive, __ATOMIC_RELAXED );
}
extern "C" void cp2077_touch_set_prompts( int total, int dialog )
{
	if( g_cpTouchState.m_prompts == total && g_cpTouchState.m_dialog == dialog ) return;
	CpTouchBegin();
	g_cpTouchState.m_prompts = total;
	g_cpTouchState.m_dialog = dialog;
	CpTouchEnd();
	std::fprintf( stderr, "CP2077 input prompts: total=%d dialog=%d\n", total, dialog );
}
#endif

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

#ifndef RED_CONFIGURATION_FINAL
#define CHECK_OVERRIDES
#endif

namespace game
{
	namespace input
	{
		static rend::DebugFilter DF_InputUseDebugManager( "Input/UseDebugManager" );

		static const CName s_configKeyBindings( RED_NAME_CONSTEXPR( "/key_bindings" ) );
		static const Uint16 c_invalidKeyCode = static_cast<Uint16>( -1 );
		static const Uint16 c_winKeyCode = static_cast<Uint16>( EInputKey::IK_Unknown5B );

		CName ParseContextName( const CName& groupName )
		{
			const red::StringView groupNameView = groupName.AsStringView();
			const Uint32 separatorIndex = groupNameView.FindReverse( '/' );

			return RED_NAME( groupNameView.SubView( separatorIndex + 1, groupNameView.Length() - separatorIndex + 1 ) );
		}

		Uint16 GetKeyValue( const CName& keyName, const rtti::EnumType* inputKeyEnum )
		{
			Int64 value;
			if ( inputKeyEnum->FindValue( keyName, value ) )
			{
				return (Uint16)value;
			}

			return c_invalidKeyCode;
		}
	}
}

game::input::Manager::Manager( Bool isDebugManager )
	: m_backend( RED_NEW( ManagerBackend ) )
	, m_contextManager( RED_NEW( ContextManager ) )
	, m_visualDebugOn( false )
	, m_sensitivityMappingDebugOn( false )
	, m_deadzoneDebugOn( false )
	, m_functionalTestsOn( false )
	, m_isDebugManager( isDebugManager )
	, m_overridesChanged( false )
	, m_lastUsedDevice( ::input::EInputDevice::INVALID )
	, m_lastOverrideStatus( OverrideStatus::OS_None )
	, m_registeredValidators( game::PoolGMPL_Input() )
	, m_registeredListeners( game::PoolGMPL_Input() )
	, m_notificationHandlerID( 0 )
{
}

game::input::Manager::~Manager()
{
	RED_DELETE( m_backend );
	RED_DELETE( m_contextManager );

	UnregisterConfigCallbacks();
}

void game::input::Manager::CacheInput( EInputKey key, EInputAction action, Float data )
{
	PC_SCOPE_FUNC();

	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	if( m_functionalTestsOn )
	{
		return;
	}

	RED_SCOPE_LOCK( m_lock );
	m_backend->CacheInput( key, action, data );
}

void game::input::Manager::DispatchInput( Float dt, const red::DynArray< ListenerAction >& actions )
{
	PC_SCOPE_FUNC();

	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );

	RED_SCOPE_LOCK( m_lock );

	for( const auto& action : actions )
	{
		m_contextManager->AddInputAction( action );
	}

	m_backend->Tick_Receivers( dt );

	m_contextManager->DispatchInput();
}

void game::input::Manager::Tick( Float dt )
{
	Tick(dt, net::Time::Invalid);
}

void game::input::Manager::Tick( Float dt, net::Time time)
{
	PC_SCOPE_FUNC();

	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );

	RED_SCOPE_LOCK( m_lock );

	m_backend->Tick( dt );

	Cache& cache = m_backend->GetCache();
	m_contextManager->ProcessInput( dt, cache, time );
	cache.Clear();
#if defined( __EMSCRIPTEN__ )
	if( !m_isDebugManager )
	{
		CpTouchPublishContexts( m_contextManager->GetCurrentContext(), m_contextManager->GetCurrentUIContext() );   // [touch]
	}
#endif
}

void game::input::Manager::RegisterListener( IInputListener* listener )
{
	RED_SCOPE_LOCK( m_lock );
	RegisterListener_NoLock( listener );
}

void game::input::Manager::SaveScriptListenerID( THandle< IScriptable > listener, Uint32 listenerID, CName actionName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_backend, InputSystem, "InputManager::m_backend is NULL" );
	m_backend->SaveScriptListenerID( listener, listenerID, actionName );
}

Uint32 game::input::Manager::RemoveScriptListenerID( THandle< IScriptable > listener, CName actionName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_backend, InputSystem, "InputManager::m_backend is NULL" );
	return m_backend->RemoveScriptListenerID( listener, actionName );
}

void game::input::Manager::RegisterListener( IInputListener* listener, CName action )
{
	RED_SCOPE_LOCK( m_lock );
	RegisterListener_NoLock( listener, action );
}

void game::input::Manager::UnregisterListener( IInputListener* listener )
{
	RED_SCOPE_LOCK( m_lock );
	UnregisterListener_NoLock( listener );
}

void game::input::Manager::UnregisterListener( IInputListener* listener, CName action )
{
	RED_SCOPE_LOCK( m_lock );
	UnregisterListener_NoLock( listener, action );
}


game::input::ManagerView::LambdaID game::input::Manager::StartRawButtonListener(const RawButtonListenerLambda& listener)
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	return m_backend->StartRawButtonListener( listener );
}

void game::input::Manager::StopRawButtonListener(LambdaID lambdaID)
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	m_backend->StopRawButtonListener( lambdaID );
}

CName game::input::Manager::GetCurrentContext() const
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->GetCurrentContext();
}

Bool game::input::Manager::ResetContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->ResetContext( contextName );
} 

Bool game::input::Manager::StoreContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->StoreContext( contextName );
}

CName game::input::Manager::RestoreContext( CName expectedContext )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->RestoreContext( expectedContext );
}

CName game::input::Manager::PopContext()
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->PopContext();
}

Bool game::input::Manager::RemoveContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentContext() m_contextManager is NULL" );
	return m_contextManager->RemoveContext( contextName );
}

Bool game::input::Manager::IsPlayerStackActive() const
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::IsPlayerStackActive() m_contextManager is NULL" );
	return m_contextManager->IsPlayerStackActive();
}

void game::input::Manager::TogglePlayerStackActive( Bool active )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::TogglePlayerStackActive() m_contextManager is NULL" );
	m_contextManager->TogglePlayerStackActive( active );
}

CName game::input::Manager::GetCurrentUIContext() const
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::GetCurrentUIContext() m_contextManager is NULL" );
	return m_contextManager->GetCurrentUIContext();
}

Bool game::input::Manager::ResetUIContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::ResetUIContext() m_contextManager is NULL" );
	return m_contextManager->ResetUIContext( contextName );
} 

Bool game::input::Manager::StoreUIContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::StoreUIContext() m_contextManager is NULL" );
	return m_contextManager->StoreUIContext( contextName );
}

CName game::input::Manager::RestoreUIContext( CName expectedContext )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::RestoreUIContext() m_contextManager is NULL" );
	return m_contextManager->RestoreUIContext( expectedContext );
}

CName game::input::Manager::PopUIContext()
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::PopUIContext() m_contextManager is NULL" );
	return m_contextManager->PopUIContext();
}

Bool game::input::Manager::RemoveUIContext( CName contextName )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::RemoveUIContext() m_contextManager is NULL" );
	return m_contextManager->RemoveUIContext( contextName );
}

Bool game::input::Manager::IsUIStackActive() const
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::IsUIStackActive() m_contextManager is NULL" );
	return m_contextManager->IsUIStackActive();
}

void game::input::Manager::ToggleUIStackActive( Bool active )
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::ToggleUIStackActive() m_contextManager is NULL" );
	m_contextManager->ToggleUIStackActive( active );
}

Bool game::input::Manager::ResetDebugContext(CName contextName)
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::ResetDebugContext() m_contextManager is NULL" );
	return m_contextManager->ResetDebugContext( contextName );
}

Bool game::input::Manager::StoreDebugContext(CName contextName)
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::StoreDebugContext() m_contextManager is NULL" );
	return m_contextManager->StoreDebugContext( contextName );
}

CName game::input::Manager::RestoreDebugContext(CName expectedContext)
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::RestoreDebugContext() m_contextManager is NULL" );
	return m_contextManager->RestoreDebugContext( expectedContext );
}

CName game::input::Manager::PopDebugContext()
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::PopDebugContext() m_contextManager is NULL" );
	return m_contextManager->PopDebugContext();
}

Bool game::input::Manager::RemoveDebugContext(CName contextName)
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::RemoveDebugContext() m_contextManager is NULL" );
	return m_contextManager->RemoveDebugContext( contextName );
}

Float game::input::Manager::GetRadialDeadzoneInner() const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetRadialDeadzoneInner();
}

Float game::input::Manager::GetRadialDeadzoneOuter() const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetRadialDeadzoneOuter();
}

#ifndef RED_CONFIGURATION_FINAL
void game::input::Manager::RegisterTickReceiver( red::WeakPtr< IInputTickReceiver > receiver, const String& dbgName )
{
	RED_SCOPE_LOCK( m_lock );
	m_backend->AddTickReceiver( receiver, dbgName );
}
#else // RED_CONFIGURATION_FINAL
void game::input::Manager::RegisterTickReceiver( red::WeakPtr< IInputTickReceiver > receiver )
{
	RED_SCOPE_LOCK( m_lock );
	m_backend->AddTickReceiver( receiver );
}
#endif // RED_CONFIGURATION_FINAL


void game::input::Manager::UnregisterTickReceiver( red::SharedPtr< IInputTickReceiver > receiver )
{
	RED_SCOPE_LOCK( m_lock );

	m_backend->RemoveTickReceiver( receiver );
}

game::input::ManagerView::LambdaID game::input::Manager::RegisterListener( const ListenerLambda& listener )
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	return m_backend->RegisterLambdaListener( listener, CName::NONE(), *this );
}

game::input::ManagerView::LambdaID game::input::Manager::RegisterListener( const ListenerLambda& listener, CName action )
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	return m_backend->RegisterLambdaListener( listener, action, *this );
}

void game::input::Manager::UnregisterListener( LambdaID id )
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	m_backend->UnregisterLambdaListener( id, *this );
}


Bool game::input::Manager::IsActionPressed( CName action ) const 
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->IsActionPressed( action );
}

Bool game::input::Manager::IsButtonPressed_DBG( EInputKey key ) const 
{
#if defined( RED_CONFIGURATION_FINAL )
	return false;
#else // RED_CONFIGURATION_FINAL
	RED_SCOPE_LOCK( m_lock );
	return m_backend->IsKeyPressed( key );
#endif // RED_CONFIGURATION_FINAL
}

Float game::input::Manager::GetActionAxisValue(CName action) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetActionAxisValue( action );
}

void game::input::Manager::RenderDebug( rend::IDebugDrawer& dd ) const
{
	PC_SCOPE_FUNC();
	
	if( dd.GetMask().Test( DF_InputUseDebugManager ) != m_isDebugManager )
	{
		return;
	}

	m_contextManager->RenderDebug( dd );
	m_backend->GetSensitivityMappingHandler()->RenderDebug( dd );
}

Bool game::input::Manager::Load(const String& mappingsFile, const String& contextsFile)
{
	GAME_ASSERT(m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL");
	if (!m_contextManager->LoadMappings(mappingsFile))
	{
		GAME_DATA_ERROR( false, InputSystem, "Could not load mappings from %hs", mappingsFile.AsChar() );
		return false;
	}

	if( !m_contextManager->LoadContexts( contextsFile ) )
	{
		GAME_DATA_ERROR( false, InputSystem, "Could not load mappings from %hs", contextsFile.AsChar() );
		return false;
	}

	return true;
}

Bool game::input::Manager::Reload()
{
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	Bool userMappingReloaded = false;
	Bool contextReloaded = false;

	userMappingReloaded = m_contextManager->ReloadInputUserMapping();
	contextReloaded = m_contextManager->ReloadContexts();

	// re-register listeners after reloading context
	if( contextReloaded )
	{
		const ManagerBackend::Listeners& listeners = m_backend->GetListeners();

		for( auto it_listener : listeners )
		{
			if( it_listener.m_allActions )
			{
				m_contextManager->RegisterListener( it_listener.m_listener );
			}
			else
			{
				for( auto it_action : it_listener.m_actions )
				{
					m_contextManager->RegisterListener( it_listener.m_listener, it_action );
				}		
			}
		}
	}

	return userMappingReloaded && contextReloaded;
}

Bool game::input::Manager::InitializeFromSettings()
{
#ifdef RED_PLATFORM_WINPC
	using namespace InGameConfig;
	System& system = System::GetInstance();

	if ( GEngine->IsBackend() )
	{
		return false;
	}

	const rtti::EnumType* inputKeyEnum = GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "EInputKey" ) );

	const red::StaticArray< CName, sc_settingsContextsMax >& settingsContextNames = GetContextsNames();
	const red::StaticArray< CName, sc_settingsContextsMax >& settingsGroupsNames = GetSettingsGroupsNames();

	for ( Uint32 index = 0; index < sc_settingsContextsMax; ++index )
	{
		const CName& contextName = settingsContextNames[ index ];
		const CName& subGroupName = settingsGroupsNames[ index ];

		red::DynArray< OverridableMapping > overrides{ game::PoolGMPL_Input() };
		GetOverridesInContext( contextName, false, overrides );

		Group& subGroup = system.GetGroup( subGroupName );
		red::DynArray< Var* > vars = subGroup.GetVars( QF_NoFilter );

		for ( Var* var : vars )
		{
			const char* overridableIDStr = var->GetName().AsChar();
			const Uint32 overridableID = String::SimpleHash( overridableIDStr, (Uint32)strlen( overridableIDStr ) );

			for ( const OverridableMapping& inputOverride : overrides )
			{
				if ( inputOverride.m_id != overridableID )
				{
					continue;
				}

				VarName* nameVar = static_cast<VarName*>( var );

				CName defaultKeyName;
				if ( inputKeyEnum->FindName( inputOverride.m_key, defaultKeyName ) )
				{
					nameVar->SetDefaultValue( defaultKeyName );

					if ( nameVar->GetValue().Empty() )
					{
						nameVar->SetValue( defaultKeyName, Source::PresetCode );
					}
				}

				if ( !nameVar->HasDefaultValue() )
				{
					Uint16 newKey = GetKeyValue( nameVar->GetValue(), inputKeyEnum );
					if ( newKey != 0 )
					{
#ifdef CHECK_OVERRIDES
						red::DynArray< OverridableMapping > conflicts{ game::PoolGMPL_Input() };
						Bool result = CheckOverrideConflicts( contextName, newKey, conflicts );

						GAME_DATA_ERROR( result, InputSystem, "Input override '%hs' present in the config should not conflict with any other override", nameVar->GetValue().AsChar() );
#endif
						Override( contextName, inputOverride.m_id, newKey );
					}
#ifdef CHECK_OVERRIDES
					else
					{
						GAME_DATA_ERROR( false, InputSystem, "Invalid key value '%hs' in the config group '%hs'", nameVar->GetValue().AsChar(), subGroupName.AsChar() );
					}
#endif
				}

				break;
			}

			auto validationHandler = [ this ]( CName groupName, CName varName, VarType type, Source source )
			{
				RED_ASSERT( type == VarType::Name, "There's a settings variable with an unexpected type" );
				return HandleVarValidation( groupName, varName );
			};
			VarValidator::ID validatorID = system.RegisterValidator( subGroupName, var->GetName(), validationHandler );
			m_registeredValidators.PushBack( validatorID );
		}
 	}

	ApplyOverrides();

	for ( const CName& subGroupName : settingsGroupsNames )
	{
		auto changeHandler = [ this ]( CName groupName, CName varName, VarType type, ChangeReason reason )
		{
			RED_ASSERT( type == VarType::Name, "There's a settings variable with an unexpected type" );
			if ( reason == ChangeReason::Accepted )
			{
				HandleVarChange( groupName, varName );
			}
			else if ( reason == ChangeReason::NeedsConfirmation || reason == InGameConfig::ChangeReason::Rejected )
			{
				HandleVarCoercion( groupName, varName );
			}
		};

		VarListener::ID listenerID = system.RegisterListener( subGroupName, changeHandler );
		m_registeredListeners.PushBack( listenerID );
	}

	auto notificationHandler = [ this ]( NotificationType type )
	{
		switch ( type )
		{
		case InGameConfig::NotificationType::ChangesApplied:
			if ( m_overridesChanged )
			{
				m_overridesChanged = false;
				ApplyOverrides();
			}
			break;
		case InGameConfig::NotificationType::ChangesRejected:
			if ( m_overridesChanged )
			{
				m_overridesChanged = false;
				UndoOverrides();
			}
			break;
		default:
			break;
		}
	};
	m_notificationHandlerID = system.RegisterNotifier( notificationHandler );
#endif
	return true;
}

void game::input::Manager::HandleVarChange( CName groupName, CName varName )
{
	using namespace InGameConfig;
	System& system = InGameConfig::System::GetInstance();
	const VarName& var = system.GetVarName( groupName, varName );

	const rtti::EnumType* inputKeyEnum = GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "EInputKey" ) );
	Uint16 key = GetKeyValue( var.GetRequestedValue(), inputKeyEnum );

	const CName contextName = ParseContextName( groupName );
	const CName overridableIDName = varName;

	{
		red::DynArray< OverridableMapping > conflicts{ game::PoolGMPL_Input() };
		Bool result = CheckOverrideConflicts( contextName, key, conflicts );
		GAME_DATA_ERROR( result, InputSystem, "Binding still has conflicts, those should be unbound in the validator" );
	}

	const char* overridableIDStr = overridableIDName.AsChar();
	const Uint32 overridableID = String::SimpleHash( overridableIDStr, (Uint32)strlen( overridableIDStr ) );

	Bool result = Override( contextName, overridableID, key );
	GAME_DATA_ERROR( result, InputSystem, "Failed to override a binding, validation shouldn't have let that happen" );
	if ( result )
	{
		SetOverrideStatus( OverrideStatus::OS_Success );
	}

	m_overridesChanged = true;
}

void game::input::Manager::HandleVarCoercion( CName groupName, CName varName )
{
	using namespace InGameConfig;
	System& system = InGameConfig::System::GetInstance();
	VarName& var = system.GetVarName( groupName, varName );

	const rtti::EnumType* inputKeyEnum = GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "EInputKey" ) );
	Uint16 key = GetKeyValue( var.GetRequestedValue(), inputKeyEnum );
	// we don't need to check collisions against invalid input or unbound actions
	if ( key == c_invalidKeyCode || key == 0 )
	{
		return;
	}

	const CName contextName = ParseContextName( groupName );
	red::DynArray< OverridableMapping > conflicts{ game::PoolGMPL_Input() };
	CheckOverrideConflicts( contextName, key, conflicts );

	Group& contextGroup = system.GetGroup( groupName );
	red::DynArray< Var* > contextGroupVars = contextGroup.GetVars( QF_NoFilter );
	const CName c_unboundKeyName = RED_NAME_CONSTEXPR( "IK_None" );

	for ( Var* otherVar : contextGroupVars )
	{
		// don't coerce against self
		if ( otherVar->GetName() == varName )
		{
			continue;
		}

		VarName* nameOtherVar = static_cast<VarName*>( otherVar );
		// don't coerce if we allowed defaults to collide
		if ( nameOtherVar->GetDefaultValue() == var.GetDefaultValue() )
		{
			continue;
		}

		// this won't be visible in conflicts, because input manager doesn't know about those changes yet
		if ( nameOtherVar->GetRequestedValue() == var.GetRequestedValue() )
		{
			SetOverrideStatus( OverrideStatus::OS_OtherUnbound );
			nameOtherVar->SetValue( c_unboundKeyName, Source::Settings );
		}

		const char* overridableIDStr = nameOtherVar->GetName().AsChar();
		const Uint32 overridableID = String::SimpleHash( overridableIDStr, (Uint32)strlen( overridableIDStr ) );

		for ( const OverridableMapping& conflict : conflicts )
		{
			if ( conflict.m_id != overridableID || 
				// it's no longer a collision, because we have changes pending
				nameOtherVar->GetRequestedValue() != var.GetRequestedValue() )
			{
				continue;
			}
			
			SetOverrideStatus( OverrideStatus::OS_OtherUnbound );
			nameOtherVar->SetValue( c_unboundKeyName, Source::Settings );
		}
	}
}

Bool game::input::Manager::HandleVarValidation( CName groupName, CName varName )
{
	using namespace InGameConfig;
	System& system = InGameConfig::System::GetInstance();
	VarName& var = system.GetVarName( groupName, varName );

	if ( var.GetDefaultValue() == var.GetRequestedValue() )
	{
		return true;
	}

	if ( var.GetRequestedValue() == RED_NAME_CONSTEXPR_NOREG( "IK_None" ) )
	{
		return true;
	}

	const rtti::EnumType* inputKeyEnum = GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "EInputKey" ) );
	Uint16 key = GetKeyValue( var.GetRequestedValue(), inputKeyEnum );
	if ( key == c_invalidKeyCode ||
		key == c_winKeyCode )
	{
		SetOverrideStatus( OverrideStatus::OS_Failure );
		return false;
	}

	const CName contextName = ParseContextName( groupName );

	red::DynArray< OverridableMapping > conflicts{ game::PoolGMPL_Input() };
	if ( !CheckOverrideConflicts( contextName, key, conflicts ) )
	{
		for ( const OverridableMapping& conflict : conflicts )
		{
			if ( conflict.m_id == 0 )
			{
				SetOverrideStatus( OverrideStatus::OS_Failure );
				return false;
			}
		}
	}

	return true;
}

void game::input::Manager::SetOverrideStatus( OverrideStatus status )
{
	if ( m_lastOverrideStatus > status )
	{
		return;
	}

	m_lastOverrideStatus = status;
}

void game::input::Manager::UnregisterConfigCallbacks()
{
	using namespace InGameConfig;
	System& system = InGameConfig::System::GetInstance();

	for ( VarValidator::ID id : m_registeredValidators )
	{
		system.UnregisterValidator( id );
	}
	m_registeredValidators.Clear();

	for ( VarListener::ID id : m_registeredListeners )
	{
		system.UnregisterListener( id );
	}
	m_registeredListeners.Clear();

	if ( m_notificationHandlerID != 0 )
	{
		system.UnregisterNotifier( m_notificationHandlerID );
	}
	m_notificationHandlerID = 0;
}

Float game::input::Manager::GetHoldTimeout( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetHoldTimeout( action );
}

Float game::input::Manager::GetActionDuration( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetActionDuration( action );
}

Float game::input::Manager::GetHoldDuration( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetHoldDuration( action );
}

Float game::input::Manager::GetHoldPercentage( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetHoldPercentage( action );
}

Bool game::input::Manager::IsHoldAction( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->IsHoldAction( action );
}

void game::input::Manager::SetInitialContexts( CName playerContextName, CName uiContextName )
{
	RED_SCOPE_LOCK( m_lock );
	m_contextManager->SetInitialContexts( playerContextName, uiContextName );
}

void game::input::Manager::CurveMappingSingleAxis( Float& x, CName curveName, Bool debug /*= false */ )
{
	RED_SCOPE_LOCK( m_lock );
	Float dummy = 0.f;
	m_backend->GetSensitivityMappingHandler()->Process( x, dummy, curveName, debug );
}

Float game::input::Manager::CurveMappingSingleAxis( const Float& x, CName curveName, Bool debug /*= false */ )
{
	RED_SCOPE_LOCK( m_lock );
	return m_backend->GetSensitivityMappingHandler()->Process( x, curveName, debug );
}

void game::input::Manager::CurveMappingDoubleAxis( Float& x, Float& y, CName curveName, Bool debug /*= false */ )
{
	RED_SCOPE_LOCK( m_lock );
	m_backend->GetSensitivityMappingHandler()->Process( x, y, curveName, debug );
}

EInputKey game::input::Manager::GetInputKeyByActionName( CName action, Bool isGamepad, const Float* axisVal ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetInputKeyByActionName( action, isGamepad, axisVal );
}

EInputKey game::input::Manager::GetUIActionToCurrentRawInput( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	// @todo MS tempshit, should pick a correct button based on device type
	return m_contextManager->GetUIActionToCurrentRawInput( action, m_backend->LastUsedPad() );
}

void game::input::Manager::GetActionToInputKeyDataList( CName action, red::DynArray<ink::InputKeyData>& keys, ::input::EInputDevice deviceId /* = input::EInputDevice::INVALID */ ) const
{
	RED_SCOPE_LOCK( m_lock );

	if( deviceId != ::input::EInputDevice::INVALID )
	{
		m_contextManager->GetActionToInputKeyDataList( action, keys, deviceId != ::input::EInputDevice::KBD_MOUSE );
	}
	else
	{
		m_contextManager->GetActionToInputKeyDataList( action, keys, m_backend->LastUsedPad() );
	}
}

void game::input::Manager::GetActionToInputKeyDataList( CName contextName, CName action, red::DynArray< ink::InputKeyData >& keys, ::input::EInputDevice deviceId /*= ::input::EInputDevice::INVALID */ ) const
{
	RED_SCOPE_LOCK( m_lock );

	if ( deviceId != ::input::EInputDevice::INVALID )
	{
		m_contextManager->GetActionToInputKeyDataList( contextName, action, keys, deviceId != ::input::EInputDevice::KBD_MOUSE );
	}
	else
	{
		m_contextManager->GetActionToInputKeyDataList( contextName, action, keys, m_backend->LastUsedPad() );
	}
}

EInputKey game::input::Manager::GetUIActionToCurrentRawInput( CName action, ::input::EInputDevice device ) const
{
	if ( device == ::input::EInputDevice::INVALID )
		return EInputKey::IK_None;

	RED_SCOPE_LOCK( m_lock );
	// @todo MS tempshit, should pick a correct button based on device type
	Bool isPad = device != ::input::EInputDevice::KBD_MOUSE;
	return m_contextManager->GetUIActionToCurrentRawInput( action, isPad );
}

EInputKey game::input::Manager::GetUIActionToCurrentRawInput( CName contextName, CName action, ::input::EInputDevice device ) const
{
	if ( device == ::input::EInputDevice::INVALID )
		return EInputKey::IK_None;

	RED_SCOPE_LOCK( m_lock );
	// @todo MS tempshit, should pick a correct button based on device type
	Bool isPad = device != ::input::EInputDevice::KBD_MOUSE;
	return m_contextManager->GetUIActionToCurrentRawInput( contextName, action, isPad );
}

String game::input::Manager::GetUIActionToCurrentInputName( CName action ) const
{
	RED_SCOPE_LOCK( m_lock );
	// @todo MS tempshit, should pick a correct button based on device type
	return m_contextManager->GetUIActionToCurrentInputName( action, m_backend->LastUsedPad() );
}

 String game::input::Manager::GetUIActionToCurrentInputName( CName action, ::input::EInputDevice device ) const
{
	if ( device == ::input::EInputDevice::INVALID )
		return String::EMPTY();

	RED_SCOPE_LOCK(m_lock);
	// @todo MS tempshit, should pick a correct button based on device type
	Bool isPad = device != ::input::EInputDevice::KBD_MOUSE;
	return m_contextManager->GetUIActionToCurrentInputName( action, isPad );
}

String game::input::Manager::GetUIActionToCurrentInputName(CName contextName, CName action, ::input::EInputDevice device) const
{
	if ( device == ::input::EInputDevice::INVALID )
		return String::EMPTY();

	RED_SCOPE_LOCK(m_lock);
	// @todo MS tempshit, should pick a correct button based on device type
	Bool isPad = device != ::input::EInputDevice::KBD_MOUSE;
	return m_contextManager->GetUIActionToCurrentInputName( contextName, action, isPad );
}

String game::input::Manager::GetUIActionToFullInputName( CName action, Bool isGamepad ) const
{
	RED_SCOPE_LOCK( m_lock );
	return m_contextManager->GetUIActionToFullInputName( action, isGamepad );
}

void game::input::Manager::GetActionsDisplayList( red::DynArray<ContextDisplayData>& actions ) const
{
	 InputBindingsViewer viewer( this );
	 viewer.GetViewData( actions );
}

void game::input::Manager::TurnFunctionalTestsOn( Bool on )
{
	RED_SCOPE_LOCK( m_lock );
	m_functionalTestsOn = on;
}

void game::input::Manager::FunctionalTestsFakeInput( EInputKey key, EInputAction action, Float data )
{
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	RED_SCOPE_LOCK( m_lock );

	if( !m_backend->IsAxis( key ) && action == EInputAction::IACT_Axis )
	{
		if( data != 0.0f )
		{
			action = EInputAction::IACT_Press;
		}
		else
		{
			action = EInputAction::IACT_Release;
		}
	}

	m_backend->CacheInput( key, action, data );
}

void game::input::Manager::FunctionalTestsReleaseAll()
{
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	GAME_ASSERT( m_functionalTestsOn, InputSystem, "Trying to provide input for functional tests, but functional tests are turned off" );

	RED_SCOPE_LOCK( m_lock );
	for( Uint16 i = 1; i < ( Uint16 )EInputKey::IK_Count; ++i )
	{
		if( m_backend->IsAxis( i ) || m_backend->IsRelative( i ) )
		{
			m_backend->CacheInput( ( EInputKey )i, EInputAction::IACT_Axis, 0.0f );
		}
		else
		{
			m_backend->CacheInput( ( EInputKey )i, EInputAction::IACT_Release, 0.0f );
		}
	}
}

Bool game::input::Manager::LastUsedPad() const
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	return m_backend->LastUsedPad();
}

Bool game::input::Manager::LastUsedKeyboard() const
{
	RED_SCOPE_LOCK( m_lock );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );
	return m_backend->LastUsedKeyboard();
}

::input::EInputDevice game::input::Manager::LastUsedDevice() const
{
	RED_SCOPE_LOCK( m_lock );
	return m_lastUsedDevice;
}

void game::input::Manager::SetLastUsedDevice( ::input::EInputDevice device )
{
	RED_SCOPE_LOCK( m_lock );
	m_lastUsedDevice = device;
}

void game::input::Manager::ToggleBlockGameInput( Bool block )
{	
	RED_SCOPE_LOCK( m_lock );

	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::ToggleBlockGameInput() m_contextManager is NULL" );
	m_contextManager->ToggleBlockGameInput( block );
	
}

void game::input::Manager::GetOverridesInContext(const CName contextName,Bool pad,red::DynArray< OverridableMapping >& outOverrides) const
{
	RED_ASSERT( m_contextManager );
	m_contextManager->GetOverridesInContext( contextName, pad, outOverrides );
}

Bool game::input::Manager::CheckOverrideConflicts(const CName contextName,Uint16 key,red::DynArray< OverridableMapping >& outConflicts) const
{
	RED_ASSERT( m_contextManager );
	return m_contextManager->CheckOverrideConflicts( contextName, key, outConflicts );
}

Bool game::input::Manager::Override(const CName contextName,Uint32 overridableID,Uint16 newKey)
{
	RED_ASSERT( m_contextManager );
	return m_contextManager->Override( contextName, overridableID, newKey );
}

void game::input::Manager::ApplyOverrides()
{
	RED_ASSERT( m_contextManager );
	m_contextManager->ApplyOverrides();
}

void game::input::Manager::UndoOverrides()
{
	RED_ASSERT( m_contextManager );
	m_contextManager->UndoOverrides();
}

void game::input::Manager::ResetOverridesToDefault()
{
	RED_ASSERT( m_contextManager );
	m_contextManager->ResetOverridesToDefault();
}


void game::input::Manager::TryApplyDeadzone(Vector2& axisInput, const CName deadzonePreset ) const
{
	RED_ASSERT( m_contextManager );
	m_contextManager->TryApplyDeadzone( axisInput, deadzonePreset );
}

game::input::OverrideStatus game::input::Manager::ConsumeLastOverrideStatus()
{
	const OverrideStatus status = m_lastOverrideStatus;
	m_lastOverrideStatus = OverrideStatus::OS_None;
	return status;
}

void game::input::Manager::RegisterListener_NoLock(IInputListener* listener)
{
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	m_contextManager->RegisterListener( listener );
	m_backend->AddListener( listener );
}


void game::input::Manager::RegisterListener_NoLock(IInputListener* listener,CName action)
{
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	m_contextManager->RegisterListener( listener, action );
	m_backend->AddListener( listener, action );
}

void game::input::Manager::UnregisterListener_NoLock(IInputListener* listener)
{
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	m_contextManager->UnregisterListener( listener );
	m_backend->RemoveListener( listener );
}

void game::input::Manager::UnregisterListener_NoLock(IInputListener* listener,CName action)
{
	GAME_ASSERT( m_contextManager, InputSystem, "CInputManager::m_contextManager is NULL" );
	GAME_ASSERT( m_backend, InputSystem, "CInputManager::m_backend is NULL" );

	m_contextManager->UnregisterListener( listener, action );
	m_backend->RemoveListener( listener, action );
}

#if !defined( NO_DEBUG_CONSOLE )

void game::input::Manager::RegisterConsoleCommands()
{
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.FunctionalTestsOn", "Turn functional tests on", &input::Manager::ConsoleCommandFunctionalTestOn, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.FunctionalTestsOff", "Turn functional tests off", &input::Manager::ConsoleCommandFunctionalTestOff, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.GetOverridesInContext", "GetOverridesInContext", &input::Manager::ConsoleCommandGetOverridesInContext, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.CheckOverrideConflicts", "CheckOverrideConflicts", &input::Manager::ConsoleCommandCheckOverrideConflicts, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.Override", "Override", &input::Manager::ConsoleCommandOverride, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.ApplyOverrides", "ApplyOverrides", &input::Manager::ConsoleCommandApplyOverrides, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.UndoOverrides", "UndoOverrides", &input::Manager::ConsoleCommandUndoOverrides, this );
	GetNativeFunRegistry().RegisterNativeFunction( "Game.Input.ResetOverridesToDefault", "ResetOverridesToDefault", &input::Manager::ConsoleCommandResetOverridesToDefault, this );
}

void game::input::Manager::UnregisterConsoleCommands()
{
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.FunctionalTestsOn" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.FunctionalTestsOff" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.GetOverridesInContext" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.CheckOverrideConflicts" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.Override" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.ApplyOverrides" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.UndoOverrides" );
	GetNativeFunRegistry().UnregisterNativeFunction( "Game.Input.ResetOverridesToDefault" );
}


Bool game::input::Manager::ConsoleCommandFunctionalTestOn( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->TurnFunctionalTestsOn( true );
	manager->FunctionalTestsReleaseAll();
	return true;
}

Bool game::input::Manager::ConsoleCommandFunctionalTestOff( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->TurnFunctionalTestsOn( false );
	manager->FunctionalTestsReleaseAll();
	return true;
}

Bool game::input::Manager::ConsoleCommandGetOverridesInContext(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	red::DynArray< OverridableMapping > overridable{ red::PoolFrame() };

	CName context = RED_NAME_CONSTEXPR( "Exploration" );
	Bool pad = false;

	if( parameters.Size() > 0 )
	{
		context = RED_NAME( parameters[ 0 ] );
	}

	if( parameters.Size() > 1 )
	{
		pad = parameters[ 1 ] == "pad";
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->GetOverridesInContext( context, pad, overridable );

	for( Uint32 i = 0; i < overridable.Size(); ++i )
	{
		invokeResult.LogLine( String::Printf( "%hs - %d", overridable[ i ].m_mappingName.AsChar(), ( (Uint32) overridable[ i ].m_key ) ).AsChar() );
	}

	return true;
}

Bool game::input::Manager::ConsoleCommandCheckOverrideConflicts(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	red::DynArray< OverridableMapping > conflicts{ red::PoolFrame() };

	CName context = RED_NAME_CONSTEXPR( "Exploration" );
	Uint16 key = 68; // IK_D

	if( parameters.Size() > 0 )
	{
		context = RED_NAME( parameters[ 0 ] );
	}

	if( parameters.Size() > 1 )
	{
		FromString( parameters[ 1 ], key );
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->CheckOverrideConflicts( context, key, conflicts );

	for( Uint32 i = 0; i < conflicts.Size(); ++i )
	{
		invokeResult.LogLine( String::Printf( "%hs in action %hs - %d", conflicts[ i ].m_mappingName.AsChar(), conflicts[ i ].m_actionName.AsChar(), (Uint32) conflicts[ i ].m_key ).AsChar() );
	}

	return true;
}

Bool game::input::Manager::ConsoleCommandOverride(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	red::DynArray< OverridableMapping > conflicts{ red::PoolFrame() };

	CName context = RED_NAME_CONSTEXPR( "Exploration" );
	Uint32 overridableID = 0;
	Uint16 key = 68; // IK_D

	if( parameters.Size() > 0 )
	{
		context = RED_NAME( parameters[ 0 ] );
	}

	if( parameters.Size() > 2 )
	{
		overridableID = parameters[ 1 ].CalcHash();
	}

	if( parameters.Size() > 2 )
	{
		FromString( parameters[ 2 ], key );
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->Override( context, overridableID, key );

	for( Uint32 i = 0; i < conflicts.Size(); ++i )
	{
		invokeResult.LogLine( String::Printf( "%hs in action %hs - %d", conflicts[ i ].m_mappingName.AsChar(), conflicts[ i ].m_actionName.AsChar(), (Uint32) conflicts[ i ].m_key ).AsChar() );
	}

	return true;
}

Bool game::input::Manager::ConsoleCommandApplyOverrides(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->ApplyOverrides();

	return true;
}

Bool game::input::Manager::ConsoleCommandUndoOverrides(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->UndoOverrides();

	return true;
}

Bool game::input::Manager::ConsoleCommandResetOverridesToDefault(IDebugConsole* callingConsole,const red::DynArray< red::String >& parameters,SCommandInvokeResult& invokeResult,void* userData)
{
	if( !userData )
	{
		invokeResult.LogLine( "Wrong user data - console command was not registered properly! It should be registered for specific object." );
		return false;
	}

	input::Manager* manager = static_cast<input::Manager*>( userData );
	manager->ResetOverridesToDefault();

	return true;
}

#endif // !NO_DEBUG_CONSOLE

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif