#pragma once

#include "../../gameStateMachine/include/gameStateMachinePublic.h"

namespace game
{
	namespace stateMachine
	{
		class StateDefinition;
		class StateMachineListDefinition;
		class StateMachineDefinition;
		class TransitionDefinition;
		struct StateMachinesDefinitionsData;

		namespace validator
		{
			template<typename ResultError>
			struct Result
			{
				Result();
				Bool IsValid() const { return m_errorCode == 0u; }
				Bool IsError( const ResultError& checkError ) const { return m_errorCode & static_cast<Uint32>( checkError ); }
				void SetError( const ResultError error );

			private:
				Uint32 m_errorCode;
			};

			template<typename ResultError>
			struct ResultWithMessage : public Result<ResultError>
			{
				void AppendMessage( const red::String& message );
				void SetMessage( const red::String& message );

			private:
				red::String m_errorMessage;
			};

			enum class TransitionResultErrors
			{
				Valid = -1,
				InvalidEventFunction,
				InvalidConditionFunction,
				InvalidUnknown,

				Count
			};

			enum class StateResultErrors
			{
				Valid = -1,
				InvalidOnEnterFunction,
				InvalidOnExitFunction,
				InvalidOnUpdateFunction,
				InvalidOnForcedExitFunction,
				InvalidEnterConditionFunction,
				InvalidInternalEnterConditionFunction,
				InvalidExitConditionFunction,
				InvalidUnknown,

				Count
			};

			enum class StateMachineResultErrors
			{
				Valid = -1,
				InvalidTypeMismatch,
				InvalidUnknown,

				Count
			};

			enum class StateMachineListResultErrors
			{
				Valid = -1,
				InvalidFirstHasInput,
				InvalidLastHasOutput,
				InvalidIncompatibleAdjacent,
				InvalidIncompatibleHomologous,
				InvalidOptional,
				InvalidUnknown,

				Count
			};

			typedef Result<TransitionResultErrors> TransitionResult;
			typedef Result<StateResultErrors> StateResult;
			typedef ResultWithMessage<StateMachineResultErrors> StateMachineResult;
			typedef ResultWithMessage<StateMachineListResultErrors> StateMachineListResult;

			TransitionResult Validate( const TransitionDefinition& state, const StateMachineDefinition& stateMachine );		// checks the state internal validity rules
			StateResult Validate( const StateDefinition& state, const StateMachineDefinition& stateMachine );				// checks the state internal validity rules
			StateMachineResult Validate( const StateMachineDefinition& stateMachine );										// checks state machine internal validity rules
			StateMachineListResult Validate( const StateMachinesDefinitionsData& stateMachineDefinitionData );				// checks the validity rules between state machines
			
			template<typename ResultError>
			Result< ResultError >::Result()
				: m_errorCode( 0u )
			{

			}

			template<typename ResultError>
			void Result< ResultError >::SetError( const ResultError error )
			{
				Int32 errorInt = static_cast<Int32>( error );
				if ( errorInt >= 0 )
				{
					m_errorCode |= 1 << errorInt;
				}
			}

			template<typename ResultError>
			void ResultWithMessage< ResultError >::AppendMessage( const red::String& message )
			{
				m_errorMessage += message;
			}

			template<typename ResultError>
			void ResultWithMessage< ResultError >::SetMessage( const red::String& message )
			{
				m_errorMessage.Set( message );
			}
		};
	}
}