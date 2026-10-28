/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "interopInterpreter.h"

#include "../../redReflection/include/standardTextWriter.h"

namespace Config
{
	TConfigVar< Bool >	cvInteropCacheEnabled( "Interop/Cache", "Enabled", true );
	TConfigVar< Int32 > cvInteropCacheInitPathLength( "Interop/Cache", "InitPathLength", 2048, 256, 32768, eConsoleVarFlag_Save );
	TConfigVar< Int32 > cvInteropCachePathIncrementPad( "Interop/Cache", "PathIncrementPad", 64, 0, 256, eConsoleVarFlag_Save );
}

namespace interop
{
	namespace prv
	{
		/// shitty path parser, hidden
		class PathParser
		{
		public:
			RED_FORCE_INLINE PathParser( const AnsiChar* txt )
				: m_start( txt )
				, m_end( txt + red::Strlen(txt) )
				, m_cur( txt )
			{}

			RED_FORCE_INLINE bool IsFinished()
			{
				SkipWhitespaces();
				return *m_cur == 0;
			}

			RED_FORCE_INLINE bool IsElementAccess() // array
			{
				SkipWhitespaces();
				return *m_cur == '[';
			}

			RED_FORCE_INLINE bool IsElementAccessEnd() // array
			{
				SkipWhitespaces();
				return *m_cur == ']';
			}

			RED_FORCE_INLINE bool IsMemberAccess() // .
			{
				SkipWhitespaces();
				return *m_cur == '.';
			}

			RED_FORCE_INLINE bool IsFunctionCall()
			{
				SkipWhitespaces();
				return *m_cur == '(';
			}

			RED_FORCE_INLINE bool IsFunctionCallEnd()
			{
				SkipWhitespaces();
				return *m_cur == ')';
			}

			RED_FORCE_INLINE bool IsAssignment()
			{
				SkipWhitespaces();
				return *m_cur == '=';
			}

			RED_FORCE_INLINE bool IsSeparator()
			{
				SkipWhitespaces();
				return *m_cur == ',';
			}

			RED_FORCE_INLINE void EatChar()
			{
				SkipWhitespaces();
				if ( *m_cur != 0)
					m_cur += 1;
			}

			RED_FORCE_INLINE const AnsiChar* GetStartOfPathAsChar() const
			{
				return m_start;
			}

			RED_FORCE_INLINE const AnsiChar* GetRestOfPathAsChar() const
			{
				return m_cur;
			}

			// Used by cache to advance the parse position
			RED_FORCE_INLINE void JumpToPosition( Uint32 pos )
			{
				// Can jump one past end of string, so will be finished any parsing
				RED_FATAL_ASSERT( pos <= m_end - m_start );
				m_cur = m_start + pos;
			}

			// TODO: remove the memory allocation
			RED_FORCE_INLINE InteropString ExtractValue(const Bool allowEscaped)
			{
				String ret;

				SkipWhitespaces();

				if ( *m_cur == '\"')
				{
					if (allowEscaped)
					{
						++m_cur;

						while (m_cur < m_end)
						{
							if (*m_cur == '\\')
							{
								++m_cur;

								if (m_cur < m_end)
								{
									switch (*m_cur++)
									{
									case 'n': ret += "\n"; break;
									case 't': ret += "\t"; break;
									case '\"': ret += "\""; break;
									case '\'': ret += "\'"; break;
									case '\\': ret += "\\"; break;
									default: break;
									}
								}
							}
							else if (*m_cur == '\"')
							{
								++m_cur;
								break;
							}
							else
							{
								ret += *m_cur++;
							}
						}
					}
					else
					{
						ret += "\"";
						++m_cur;

						while (m_cur < m_end)
						{
							if (*m_cur == '\\')
							{
								++m_cur;

								if (m_cur < m_end)
								{
									switch (*m_cur++)
									{
									case 'n': ret += "\\n"; break;
									case 't': ret += "\\t"; break;
									case '\"': ret += "\\\""; break;
									case '\'': ret += "\\\'"; break;
									case '\\': ret += "\\\\"; break;
									default: break;
									}
								}
							}
							else if (*m_cur == '\"')
							{
								ret += "\"";
								++m_cur;
								break;
							}
							else
							{
								ret += *m_cur++;
							}
						}
					}
				}
				else
				{
					Uint32 stackCount = 0;
					while ( m_cur < m_end )
					{
						if (*m_cur == '{' || *m_cur == '[' || *m_cur == '(')
							stackCount += 1;

						if (*m_cur == '}' || *m_cur == ']' || *m_cur == ')')
						{
							if (stackCount == 0)
								break;
							stackCount -= 1;
						}

						if (*m_cur == ',' && stackCount == 0)
							break;

						ret += *m_cur;
						++m_cur;
					}
				}

				return ret;
			}

