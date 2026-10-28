/*
* Copyright (c) 2018-20 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "gameAutoSaveSystem.h"

#include "../../world/include/gameInstance.h"
#include "../../engine/include/tickInfo.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../gameCore/include/telemetrySystemInterface.h"

// CP2077 web [saves]: ?sandbox / ?sandboxWorld / ?sandboxGamedef pages (gym worlds started from the
// command line, see native-browser.html) never auto-save. A gym autosave became the main menu's
// CONTINUE entry and loading it crashed. &sandboxSaves=1 re-enables saving for debugging.
#include <atomic>
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
namespace cpweb_saves
{
	static Bool IsSandboxNoSavePage()
	{
		static std::atomic< Int32 > s_value{ -1 };
		Int32 v = s_value.load();
		if ( v < 0 )
		{
#ifdef __EMSCRIPTEN__
			v = MAIN_THREAD_EM_ASM_INT( {
				try {
					const q = new URLSearchParams( location.search );
					const sandbox = q.has( 'sandbox' ) || q.has( 'sandboxWorld' ) || q.has( 'sandboxGamedef' );
					const allow = q.get( 'sandboxSaves' );
					const quest = q.get( 'start' ) === 'nomadquest' && q.get( 'questSaves' ) === '0'; /* [nomad2] checkpoint saves ON by default on nomadquest (save-compile stack smash fixed in packageObjectSerializer.cpp); &questSaves=0 turns them off */
					const watson = q.get( 'start' ) === 'watson' && !( q.get( 'watsonSaves' ) === '1' || allow === '1' || allow === 'true' ); /* [watson] debug q001 start: no saves (as nomadfree); &watsonSaves=1 re-enables */
					return ( ( sandbox && !( allow === '1' || allow === 'true' ) ) || quest || watson ) ? 1 : 0;
				} catch ( e ) { return 0; }
			} );
#else
			v = 0;
#endif
			s_value.store( v );
			std::fprintf( stderr, "CP2077 saves: sandbox page=%d -> autosave %s\n", v, v ? "DISABLED" : "enabled" );
		}
		return v > 0;
	}
}


RTTI_BEGIN_TYPE_IN_NAMESPACE( AutoSaveSystem, game );
	RTTI_PARENT_TYPE( game::IAutoSaveSystem );
	RTTI_NATIVE_FUNCTION( "RequestCheckpoint", funcRequestCheckpoint );
RTTI_END_TYPE();

namespace Config
{
	TConfigVar< Bool > cvAutoSaveEnable( "SaveConfig", "AutoSaveEnabled", true, eConsoleVarFlag_Save );
	TConfigVar< Int32 > cvAutoSavePeriod( "SaveConfig", "AutoSavePeriod", 300, 0, 3600, eConsoleVarFlag_Save );
	TConfigVar< Int32 > cvAutoSavePeriodOnLock( "SaveConfig", "AutoSavePeriodOnLock", 30, 10, 120, eConsoleVarFlag_Save );
}

namespace game
{
	static rend::DebugFilter DF_AutoSave = rend::DebugFilter( "GameSystem/AutoSave" );

	AutoSaveRequest::AutoSaveRequest( const IAutoSaveSystem::CallbackFunc&& func, const Uint8 flags, const red::String& debugString )
		: m_callback{ func }
		, m_state{ State::Pending }
		, m_flags{ flags }
	{}

	AutoSaveRequest::~AutoSaveRequest() = default;

	Bool AutoSaveRequest::IsDone() const
	{
		return m_state.GetValue() == State::Completed;
	}

	Bool AutoSaveRequest::IsCancelled() const
	{
		return m_state.GetValue() == State::Cancelled;
	}

	void AutoSaveRequest::Complete( const bool isSuccess )
	{
		if( m_state.CompareExchange( State::Completed, State::Pending ) == State::Pending )
		{
			m_callback( isSuccess );
			m_callback = nullptr;
		}
	}

	void AutoSaveRequest::Cancel()
	{
		if( m_state.CompareExchange( State::Cancelled, State::Pending ) == State::Pending )
		{
			m_callback = nullptr;
		}
	}

	Uint8 AutoSaveRequest::Flags() const
	{
		return m_flags;
	}
	
	bool AutoSaveRequest::HasFlag( const SaveRequestFlag flag ) const
	{
		return m_flags & flag;
	}

	const red::String& AutoSaveRequest::DebugString() const
	{
		return m_debugString;
	}

	AutoSaveSystem::AutoSaveSystem() = default;
	AutoSaveSystem::~AutoSaveSystem() = default;

	void AutoSaveSystem::OnInitialize( job::Counter& counter )
	{
		PausePeriodicCooldown(); // this is too fast to do it async
	}

	void AutoSaveSystem::RegisterUpdateFunctions( UpdatableSystemRegistrar& registrar )
	{
		registrar.Register( UpdateTickGroup::PreRenderUpdate, *this, "AutoSaveSystem/Tick", [this]( const UpdateContext& updateContext, job::Builder& builder )
		{
			if( updateContext.m_timeDelta > 0.00001f && 
				!updateContext.m_tickInfo.IsGameplayHidden() ) // don't tick when the game is paused, but tick with real delta when it's not
			{
				ProcessPeriodicCooldown( updateContext.m_tickInfo.m_realTimeDelta );
			}
		});
	}

	void AutoSaveSystem::WorldAttached( world::RuntimeScene& scene )
	{
		ResetPeriodicCooldown( true );
	}

	void AutoSaveSystem::WorldPendingDetach( world::RuntimeScene& scene )
	{
		PausePeriodicCooldown();
	}

