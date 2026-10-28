#include "native-initialization-job-trace.h"
/**
* Copyright (c) 2015 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "gameFrameworkImpl.h"

#include "../../gameCore/include/gameInstance.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../engine/include/systemsUpdater.h"
#include "../../redCore/include/commandline.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../navmeshGeneration/include/navmeshGenerationData.h"
#include "gameStateMachineImpl.h"

namespace gsm
{
	namespace helper
	{
		template< typename T >
		bool ParseCommandlineParam( const AnsiChar* name, const red::CommandLine& commandline, ParamsView& outParams )
		{
			String valueStr;
			if ( commandline.GetFirstOption( name, valueStr ) )
			{
				auto valueHolder = rtti::ValueHolder::Parse( valueStr.AsChar() );
				if ( valueHolder )
				{
					T value;

					if ( GetTypeObject<T>() && GetTypeObject<T>()->WriteValue( IRTTIContext::GetDefault(), &value, rtti::AccessPath(), *valueHolder ) )
					{
						outParams.Push<T>( RED_NAME( red::StringView( name ) ), value );
						return true;
					}
					else
					{
						RED_LOG_WARNING( "GSM: Failed to set value for initialization parameter '%hs' of type '%hs' from '%hs'",
							name, ( GetTypeObject<T>() ? GetTypeObject<T>()->GetName().AsChar() : "<null>" ), valueStr.AsChar() );
					}
				}
				else
				{
					RED_LOG_WARNING( "GSM: Failed to parse value for initialization parameter '%hs' of type '%hs' from '%hs'",
						name, ( GetTypeObject<T>() ? GetTypeObject<T>()->GetName().AsChar() : "<null>" ), valueStr.AsChar() );
				}
			}

			return false;
		}

		template<>
		bool ParseCommandlineParam< world::WorldID >( const AnsiChar* name, const red::CommandLine& commandline, ParamsView& outParams )
		{
			String valueStr;
			if ( commandline.GetFirstOption( name, valueStr ) )
			{
				world::WorldID value = world::WorldID::CreateFromStringPath( valueStr );
				if ( value.IsValid() )
				{
					outParams.Push< world::WorldID >( RED_NAME( red::StringView( name ) ), value );
					return true;
				}
				else
				{
					RED_LOG_WARNING( "GSM: Failed to parse value for initialization parameter '%hs' of type '%hs' from '%hs'",
						name, ( GetTypeObject< world::WorldID >() ? GetTypeObject< world::WorldID >()->GetName().AsChar() : "<null>" ), valueStr.AsChar() );
				}
			}

			return false;
		}

		void ParseInitialParameters( const red::CommandLine& commandline, ParamsView& outParams )
		{
			// expected format of session initial arguments
			/// -world=w2_nyc
			/// -startPosition={X=10,Y=20,Z=50}
			/// -startRotation={Yaw=100}
			/// -autojoin

			// only selected set of parameters is allowed
			const Bool hasGameDefinition = ParseCommandlineParam<String>( "gameDefinition", commandline, outParams );
			if ( !hasGameDefinition )
			{
				if ( commandline.HasOption( "world" ) && !ParseCommandlineParam<world::WorldID>( "world", commandline, outParams ) )
				{
					RED_FATAL( "Failed to parse world from commandline '%hs'. Make sure path is correct and also that the world has been compiled", commandline.Internal_GetRawCommandLine().AsChar() );
				}
				const Bool hasStartSpawnPoint = ParseCommandlineParam<String>("startSpawnPoint", commandline, outParams );
				if ( !hasStartSpawnPoint )
				{
					ParseCommandlineParam<Vector3>( "startPosition", commandline, outParams );
					ParseCommandlineParam<EulerAngles>( "startRotation", commandline, outParams );
				}

				ParseCommandlineParam<String>( "startQuest", commandline, outParams );
			}

			ParseCommandlineParam<String>( "playerGender", commandline, outParams );
			ParseCommandlineParam<Bool>( "alwaysSpawnedDisabled", commandline, outParams );
			ParseCommandlineParam<Bool>( "forceSpawnInView", commandline, outParams );
			ParseCommandlineParam<Float>( "entitySpawnRadius", commandline, outParams );
			ParseCommandlineParam<Bool>( "noCrowd", commandline, outParams );
			ParseCommandlineParam<Bool>( "noReducedCrowd", commandline, outParams );
			ParseCommandlineParam<Bool>( "ignoreSavedCensorFlags", commandline, outParams );

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
			if ( commandline.HasOption( "overrideLocalNavmesh" ) )
			{
				nav::AllowLocalDebugToOverrideGameData();
			}
#endif

			// autojoin parameter does not have any parameters
			if (commandline.HasOption("autojoin")) {
				outParams.Push< Bool >( RED_NAME_CONSTEXPR( "autojoin" ), true);
				ParseCommandlineParam<String>("server", commandline, outParams);
			}

			if ( commandline.HasOption( "mainmenu" ) )
				outParams.Push< Bool >( RED_NAME_CONSTEXPR( "mainmenu" ), true );

			String sessionTypeAsString;
			if ( commandline.GetFirstParam( "sessionType", sessionTypeAsString ) )
			{
				if ( sessionTypeAsString.EqualsNC( "LAN" ) )
				{
					outParams.Push< Bool >( RED_NAME_CONSTEXPR( "LAN" ), true );
				}
				else if ( sessionTypeAsString.EqualsNC( "Internet" ) )
				{
					outParams.Push< Bool >( RED_NAME_CONSTEXPR( "LAN" ), false );
				}
				else
				{
					RED_LOG_ERROR( "Invalid 'sessionType' parameter, value = '%s'", sessionTypeAsString.AsChar() );
				}
			}

			String playerRecordID;
			if ( commandline.GetFirstOption( "playerRecordID", playerRecordID ) )
				outParams.Push< CName >( RED_NAME_CONSTEXPR( "playerRecordID" ), RED_NAME( playerRecordID ) );

			String sessionLengthParam;
			if ( commandline.GetFirstOption( "sessionLength", sessionLengthParam ) )
			{
				net::Time sessionLength;
				sessionLength.SetFromSecs( (Float)red::StringToDouble( sessionLengthParam.AsChar() ) );
				outParams.Push< net::Time >( RED_NAME_CONSTEXPR( "sessionLength" ), sessionLength );
			}

			// other special states
			String initialState;
			if ( commandline.GetFirstOption( "state", initialState ) )
			{
				outParams.Push< String >( RED_NAME_CONSTEXPR( "state" ), initialState );
			}

			String saveToLoad;
			if ( commandline.GetFirstOption( "save", saveToLoad ) )
			{
				outParams.Push< String >( RED_NAME_CONSTEXPR( "save" ), saveToLoad );
			}

			if( commandline.HasOption( "forceShutdownAfterSessionLoaded" ) )
			{
				outParams.Push< Bool >( RED_NAME_CONSTEXPR( "forceShutdownAfterSessionLoaded" ), true );
			}

			if ( commandline.HasOption( "skipStartScreen" ) )
			{
				outParams.Push< Bool >( RED_NAME_CONSTEXPR( "skipStartScreen" ), true );
			}
		}

		void SetupInitialParameters( const InitParams& params, ParamsView& outParams )
		{
			outParams.Push< Bool >( RED_NAME_CONSTEXPR( "functionalTestsMode" ), params.m_functionalTestsEngine );
			outParams.Push< Bool >( RED_NAME_CONSTEXPR( "automator" ), params.m_automator );
			outParams.Push< Bool >( RED_NAME_CONSTEXPR( "headless" ), params.m_headless );
		}

	} // helper

	CGameFramework::CGameFramework( const red::CommandLine& commandline, const InitParams& params, const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
		: m_processor( red::CreateUniquePtr< SystemsUpdater >() )
		, m_gameInstance( red::CreateUniquePtr< game::Instance >() )
		, m_runtimeScene( red::CreateUniquePtr< world::RuntimeScene >() )
		, m_stateMachine( nullptr )
	{
		world::RuntimeMode runtimeMode{ red::MultiplayerSetup::IsClient() ? world::RuntimeMode::Multiplayer_Client : world::RuntimeMode::Singleplayer };

		job::Builder builder{ context };
		builder.DispatchJob( "CGameFramework/GameInstance", [this, runtimeMode, &keepRunning]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameFramework/GameInstance"};
			m_gameInstance->Initialize( { runtimeMode }, context, keepRunning );
		} );

		// TODO: Run it in parallel to game instance initialisation
		builder.DispatchJob( "CGameFramework/RuntimeScene", [this, runtimeMode, &keepRunning]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameFramework/RuntimeScene"};
			m_runtimeScene->Initialize( { runtimeMode }, context, keepRunning );
		} );

		builder.DispatchJob( "CGameFramework/StateMachine", [this, &commandline, params, &keepRunning]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameFramework/StateMachine"};
			// initial parameters for the state machine
			red::SharedPtr< Params, red::PoolEngine > paramStack( RED_NEW( Params, red::PoolEngine ) );
			ParamsView paramStackView( *paramStack, 0 );

			// set initial params passed from engine
			helper::SetupInitialParameters( params, paramStackView  );

			// parse initial parameters like start position, test area, etc
			helper::ParseInitialParameters( commandline, paramStackView );

			// create global state machine for the session
			m_stateMachine = StateMachine::Create( paramStackView, m_processor.Get(), m_gameInstance.Get(), m_runtimeScene.Get() );
			m_stateMachine->DispatchInitialization( context, keepRunning );
		} );
	}

	CGameFramework::~CGameFramework()
	{
		// close state machine
		RED_DELETE( m_stateMachine );
	}

	const Bool CGameFramework::ProcessFrame( const Frame& frame )
	{
		return m_stateMachine->ProcessFrame( frame );
	}

} // namespace gsm