			// TODO: remove the memory allocation
			RED_FORCE_INLINE InteropString ExtractIdentifier()
			{
				String ret;

				SkipWhitespaces();

				if ( *m_cur == '\"' )
				{
					++m_cur;

					while ( m_cur < m_end )
					{
						if ( *m_cur == '\"' )
						{
							++m_cur;
							break;
						}

						ret += *m_cur;
						++m_cur;
					}
				}
				else
				{
					const char* startChar = m_cur;
					while ( m_cur < m_end )
					{
						if ( !IsValidIdentChar( *m_cur ) )
							break;

						ret += *m_cur;
						++m_cur;
					}
				}

				return ret;
			}

		private:
			const AnsiChar*		m_start;
			const AnsiChar*		m_cur;
			const AnsiChar*		m_end;

			void SkipWhitespaces()
			{
				while ( m_cur < m_end && *m_cur <= ' ')
					++m_cur;				
			}

			static Bool IsValidIdentChar( const AnsiChar ch )
			{
				if ( ch == '_' || ch == ':' || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ) return true;
				if ( ch >= '0' && ch <= '9' ) return true;
				return false;
			}
		};

		// Stack of levels for depth-first array access with backtracking. 
		// Being path based can of course lead to redundantly cached objects, but good enough for now.
		// The intended pattern is depth first access something like this.
		// objA[0].objB[0].a, objA[0].objB[0].b, objA[0].objB[1].a, objA[0].objB[1].b,
		// objA[1].objB[0].a, objA[1].objB[0].b, objA[1].objB[1].a, objA[1].objB[1].b.
		//
		// The cache is cleared on interop assignment or functor call, as the side effects can't ascertained
		// but should help speed up reads
		class InteropCache : public IInteropCache
		{
		public:
			InteropCache()
				: m_nextCacheString( 0 )
				, m_cacheStack{ red::PoolInterop() }
				, m_cacheString{ red::PoolInterop() }
			{
				m_cacheString.Resize( Config::cvInteropCacheInitPathLength.Get() ); // Alloc string memory upfront
				m_cacheStack.Reserve( 32 ); // Reserve cache depth
			}

			Bool IsEmpty() const
			{
				return m_cacheStack.Empty();
			}

			void Clear()
			{
				m_cacheStack.Clear();
				m_nextCacheString = 0;
				m_cacheString[ m_nextCacheString ] = '\0';
			}

			Bool RestoreRootFromCache( PathParser& parser, ObjectPtr& outPtr )
			{
				if (!Config::cvInteropCacheEnabled.Get())
				{
					return false;
				}

				// Try to use as much of the cached objects as possible along the interop path
				Uint32 offset = 0;
				Uint32 numMatches = 0;
				for ( Int32 i = 0, len = m_cacheStack.Size(); i < len; ++i )
				{
					const Uint32 pos = m_cacheStack[i].m_substrOffset;
					const Uint32 pathLen = m_cacheStack[i].m_substrLength;
					const char* const path = &m_cacheString[ pos ];
					if ( red::Strcmp( parser.GetRestOfPathAsChar() + offset, path, pathLen ) != 0 )
					{
						break;
					}
					++numMatches;
					offset += pathLen;
				}

				// Clear the cache that doesn't correspond to the new interop path
				const Uint32 numMismatches = m_cacheStack.Size() - numMatches;
				for ( Uint32 i = 0; i < numMismatches; ++i )
				{
					m_cacheStack.PopBack();				
				}

				if ( m_cacheStack.Empty() )
				{
					m_nextCacheString = 0;
					m_cacheString[ 0 ] = '\0';
					return false;
				}

				const auto& cacheInfo = m_cacheStack[ numMatches - 1 ];

				// Fast-forward parsing the interop path past the cached path
				const Uint32 pathLen = cacheInfo.m_substrOffset + cacheInfo.m_substrLength;
				m_cacheString[ pathLen ] = '\0';
				m_nextCacheString = pathLen;
				parser.JumpToPosition( pathLen );
				outPtr = cacheInfo.m_object;
				return true;
			}

