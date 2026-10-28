/**
* Copyright (c) 2013 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "redIOSystemFileLinuxAPI.h"

#if ( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )

#include "../../redSystem/include/threads.h"
#include <utility>
#include <string.h>

#ifdef RED_PLATFORM_WEB
// [hitch] Off-main-thread archive reads. Every ::read on a pthread is a SYNCHRONOUS proxy to the browser main
// thread, where resource-filesystem-web.js serves original game files with a synchronous XHR per 1 MiB block
// (39 s of main-thread blocking in 262 s of sandbox play, [compress]). While that XHR runs, the main thread
// cannot present, encode GPU work or service any other worker proxy, so the whole game freezes.
// For read-only original resource files (/runtime/game-files.json entries) the read now runs on the CALLING
// pthread: an EM_JS (executed on the caller, not proxied) issues the same ranged, If-Match-checked request as a
// synchronous XHR inside that worker, into a small per-worker block cache. The file position is kept here per
// descriptor. The descriptor is still opened through ::open (existence, fstat, close semantics unchanged).
// Any failure falls back to the original ::lseek + ::read path. ?syncResIO=1 restores the old path.
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <mutex>
#include <unordered_map>

EM_JS(int, CpResLookup, (const char* path), {
    if (!ENVIRONMENT_IS_PTHREAD || typeof XMLHttpRequest === 'undefined') return -1;
    const R = globalThis.__cpResIO || (globalThis.__cpResIO = {files: null, byPath: null, cache: new Map(), bytes: 0,
        stats: {reads: 0, bytes: 0, ms: 0, maxMs: 0, requests: 0, hits: 0, failures: 0}});
    if (!R.files) {
        try {
            const x = new XMLHttpRequest();
            x.open('GET', '/runtime/game-files.json', false);
            x.send(null);
            if (x.status !== 200) return -1;
            const manifest = JSON.parse(x.responseText);
            if (manifest.version !== 1 || !Array.isArray(manifest.files)) return -1;
            R.files = manifest.files; R.byPath = new Map();
            manifest.files.forEach((f, i) => R.byPath.set(f.path, i));
        } catch (e) { return -1; }
    }
    const index = R.byPath.get(UTF8ToString(Number(path)));
    return index === undefined ? -1 : index;
});
EM_JS(double, CpResSize, (int index), {
    const R = globalThis.__cpResIO; const f = R && R.files && R.files[index];
    return f ? f.size : -1;
});
// Returns bytes copied (0 at EOF) or -1 (caller falls back to the main-thread path).
EM_JS(int, CpResRead, (int index, double offset, void* dest, int length), {
    const R = globalThis.__cpResIO; if (!R || !R.files) return -2; // this worker has not loaded the manifest yet
    const f = R.files[index];
    if (!f || !(offset >= 0) || length < 0) return -1;
    const count = Math.min(length, Math.max(0, f.size - offset));
    if (count === 0) return 0;
    const began = performance.now();
    const url = f.path.split('/').map(encodeURIComponent).join('/');
    const fetchRange = (start, end) => {
        try {
            const x = new XMLHttpRequest();
            x.open('GET', url, false);
            x.responseType = 'arraybuffer';
            x.setRequestHeader('Range', 'bytes=' + start + '-' + end);
            x.setRequestHeader('If-Match', f.etag);
            x.send(null);
            ++R.stats.requests;
            if (x.status !== 206 || x.getResponseHeader('ETag') !== f.etag ||
                x.getResponseHeader('Content-Range') !== 'bytes ' + start + '-' + end + '/' + f.size) return null;
            const bytes = new Uint8Array(x.response);
            return bytes.length === end - start + 1 ? bytes : null;
        } catch (e) { return null; }
    };
    const blockSize = 1048576, cacheLimit = 16 * 1048576;
    const block = number => {
        const key = index + ':' + number;
        const cached = R.cache.get(key);
        if (cached) { R.cache.delete(key); R.cache.set(key, cached); ++R.stats.hits; return cached; }
        const start = number * blockSize, end = Math.min(start + blockSize, f.size) - 1;
        const bytes = fetchRange(start, end);
        if (!bytes) return null;
        while (R.bytes + bytes.length > cacheLimit && R.cache.size) {
            const oldest = R.cache.keys().next().value;
            R.bytes -= R.cache.get(oldest).length; R.cache.delete(oldest);
        }
        R.cache.set(key, bytes); R.bytes += bytes.length;
        return bytes;
    };
    const target = Number(dest);
    let copied = 0;
    if (count >= 524288) {
        // Large reads: fetch the exact range straight into the engine heap (no block-cache copy).
        const bytes = fetchRange(offset, offset + count - 1);
        if (!bytes) { ++R.stats.failures; return -1; }
        growMemViews(); HEAPU8.set(bytes, target); copied = count;
    } else {
        while (copied < count) {
            const at = offset + copied, bytes = block(Math.floor(at / blockSize));
            if (!bytes) { ++R.stats.failures; return -1; }
            const within = at % blockSize, take = Math.min(count - copied, bytes.length - within);
            if (take <= 0) { ++R.stats.failures; return -1; }
            growMemViews(); HEAPU8.set(bytes.subarray(within, within + take), target + copied);
            copied += take;
        }
    }
    const ms = performance.now() - began;
    ++R.stats.reads; R.stats.bytes += copied; R.stats.ms += ms; if (ms > R.stats.maxMs) R.stats.maxMs = ms;
    if ((R.stats.reads & 1023) === 0)
        err('CP2077 off-main io probe: reads=' + R.stats.reads + ' MiB=' + (R.stats.bytes / 1048576).toFixed(1) +
            ' ms=' + R.stats.ms.toFixed(0) + ' maxMs=' + R.stats.maxMs.toFixed(1) + ' requests=' + R.stats.requests +
            ' blockHits=' + R.stats.hits + ' failures=' + R.stats.failures);
    return copied;
});

// [ioahead] lookahead hint: the async IO worker announces the byte range of a queued op; the bundle's io layer
// (cp2077IoHint, cp2077.js) asks the IO helper to fetch the missing blocks in parallel into the shared cache.
EM_JS(int, CpResHintJS, (int index, double offset, double length), {
    return typeof cp2077IoHint === 'function' ? cp2077IoHint(index, offset, length) : 0;
});

namespace
{
	struct CpResFile { int index; long long position; long long size; };
	std::mutex cpResMutex;
	std::unordered_map< int, CpResFile > cpResFiles;
	bool CpResEnabled()
	{
		static const bool enabled = []
		{
			const int value = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('syncResIO') === '1' ? 0 : 1; });
			fprintf( stderr, "CP2077 hitch switches: offMainResIO=%d\n", value );
			return value != 0;
		}();
		return enabled;
	}
	bool CpResGet( int fd, CpResFile& out )
	{
		std::lock_guard< std::mutex > lock( cpResMutex );
		auto found = cpResFiles.find( fd );
		if ( found == cpResFiles.end() ) return false;
		out = found->second; return true;
	}
	void CpResSetPosition( int fd, long long position )
	{
		std::lock_guard< std::mutex > lock( cpResMutex );
		auto found = cpResFiles.find( fd );
		if ( found != cpResFiles.end() ) found->second.position = position;
	}
	void CpResForget( int fd )
	{
		std::lock_guard< std::mutex > lock( cpResMutex );
		cpResFiles.erase( fd );
	}
	void CpResRegister( int fd, const char* path, int descriptorFlags )
	{
		if ( ( descriptorFlags & O_ACCMODE ) != O_RDONLY || emscripten_is_main_runtime_thread() || !CpResEnabled() ) return;
		char resolved[ PATH_MAX ];
		if ( !::realpath( path, resolved ) ) return;
		const int index = CpResLookup( resolved );
		if ( index < 0 ) return;
		const double size = CpResSize( index );
		if ( size < 0 ) return;
		std::lock_guard< std::mutex > lock( cpResMutex );
		cpResFiles[ fd ] = CpResFile{ index, 0, static_cast< long long >( size ) };
	}
}
#endif

#ifdef RED_PLATFORM_WEB
namespace io { namespace linux {
	// ?ioLookahead=N (2..32) with ?ioShared=1: async IO worker fill depth; 1 (original) otherwise.
	int CpIoLookahead()
	{
		const int value = MAIN_THREAD_EM_ASM_INT({
			const q = new URLSearchParams(location.search); const v = Math.floor(Number(q.get('ioLookahead')));
			if (q.get('ioShared') !== '1' || !(v >= 2)) return 1; return v > 32 ? 32 : v;
		});
		fprintf( stderr, "CP2077 io lookahead: %d\n", value );
		return value;
	}
	void CpResHint( int fd, long long offset, unsigned length )
	{
		CpResFile resource;
		if ( length == 0 || offset < 0 || !CpResGet( fd, resource ) ) return;
		CpResHintJS( resource.index, static_cast< double >( offset ), static_cast< double >( length ) );
	}
} }
#endif

#define ERROR_MESSAGE( errorBuffer )			\
	Int32 errorCode = errno;					\
	char errorBuffer[256];						\
	strerror_r( errorCode, errorBuffer, 256 );

namespace io
{

	Uint32 LastError()
	{
		#ifdef RED_PLATFORM_WEB
        return static_cast<Uint32>( errno );
#else
        // TODO
        return 0;
#endif
	}

	namespace linux
	{
		const Int32 c_invalidDescriptor = -1;

		//////////////////////////////////////////////////////////////////////////
		// SystemFile
		//////////////////////////////////////////////////////////////////////////
		SystemFile::SystemFile()
			: m_fileDescriptor( c_invalidDescriptor )
		{
		}

		SystemFile::SystemFile( SystemFile&& other )
			: m_fileDescriptor( other.m_fileDescriptor )
		{
			other.m_fileDescriptor = c_invalidDescriptor;
		}

		SystemFile& SystemFile::operator=( SystemFile&& other )
		{
			if ( this != &other )
			{
				Close();
				std::swap( m_fileDescriptor, other.m_fileDescriptor );
			}
			return *this;
		}

		SystemFile::~SystemFile()
		{
			Close();
		}

		Bool SystemFile::Open( const char* path, Uint32 openFlags )
		{
			RED_ASSERT( m_fileDescriptor == c_invalidDescriptor );

			Int32 descriptorFlags = 0;

			if ( ( openFlags & ( eOpenFlag_Read | eOpenFlag_Write ) ) == ( eOpenFlag_Read | eOpenFlag_Write ) )
			{
				descriptorFlags |= O_RDWR;
			}
			else
			{
				if ( openFlags & eOpenFlag_Read )
				{
					descriptorFlags |= O_RDONLY;
				}
				if ( openFlags & eOpenFlag_Write )
				{
					descriptorFlags |= O_WRONLY;
				}
			}

			if ( openFlags & eOpenFlag_Append )
			{
				descriptorFlags |= O_APPEND;
			}
			if ( openFlags & eOpenFlag_Create )
			{
				descriptorFlags |= O_CREAT;
			}

			// #tbd: check if O_RDONLY is set and fail?
			if ( openFlags & eOpenFlag_Truncate )
			{
				descriptorFlags |= O_TRUNC;
			}

			m_fileDescriptor = ::open( path, descriptorFlags, S_IRUSR | S_IWUSR );
			if ( m_fileDescriptor == c_invalidDescriptor )
			{
				ERROR_MESSAGE( errorBuffer );
				RED_LOG_ERROR( "RedIO: open failed to open '%hs', openFlags=%d, error=%hs", path, descriptorFlags, errorBuffer );
				return false;
			}
#ifdef RED_PLATFORM_WEB
			CpResRegister( m_fileDescriptor, path, descriptorFlags );
#endif

			return true;
		}

		Bool SystemFile::Close()
		{
			if ( m_fileDescriptor != c_invalidDescriptor )
			{
#ifdef RED_PLATFORM_WEB
				CpResForget( m_fileDescriptor );
#endif
				const Int32 err = ::close( m_fileDescriptor );
				m_fileDescriptor = c_invalidDescriptor;
				if ( err != 0 )
				{
					return false;
				}
			}

			return true;
		}

		Bool SystemFile::Read( void* dest, Uint32 length, Uint32& outNumberOfBytesRead )
		{
#ifdef RED_PLATFORM_WEB
			CpResFile resource;
			if ( CpResGet( m_fileDescriptor, resource ) )
			{
				const int chunk = length > 0x7fffffffu ? 0x7fffffff : static_cast< int >( length );
				int copied = CpResRead( resource.index, static_cast< double >( resource.position ), dest, chunk );
				if ( copied == -2 ) { CpResLookup( "" ); copied = CpResRead( resource.index, static_cast< double >( resource.position ), dest, chunk ); }
				if ( copied >= 0 )
				{
					CpResSetPosition( m_fileDescriptor, resource.position + copied );
					outNumberOfBytesRead = static_cast< Uint32 >( copied );
					return true;
				}
				// Off-main read failed: forget the descriptor and continue on the original path from this position.
				CpResForget( m_fileDescriptor );
				::lseek( m_fileDescriptor, resource.position, SEEK_SET );
			}
#endif
			const ssize_t readSize = ::read( m_fileDescriptor, dest, length );
			if ( readSize < 0 )
			{
				ERROR_MESSAGE( errorBuffer );
				RED_LOG_ERROR( "RedIO: fread failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
				outNumberOfBytesRead = 0;
				return false;
			}

			outNumberOfBytesRead = static_cast< Uint32 >( readSize );

			return true;
		}

		Bool SystemFile::Write( const void* src, Uint32 length, Uint32& outNumberOfBytesWritten )
		{
			const ssize_t writeSize = ::write( m_fileDescriptor, src, length );
			if ( writeSize < 0 )
			{
				ERROR_MESSAGE( errorBuffer );
				RED_LOG_ERROR( "RedIO: fwrite failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
				outNumberOfBytesWritten = 0;
				return false;
			}

			outNumberOfBytesWritten = static_cast< Uint32 >( writeSize );

			return true;
		}

		Bool SystemFile::Seek( Int64 offset, ESeekOrigin seekOrigin )
		{
			static_assert( static_cast< Int32 >( eSeekOrigin_Set ) == SEEK_SET, "Enum mismatch" );
			static_assert( static_cast< Int32 >( eSeekOrigin_Current ) == SEEK_CUR, "Enum mismatch" );
			static_assert( static_cast< Int32 >( eSeekOrigin_End ) == SEEK_END, "Enum mismatch" );

#ifdef RED_PLATFORM_WEB
			CpResFile resource;
			if ( CpResGet( m_fileDescriptor, resource ) )
			{
				const long long base = seekOrigin == eSeekOrigin_Set ? 0 : seekOrigin == eSeekOrigin_Current ? resource.position : resource.size;
				const long long target = base + offset;
				if ( target >= 0 ) { CpResSetPosition( m_fileDescriptor, target ); return true; }
				errno = EINVAL;
				RED_LOG_ERROR( "RedIO: fseek failed for descriptor %d (negative position)", m_fileDescriptor );
				return false;
			}
#endif
			const Int64 newPos = ::lseek( m_fileDescriptor, offset, static_cast< Int32 >( seekOrigin ) );

			if ( newPos >= 0 )
			{
				return true;
			}

			ERROR_MESSAGE( errorBuffer );
			RED_LOG_ERROR( "RedIO: fseek failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
			return false;
		}

		Int64 SystemFile::Tell() const
		{
#ifdef RED_PLATFORM_WEB
			CpResFile resource;
			if ( CpResGet( m_fileDescriptor, resource ) ) return resource.position;
#endif
			const Int64 pos = ::lseek( m_fileDescriptor, 0, SEEK_CUR );
			if ( pos >= 0 )
			{
				return pos;
			}

			ERROR_MESSAGE( errorBuffer );
			RED_LOG_ERROR( "RedIO: ftell failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
			return -1;
		}

		Bool SystemFile::Truncate( Uint64 totalFileSize )
		{
			const auto size = static_cast< off_t >( totalFileSize );
			if ( size < 0 )
			{
				return false;
			}
			const Int32 err = ::ftruncate( m_fileDescriptor, size );
			if ( err == 0 )
			{
				return true;
			}

			ERROR_MESSAGE( errorBuffer );
			RED_LOG_ERROR( "RedIO: ftruncate failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
			return false;
		}

		Bool SystemFile::IsValid() const
		{
			return m_fileDescriptor != c_invalidDescriptor;
		}

		Bool SystemFile::Flush()
		{
			const Int32 err = ::fsync( m_fileDescriptor );
			if ( err == 0 )
			{
				return true;
			}

			ERROR_MESSAGE( errorBuffer );
			RED_LOG_ERROR( "RedIO: fflush failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
			return false;
		}

		Uint64 SystemFile::GetFileSize() const
		{
			struct stat stat;
			const Int32 err = ::fstat( m_fileDescriptor, &stat );
			if ( err != 0 )
			{
				ERROR_MESSAGE( errorBuffer );
				RED_LOG_ERROR( "RedIO: fstat failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
				return 0;
			}

			return stat.st_size;
		}

		Uint64 SystemFile::GetFileTimestamp() const
		{
			struct stat stat;
			const Int32 err = ::fstat( m_fileDescriptor, &stat );
			if ( err != 0 )
			{
				ERROR_MESSAGE( errorBuffer );
				RED_LOG_ERROR( "RedIO: fstat failed for descriptor %d. Error=%hs", m_fileDescriptor, errorBuffer );
				return 0;
			}

			// Unix time epoch stores
			// nanoseconds since 00:00:00 1 Jan 1970 UTC
			// Convert it to Windows timestamp which is counted in
			// 100 nanosecond intervals since 00:00:00 1 Jan 1601 UTC
			const Uint64 epochDifference = 116444736000000000ull;
			#ifdef RED_PLATFORM_WEB
            // POSIX stat uses seconds plus nanoseconds, FILETIME uses 100 ns ticks.
            const Int64 unixTicks = static_cast<Int64>(stat.st_mtim.tv_sec) * 10000000ll
                + static_cast<Int64>(stat.st_mtim.tv_nsec) / 100ll;
            return static_cast<Uint64>(unixTicks + static_cast<Int64>(epochDifference));
#else
            return ( stat.st_mtime / 100 ) - epochDifference;
#endif
		}

		Uint32 SystemFile::GetFileHandle() const
		{
			return static_cast< Uint32 >( m_fileDescriptor );
		}

		Uint32 SystemFile::GetFileID() const
		{
			return GetFileHandle();
		}

	}
}

#else
	RED_NO_EMPTY_FILE();
#endif // #if ( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )