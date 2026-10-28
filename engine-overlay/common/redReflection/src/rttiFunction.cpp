#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
/**
* Copyright (c) 2007-20 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "rttiFunction.h"
#include "scriptOpcodes.h"
#include "scriptDebugger.h"
#include "scriptingSystem.h"
#include "scriptStackFrame.h"
#include "scriptable.h"
#include "scriptNativeFunctionMap.h"
#include "../../redContainers/include/dynArrayAccessor.h"
#include "rttiPointerTypesImpl.h"
#include "scriptLog.h"

using red::DynArray;

namespace
{
	const Uint32 c_maxReplicatedParamsNum = 10;
	const Uint32 c_maxDataSize = 1024;
}

namespace rtti
{
	namespace impl
	{
		// Utility: context that just wraps result pointer
		struct FunctionContext_ResultWrapper final : public FunctionContext
		{
			RED_INLINE FunctionContext_ResultWrapper( void* object, const rtti::ClassType* objectClass, void* result, const rtti::IType* resultType, void* userData )
				: FunctionContext( object, objectClass, userData )
				, m_result( result )
				, m_resultType( resultType )
			{}
			RED_INLINE FunctionContext_ResultWrapper( IScriptable* scriptable, void* result, const rtti::IType* resultType, const void* userData )
				: FunctionContext( scriptable, userData )
				, m_result( result )
				, m_resultType( resultType )
			{}

			virtual void* GetResultPtr() override { return m_result; }
			virtual const rtti::IType* GetResultType() const { return m_resultType; }

		private:
			void* m_result;
			const rtti::IType* m_resultType;
		};
	}

	IFunctionCollector::~IFunctionCollector()
	{}

	Function::Function( CName name, CName familyName, Uint32 flags )
		: m_name( name )
		, m_familyName( familyName )
		, m_returnProperty( nullptr )
		, m_repFunction( nullptr )
		, m_parameters( red::PoolRTTIProperty() )
		, m_localVars( red::PoolRTTIProperty() )
		, m_flags( flags )
		, m_size( 0 )
	{}

	Function::Function( Function&& other )
		: m_name( std::move( other.m_name ) )
		, m_familyName( std::move( other.m_familyName ) )
		, m_returnProperty( other.m_returnProperty )
		, m_repFunction( other.m_repFunction )
		, m_parameters( std::move( other.m_parameters ) )
		, m_localVars( std::move( other.m_localVars ) )
		, m_code( std::move( other.m_code ) )
		, m_flags( other.m_flags )
		, m_size( other.m_size )
	{
		other.m_returnProperty = nullptr;
		other.m_repFunction = nullptr;
	}

	Function::~Function()
	{
		RED_DELETE( m_returnProperty );
		red::alg::ClearPtr( m_localVars );
		red::alg::ClearPtr( m_parameters );
	}

	Uint64 Function::GetFunctionHash() const
	{
		Uint64 decoratedNameHash = m_familyName.GetHash();
		for ( const auto& param : m_parameters )
		{
			decoratedNameHash = red::CombineHashes64( decoratedNameHash, GetRttiSystem().NativeNameToScriptAlias( param->GetType()->GetName() ).GetHash() );
		}

		return decoratedNameHash;
	}

	//////////////////////////////////////////////////////////////////////////

	void Function::AddParameter( const CName name, const rtti::IType* type, Bool isOptional, Bool isSkipable, Bool isReference, const Property*& prop )
	{
		RED_ASSERT( type != nullptr );
		RED_ASSERT( FindProperty( name ) == nullptr );

		Uint64 flags = PF_FuncParam;
		if ( isOptional ) flags |= PF_FuncOptionaParam;
		if ( isSkipable ) flags |= PF_FuncSkipParam;
		if ( isReference ) flags |= PF_FuncOutParam;

		prop = RED_NEW( Property )( const_cast< const rtti::IType* >( type ), nullptr, m_size, name, flags );
		m_parameters.PushBack( prop );
	}

	void Function::AddLocal( const CName name, const rtti::IType* type, const Property*& prop  )
	{
		RED_ASSERT( type != nullptr );
		RED_ASSERT( FindProperty( name ) == nullptr );

		Uint64 flags = PF_FuncLocal;

		prop = RED_NEW( Property )( const_cast< const rtti::IType* >( type ), nullptr, 0, name, flags );
		m_localVars.PushBack( prop );
	}

	void Function::SetReturnType( const rtti::IType* retType )
	{
		RED_ASSERT( m_returnProperty == nullptr );
		RED_ASSERT( retType != nullptr );
		m_returnProperty = RED_NEW( Property )( const_cast< const rtti::IType* >( retType ), nullptr, 0, RED_NAME_CONSTEXPR( "__return" ), PF_FuncRetValue );
	}	

	const Property* Function::FindProperty( CName propName ) const
	{
		// Search in local first
		for ( Uint32 i=0; i<m_localVars.Size(); i++ )
		{
			const Property* prop = m_localVars[i];
			if ( prop->GetName() == propName )
			{
				return prop;
			}
		}

		// Search in params
		for ( Uint32 i=0; i<m_parameters.Size(); i++ )
		{
			const Property* prop = m_parameters[i];
			if ( prop->GetName() == propName )
			{
				return prop;
			}
		}

		// Not found
		return nullptr;
	}

	void Function::GetProperties( DynArray< const Property* >& properties ) const
	{
		// Start with the parameters
		properties.PushBack( m_parameters );

		// And the local variables
		properties.PushBack( m_localVars );
	}

	void Function::Reset()
	{
		// Clear properties
		red::alg::ClearPtr( m_parameters );
		red::alg::ClearPtr( m_localVars );

		// Remove return value
		if ( m_returnProperty )
		{
			RED_DELETE( m_returnProperty );
			m_returnProperty = nullptr;
		}
	}

	//////////////////////////////////////////////////////////////////////////

	Bool Function::ReplicatedCall( IScriptable* context, CScriptStackFrame& stackFrame ) const
	{
		// Capture input parameters

		void* newStack = RED_ALLOCA( m_size );
		InitializeParams( stackFrame, newStack, nullptr );

		// Set up input parameters

		FunctionParamData params[ c_maxReplicatedParamsNum ];
		const Uint32 numParams = m_parameters.Size();
		RED_ASSERT( numParams <= RED_ARRAY_COUNT_U32( params ) );
		for ( Uint32 i = 0; i < numParams; ++i )
		{
			params[ i ].m_type = m_parameters[ i ]->GetType();
			params[ i ].m_data = m_parameters[ i ]->GetOffsetPtr( newStack );
		}

		// Perform replicated call

		RED_FATAL( "TODO 2017-04-17: Maciej Sawitus: Need to pass some context around scripting virtual machine (think dedicated server with multiple sessions in it)" );
		//return ReplicatedCallGeneric( ReplicatedCallContext( context ), GetName(), params, numParams );
		
		DestroyParams( stackFrame, newStack, nullptr, nullptr );
		
		return false;
	}

	Bool Function::CallNative( FunctionContext& ctx, CScriptStackFrame& stackFrame ) const
	{
		// Get the function
		if ( IsStatic() )
		{
			const Uint32 nativeIndex = GetNativeFunctionIndex();
			TNativeGlobalFunc nativeFunction = CScriptNativeFunctionMap::GetGlobalNativeFunction( nativeIndex );
			if ( nativeFunction != nullptr )
			{
				// We need to pass scriptable context here, cause global/static function
				// may be an operator which calls member methods (which need a valid context).
				( *nativeFunction )( ctx.GetScriptableContext(), stackFrame, ctx.GetResultPtr(), ctx.GetResultType() );
				return true;
			}
			else
			{
				RED_LOG_WARNING( "Core: Trying to call nonexported global function %hs.", GetName().AsChar() );
				return false;
			}
		}
		else if ( IScriptable* scriptableContext = ctx.GetScriptableContext() )
		{
			const Uint32 nativeIndex = GetNativeFunctionIndex();
			TNativeFunc nativeFunction = CScriptNativeFunctionMap::GetClassNativeFunction( nativeIndex );
			if ( nativeFunction != nullptr )
			{
				// Call function in context of current object
				( scriptableContext->*nativeFunction )( stackFrame, ctx.GetResultPtr(), ctx.GetResultType() );
				return true;
			}
			else
			{
				RED_LOG_WARNING( "Core: Trying to call nonexported function %hs from C++ class %hs.", GetName().AsChar(), GetClass()->GetName().AsChar() );
				return false;
			}
		}
		else // use raw object, its class and native function caller
		{
			auto nativeFunctionCaller = GetNativeFunctionCaller();
			RED_FATAL_ASSERT( nativeFunctionCaller != nullptr, "Native function with no scriptable context needs to have native function caller specified." );
			RED_FATAL_ASSERT( ctx.ValidateNativeContext( this ), "Function has no context specified or context class does not match function class" );
			nativeFunctionCaller->Call( ctx.GetContext(), stackFrame, ctx.GetResultPtr(), ctx.GetResultType() );
			return true;
		}
	}

#ifdef __EMSCRIPTEN__
	// CP2077 web [flash] script profiler (?scriptProf=1): self/inclusive wall time per scripted function, all threads.
	// Every ~4 s one thread prints the top 30 by self time and resets. Off by default (one relaxed load per call).
	namespace cpScriptProf
	{
		struct Stat { double self = 0, incl = 0; unsigned calls = 0; };
		std::atomic< int > s_enabled{ -1 };
		std::mutex s_mutex;
		std::unordered_map< const Function*, Stat > s_global;
		double s_lastDump = 0;
		thread_local std::unordered_map< const Function*, Stat >* t_local = nullptr;
		thread_local double t_child = 0, t_lastFlush = 0;
		bool Enabled()
		{
			int v = s_enabled.load( std::memory_order_relaxed );
			if ( v < 0 )
			{
				v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'scriptProf' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
				s_enabled.store( v );
				if ( v ) std::fputs( "CP2077 script profiler enabled (?scriptProf=1)\n", stderr );
			}
			return v > 0;
		}
		void Flush( double now )
		{
			std::vector< std::pair< const Function*, Stat > > top;
			{
				std::lock_guard< std::mutex > lock( s_mutex );
				for ( auto& kv : *t_local ) { auto& g = s_global[ kv.first ]; g.self += kv.second.self; g.incl += kv.second.incl; g.calls += kv.second.calls; }
				t_local->clear();
				if ( s_lastDump == 0 ) s_lastDump = now;
				if ( now - s_lastDump < 4000 ) return;
				const double span = now - s_lastDump;
				s_lastDump = now;
				top.assign( s_global.begin(), s_global.end() );
				s_global.clear();
				std::sort( top.begin(), top.end(), []( const auto& a, const auto& b ) { return a.second.self > b.second.self; } );
				if ( top.size() > 30 ) top.resize( 30 );
				double total = 0; for ( auto& kv : top ) total += kv.second.self;
				std::fprintf( stderr, "CP2077 script profile: window=%.0fms top30self=%.1fms\n", span, total );
				for ( auto& kv : top )
				{
					const Function* fn = kv.first;
					const char* cls = fn->GetClass() ? fn->GetClass()->GetName().AsChar() : "-";
					std::fprintf( stderr, "CP2077 script profile: self=%.1fms incl=%.1fms calls=%u %s::%s\n", kv.second.self, kv.second.incl, kv.second.calls, cls, fn->GetName().AsChar() );
				}
			}
		}
		struct Scope
		{
			const Function* fn = nullptr; double start = 0, savedChild = 0;
			explicit Scope( const Function* f )
			{
				if ( !Enabled() ) return;
				fn = f; savedChild = t_child; t_child = 0; start = emscripten_get_now();
			}
			~Scope()
			{
				if ( !fn ) return;
				const double now = emscripten_get_now();
				const double dur = now - start;
				if ( !t_local ) t_local = new std::unordered_map< const Function*, Stat >();
				Stat& st = ( *t_local )[ fn ];
				st.self += dur - t_child; st.incl += dur; ++st.calls;
				t_child = savedChild + dur;
				if ( now - t_lastFlush > 500 ) { t_lastFlush = now; Flush( now ); }
			}
		};
	}
#endif

	Bool Function::CallScripted( CScriptStackFrame& frame, void* result, const rtti::IType* resultType ) const
	{
		// shortcut.. apparently there are functions that have no code
		const Uint8* codeEnd = m_code.GetCodeEnd();
		if ( codeEnd == nullptr )
		{
			return true;
		}
#ifdef __EMSCRIPTEN__
		cpScriptProf::Scope cpScriptProfScope( this );
#endif

		// Call constructors for local parameters
		for ( Uint32 i = 0, count = m_localVars.Size(); i < count; i++ )
		{
			const Property* prop = m_localVars[ i ];
			void* propData = prop->GetOffsetPtr( frame.m_locals );
			const IType * propType = prop->GetType();
			prop->GetType()->Construct( propData );
			if( propType->GetType() == RT_Array )
			{
				red::DynArrayAccessor & array = red::DynArrayAccessor::GetRef( propData );
				array.SetPool( red::PoolScript() );
			}
		}

		IScriptable* context = frame.GetContext();

#ifndef NO_SCRIPT_DEBUG
		// Since we have a newly created CScriptStackFrame, we need to initialise it's debug data
		// (or the debugger will go to the last line of the file with the default value of -1)
		// Currently we don't know the span that represents the function entry line, so 0 will have to suffice
		frame.m_debugData = CScriptStackFrame::DebugData( 0, m_code.GetSourceLine(), 0, 0 );
		IScriptingSystem::GetInstance().DebugBreakpoint( script::BreakpointType::FunctionEntry, context, frame );
#endif

		RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_PUSH_CALLSTACK( context, this );

		// Execute script code
		while ( frame.m_code < codeEnd )
		{
			// Process return opcode
			if ( *frame.m_code == OP_Return )
			{
				frame.Step( context, result, resultType );
				break;
			}

			// Normal opcode
			frame.Step( context, nullptr, nullptr );
		}

		RED_SCRIPTABLE_THREAD_SAFETY_MONITOR_POP_CALLSTACK();

#ifndef NO_SCRIPT_DEBUG

		// We've exited function while stepping over expression, make sure we enter first breakpoint in parent scope
		if ( frame.GetParent() != nullptr )
		{
			IScriptingSystem::GetInstance().DebugBreakpoint( script::BreakpointType::FunctionExit, context, *frame.GetParent() );
		}
#endif

		// Cleanup local parameters
		void* resultRef = nullptr;
		if ( result && resultType  && resultType->GetType() == ERTTITypeType::RT_ScriptReference )
		{
			auto* ref = static_cast< rtti::ScriptedReferenceType* >( result );
			resultRef = ref->GetReferencedObject();
		}

		for ( Uint32 i = 0; i < m_localVars.Size(); i++ )
		{
			const Property* prop = m_localVars[ i ];
			const auto* propType = prop->GetType();
			if( propType->NeedsCleaning() )
			{
				void* propData = prop->GetOffsetPtr( frame.m_locals );

				if( frame.m_lValuePtr && frame.m_lValuePtr == propData )
				{
					if(frame.m_canElideCopy)
					{
						const auto* propType = prop->GetType();
						propType->Copy( result, propData );
					}

					frame.m_lValuePtr = nullptr;
					frame.m_lValueType = nullptr;
				}

				if( result && propData == resultRef )
				{
					SCRIPT_WARNING( frame, "Function %s returns reference to local variable %s", GetName().AsChar(), prop->GetName().AsChar() );
				}

				prop->GetType()->Destruct( propData );
			}
			
		}

		// Called
		return true;
	}
	
	// TODO??? move to a separate class and make it friend OR
	// make friend to OpFinalFunc and OpVirtualFunc
	Bool Function::Call_ScriptsSystem( IScriptable* context, CScriptStackFrame& stackFrame, void* result, const rtti::IType* resultType ) const
	{
        const bool cp2077TraceCall=stackFrame.m_function && std::strstr(stackFrame.m_function->GetName().AsChar(),"OnVehicleSystemAttach");
        if(cp2077TraceCall)std::fprintf(stderr,"CP2077 script call probe: phase=entered callee=%p context=%p result=%p\n",static_cast<const void*>(this),static_cast<void*>(context),result);

		// restore code below in the #CPO project
		// for now we don't need to check it because in SP all functions are not replicable
		/*
		if ( IsReplicable() )
		{
			return ReplicatedCall( context, stackFrame );
		}
		*/

		// Call to native function via virtual call
		if ( IsNative() )
		{
			if(cp2077TraceCall)std::fputs("CP2077 script call probe: phase=native-wrapper\n",stderr);
			impl::FunctionContext_ResultWrapper ctx( context, result, resultType, stackFrame.m_userData );
			return CallNative( ctx, stackFrame );
		}

		// Allocate stack space
		if(cp2077TraceCall)std::fprintf(stderr,"CP2077 script call probe: phase=scripted-stack size=%u\n",unsigned(m_size));
		void* newStack = RED_ALLOCA( m_size );

		// Capture values for input parameters
		OutputParams outputParams;
		if(cp2077TraceCall)std::fprintf(stderr,"CP2077 script call probe: phase=before-params stack=%p\n",newStack);
		InitializeParams( stackFrame, newStack, &outputParams );
		if(cp2077TraceCall)std::fputs("CP2077 script call probe: phase=after-params\n",stderr);

		CScriptStackFrame frame( &stackFrame, context, this, newStack, newStack, nullptr );
		const Bool res = CallScripted( frame, result, resultType );

		// Copy output parameters
		for ( const auto& param : outputParams )
			param.m_type->Move( param.m_dataOffset, param.m_localOffset );

		DestroyParams( stackFrame, newStack, result, resultType );

		return res;
	}

	Bool Function::Call( ScriptedFunctionContext& ctx, CScriptStackFrame* stackFrame ) const
	{
		RED_FATAL_ASSERT( !IsNative(), "Only scripted methods can be called this way." );

	#ifndef NO_SCRIPT_DEBUG 
		IScriptingSystem::GetInstance().ThreadStart();
	#endif

		// Allocate stack space and initialize params (if needed)
		void* newStack = RED_ALLOCA( m_size );
		const ScriptedFunctionContext::Setup setup = ctx.InitializeParamsAndResult( this, newStack );

		CScriptStackFrame frame( stackFrame, ctx.GetScriptableContext(), this, setup.m_locals, setup.m_params, ctx.m_userData );
		const Bool res = CallScripted( frame, ctx.GetResultPtr(), ctx.GetResultType() );
		ctx.OnCalled( res );

		if ( setup.m_destroyParams )
		{
			DestroyParams( frame, setup.m_params, ctx.GetResultPtr(), ctx.GetResultType() );
		}

	#ifndef NO_SCRIPT_DEBUG 
		IScriptingSystem::GetInstance().ThreadEnd();
	#endif
		return res;
	}

	Bool Function::Call( ScriptedOrNativeFunctionContext& ctx ) const
	{
		if ( IsNative() )
		{
			// Build the fake opcodes that pass the parameters through
			Uint8 code[ ScriptedOrNativeFunctionContext::c_maxCodeSize ];
			ctx.CreateCodeForParams( code );

			// Create a script function stack for the caller function
			CScriptStackFrame stackFrame( ctx.GetScriptableContext(), code, ctx.m_userData );
			stackFrame.m_repContext = ctx.m_replicationContext;

			return CallNative( ctx, stackFrame );			
		}

		return Call( ctx.AsScriptedFunctionContext() );
	}

	void Function::CalcDataLayout()
	{
		// Get all function properties
		DynArray< const Property* > properties{ red::PoolRTTIProperty() };
		GetProperties( properties );

		// Calculate the layout
		m_size = Property::CalcDataLayout( properties, 0 );
	}

	void Function::InitializeParams( CScriptStackFrame& parentStackFrame, void* stack, OutputParams* outputParams ) const
	{
		red::Memset( stack, 0, m_size );

		// Evaluate function params (in previous context)
		for( const Property* const prop : m_parameters )
		{
			// Evaluate function input param
			const IType* const propType = prop->GetType();
			void* const propData = red::OffsetPtr( stack, prop->GetDataOffset() );

			propType->Construct( propData );
			if( propType->GetType() == RT_Array )
			{
				red::DynArrayAccessor & array = red::DynArrayAccessor::GetRef( propData );
				array.SetPool( red::PoolScript() );
			}
		}

		for( const Property* const prop : m_parameters )
		{
			const IType* const propType = prop->GetType();
			const Uint64 propFlags = prop->GetFlags();
			void* const propData = red::OffsetPtr( stack, prop->GetDataOffset() );

			parentStackFrame.Step( parentStackFrame.GetContext(), propData, propType );

			// Remember output params
			if( ( propFlags & PF_FuncOutParam ) && outputParams != nullptr && !outputParams->Full() )
			{
				OutputParam& param = outputParams->EmplaceBack();
				param.m_dataOffset = parentStackFrame.m_lValuePtr;
				param.m_localOffset = propData;
				param.m_type = propType;
			}
		}

		// Here we should found a "params_end" opcode
		RED_ASSERT( *parentStackFrame.m_code == OP_ParamEnd );
		parentStackFrame.m_code++;
	}

	void Function::DestroyParams( CScriptStackFrame& frame, void* stack, void* result, const rtti::IType* /*resultType*/ ) const
	{
		// ctremblay: I remove cache of param to destroy. If it is a perf issue, it can be optimized. Let me know.

		// Cleanup the function after parameters
		for ( Uint32 i = 0; i < m_parameters.Size(); ++i )
		{
			const Property* prop = m_parameters[ i ];
			const auto* propType = prop->GetType();
			if( propType->NeedsCleaning() )
			{
				void* propData = prop->GetOffsetPtr( stack );

				if( frame.m_lValuePtr && frame.m_lValuePtr == propData )
				{
					if( frame.m_canElideCopy )
					{

						propType->Copy( result, propData );
					}

					frame.m_lValuePtr = nullptr;
					frame.m_lValueType = nullptr;
				}

				prop->GetType()->Destruct( propData );
			}
		}
	}

	NativeMemberFunction::NativeMemberFunction( const rtti::ClassType* parentClass, CName name, CName familyName, TNativeFunc func, Uint32 flags )
		: Function( name, familyName, flags | FF_NativeFunction )
		, m_class( parentClass )
	{
		// Map native function to index
		m_nativeFunction = CScriptNativeFunctionMap::RegisterClassNative( func );
		RED_ASSERT( m_nativeFunction );
	}

	NativeMemberFunction::NativeMemberFunction( const rtti::ClassType* parentClass, CName name, CName familyName, TNativeGlobalFunc func, Uint32 flags )
		: Function( name, familyName, flags | FF_NativeFunction | FF_StaticFunction )
		, m_class( parentClass )
	{
		// Map native function to index
		m_nativeFunction = CScriptNativeFunctionMap::RegisterGlobalNative( func );
		RED_ASSERT( m_nativeFunction );
	}

	NativeMemberFunction::NativeMemberFunction( NativeMemberFunction&& other )
		: Function( std::move( other ) )
		, m_class( other.m_class )
		, m_nativeFunction( other.m_nativeFunction )
	{
		other.m_class = nullptr;
	}

	NativeGlobalFunction::NativeGlobalFunction( CName name, CName familyName, TNativeGlobalFunc func )
		: Function( name, familyName, FF_NativeFunction | FF_StaticFunction )
	{
		// Map native function to index
		m_nativeFunction = CScriptNativeFunctionMap::RegisterGlobalNative( func );
		RED_ASSERT( m_nativeFunction );
	}

	NativeGlobalFunction::NativeGlobalFunction( NativeGlobalFunction&& other )
		: Function( std::move( other ) )
		, m_nativeFunction( other.m_nativeFunction )
	{
	}

	ScriptedMemberFunction::ScriptedMemberFunction( const rtti::ClassType* parentClass, CName name, CName familyName, Uint32 flags )
		: Function( name, familyName, flags )
		, m_class( parentClass )
	{}

	ScriptedMemberFunction::ScriptedMemberFunction( ScriptedMemberFunction&& other )
		: Function( std::move( other ) )
		, m_class( other.m_class )
	{
		other.m_class = nullptr;
	}

} // rtti