			// Precondition, restore fromcache was called at the beginning, so all the previous elements (if any) match
			void CacheNextLevel( const PathParser& parser, const ObjectPtr& ptr )
			{
				if (!Config::cvInteropCacheEnabled.Get())
				{
					return;
				}

				// Get the current path substring, excluding the part that's in an earlier cache level
				// E.g., .a.b[0].c[1] could be split as .a.b[0] .c[1]
				// For now be lame and assume no whitespace differences.
				const Uint32 startOffset = m_cacheStack.Empty() ? 0 : m_cacheStack.Back().m_substrOffset + m_cacheStack.Back().m_substrLength;
				const char* const start = parser.GetStartOfPathAsChar() + startOffset;
				const char* const end = parser.GetRestOfPathAsChar();
				const Uint32 len = static_cast< Uint32 >( end - start );

				m_cacheStack.PushBack(CacheInfo());
				auto& info = m_cacheStack.Back();
				{
					info.m_object = ptr;
					info.m_substrOffset = PushString( start, len );
					info.m_substrLength = len;
				}
			}

		private:
			// Add a string to the cache, return the starting offset
			Uint32 PushString( const char* str, Uint32 len )
			{
				const Uint32 startStartPos = m_nextCacheString;
				if ( m_nextCacheString + len + 1 /*terminating null*/ > m_cacheString.Size() )
				{
					const Uint32 diff = m_nextCacheString + len + 1 - m_cacheString.Size();
					
					// Should rarely need to grow, so don't realloc for amortized performance
					m_cacheString.Grow( diff + Config::cvInteropCachePathIncrementPad.Get() );
				}
				red::Memcpy( &m_cacheString[ m_nextCacheString ], str, len );
				m_nextCacheString += len;
				m_cacheString[ m_nextCacheString ] = '\0';
				return startStartPos;
			}

			struct CacheInfo
			{
				Uint32			m_substrOffset;		// offset in cacheString
				Uint32			m_substrLength;		// length in cacheString
				ObjectPtr		m_object;			// cached object
			};

			red::DynArray<CacheInfo>	m_cacheStack;
			red::DynArray< char >		m_cacheString; // cached interop path buffer; null terminating paths purely for debug view, since substr length known
			Uint32						m_nextCacheString;
		};

		/// The interpreter
		class Interpreter : public IContext
		{
		public:
			explicit Interpreter( InteropCache& cache )
				: m_cache( cache )
			{
			}

			const Bool Execute( const ObjectPtr& root, const InteropString& txt, InteropResult& result )
			{
				// exceute the code
				PathParser parser( txt.AsChar() );

				ObjectPtr cachedRoot = root;
				m_cache.RestoreRootFromCache( parser, cachedRoot );
				if ( !Execute( cachedRoot, parser, result, false /*isConst*/ ) )
					ThrowException( "Internal operation failed" );

				// emit exception
				if ( HasErrors() )
				{
					result.m_children.Clear();
					result.m_data = m_exception;
					result.m_metaType = IBaseObject::MetaType::Exception;
					result.m_type = InteropType( "<exception>" );
					return false;
				}

				// Is never empty if called SaveCallstack()
				if (!m_callstack.Empty())
				{
					result.m_children.Clear();
					result.m_data = m_callstack;
					result.m_metaType = IBaseObject::MetaType::Callstack;
					result.m_type = InteropType("<callstack>");
					return true;
				}

				return true;
			}

		private:
			void SaveCallstack( InteropString&& callstack )
			{
				m_callstack = std::move(callstack);
				if (m_callstack.Empty())
				{
					// Ensure not empty, so we can be sure it was requested
					m_callstack = "<callstack missing>";
				}
			}

			virtual const Bool ThrowException( STATIC_CHECK_PRINTF_MSC const AnsiChar* txt, ... ) override final
			{
				// capture the first exception only
				if ( HasErrors() )
					return false; // always returns false

				AnsiChar buf[ 2048 ];

				va_list args;
				va_start( args, txt );
				red::VSNPrintF( buf, RED_ARRAY_COUNT_U32(buf), txt, args );
				va_end( args );

				String exceptionString( buf );
				if ( !buf[0] || !text::SaveToText( &exceptionString, GetTypeObject<String>(), m_exception ) )
				{
					m_exception = "\"Unknown exception\"";
				}
				return false; // always returns false
			}

			virtual const Bool HasErrors() const override final
			{
				return !m_exception.Empty();
			}

