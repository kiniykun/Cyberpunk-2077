/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "packageBuilder.h"
#include "packageLayoutSaver.h"
#include "packageTableOfContentBuilder.h"
#include "packageWriteStream.h"
#include "packageTypeSerializerDictionary.h"
#include "packageWriter.h"
#include "packageCompiler.h"
#include "packageRemapper.h"
#include "packageUtils.h"
#include "packageReader.h"
#include "packageReadStream.h"
#include "packageVersion.h"
#include "packageErrorReporter.h"
#include "packageTableOfContentRemap.h"
#include "packageInspector.h"

// CP2077 web [nomad2]: the fast path of WriteAndRemapObjects (rebuild a builder from a compiled package: save reload,
// game load) copies the object data verbatim and RED_VERIFYs that re-mapping every name/resource of the package
// gives back the same index. RED_VERIFY is compiled out here; if the package string table holds two entries that map
// to the same CName (e.g. names whose string is not in this build's name pool), every later name index is shifted,
// the copied property descriptors point at the wrong names/types, and the next save compile reads garbage
// (property counts of 30000+, runaway recursion, heap growth). Detect that and take the remapping path instead.
#include <atomic>
#include <cstdio>
#include <unordered_set>
namespace cpweb_nomad2_builder
{
	static Bool TablesRoundTrip( const red::Package& package )
	{
		static std::atomic< Uint32 > s_logs{ 0 };
		Bool ok = true;
		std::unordered_set< Uint64 > seen;
		seen.reserve( package.stringTable.Size() * 2 + 1 );
		Uint32 dups = 0;
		for( red::PackageNameIterator it( package ); it.IsValid(); it.Next() )
		{
			const CName name = it.GetName();
			if( !seen.insert( name.GetHash() ).second )
			{
				ok = false;
				if( ++dups <= 6 && s_logs.fetch_add( 1 ) < 24 )
				{
					std::fprintf( stderr, "CP2077 nomad2 package: duplicate name at index %u hash=%016llx '%s' (of %u names)\n", it.GetIndex(), (unsigned long long)name.GetHash(), name.AsChar(), (unsigned)package.stringTable.Size() );
				}
			}
		}
		seen.clear();
		Uint32 rdups = 0;
		for( red::PackageResourceIterator it( package ); it.IsValid(); it.Next() )
		{
			if( !seen.insert( it.GetPath().GetHash() ).second )
			{
				ok = false;
				++rdups;
			}
		}
		if( !ok && s_logs.fetch_add( 1 ) < 24 )
		{
			std::fprintf( stderr, "CP2077 nomad2 package: name dups=%u resource dups=%u -> remapping path instead of verbatim copy\n", dups, rdups );
		}
		static std::atomic< Uint32 > s_calls{ 0 };
		if( s_calls.fetch_add( 1 ) < 8 )
		{
			std::fprintf( stderr, "CP2077 nomad2 package: rebuild names=%u resources=%u objects=%u roots=%u roundtrip=%d\n", (unsigned)package.stringTable.Size(), (unsigned)package.resourceTable.Size(), (unsigned)package.objectTable.Size(), (unsigned)package.rootObjectTable.Size(), ok ? 1 : 0 );
		}
		return ok;
	}
}

namespace red
{
	class PackageDefaultSaver : public PackageLayoutSaver
	{
	public:
		PackageDefaultSaver();
		virtual ~PackageDefaultSaver();

	private:

		virtual void OnWriteUserLayout( PackageStream & stream ) const override final;
		virtual Uint32 OnGetUserLayoutSize() const override final;
	};

	PackageDefaultSaver::PackageDefaultSaver()
	{}

	PackageDefaultSaver::~PackageDefaultSaver()
	{}

	void PackageDefaultSaver::OnWriteUserLayout( PackageStream & ) const
	{}

	Uint32 PackageDefaultSaver::OnGetUserLayoutSize() const
	{
		return 0;
	}

	PackageBuilder::PackageBuilder()
		: m_table( red::CreateUniquePtr< PackageTable >() )
	{}
	
