/**
Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "packageTableCompiler.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>
#include <cstdio>
#include <string>
#include "packageTable.h"
#include "packageSerializer.h"
#include "packageTableOfContent.h"
#include "packageReadWriteStream.h"
#include "packageTypeSerializerDictionary.h"
#include "packageTypeSerializer.h"
#include "resourceToken.h"
#include "packageVersion.h"
#include "packageReadStream.h"
#include "packageWriteStream.h"

extern "C" Uint32 cpweb_PackageReadOverrunCount(); // packageReadStream.cpp [saves]

namespace red
{
	PackageTableOfContentCompiler::PackageTableOfContentCompiler()
		: m_inputTable( nullptr )
		, m_outputTable( nullptr )
		, m_remappedObject( red::PoolEngine() )
		, m_remappedObjectLookup( red::PoolEngine() )
		, m_remappedName( red::PoolEngine() )
		, m_remappedResource( red::PoolEngine() )
	{}

	PackageTableOfContentCompiler::~PackageTableOfContentCompiler()
	{}

	void PackageTableOfContentCompiler::Initialize( const PackageTableOfContentSanitizerParameter & param )
	{
		m_inputTable = param.inputTable;
		m_outputTable = param.outputTable;

		m_remappedObjectLookup.Resize( param.inputTable->objectTable.Size() );
		for ( auto& index : m_remappedObjectLookup )
		{
			index = c_invalidIndex;
		}
	}

	PackageTableOfContent::NameIndex PackageTableOfContentCompiler::OnMapName( CName name )
	{
		auto iter = m_outputTable->stringLookup.Find( name );
		if ( iter == m_outputTable->stringLookup.End() )
		{
			m_outputTable->stringTable.PushBack( name );
			auto index = m_outputTable->stringTable.Size() - 1;
			m_outputTable->stringLookup.Insert(name, index);
			return static_cast< NameIndex >( index );
		}

		return static_cast< NameIndex >( iter.Value() );
	}
	
	PackageTableOfContent::ObjectIndex PackageTableOfContentCompiler::OnMapObject( const ISerializable * object,  const void * referenceObject )
	{
		RED_FATAL( "Sanitizer can not map regular object, only remap." );
		return 0;
	}
	
	PackageTableOfContent::ResourceIndex PackageTableOfContentCompiler::OnMapResource( const res::ResourcePath & path, PackageResourceImportType importType )
	{
		auto predicate = [path]( const PackageTableResourceDescriptor & descriptor ) { return descriptor.path == path; };
		auto iter = std::find_if( m_outputTable->resourceTable.Begin(), m_outputTable->resourceTable.End(), predicate );
		if( iter == m_outputTable->resourceTable.End() )
		{
			m_outputTable->resourceTable.PushBack( { path, importType } );
			return m_outputTable->resourceTable.Size() - 1;
		}

		iter->importType |= importType;
		return static_cast< ResourceIndex >( std::distance( m_outputTable->resourceTable.Begin(), iter ) );
	}
	
	CName PackageTableOfContentCompiler::OnUnmapName( NameIndex index ) const
	{
		if( index < m_inputTable->stringTable.Size() )
		{
			return m_inputTable->stringTable[index];
		}

		return CName();
	}
	
	void PackageTableOfContentCompiler::OnUnmapObject( ObjectIndex index, SerializableHandle& handle ) const 
	{
		if( index < m_inputTable->objectTable.Size() )
		{
			handle = m_inputTable->objectTable[ index ].object->HandleFromThis();
		}
	}

	res::ResourcePath PackageTableOfContentCompiler::OnUnmapResource( ResourceIndex index, res::ResourceTokenHandle & token ) const
	{
		if( index < m_inputTable->resourceTable.Size() )
		{
			return m_inputTable->resourceTable[ index ].path;
		}
		
		return res::ResourcePath();
	}
	
	PackageTableOfContent::ObjectIndex PackageTableOfContentCompiler::OnRemapObject( ObjectIndex index )
	{
		ObjectIndex newObjectId = m_remappedObjectLookup[ index ];

		if( newObjectId == c_invalidIndex )
		{
			m_remappedObject.PushBack( index );

			const PackageTableObjectDescriptor & descriptor = m_inputTable->objectTable[ index ];

			// ctremblay: DO NOT REMAP name here. It is done while processing object.
			m_outputTable->objectTable.PushBack( descriptor );
			newObjectId = m_outputTable->objectTable.Size() - 1;
			m_outputTable->objectLookup[ descriptor.objectId ] = newObjectId;
			m_outputTable->pendingObjectContainer.PushBack( newObjectId );
			
			m_remappedObjectLookup[ index ] = newObjectId;
			return newObjectId;
		}

		return newObjectId;
	}

	PackageTableOfContent::NameIndex PackageTableOfContentCompiler::OnRemapName( NameIndex index )
	{
		const CName inputName = UnmapName( index );
		return MapName( inputName );
	}

	PackageTableOfContent::ResourceIndex PackageTableOfContentCompiler::OnRemapResource( ResourceIndex index, PackageResourceImportType importType )
	{
		res::ResourceTokenHandle token;
		const res::ResourcePath inputPath = UnmapResource( index, token );
		return MapResource( inputPath, importType );
	}

	void PackageTableOfContentCompiler::UpdateInputTable()
	{
		// ctremblay: If Object Index are updated in place, Input Table need all object position updated.
		PackageTable::ObjectTable oldObjectTable = m_inputTable->objectTable;
		PackageTable::ObjectTable & objectTable = m_inputTable->objectTable;
		PackageTable::ObjectLookup & objectLookup = m_inputTable->objectLookup;
		objectTable.Clear();
		objectTable.Resize( m_remappedObject.Size() );
		objectLookup.Clear();
		for( ObjectIndex index = 0, end = m_remappedObject.Size(); index != end; ++index )
		{
			const ObjectIndex oldIndex = m_remappedObject[ index ];
			PackageTableObjectDescriptor descriptor = oldObjectTable[ oldIndex ]; 
			descriptor.nameIndex = RemapName( descriptor.nameIndex );
			objectTable[ index ] = descriptor;	
			objectLookup[ descriptor.objectId ] = index;
		}

		PackageTable::RootObjectTable & rootObjectTable = m_inputTable->rootObjectTable; 
		const Uint32 rootObjectCount = rootObjectTable.Size();

		for( Int32 index = 0; index != rootObjectCount; ++index )
		{
			rootObjectTable[ index ] = index;
		}

		m_inputTable->stringTable = m_outputTable->stringTable;
		m_inputTable->stringLookup = m_outputTable->stringLookup;
		m_inputTable->resourceTable = m_outputTable->resourceTable;
	}


	PackageTableCompilerSerializer::PackageTableCompilerSerializer()
		: m_tableOfContent( nullptr )
		, m_dictionary( nullptr )
	{}

	PackageTableCompilerSerializer::~PackageTableCompilerSerializer()
	{}

	void PackageTableCompilerSerializer::Initialize( const PackageSerializerSanitizerParameter & param )
	{
		m_inputStream = param.inputStream;
		m_outputStream = param.outputStream;
		m_tableOfContent = param.tableOfContent;
		m_dictionary = param.dictionary;
	}

	void PackageTableCompilerSerializer::OnSerialize( void * buffer, Uint64 size )
	{
		if(!buffer)
		{
			const void * cursor = m_inputStream->GetReadCursor();
			m_outputStream->Write( cursor, size );
			m_inputStream->Skip( size );
		}
		else
		{
			m_inputStream->Read( buffer, size );
			m_outputStream->Write( buffer, size );
		}
	}

	// CP2077 web [nomadplay]: nesting-depth diagnostic + guard. A game save on the worker overflowed the V8 stack
	// (RangeError) in this recursion. Logs the type chain when the depth gets suspicious and stops descending
	// past c_npMaxDepth (that value is skipped instead of crashing the worker).
	static thread_local Uint32 s_npDepth = 0;
	static thread_local const rtti::IType* s_npChain[ 64 ];
	static constexpr Uint32 c_npLogDepth = 48;
	static constexpr Uint32 c_npMaxDepth = 160;
	struct NpDepthScope
	{
		NpDepthScope( const rtti::IType* t ) { if ( s_npDepth < 64 ) { s_npChain[ s_npDepth ] = t; } ++s_npDepth; }
		~NpDepthScope() { --s_npDepth; }
	};

	bool PackageTableCompilerSerializer::OnSerializeType( const PackageSerializeTypeParameter & param )
	{
		NpDepthScope npScope( param.type );
		if ( s_npDepth == c_npLogDepth || s_npDepth == c_npMaxDepth )
		{
			static std::atomic< Uint32 > s_npLogs{ 0 };
			if ( s_npLogs.fetch_add( 1 ) < 6 )
			{
				std::string chain;
				for ( Uint32 i = 0; i < 64 && i < s_npDepth; ++i )
				{
					chain += s_npChain[ i ] ? s_npChain[ i ]->GetName().AsChar() : "?";
					chain += " > ";
				}
				std::fprintf( stderr, "[nomadplay] save serializer depth %u: %s\n", s_npDepth, chain.c_str() );
			}
		}
		if ( s_npDepth > c_npMaxDepth )
		{
			return true;
		}
		const PackageTypeSerializer * serializer = m_dictionary->FindTypeSerializer( param.type );
		if( serializer )
		{
			PackageTypeSerializer::RemapContext context =
			{
				*this,
				*m_inputStream,
				*m_outputStream,
				*m_tableOfContent,
				c_packageCurrentVersion,
				true
			};
		
			serializer->RemapValue( context, param );
			return true;
		}

		return false;
	}

	PackageTableCompiler::PackageTableCompiler()
		: m_inputTable( nullptr )
		, m_serializerDictionary( nullptr )
		, m_system( &GetRttiSystem() )
	{}

	PackageTableCompiler::~PackageTableCompiler()
	{}

	void PackageTableCompiler::Initialize( const PackageTableCompilerParameter & param )
	{
		m_inputTable = param.inputTable;
		m_outputTable = param.outputTable;
		m_inputStream = CreateReadStream( param.inputBuffer );
		m_outputStream = CreateWriteStream( param.memoryReserve );
		m_serializerDictionary = param.serializerDictionary;
	}

	UniqueBuffer PackageTableCompiler::Execute()
	{
		RED_FATAL_ASSERT( m_inputTable, "No input PackageTable provided. Cannot execute compilation." );
		RED_FATAL_ASSERT( m_inputTable, "No output PackageTable provided. Cannot execute compilation." );
		if( !m_inputTable->rootObjectTable.Empty() )
		{
			PackageTableOfContentCompiler tocSanitizer;
			tocSanitizer.Initialize( { m_inputTable, m_outputTable } );
			for( Int32 rootObjectId : m_inputTable->rootObjectTable )
			{
				if( rootObjectId != c_invalidObjectIndex )
				{
					PackageTableOfContent::ObjectIndex remappedId = tocSanitizer.RemapObject( rootObjectId );
					m_outputTable->rootObjectTable.PushBack( remappedId );
				}
				else
				{ /* Object type doesnt exist anymore. Data will be dropped. */ }
			}

			SanitizeAllPendingObject( &tocSanitizer );
			{ // [nomad2] diag: every compiled object must start with a sane property count
				static std::atomic< Uint32 > s_tdiag{ 0 };
				const red::BlobView out = m_outputStream->GetBufferView();
				Uint32 bad = 0, zero = 0;
				for( Uint32 i = 0, n = m_outputTable->objectTable.Size(); i != n; ++i )
				{
					const PackageTableObjectDescriptor& d = m_outputTable->objectTable[ i ];
					if( d.propertyDataSize < 2 ) { ++zero; continue; }
					const Uint16 cnt = ( Uint64( d.propertyDataOffset ) + 2 <= out.Size() ) ? *reinterpret_cast< const Uint16* >( static_cast< const Uint8* >( out.Data() ) + d.propertyDataOffset ) : 0xFFFF;
					const Uint32 need = 2u + 8u * cnt;
					if( cnt > 300 || need > d.propertyDataSize )
					{
						if( ++bad <= 6 && s_tdiag.fetch_add( 1 ) < 30 )
						{
							std::fprintf( stderr, "CP2077 nomad2 save: table-compiled object %u off=%u size=%u count=%u (bad)\n", i, d.propertyDataOffset, d.propertyDataSize, (unsigned)cnt );
						}
					}
				}
				if( s_tdiag.fetch_add( 1 ) < 30 )
				{
					std::fprintf( stderr, "CP2077 nomad2 save: table compile objects=%u bad=%u zeroSize=%u out=%u\n", (unsigned)m_outputTable->objectTable.Size(), bad, zero, (unsigned)out.Size() );
				}
			}

			// ctremblay: input table do not need to be updated for now as remapping is not done inline anymore
			// However! it might get back to inline at one point as it is much better.
			//tocSanitizer.UpdateInputTable();
		}

		return m_outputStream->ReleaseBuffer();
	}

	void PackageTableCompiler::SanitizeAllPendingObject( PackageTableOfContent * toc )
	{
		PackageTableCompilerSerializer serializer;
		serializer.Initialize( { m_inputStream.Get(), m_outputStream.Get(), toc, m_serializerDictionary }  );

		const Uint32 cpwebOverrunsAtStart = cpweb_PackageReadOverrunCount(); // [nomad2] runaway guard
		for( Uint32 index = 0; index != m_outputTable->pendingObjectContainer.Size(); ++index )
		{
			if( cpweb_PackageReadOverrunCount() - cpwebOverrunsAtStart > 64 )
			{
				std::fprintf( stderr, "CP2077 nomad2 save: package compile poisoned (%u remap overruns) - stopped at object %u of %u\n", cpweb_PackageReadOverrunCount() - cpwebOverrunsAtStart, index, (unsigned)m_outputTable->pendingObjectContainer.Size() );
				break;
			}
			Int32 pendingObject = m_outputTable->pendingObjectContainer[ index ];
			PackageTableObjectDescriptor & descriptorValue = m_outputTable->objectTable[pendingObject]; // ctremblay: NO reference here. array can grow !

			const CName objectTypename = toc->UnmapName( descriptorValue.nameIndex ); 
			const rtti::ClassType * objectType = m_system->FindClass( objectTypename );

			descriptorValue.nameIndex = toc->MapName( objectTypename );

			const PackageSerializeTypeParameter param = 
			{
				nullptr,
				objectType,
				nullptr
			};

			m_inputStream->Seek( descriptorValue.propertyDataOffset );
			{ // [nomad2] diag: object property count at the input offset
				static std::atomic< Uint32 > s_cdiag{ 0 };
				const Uint64 off = descriptorValue.propertyDataOffset;
				if( descriptorValue.propertyDataSize >= 2 && off + 2 <= m_inputStream->GetBufferSize() )
				{
					const Uint16 cnt = *static_cast< const Uint16* >( m_inputStream->GetReadCursor() );
					if( cnt > 300 && s_cdiag.fetch_add( 1 ) < 12 )
					{
						std::fprintf( stderr, "CP2077 nomad2 save: compile object #%u (input #%d) %s off=%llu size=%u count=%u inputBuffer=%u\n", index, pendingObject, objectTypename.AsChar(), (unsigned long long)off, descriptorValue.propertyDataSize, (unsigned)cnt, m_inputStream->GetBufferSize() );
					}
				}
			}
		
			// ctremblay: objectTable can grow in this function call. 
			// Do not use descriptorValue after. 
			const Uint32 startPosition = static_cast<Uint32>(m_outputStream->GetPosition());
			{
				// [nomadplay] save-crash diagnostic: print each object class the first time it is compiled, so the last
				// line before a trap names the culprit; a class missing from RTTI is reported and skipped.
				static std::mutex s_npSeenMutex;
				static std::vector< Uint64 > s_npSeen;
				Bool first = false;
				{
					std::lock_guard< std::mutex > lock( s_npSeenMutex );
					const Uint64 h = objectTypename.GetHash();
					if ( std::find( s_npSeen.begin(), s_npSeen.end(), h ) == s_npSeen.end() && s_npSeen.size() < 8192 )
					{
						s_npSeen.push_back( h );
						first = true;
					}
				}
				if ( first || !objectType )
				{
					std::fprintf( stderr, "[nomadplay] save compile object #%u class %s%s\n", index, objectTypename.AsChar(), objectType ? "" : " (NOT IN RTTI - skipped)" );
				}
			}
			if ( objectType )
			serializer.SerializeType( param );
			const Uint32 endPosition = static_cast<Uint32>(m_outputStream->GetPosition());

			PackageTableObjectDescriptor & result = m_outputTable->objectTable[pendingObject]; // ctremblay: NO reference here. array can grow !
			result.propertyDataOffset = startPosition;
			result.propertyDataSize = endPosition - startPosition;
		}

		m_outputTable->pendingObjectContainer.Clear();
	}

	red::UniquePtr< PackageTableCompiler > CreatePackageTableCompiler( const PackageTableCompilerParameter & param )
	{
		red::UniquePtr< PackageTableCompiler > compiler = red::CreateUniquePtr< PackageTableCompiler >();
		compiler->Initialize( param );
		return compiler;
	}
}	