			Bool Execute( const ObjectPtr& obj, PathParser& path, InteropResult& result, const Bool isConst )
			{
				// exception encountered, prevent recursion
				if ( HasErrors() )
					return false;

				// invalid object reached
				if ( !obj )
					return ThrowException( "Trying to access NULL object" );

				// end of path
				if ( path.IsFinished() )
					return ExtractValue( obj.Get(), result, isConst );

				// assignment
				if ( path.IsAssignment() )
				{
					// Invalidate all cached objects
					m_cache.Clear();

					if ( isConst )
						return ThrowException("Unable to set value on read only object");

					path.EatChar(); // '='
					const InteropString value = path.ExtractValue(false);
					return SetValue( obj.Get(), value, result );
				}

				// exploration			
				switch ( obj->GetMetaType() )
				{
					case IBaseObject::MetaType::Value:
					{
						InteropString ident;
						if ( path.IsMemberAccess() )
						{
							path.EatChar(); // consume '.'
							ident = path.ExtractIdentifier();
						}

						IValueObject* valObj = static_cast<IValueObject*>( obj.Get() );

						// we should have a valid identifier
						if ( ident.Empty() )
							return ThrowException( "Invalid operation on value object" );

						if (ident == "__callstack")
						{
							if (!path.IsFinished())
							{
								return ThrowException("__callstack must be final in path, path '%hs'", path.GetRestOfPathAsChar());
							}
							SaveCallstack(obj->GetCallstack());
							return true;
						}

						auto childPtr = obj->GetUnlistedChild( *this, ident.AsChar() );
						if ( HasErrors() )
							return false;

						if ( !childPtr )
							return ThrowException( "Child '%hs' not found in '%hs'", ident.AsChar(), obj->GetType().AsChar() );

						// recurse
						return Execute( childPtr, path, result, isConst | valObj->IsReadOnly() );
					}

					case IBaseObject::MetaType::Compound:
					{
						InteropString ident;
						Bool isElementAccess = false;
						if ( path.IsMemberAccess() )
						{
							path.EatChar(); // consume '.'
							ident = path.ExtractIdentifier();
						}
						else if ( path.IsElementAccess() )
						{
							isElementAccess = true;
							path.EatChar(); // consume '['
							ident = path.ExtractIdentifier();

							// check that the expected ']' is there
							if ( !path.IsElementAccessEnd() )
								return ThrowException( "Missing ']'" );

							path.EatChar(); // consume ']'
						}

						// we should have a valid identifier
						if ( ident.Empty() )
							return ThrowException( "Invalid operation on compound object, type '%hs', path '%hs'", obj->GetType().AsChar(), path.GetRestOfPathAsChar() );

						if (ident == "__callstack")
						{
							if (!path.IsFinished())
							{
								return ThrowException("__callstack must be final in path, path '%hs'", path.GetRestOfPathAsChar());
							}
							SaveCallstack(obj->GetCallstack());
							return true;
						}

						// get the child
						auto* compoundObject = static_cast<ICompoundObject*>( obj.Get() );
						auto childPtr = compoundObject->GetChild( *this, RED_NAME( ident ) );
						if (HasErrors())
							return false;					

						// get the UNLISTED child
						if ( !childPtr )
						{
							childPtr = compoundObject->GetUnlistedChild( *this, ident.AsChar() );
							if (HasErrors())
								return false;						
						}

						// child not found
						if ( !childPtr )
							return ThrowException( "Child '%hs' not found in '%hs'", ident.AsChar(), obj->GetType().AsChar() );

						if ( isElementAccess )
						{
							m_cache.CacheNextLevel( path, childPtr );
						}

						// recurse
						return Execute( childPtr, path, result, isConst | compoundObject->IsReadOnly() );
					}

					case IBaseObject::MetaType::Array:
					{
						if ( path.IsElementAccess() )
						{
							path.EatChar();// '[', hacked
							InteropString ident = path.ExtractValue(true);
							path.EatChar(); // ']', hacked

							if ( ident.Empty() )
								return ThrowException( "Array index must be specified in the [] operator" );

							Uint32 index = atoi( ident.AsChar() );

							// validate index
							auto* arrayObject = static_cast< IArrayObject* >( obj.Get() );
							const auto arraySize = arrayObject->GetSize( *this );
							if ( HasErrors() )
								return false;

							if ( index >= arraySize )
								return ThrowException( "Out of bounds array access (%d > %d)", index, arraySize );

							// get the child (still can return bullcrap)
							auto childPtr = arrayObject->GetElement( *this, index );

							// recurse
							return Execute( childPtr, path, result, isConst );
						}
						else if ( path.IsMemberAccess() )
						{
							path.EatChar(); // consume '.'
							InteropString ident = path.ExtractIdentifier();

							if ( ident.Empty() )
								return ThrowException( "Array function must be specified after the ." );

							if (ident == "__callstack")
							{
								if (!path.IsFinished())
								{
									return ThrowException("__callstack must be final in path, path '%hs'", path.GetRestOfPathAsChar());
								}
								SaveCallstack(obj->GetCallstack());
								return true;
							}

							// get the UNLISTED child
							auto* arrayObject = static_cast< IArrayObject* >( obj.Get() );
							auto childPtr = arrayObject->GetUnlistedChild( *this, ident.AsChar() );
							if ( !childPtr )
								return ThrowException( "Function '%hs' not found in array", ident.AsChar() );

							// recurse
							return Execute( childPtr, path, result, isConst );

						}
						else
						{
							return ThrowException( "Invalid operation on array object" );

						}
					}

					case IBaseObject::MetaType::Functor:
					{
						if (path.IsMemberAccess())
						{
							path.EatChar(); // consume '.'
							InteropString ident = path.ExtractIdentifier();
							if (ident == "__callstack")
							{
								if (!path.IsFinished())
								{
									return ThrowException("__callstack must be final in path, path '%hs'", path.GetRestOfPathAsChar());
								}
								SaveCallstack(obj->GetCallstack());
								return true;
							}

							m_cache.Clear();
							return ThrowException("Invalid operation on functor object");
						}

						// Invalidate all cached objects - unknown side effects from functors.
						m_cache.Clear();

						// if this is not the end we expect the "("
						if ( !path.IsFunctionCall() )
							return ThrowException( "Invalid operation on functor object" );

						auto* functorObject = static_cast<IFunctorObject*>( obj.Get() );

						// we are not allowed to call functors on read only data
						// NOTE: we may add a "const" functor types that would allow this
						if ( isConst && !functorObject->IsConst() )
							return ThrowException( "Calling functor on read only data is not allowed" );

						// eat the '('
						path.EatChar();

						// parse argument
						red::DynArray< InteropString > args{ red::PoolInterop() };
						while ( !path.IsFunctionCallEnd() )
						{
							// we need element separator between arguments
							if ( !args.Empty() )
							{
								if ( !path.IsSeparator() )
									return ThrowException( "Expected element separator" );
								path.EatChar(); // ','
							}

							// parse the value if it's there, NOTE: it's allowed to be empty, we do support the func(,,,) syntax
							InteropString arg;
							if ( !path.IsSeparator() && !path.IsFunctionCallEnd() )
							{
								arg = path.ExtractValue(false);
							}

							// accumulate all arguments
							args.PushBack( arg );
						}

						// eat the call end
						path.EatChar(); // ')'

						// TODO: argument parsing

						// execute
						auto callResult = functorObject->Execute( *this, args.Size(), args.TypedData() );
						if ( HasErrors() )
							return false; // exception was thrown

						// setup result
						result.m_children.Clear();
						result.m_metaType = IBaseObject::MetaType::Value;
						result.m_type = InteropType( functorObject->GetType(), true ); // return values of functors are constant
						result.m_data = std::move( callResult );

						// return state
						return true;
					}

					case IBaseObject::MetaType::Notifier:
					{
						if (path.IsMemberAccess())
						{
							path.EatChar(); // consume '.'
							InteropString ident = path.ExtractIdentifier();
							if (ident == "__callstack")
							{
								if (!path.IsFinished())
								{
									return ThrowException("__callstack must be final in path, path '%hs'", path.GetRestOfPathAsChar());
								}
								SaveCallstack(obj->GetCallstack());
								return true;
							}
						}
						return ThrowException("Invalid operation on notifier object");
					}
				}

				return ThrowException( "Unknown operation" );
			}