	PackageBuilder::PackageBuilder( PackageBuilder && other )
		: m_table( std::move( other.m_table ) )
		, m_packageWriter( std::move( other.m_packageWriter ) )
		, m_tableBuilder( std::move( other.m_tableBuilder ) )
		, m_stream( std::move( other.m_stream ) )
		, m_dictionary( std::move( other.m_dictionary ) )
		, m_errorReporter( std::move( other.m_errorReporter ) )
		, m_removedRootObjects( std::move( other.m_removedRootObjects ) )
		, m_cookingPlatform( PLATFORM_None )
		, m_discardResourcePathString( false )
	{
		m_tableBuilder->Initialize( this, m_table.Get() );
	}

	PackageBuilder::~PackageBuilder()
	{}

	PackageBuilder& PackageBuilder::operator=( PackageBuilder && other )
	{
		PackageBuilder( std::move( other ) ).Swap( *this );
		return *this;
	}

	void PackageBuilder::Initialize( const PackageBuilderParameter & param )
	{
		m_tableBuilder = red::CreateUniquePtr< PackageTableOfContentBuilder >();
		m_tableBuilder->Initialize( this, m_table.Get() );
		m_stream = CreateWriteStream( param.memoryReserve );
		m_dictionary = CreatePackageTypeSerializerDictionary();
		m_errorReporter = red::CreateUniquePtr< PackageErrorReporter >();
		m_packageWriter = CreatePackageWriter( { m_stream.Get(), m_tableBuilder.Get(), m_dictionary.Get(), m_errorReporter.Get(), param.packagePropertyFlags, param.cookingPlatform } );
		m_removedRootObjects.Clear();
		m_cookingPlatform = param.cookingPlatform;
		m_discardResourcePathString = param.discardResourcePathString;
	}

	Uint32 PackageBuilder::WriteObject( const ISerializable * object )
	{
		RED_FATAL_ASSERT( object , "Cannot write a null object in Package." );
		return WriteObject( object, object->GetClass()->GetDefaultObject() );
	}

	Uint32 PackageBuilder::WriteObject( const ISerializable * object, const void * referenceObject )
	{
		RED_FATAL_ASSERT( object, "Cannot write a null object in Package." );

		const Int32 nextIndex = m_table->rootObjectTable.Size();	
		const Int32 mappedIndex = m_tableBuilder->MapRootObject( object, referenceObject );
		if( mappedIndex == nextIndex )
		{
			m_tableBuilder->SetCurrentObjectIndex( mappedIndex );
			WriteAllPendingObject();
		}
		else
		{ /* object was already mapped therfor no need to redo it. */ }

		return mappedIndex;
	}

	void PackageBuilder::WriteAllPendingObject()
	{
		for( Uint32 index = 0; index != m_table->pendingObjectContainer.Size(); ++index )
		{
			Int32 pendingObject = m_table->pendingObjectContainer[ index ];

			PackageTableObjectDescriptor descriptorValue = m_table->objectTable[ pendingObject ]; // ctremblay: NO reference here. array can grow !

			PreSaveContext context;
			context.isCooking = m_cookingPlatform != PLATFORM_None;
			context.cookingPlatform = m_cookingPlatform;
			const_cast< ISerializable* >( descriptorValue.object )->OnPreSave( context );

			const PackageSerializeTypeParameter param = 
			{
				const_cast< ISerializable* >( descriptorValue.object ),
				descriptorValue.object->GetClass(),
				descriptorValue.referenceObject
			};

			const Uint32 startPosition = static_cast< Uint32 >( m_stream->GetPosition() );
			m_packageWriter->SerializeType( param );
			const Uint32 endPosition = static_cast< Uint32 >( m_stream->GetPosition() );
			
			PackageTableObjectDescriptor & result = m_table->objectTable[ pendingObject ]; // ctremblay: reference here. array can't grow anymore !
			result.propertyDataOffset = startPosition;
			result.propertyDataSize = endPosition - startPosition;
		}

		m_table->pendingObjectContainer.Clear();
	}

	SerializableHandle PackageBuilder::ReadRootObject( Uint32 index ) const
	{
		RED_FATAL_ASSERT( index < m_table->rootObjectTable.Size(), "index is out of bound. PackageBuilder do not have this object." );

		const Uint32 unmappedIndex = m_table->rootObjectTable[ index ];
		return ReadObject( unmappedIndex );
	}

	SerializableHandle PackageBuilder::ReadObject( Uint32 index ) const
	{
		RED_FATAL_ASSERT( index < m_table->objectTable.Size(), "index is out of bound. PackageBuilder do not have this object." );

		const PackageTableObjectDescriptor & descriptor = m_table->objectTable[index];
		const CName objectTypename = m_tableBuilder->UnmapName( descriptor.nameIndex );

		SerializableHandle handle = CreateObject( objectTypename );

		if( handle )
		{
			PackageReadStream stream;
			stream.SetBuffer( m_stream->GetBufferView() );

			PackageReaderParameter readerParam =
			{
				c_packageCurrentVersion,
				&stream,
				m_tableBuilder.Get(),
				m_dictionary.Get(),
				m_errorReporter.Get(),
			};

			PackageReader reader;
			reader.Initialize( readerParam );

			stream.Seek( descriptor.propertyDataOffset );
			reader.SerializeType( { handle.Get(), handle->GetClass(), nullptr } );
		}

		return handle;
	}

	CompiledPackage PackageBuilder::BuildPackage() const
	{
		PackageDefaultSaver	saver;
		return BuildPackage( saver );
	}

	CompiledPackage PackageBuilder::BuildPackage( const PackageLayoutSaver & saver, Uint32 memoryReserve /* = RED_KILO_BYTE( 4 ) */ ) const
	{
		if( !m_table->rootObjectTable.Empty() )
		{
			PackageCompilerParameter compilerParam = 
			{
				m_stream->GetBuffer(),
				m_table.Get(),
				m_dictionary.Get(),
				memoryReserve,
				m_discardResourcePathString
			};

			PackageCompiler compiler;

			compiler.Initialize( compilerParam );

			CompiledPackage cpwebResult = compiler.Compile( saver );
			{ // [nomad2] diag: verify the compiled package object table against its data
				static std::atomic< Uint32 > s_pdiag{ 0 };
				const red::Package& pk = cpwebResult.package;
				Uint32 bad = 0;
				for( Uint32 i = 0, n = pk.objectTable.Size(); i != n; ++i )
				{
					const Uint32 off = pk.objectTable[ i ].dataOffset;
					const Uint32 end = ( i + 1 < n ) ? pk.objectTable[ i + 1 ].dataOffset : pk.buffer.Size();
					if( end <= off + 1 ) continue;
					const Uint16 cnt = ( Uint64( off ) + 2 <= pk.buffer.Size() ) ? *reinterpret_cast< const Uint16* >( static_cast< const Uint8* >( pk.buffer.Data() ) + off ) : 0xFFFF;
					if( cnt > 300 || 2u + 8u * cnt > end - off )
					{
						if( ++bad <= 6 && s_pdiag.fetch_add( 1 ) < 30 )
						{
							std::fprintf( stderr, "CP2077 nomad2 package: compiled object %u off=%u size=%u count=%u (bad)\n", i, off, end - off, (unsigned)cnt );
						}
					}
				}
				if( s_pdiag.fetch_add( 1 ) < 30 )
				{
					std::fprintf( stderr, "CP2077 nomad2 package: compiled objects=%u bad=%u bufferSize=%u first=%u bufBase=%p resultBase=%p\n", (unsigned)pk.objectTable.Size(), bad, (unsigned)pk.buffer.Size(), pk.objectTable.Size() ? pk.objectTable[ 0 ].dataOffset : 0u, (const void*)pk.buffer.Data(), (const void*)cpwebResult.buffer.Get() );
				}
			}
			return cpwebResult;
		}

		return CompiledPackage();
	}

	void PackageBuilder::OverrideObject( Uint32 index, const ISerializable* object )
	{
		OverrideObject( index, object, object->GetClass()->GetDefaultObject() );
	}
	
