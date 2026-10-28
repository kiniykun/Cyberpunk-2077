#!/usr/bin/env python3
"""[w32-tdbstream] mem=ios: TweakDB main blob without the 27.5 MB whole-file buffer and without the 1.8M-entry
reserve + Shrink copy. The flat section (25.5 of 27.5 MB) is deserialized straight from a buffered file reader; only
records/queries/group tags (~2 MB) are read into memory. The flat array keeps its reserve (untouched tail pages are
never dirtied), so no 14 MB Shrink copy. Logs the flat size. Keyed by ORIGINAL depot paths."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
G = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\gameTweakDB'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag):
    p = copy(orig); assert 'build-w32' in str(p), p
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)

patch(G + r'\include\dataSerializerInterface.h', [
 ('''#include "tweakDB.h"
''', '''#include "tweakDB.h"
#include <cstdio> // [w32-tdbstream]
'''),
 ('''			static void DeserializeFlatType( const rtti::IType* type, TweakDB& tdb, CMemoryFileReader& memoryReader )''',
  '''			template< class TReader > // [w32-tdbstream] memory reader or streamed IFile
			static void DeserializeFlatType( const rtti::IType* type, TweakDB& tdb, TReader& memoryReader )'''),
 ('''				RED_LOG( "Loading TweakDB blob [flat] took %0.3lf ms", timer.GetDeltaMS() );
			}''', '''				RED_LOG( "Loading TweakDB blob [flat] took %0.3lf ms", timer.GetDeltaMS() );
			}

			// [w32-tdbstream] same as DeserializeFlat, reading from a (buffered) file positioned at the flat section;
			// the 1.8M reserve is kept (no Shrink copy: its untouched tail pages are never dirtied).
			static void DeserializeFlatStream( TweakDB& tdb, IFile& memoryReader )
			{
				RED_SCOPE_LOCK( tdb.m_flatLock );
				TweakDB::FlatMap& flat = tdb.m_flat;
				flat.Reserve( 1'880'000 ); // [w32-tdbstream] this cooked blob has 1,874,323 flats: no regrow
				Uint32 typesAmount = 0;
				memoryReader << typesAmount;
				red::DynArray< TypeInfo > types{ game::PoolGMPL_TDB() };
				types.Reserve( typesAmount );
				for ( Uint32 i = 0; i < typesAmount; ++i )
				{
					red::CNameHash typeHash;
					memoryReader << typeHash;
					Uint32 valuesAmount = 0;
					memoryReader << valuesAmount;
					const rtti::IType* type = GetRttiSystem().FindType( CName( typeHash ) );
					types.PushBack( TypeInfo{ type, valuesAmount } );
				}
				const Uint32 runtimeBlobSize = static_cast<Uint32>( CalculateRuntimeBlobSize( types ) );
				Uint32 runtimeBlobAlignment = 4;
				if ( !types.Empty() )
				{
					runtimeBlobAlignment = types.Front().type->GetAlignment();
				}
				TweakDB::s_dataBlob = RED_ALLOCATE_ALIGNED( game::PoolGMPL_TDB_Data, runtimeBlobSize, runtimeBlobAlignment );
				tdb.m_allocator.Initialize( TweakDB::s_dataBlob, runtimeBlobSize );
				for ( const TypeInfo& type : types )
				{
					DeserializeFlatType( type.type, tdb, memoryReader );
				}
				flat.Sort();
				std::fprintf( stderr, "CP2077 w32 tweakdb stream: flat=%u runtimeBlob=%u types=%u\\n", Uint32( flat.Size() ), runtimeBlobSize, typesAmount );
			}'''),
], '[w32-tdbstream]')

patch(G + r'\src\tweakDBLoader.cpp', [
 ('''Bool game::data::TweakDBLoader::LoadMainBlob( TweakDB* tdb, const red::AbsolutePath& path, TweakDBErrorLog& errorLog )
{
	RED_ASSERT( tdb != nullptr );

	const red::AbsolutePath blobPath = ( !path.Empty() ? path : GFileManager->GetCacheDirectory() ).AddFilePath( TWEAKDB_BLOB_FILENAME );
''', '''extern "C" int CpWebMemIos(); // [w32-tdbstream]
Bool game::data::TweakDBLoader::LoadMainBlob( TweakDB* tdb, const red::AbsolutePath& path, TweakDBErrorLog& errorLog )
{
	RED_ASSERT( tdb != nullptr );

	const red::AbsolutePath blobPath = ( !path.Empty() ? path : GFileManager->GetCacheDirectory() ).AddFilePath( TWEAKDB_BLOB_FILENAME );
	if ( ::CpWebMemIos() ) // [w32-tdbstream] flat section streamed from the file, the rest (~2 MB) read into memory
	{
		red::UniquePtr< IFile > file( GFileManager->CreateFileReader( blobPath, FOF_AbsolutePath | FOF_Buffered ) );
		if ( file )
		{
			TweakDBBlobHeader header;
			file->Serialize( &header, sizeof( header ) );
			const Uint64 fileSize = file->GetSize();
			if ( !DataSerializerInterface::CheckHeader( header ) || header.m_flatOffset > header.m_recordsOffset || header.m_recordsOffset > fileSize )
			{
				errorLog.LogValidationError( TWEAKDB_BLOB_FILENAME, 0, 0, ErrorMessages::BLOB_WRONG_HEADER );
				return false;
			}
			file->Seek( header.m_flatOffset );
			DataSerializerInterface::DeserializeFlatStream( *tdb, *file.Get() );
			const Uint32 restSize = Uint32( fileSize - header.m_recordsOffset );
			red::DynArray< Uint8 > rest{ game::PoolGMPL_TDB() };
			rest.Resize( restSize );
			file->Seek( header.m_recordsOffset );
			file->Serialize( rest.Data(), restSize );
			file.Reset();
			const red::BlobView restView( rest.Data(), restSize );
			const Uint32 q = header.m_queriesOffset - header.m_recordsOffset, g = header.m_groupTagsOffset - header.m_recordsOffset;
			DataSerializerInterface::DeserializeRecords( *tdb, restView.Range( 0, q ) );
			DataSerializerInterface::DeserializeQueries( *tdb, restView.Range( q, g - q ) );
			DataSerializerInterface::DeserializeGroupTags( *tdb, restView.Range( g ) );
			return true;
		}
	}
'''),
], '[w32-tdbstream]')