			template< typename T >
			Bool CommonValueSetter( IBaseObject* obj, const InteropString& val, InteropResult& result )
			{
				T* valueObject = static_cast< T* >( obj );

				if ( valueObject->IsReadOnly() )
					return ThrowException( "Unable to set value to a read-only object" );

				valueObject->Set( *this, val );

				// continue if valid
				if ( !HasErrors() )
					valueObject->Get( *this, result.m_data );

				return true;
			}

			Bool SetValue( IBaseObject* obj, const InteropString& val, InteropResult& result )
			{
				result.m_metaType = obj->GetMetaType();
				result.m_type = InteropType( obj->GetType() );

				switch( obj->GetMetaType() )
				{
				case IBaseObject::MetaType::Value:
					return CommonValueSetter< IValueObject >( obj, val, result );

				case IBaseObject::MetaType::Array:
					return CommonValueSetter< IArrayObject >( obj, val, result );

				case IBaseObject::MetaType::Compound:
					return CommonValueSetter< ICompoundObject >( obj, val, result );

					// functor in it's final form, extract the return type
				case IBaseObject::MetaType::Functor:
					return ThrowException( "Unable to set value to a functor" );

					 // notifier value is it's internal ID
				case IBaseObject::MetaType::Notifier:
					return ThrowException( "Unable to set value to a notifier" );

				default:
					return ThrowException( "Unable to set value to a unknown object" );
				}
			}