	void PackageBuilder::OverrideObject( Uint32 index, const ISerializable* object, const void * referenceObject )
	{
		m_tableBuilder->OverrideRootObject( object, referenceObject, index );
		WriteAllPendingObject();
	}

	void PackageBuilder::OverrideResourcePath( Uint32 index, const res::ResourcePath& path )
	{
		m_table->resourceTable[ index ].path = path;
	}

	void PackageBuilder::RemoveObject( Uint32 index, bool allowRecycling )
	{
		m_tableBuilder->RemoveRootObject( index, allowRecycling );
	}

	Uint32 PackageBuilder::WriteObject( Uint32 index, const Package & package )
	{
		RED_FATAL_ASSERT( index < package.rootObjectTable.Size(), "Out of Bound Index." );

		BitSetDynamic mask( package.rootObjectTable.Size(), red::PoolEngine() );
		mask.Set( index );

		const Uint32 beforeRootTableSize = m_table->rootObjectTable.Size();

		WriteAndRemapObjects( mask, package );
		
		const Uint32 afterRootTableSize = m_table->rootObjectTable.Size(); // ctremblay: if no object push to array, it was invalid.

		return beforeRootTableSize != afterRootTableSize ? beforeRootTableSize : c_invalidObjectIndex;
	}

	void PackageBuilder::AppendPackage( const Package & package )
	{	
		BitSetDynamic mask( package.rootObjectTable.Size(), red::PoolEngine() );
		mask.SetAll();
		WriteAndRemapObjects( mask, package );
	}

	void PackageBuilder::WriteAndRemapObjects( const BitSetDynamic & mask, const Package & package )
	{
		if( m_table->objectTable.Empty() && m_table->stringTable.Empty() && m_table->resourceTable.Empty() && package.version == c_packageCurrentVersion && mask.IsAllSet() && cpweb_nomad2_builder::TablesRoundTrip( package ) )
		{
			m_table->resourceTable.Reserve( package.resourceTable.Size() );
			m_table->stringTable.Reserve( package.stringTable.Size() );
			m_table->stringLookup.Reserve( package.stringTable.Size() );
			m_table->objectTable.Reserve( package.objectTable.Size() );
			m_table->rootObjectTable.Reserve( package.rootObjectTable.Size() );
				
			PackageResourceIterator resourceIterator( package );
			for( ; resourceIterator.IsValid(); resourceIterator.Next() )
			{
				RED_VERIFY( m_tableBuilder->MapResource( resourceIterator.GetPath(), resourceIterator.GetImportType() ) == resourceIterator.GetIndex() );
			}

			PackageNameIterator nameIterator( package );
			for( ; nameIterator.IsValid(); nameIterator.Next() )
			{
				RED_VERIFY( m_tableBuilder->MapName( nameIterator.GetName() ) == nameIterator.GetIndex() );	
			}

			const Uint32 dataOffset = package.objectTable.Front().dataOffset;
			red::BlobView objectData =  package.buffer.Range( dataOffset );

			const Uint64 cpwebP0 = m_stream->GetPosition(); // [nomad2] diag: must be 0 for the verbatim copy
			m_stream->Write( objectData.Data(), objectData.Size() );

			PackageObjectIterator objectIterator( package );
			for( ; objectIterator.IsValid(); objectIterator.Next() )
			{
				Uint32 index = objectIterator.GetCurrentIndex();
				
				if( index < package.rootObjectTable.Size() )
				{
					m_table->rootObjectTable.PushBack( index );

					if( !objectIterator.IsTypeValid() )
					{
						m_removedRootObjects.PushBack( index );
					}
				}

				PackageTableObjectDescriptor descriptor =
				{
					SerializableID(),
					nullptr,
					nullptr,
					m_tableBuilder->MapName( objectIterator.GetTypeName() ),
					objectIterator.GetDataOffset() - dataOffset,
					objectIterator.GetDataSize()
				}; 

				m_table->objectTable.PushBack( descriptor );
			}
			{ // [nomad2] diag: compare each object property count in the package vs the rebuilt builder stream
				static std::atomic< Uint32 > s_diag{ 0 };
				const red::BlobView view = m_stream->GetBufferView();
				Uint32 bad = 0, checked = 0;
				for( Uint32 i = 0, n = m_table->objectTable.Size(); i != n; ++i )
				{
					const PackageTableObjectDescriptor& d = m_table->objectTable[ i ];
					const Uint64 off = d.propertyDataOffset; // what the save compiler will Seek() to
					if( d.propertyDataSize < 2 ) continue;
					++checked;
					const Uint16 cnt = ( off + 2 <= view.Size() ) ? *reinterpret_cast< const Uint16* >( static_cast< const Uint8* >( view.Data() ) + off ) : 0xFFFF;
					if( cnt > 300 )
					{
						if( ++bad <= 6 && s_diag.fetch_add( 1 ) < 24 )
						{
							std::fprintf( stderr, "CP2077 nomad2 package: rebuilt object %u type=%s off=%llu size=%u count=%u (P0=%llu base=%u copy=%u)\n", i, m_tableBuilder->UnmapName( d.nameIndex ).AsChar(), (unsigned long long)off, d.propertyDataSize, (unsigned)cnt, (unsigned long long)cpwebP0, dataOffset, (unsigned)objectData.Size() );
						}
					}
				}
				if( s_diag.fetch_add( 1 ) < 24 )
				{
					std::fprintf( stderr, "CP2077 nomad2 package: verbatim rebuild checked=%u bad=%u P0=%llu base=%u copy=%u streamPos=%llu\n", checked, bad, (unsigned long long)cpwebP0, dataOffset, (unsigned)objectData.Size(), (unsigned long long)m_stream->GetPosition() );
				}
			}

			for( auto iter = m_removedRootObjects.RBegin(), end = m_removedRootObjects.REnd(); iter != end; ++iter  )
			{
				RemoveObject( *iter, false );
			}
		}
		else
		{
			PackageObjectIterator objectIterator( package );
			PackageRemapTable remapTable;
			remapTable.nextIndex = m_table->objectTable.Size();
			PackageTableOfContentView inputTable( package );
			UniquePtr< PackageTableOfContentRemap > remapTableOfContent = CreatePackageTableOfContentRemap( {&inputTable, m_tableBuilder.Get(), &remapTable} );
			UniquePtr< PackageRemapper > remapper = CreatePackageRemapper( {package.version, m_stream.Get(), remapTableOfContent.Get(), m_dictionary.Get(), true} );

			for( BitSetConstIterator< BitSetDynamic > iter( mask ); iter.IsValid(); iter.FindNext() )
			{
				auto remappedIndex = remapTableOfContent->RemapObject( iter.GetIndex() );
				if( remappedIndex != c_invalidObjectIndex )
				{
					m_table->rootObjectTable.PushBack( remappedIndex );
				}
				else
				{
					m_removedRootObjects.PushBack( iter.GetIndex() );
				}
			}

			for( Uint32 pendingIndex = 0; pendingIndex != remapTable.pendingRemapping.Size(); ++pendingIndex )
			{
				const PackageRemapTable::PackageRemapObjectIndex pendingRemap = remapTable.pendingRemapping[ pendingIndex ];

				const PackageTableOfContent::ObjectIndex packageIndex = pendingRemap.oldIndex;
				const PackageTableOfContent::ObjectIndex newIndex = pendingRemap.newIndex;

				objectIterator.SetCurrentIndex( packageIndex );
				const BlobView data = objectIterator.GetData();

				const Uint64 startStreamPosition = m_stream->GetPosition();

				PackageTableObjectDescriptor descriptor =
				{
					SerializableID(),
					nullptr,
					nullptr,
					m_tableBuilder->MapName( objectIterator.GetTypeName() ),
					static_cast< Uint32 >( startStreamPosition ),
					0
				};

				m_table->objectTable.PushBack( descriptor );
				const Int32 objectPosition = m_table->objectTable.Size() - 1;
				m_table->objectLookup[ descriptor.objectId ] = objectPosition;

				remapper->Execute( objectIterator.GetType(), data );

				const Uint64 endStreamPosition = m_stream->GetPosition();
				m_table->objectTable[ objectPosition ].propertyDataSize = static_cast< Uint32 >( endStreamPosition - startStreamPosition );
			}
		}
	}