	void AutoSaveSystem::WorldDetached( world::RuntimeScene& scene )
	{
		PausePeriodicCooldown();
	}

	void AutoSaveSystem::OnGamePrepared()
	{
		ResetPeriodicCooldown( true );
	}

	IGameSystem::PreSaveResponse AutoSaveSystem::OnPreGameSave( const job::RunContext& context, game::MetadataCollector& collector )
	{
		PausePeriodicCooldown();
		collector.Push< red::String >( RED_NAME_CONSTEXPR( "debugString" ), m_debugString );
		return PreSaveResponse::NothingToSave;
	}

	void AutoSaveSystem::OnPostGameSave()
	{
		FirePendingCallbacks( true );
		ResetPeriodicCooldown( true );
	}

	void AutoSaveSystem::OnSaveGameFailed()
	{
		FirePendingCallbacks( false );
		ResetPeriodicCooldown( false );
	}

	Bool AutoSaveSystem::AutoSavesEnabled() const
	{
		if ( cpweb_saves::IsSandboxNoSavePage() )
		{
			return false;
		}
		return Config::cvAutoSaveEnable.Get();
	}

	red::SharedPtr< game::IAutoSaveRequest > AutoSaveSystem::RequestAutoSave( const CallbackFunc&& func, const Uint8 flags, const red::String& debugString )
	{
		// CP2077 web [saves]: sandbox pages never save, not even point-of-no-return / end-game saves.
		const bool shouldSave = !cpweb_saves::IsSandboxNoSavePage() && ( AutoSavesEnabled() || ( flags & game::SaveRequestFlag::PointOfNoReturn ) || ( flags & game::SaveRequestFlag::EndGame ) );
		if( !shouldSave )
		{
			func( false );
			return nullptr;
		}

		auto request = red::CreateSharedPtr< AutoSaveRequest >( (const CallbackFunc&&)( func ), flags, debugString );

		RED_SCOPE_LOCK( m_lock );
		m_pendingRequests.PushBack( request );

		return request;
	}

	ShouldAutoSave AutoSaveSystem::ConsumeAllAutoSaveRequests()
	{
		ShouldAutoSave result = { false, 0 };

		RED_SCOPE_LOCK( m_lock );

		for ( auto& request : m_pendingRequests )
		{
			if( request->IsCancelled() )
			{
				continue;
			}

			if ( result.flags == 0 )
			{
				if( request->HasFlag( SaveRequestFlag::Periodic ) )
				{
					result = { true, request->Flags() };
				}
				if( request->HasFlag( SaveRequestFlag::Checkpoint ) )
				{
					result = { true, request->Flags() };
				}
			}
			if( request->HasFlag( SaveRequestFlag::Force ) )
			{
				result = { true, request->Flags() };
			}
			if( request->HasFlag( SaveRequestFlag::PointOfNoReturn ) )
			{
				result = { true, request->Flags() };
			}
			if( request->HasFlag( SaveRequestFlag::EndGame ) )
			{
				result = { true, request->Flags() };
			}

			// this is going to overwrite the debug string
			if ( !request->DebugString().Empty() )
			{
				// TODO check debugString?
				m_debugString = request->DebugString();
				if( auto telemetrySys = GetGameInstance()->GetSystemPtr< game::ITelemetrySystem >() )
				{
					telemetrySys->LogSaveMetaDataDebugString( m_debugString );
				}
			}

			m_pendingCallbacks.PushBack( request );
		}

		m_pendingRequests.Clear();
		return result;
	}

	void AutoSaveSystem::PausePeriodicCooldown()
	{
		m_periodicCooldown = 0.f;
	}

	void AutoSaveSystem::ResetPeriodicCooldown( const bool isSuccess )
	{
		const auto cooldown = static_cast< float >( isSuccess ? Config::cvAutoSavePeriod.Get() : Config::cvAutoSavePeriodOnLock.Get() );
		m_periodicCooldown = math::Max( m_periodicCooldown, cooldown );
	}

	void AutoSaveSystem::ProcessPeriodicCooldown( const float realTimeDelta )
	{
		if ( AutoSavesEnabled() && m_periodicCooldown > 0.f )
		{
			m_periodicCooldown -= realTimeDelta;

			if ( m_periodicCooldown <= 0.f )
			{
				PausePeriodicCooldown();
				RequestAutoSave( []( Bool success ){ RED_UNUSED( success ); }, SaveRequestFlag::Periodic );
			}
		}
	}

	void AutoSaveSystem::FirePendingCallbacks( const bool isSuccess )
	{
		RED_SCOPE_LOCK( m_lock );

		for ( auto& request : m_pendingCallbacks )
		{
			request->Complete( isSuccess );
		}
		m_pendingCallbacks.Clear();
	}

	void AutoSaveSystem::OnRenderDebug( const RenderDebugContext& context )
	{
#if !defined( RED_CONFIGURATION_FINAL )
		if ( context.debugDrawer.GetMask().Test( DF_AutoSave ) )
		{
			rend::FragmentBuilder builder( context.debugDrawer );
			builder.EnableTextBackground( Color::BLACK() );
			builder.SetColor( Color::LIGHT_CYAN() );
			builder.AddFlatText( 20, 140, String::Printf( "AutoSave in: %.1f sec.", m_periodicCooldown ).AsChar() );
		}
#else
		RED_UNUSED( context );
#endif
	}

	void AutoSaveSystem::funcRequestCheckpoint( CScriptStackFrame& stack, void* result, const rtti::IType* resultType )
	{
		FINISH_PARAMETERS;
		RequestAutoSave( []( Bool success ){ RED_UNUSED( success ); }, SaveRequestFlag::Checkpoint );
	}

} // game