			Bool ExtractValue( IBaseObject* obj, InteropResult& result, const Bool isReadOnly )
			{
				result.m_metaType = obj->GetMetaType();
				result.m_type = InteropType( obj->GetType(), isReadOnly );

				if ( obj->GetMetaType() == IBaseObject::MetaType::Value )
				{
					InteropString valueStr;

					auto* valueObject = static_cast< IValueObject* >( obj );
					result.m_type.m_const |= valueObject->IsReadOnly();
					valueObject->Get( *this, result.m_data );
					result.m_type.m_flags |= valueObject->IsDefault();
					result.m_type.m_flags |= ( valueObject->IsMultiValue() << 2 );
					return true;
				}
				else if ( obj->GetMetaType() == IBaseObject::MetaType::Compound )
				{
					auto* compoundObject = static_cast<ICompoundObject*>( obj );
					result.m_type.m_const |= compoundObject->IsReadOnly();
					result.m_type.m_flags |= compoundObject->IsDefault();
					result.m_type.m_flags |= ( compoundObject->IsMultiValue() << 2 );
					compoundObject->GetChildrenInfo( *this, result.m_children );
					compoundObject->Get( *this, result.m_data );
					return true;
				}
				else if ( obj->GetMetaType() == IBaseObject::MetaType::Array )
				{
					auto* arrayObject = static_cast< IArrayObject* >( obj );
					const Uint32 size = arrayObject->GetSize( *this );
					result.m_data = InteropString::Printf( "%u", size );
					result.m_type.m_const |= arrayObject->IsReadOnly();
					result.m_type.m_flags |= ( arrayObject->IsMultiValue() << 2 );
					return true;
				}
				else if ( obj->GetMetaType() == IBaseObject::MetaType::Functor ) // functor in it's final form, extract the return type
				{
					auto* functorObject = static_cast< IFunctorObject* >( obj );

					result.m_type = InteropType( functorObject->GetType(), true ); // return type of functors are read only
					result.m_data = InteropString::EMPTY();

					// report types of arguments as children
					red::DynArray< CName > children{ red::PoolInterop() };
					functorObject->GetParamTypes( *this, children );

					result.m_children.Reserve( children.Size() );
					for( CName& name : children )
						result.m_children.PushBack( ChildWithCategory( name, CName::NONE() ) );

					return true;
				}
				else if ( obj->GetMetaType() == IBaseObject::MetaType::Notifier ) // notifier value is it's internal ID
				{
					auto* notifierObject = static_cast< INotifierObject* >( obj );
					result.m_data = String::Printf( "%u", notifierObject->GetNotificationId() );
					result.m_type = InteropType( GetTypeName<Uint32>(), true ); // ID of the notifier is read only
					return true;
				}
				else 
				{
					return ThrowException( "Unable to get value to a unknown object" );
				}
			}

			InteropString	m_exception;
			InteropString	m_callstack;
			InteropCache&	m_cache;
		};

	} // prv

	//----------------------------

	const Bool Execute( const ObjectPtr& root, const InteropString& command, InteropResult& result, IInteropCache& cache )
	{
		prv::Interpreter interpreter( static_cast< prv::InteropCache& >( cache ) );
		return interpreter.Execute( root, command, result );
	}

	const Bool Execute( const ObjectPtr& root, const InteropString& command, InteropResult& result )
	{
		prv::InteropCache tmpCache;
		prv::Interpreter interpreter( tmpCache );
		return interpreter.Execute( root, command, result );
	}

	//----------------------------

	red::UniquePtr< interop::IInteropCache > IInteropCache::Create()
	{
		return red::CreateUniquePtr< prv::InteropCache >();
	}

} // interop