	Uint32 PackageBuilder::MergeObject( Uint32 index, Uint32 packageIndex, const red::Package & package )
	{
		RED_FATAL_ASSERT( packageIndex < package.objectTable.Size(), "Out of bound package Index." );
		RED_FATAL_ASSERT( index < m_table->objectTable.Size(), "Out of bound package Index." );

		// ctremblay: This commented code is a start on how it should be implemented to be deadly optimimal. But since it is currently a tool only feature, no need. 
// 		PackageObjectIterator packageObjectIterator( package );
// 		packageObjectIterator.SetCurrentIndex( packageIndex );
// 		const PackagePropertyView packageProperties = packageObjectIterator.GetPropertyView();
// 
// 		const PackagePropertyView objectProperties = Internal_GetObjectPropertyView( index )


		SerializableHandle currentObject = ReadRootObject( index );
		PackageInspector inspector;
		inspector.Initialize( package );
		inspector.ReadObject( *currentObject, packageIndex );
		OverrideObject( index, currentObject.Get() );

		return index;
	}

	void PackageBuilder::Swap( PackageBuilder & swapWith )
	{
		std::swap( m_table, swapWith.m_table );
		std::swap( m_packageWriter, swapWith.m_packageWriter );
		std::swap( m_tableBuilder, swapWith.m_tableBuilder );
		std::swap( m_stream, swapWith.m_stream );
		std::swap( m_dictionary, swapWith.m_dictionary );
		std::swap( m_errorReporter, swapWith.m_errorReporter );
		std::swap( m_removedRootObjects, swapWith.m_removedRootObjects );
	
		if( m_tableBuilder )
		{
			m_tableBuilder->Initialize( this, m_table.Get() );
		}

		if( swapWith.m_tableBuilder )
		{
			swapWith.m_tableBuilder->Initialize( &swapWith, swapWith.m_table.Get() );
		}
	}

	Uint32 PackageBuilder::GetObjectCount() const
	{
		return m_table->objectTable.Size();
	}

	Uint32 PackageBuilder::GetRootObjectCount() const
	{
		return m_table->rootObjectTable.Size();
	}

	red::ArraySpan< const Int16 > PackageBuilder::GetRemovedRootObjectIndices() const
	{
		return m_removedRootObjects;
	}

	PackagePropertyView PackageBuilder::Internal_GetObjectPropertyView(Uint32 index) const
	{
		RED_FATAL_ASSERT( index < m_table->objectTable.Size(), "Out of Bound object request." );

		const PackageTableObjectDescriptor & descriptor = m_table->objectTable[ index ];
		const red::BlobView data = m_stream->GetRange( descriptor.propertyDataOffset, descriptor.propertyDataOffset + descriptor.propertyDataSize );
		return PackagePropertyView( data, m_tableBuilder.Get() );
	}

	bool PackageBuilder::Internal_IsObjectWrittenAtPosition( const SerializableHandle & handle, Uint32 index ) const
	{
		if( index < m_table->objectTable.Size() )
		{
			return m_table->objectTable[ index ].object == handle;
		}	

		return false;
	}

	void PackageBuilder::Internal_SetPackageTable( red::UniquePtr< PackageTable > table )
	{
		m_table = std::move( table );
	}

	red::UniqueBuffer PackageBuilder::Internal_ReleaseWrittenBuffer()
	{
		return m_stream->ReleaseBuffer();
	}

	red::UniquePtr< PackageBuilder > CreatePackageBuilder( const PackageBuilderParameter & param  )
	{
		RED_FATAL_ASSERT( param.memoryReserve > 0, "Please reserve some memory." );  
		red::UniquePtr< PackageBuilder > builder = red::CreateUniquePtr< PackageBuilder >();
		builder->Initialize( param );
		return builder;
	}
}

