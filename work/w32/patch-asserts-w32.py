#!/usr/bin/env python3
"""[w32] Guarded wasm32 replacements of pointer-width layout asserts, applied to wasm32 copies only.
Each entry: (original path as compiled, exact original line text (trimmed), wasm32 replacement line).
The copy gets   #if defined( __wasm32__ ) <replacement> #else <original> #endif   so the wasm64 text is preserved.
Copies are created through w32-overlay.py (outer VFS overlay); live wasm64 files are never edited.
Re-runnable: an entry already applied is skipped."""
import subprocess, sys
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'
OV = r'D:\2077\work\w32\w32-overlay.py'
DEV = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev'
C = DEV + r'\src\common'
PX = DEV + r'\external\PhysX342\PhysX_3.4\Include'
ENTRIES = [
    # Cache-line / packing intent only: pointer members shrink on wasm32, the bound still holds.
    (C + r'\redMemory\src\tlsfAllocator.h', 'static_assert( sizeof( TLSFAllocator ) == 64, "TLSFAllocator should fit in one cache line." );',
     'static_assert( sizeof( TLSFAllocator ) <= 64, "TLSFAllocator should fit in one cache line." ); // [w32]'),
    (C + r'\redMemory\src\bigSizeAllocator.h', 'static_assert( sizeof( BigSizeAllocator ) == 128, "Big size allocator has to fit in two cache lines" );',
     'static_assert( sizeof( BigSizeAllocator ) <= 128, "Big size allocator has to fit in two cache lines" ); // [w32]'),
    (C + r'\redMemory\src\slabAllocator.cpp', 'static_assert( sizeof( SlabAllocator ) % 64 == 0, "SlabAllocator size is not multiple of cache line size (64 bytes). Update m_padding2." );',
     'static_assert( sizeof( SlabAllocator ) <= 768, "SlabAllocator grew" ); // [w32] padding was sized for 8-byte pointers (false-sharing intent only)'),
    (C + r'\redMemory\src\slabAllocator.cpp', 'static_assert( sizeof( SlabList ) == 16, "Slab structure need to be of size 16. Fast lookup require this!" );',
     'static_assert( sizeof( SlabList ) == 2 * sizeof( void* ), "Slab list is two pointers" ); // [w32] plain array indexing, any stride works'),
    (C + r'\redContainers\include\string\stringView.h', 'static_assert( sizeof( StringView ) == 12, "Size of StringView is expected to be 12 bytes" );',
     'static_assert( sizeof( StringView ) == 8, "Size of StringView is expected to be pointer + Uint32" ); // [w32]'),
    (C + r'\animation\include\eventsCollector.h', 'static_assert( sizeof(FiredEvent) == 24, "" );',
     'static_assert( sizeof(FiredEvent) <= 24, "" ); // [w32] THandle holds two 4-byte pointers'),
    (C + r'\renderer\src\renderBatchData.h', 'static_assert( sizeof( SProxyBatchData ) == 24, "Don\'t make this struct bigger" );',
     'static_assert( sizeof( SProxyBatchData ) <= 24, "Don\'t make this struct bigger" ); // [w32] two 4-byte pointers'),
    (C + r'\redCore\src\nameRegistry.cpp', 'static_assert( sizeof( Storage ) == 24, "Unexpected size of red::NameRegistry::Storage." );',
     'static_assert( sizeof( Storage ) == 16 + sizeof( void* ), "Unexpected size of red::NameRegistry::Storage." ); // [w32] in-memory only, next is 4 bytes'),
    # Web script serializer: disk operands stay Int64, in-memory operand is intptr_t (4 bytes on wasm32); the
    # surrounding code already converts with static_cast both ways.
    (C + r'\redReflection\src\scriptDataObject.cpp', 'static_assert(sizeof(T) == sizeof(Int64), "Serialized pointer-width operand must remain 64-bit");',
     'static_assert(sizeof(T) <= sizeof(Int64), "Serialized pointer-width operand must fit the 64-bit disk operand"); // [w32]', 2),
    # Handle-alignment probe: a handle is two pointers (8 bytes on wasm32); copy pointer-sized words, not Uint64.
    (C + r'\redReflection\src\scriptOpcodes.cpp', 'static_assert(sizeof(THandle<IScriptable>) == sizeof(Uint64)*2);',
     'static_assert(sizeof(THandle<IScriptable>) == sizeof(std::uintptr_t)*2); // [w32]'),
    (C + r'\redReflection\src\scriptOpcodes.cpp', 'Uint64 oldBits[2], newBits[2];',
     'std::uintptr_t oldBits[2], newBits[2]; // [w32] pointer-sized handle words'),
    (C + r'\redIO\src\redIOAsyncFileHandleCache.cpp', 'static_assert( sizeof( FileEntry ) == 16, "FileEntry size increased" );',
     'static_assert( sizeof( FileEntry ) <= 16, "FileEntry size increased" ); // [w32] 4-byte AsyncFile*'),
    # PhysXGeometryWrapper keeps its fixed 48-byte storage; the (smaller on wasm32) PxGeometryHolder must fit in it.
    (C + r'\physics\src\physicsSceneStructs.cpp', 'static_assert( sizeof( PhysXGeometryWrapper ) == sizeof( physx::PxGeometryHolder ), "Invalid size of PhysXGeometryWrapper." );',
     'static_assert( sizeof( PhysXGeometryWrapper ) >= sizeof( physx::PxGeometryHolder ), "Invalid size of PhysXGeometryWrapper." ); // [w32]'),
    # StateBlock got its multiple-of-8 size from the 8-byte 'next' pointer on x64; keep the property explicitly.
    (C + r'\physics\src\physicsProxyStateAllocator.h', 'struct StateBlock',
     'struct alignas( 8 ) StateBlock // [w32] keep sizeof( StateBlock ) % 8 == 0 (x64 had an 8-byte next pointer)'),
    (C + r'\renderer\src\renderProxyMesh.h', 'static_assert( sizeof( Chunk ) == 32, "Chunk size changed." );',
     'static_assert( sizeof( Chunk ) <= 32, "Chunk size changed." ); // [w32] RenderMaterialInfo pointer is 4 bytes'),
    (C + r'\renderer\src\visibilityOcclusionBuffer.cpp', 'static_assert( sizeof( GatheredOccludersPage ) == 4096, "Invalid gathered occluders page size" );',
     'static_assert( sizeof( GatheredOccludersPage ) <= 4096, "Invalid gathered occluders page size" ); // [w32] 12-byte occluders'),
    # bytes is U64 (the original x64 size_t was 64-bit); the read size was already validated as Uint32 above.
    (C + r'\renderer\src\binkIO.cpp', 'red::BlobSpan destinationSpan{ destination, bytes };',
     'red::BlobSpan destinationSpan{ destination, static_cast< size_t >( bytes ) }; // [w32] size_t is 32-bit'),
    # Pointer used as a unique 64-bit key: a 4-byte pointer zero-extends into the key.
    (C + r'\gameAI\src\replicatedMovePolicies.cpp', 'RED_STATIC_ASSERT( sizeof( value ) == sizeof( Uint64 ) );',
     'RED_STATIC_ASSERT( sizeof( value ) <= sizeof( Uint64 ) ); // [w32]'),
    # Anim command queue: payload address = command address + stored function-pointer size (4 bytes on wasm32).
    (C + r'\animation\include\animCommands.h', 'static const Uint32 s_commandFuncSize = 8;',
     'static const Uint32 s_commandFuncSize = sizeof( AnimCommandFunc ); // [w32] StoreCommandPayload writes sizeof(Func) bytes'),
    # PhysX userData is widened to 8 bytes on wasm32 (see PhysX header entries), so the packed data still fits.
    (C + r'\physics\include\physicsUserData.h', 'static_assert( sizeof( PackedUserData ) == sizeof( void* ), "Error: Packed User Data struct too large" );',
     'static_assert( sizeof( PackedUserData ) == 8, "Error: Packed User Data struct too large" ); // [w32]'),
    (C + r'\physics\include\physicsUserData.h', 'static_assert( sizeof( ShapeUserData ) == sizeof( void* ), "Error: Shape User Data struct too large" );',
     'static_assert( sizeof( ShapeUserData ) == 8, "Error: Shape User Data struct too large" ); // [w32]'),
    # The engine placement-news 8-byte records into these userData fields (physicsUserData.cpp). Keep 8 bytes on wasm32;
    # alignment stays 4 (two PxU32 words) and the member is still named userData / mUserData.
    (PX + r'\PxActor.h', 'void*			userData;	//!< user can assign this to whatever, usually to create a 1:1 relationship with a user object.',
     'union { void* userData; PxU32 cpwebUserDataStorage[2]; }; // [w32] 8 bytes: engine stores PackedUserData here'),
    (PX + r'\PxShape.h', 'void*					userData;	//!< user can assign this to whatever, usually to create a 1:1 relationship with a user object.',
     'union { void* userData; PxU32 cpwebUserDataStorage[2]; }; // [w32] 8 bytes: engine stores ShapeUserData here'),
    (PX + r'\PxBroadPhase.h', 'void*			userData;	//!< Region\'s user-provided data',
     'union { void* userData; PxU32 cpwebUserDataStorage[2]; }; // [w32] engine placement-news a Uint64 here'),
    (PX + r'\characterkinematic\PxControllerObstacles.h', 'void*					mUserData;',
     'union { void* mUserData; PxU32 cpwebUserDataStorage[2]; }; // [w32] engine placement-news a Uint64 here'),
]
def resolve(orig):
    return subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip()
for entry in ENTRIES:
    orig, old, new = entry[:3]; expect = entry[3] if len(entry) > 3 else 1
    subprocess.run([PY, OV, 'add', orig], capture_output=True, text=True, check=True)
    p = Path(resolve(orig))
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape')
    crlf = '\r\n' in raw
    lines = raw.replace('\r\n', '\n').split('\n')
    if any(new.strip() in l for l in lines): print('skip (applied)', p.name, new[:50]); continue
    hits = [i for i, l in enumerate(lines) if l.strip() == old.strip()]
    if len(hits) != expect: print('ANCHOR', len(hits), p, old[:70]); sys.exit(1)
    for i in reversed(hits):
        ind = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
        lines[i:i + 1] = ['#if defined( __wasm32__ )', ind + new, '#else', lines[i], '#endif']
    out = '\n'.join(lines)
    p.write_bytes((out.replace('\n', '\r\n') if crlf else out).encode('utf-8', errors='surrogateescape'))
    print('patched', p.name, '|', new[:60])
