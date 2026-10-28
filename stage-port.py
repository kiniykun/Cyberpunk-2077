"""Stage the original engine bootstrap with browser loop adaptations.

This does not replace the engine with a viewer. Original depot files stay on D:.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parent
SOURCE = Path(r"D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src")
OVERLAY = ROOT / "engine-overlay"

import datetime, os, shutil
STAGE_BACKUP = ROOT / 'build' / 'stage-port-backups' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')

def write_changed(path, text):
    if path.exists() and path.read_text(encoding='utf-8', errors='replace') == text:
        return
    if path.exists():
        # Overlays are also edited by later build scripts. Never discard such edits silently: keep the
        # replaced file so it can be compared and restored (2026-10-13: nine hand-edited overlays were lost).
        backup = STAGE_BACKUP / path.resolve().relative_to(ROOT)
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup)
        print('stage-port: replaced differing file, previous copy kept at', backup)
    temporary = path.with_name(path.name + '.staging')
    temporary.write_text(text, encoding='utf-8', newline='\n')
    temporary.replace(path)

def stage(relative, transform):
    # Normalize '..' without following the D:\cp2077SourceCode junction: compilers open sources through the
    # alias, so VFS names must use it too or the overlay is silently skipped.
    original = Path(os.path.normpath(SOURCE / relative))
    data = original.read_bytes()
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = data.decode("cp1252")
    text = text.replace("\r\n", "\n")
    adapted = transform(text)
    output = OVERLAY / relative
    output.parent.mkdir(parents=True, exist_ok=True)
    write_changed(output, adapted)
    return {"source": str(original), "sourceSHA256": hashlib.sha256(data).hexdigest(),
            "overlay": str(output), "overlaySHA256": hashlib.sha256(output.read_bytes()).hexdigest()}

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Source changed: expected exactly one match for {old[:70]!r}")
    return text.replace(old, new, 1)

def adapt_browser_viewport(text):
    text = replace_once(text, '#include "renderViewportWindowPC.h"', '#include "renderViewportWindowPC.h"\n#ifdef __EMSCRIPTEN__\n#include "viewport-window-web.h"\n#endif')
    return replace_once(text, '#if defined RED_PLATFORM_WINPC\n\twindow =', '#if defined __EMSCRIPTEN__\n\t{\n\t\tauto browserWindow = red::CreateUniquePtr<ViewportWindowWeb>(width, height);\n\t\tif (!browserWindow->IsReady()) return ViewportHandle();\n\t\twindow = std::move(browserWindow);\n\t}\n#elif defined RED_PLATFORM_WINPC\n\twindow =')

def adapt_launcher(text):
    # States must outlive mainRed once Run yields to requestAnimationFrame.
    text = replace_once(text, 'extern void InitModuleDependencies();', '''extern void InitModuleDependencies();

#ifdef __EMSCRIPTEN__
# define CP_BROWSER_LIFETIME static
#else
# define CP_BROWSER_LIFETIME
#endif''')
    for declaration in [
        'red::GameAppInstance game{ "cpLauncher" };',
        'FinalGameAppBaseInitializationState gameBaseInitState;',
        'red::GameAppInitializationState gameInitState;',
        'red::GameAppRunningState gameRunningState;',
        'red::GameAppShutdownState shutdownState;',
    ]:
        text = replace_once(text, declaration, 'CP_BROWSER_LIFETIME ' + declaration)
    return text

def adapt_loop(text):
    text = replace_once(text, '#include "gameApp.h"', '''#include "gameApp.h"
#ifdef __EMSCRIPTEN__
# include <emscripten/emscripten.h>
#endif''')
    begin = '\tInt32 GameAppInstance::Run()\n\t{\n'
    browser = '''\tInt32 GameAppInstance::Run()
\t{
#ifdef __EMSCRIPTEN__
\t\t// Keep the original state machine, scheduling one iteration per browser frame.
\t\t// Browser PumpMessages and rendering backends still need implementation.
\t\temscripten_set_main_loop_arg( []( void* context )
\t\t{
\t\t\tauto& game = *static_cast<GameAppInstance*>( context );
\t\t\tif ( !game.PumpMessages() )
\t\t\t{
\t\t\t\tRED_LOG_ERROR( "[GameApplication] PumpMessages failed. Requesting for game exit" );
\t\t\t\tGEngine->RequestExit( -66 );
\t\t\t}
\t\t\tgame.TickStates();
\t\t\tif ( !game.m_currentState )
\t\t\t{
\t\t\t\temscripten_cancel_main_loop();
\t\t\t}
\t\t}, this, 0, false );
\t\treturn 0; // Loop scheduled; this is not the final game return value.
#else
'''
    text = replace_once(text, begin, browser)
    text = replace_once(text, '\t\treturn m_returnValue;\n\t}\n', '\t\treturn m_returnValue;\n#endif\n\t}\n')
    text = '#ifdef RED_PLATFORM_WEB\n#include <cstdio>\n#endif\n' + text
    text = replace_once(text, 'case StateContext::StateContext_EnterState:', '''case StateContext::StateContext_EnterState:
#ifdef RED_PLATFORM_WEB
                std::fprintf(stderr, "CP2077 native state enter: %s\\n", m_currentState->GetName());
#endif''')
    text = replace_once(text, 'case StateContext::StateContext_ExitState:', '''case StateContext::StateContext_ExitState:
#ifdef RED_PLATFORM_WEB
                std::fprintf(stderr, "CP2077 native state exit: %s\\n", m_currentState->GetName());
#endif''')
    return text

def adapt_architecture(text):
    text = replace_once(text, '#if defined( _DURANGO )', '''#if defined( __EMSCRIPTEN__ )
# define RED_PLATFORM_WEB
#elif defined( _DURANGO )''')
    text = replace_once(text, '#if defined( _M_X64 ) || defined( __x86_64__ )', '''#if defined( __wasm64__ )
# define RED_ARCH_WASM64
# define RED_ENDIAN_LITTLE
#elif defined( __wasm32__ )
# define RED_ARCH_WASM32
# define RED_ENDIAN_LITTLE
#elif defined( _M_X64 ) || defined( __x86_64__ )''')
    text = replace_once(text, '#\tinclude <x86intrin.h>', '''# ifdef __EMSCRIPTEN__
#  include <smmintrin.h>
# else
#  include <x86intrin.h>
# endif''')
    return text

def adapt_os(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '''#elif defined( RED_PLATFORM_WEB )
# include <cstdarg>
# include <utility>
# include <fcntl.h>
# include <unistd.h>
# include <time.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <pthread.h>
#elif defined( RED_PLATFORM_LINUX )''')

def adapt_crt(text):
    # Reuse the existing libc paths; Web is not treated as a Linux OS elsewhere.
    if text.count('#ifdef RED_PLATFORM_LINUX') != 2:
        raise RuntimeError('CRT platform branches changed')
    text = text.replace('#ifdef RED_PLATFORM_LINUX', '#if defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')
    text = replace_once(text, '#if defined(RED_PLATFORM_LINUX)', '#if defined(RED_PLATFORM_LINUX) || defined(RED_PLATFORM_WEB)')
    text = text.replace('#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX )', '#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')
    # The original Linux VSPrintF ignores count for narrow text. Keep bounds
    # on the new browser path instead of inheriting that buffer overrun risk.
    text = replace_once(text, 'return ::vsprintf( buffer, format, arg );', '''#ifdef RED_PLATFORM_WEB
        return ::vsnprintf( buffer, count, format, arg );
#else
        return ::vsprintf( buffer, format, arg );
#endif''')

    text = '#ifdef __EMSCRIPTEN__\n#include "utf16-runtime.h"\n#define CPWEB_WIDE(name) ::cpweb::name\n#else\n#define CPWEB_WIDE(name) ::name\n#endif\n' + text
    import re
    for name in ['wcslen','wcsnlen','wcsncpy','wcsncat','wcscmp','wcsncmp','wcschr','wcsrchr','wcsstr','wcstol','wcstoul','vswprintf']:
        text = re.sub(r'(?:std::|::)' + name + r'\b', 'CPWEB_WIDE(' + name + ')', text)
    text = text.replace('Int32 retval = VSNPrintF( buffer, count, format, arglist );', 'Int32 retval = buffer ? cpweb::vsnprintf_ansi( buffer, count, format, arglist ) : -1;', 1)
    # Windows narrow %hs formatting is used by native assertion messages too.
    text = text.replace('return ::vsnprintf( buffer, count, format, arg );',
                        'return cpweb::vsnprintf_ansi( buffer, count, format, arg );')
    return text

def adapt_thread_id(text):
    text = replace_once(text, '#include "redSystemPublic.h"', '#include "redSystemPublic.h"\n#ifdef RED_PLATFORM_WEB\n#include <atomic>\n#endif')
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '''#elif defined( RED_PLATFORM_WEB )
            // Engine IDs are 32-bit; wasm64 pthread handles are not. Assign a
            // distinct engine ID per worker instead of truncating a pointer.
            static std::atomic<Uint32> nextId{ 1 };
            static thread_local const Uint32 workerId = nextId.fetch_add( 1, std::memory_order_relaxed );
            id = workerId;
#elif defined( RED_PLATFORM_LINUX )''')

def adapt_debug_attachment(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '#elif defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')

def adapt_debug_macros(text):
    text = replace_once(text, '// portable RED_DEBUG_BREAK macro', '''#ifdef RED_PLATFORM_WEB
#include <emscripten/emscripten.h>
#endif
// portable RED_DEBUG_BREAK macro''')
    text = replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n# define RED_DEBUG_BREAK()', '#elif defined( RED_PLATFORM_WEB )\n# define RED_DEBUG_BREAK() emscripten_debugger()\n#elif defined( RED_PLATFORM_LINUX )\n# define RED_DEBUG_BREAK()')
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n#\tinclude <sys/types.h>', '#elif defined( RED_PLATFORM_WEB )\n# define RED_BREAKPOINT() emscripten_debugger()\n#elif defined( RED_PLATFORM_LINUX )\n#\tinclude <sys/types.h>')

def adapt_bit_utils(text):
    text = replace_once(text, '\t#include <x86intrin.h>', '#elif defined( RED_PLATFORM_WEB )\n    // Integer bit operations use compiler builtins on WebAssembly.\n#else\n\t#include <x86intrin.h>').replace('\n#else\n#elif defined( RED_PLATFORM_WEB )', '\n#elif defined( RED_PLATFORM_WEB )')
    text = text.replace('#ifdef RED_ARCH_X64', '#if defined( RED_ARCH_X64 ) || defined( RED_ARCH_WASM64 ) || defined( RED_ARCH_WASM32 )')
    for original, portable in [
        ('__tzcnt_u32( mask )', '__builtin_ctz( mask )'),
        ('__tzcnt_u64( mask )', '__builtin_ctzll( mask )'),
        ('__lzcnt32( mask )', '__builtin_clz( mask )'),
        ('__lzcnt64( mask )', '__builtin_clzll( mask )'),
    ]:
        # Clang builtins implement the same nonzero operation on all targets;
        # the original zero guards remain intact.
        text = replace_once(text, original, portable)
    text = replace_once(text, '__builtin_popcountl( mask )', '__builtin_popcountll( mask )')
    return replace_once(text, 'return ( value << shift ) | ( value >> ( 64 - shift ));', '''#ifdef RED_PLATFORM_WEB
                return __builtin_rotateleft64( value, static_cast<unsigned>( shift ) );
#else
                return ( value << shift ) | ( value >> ( 64 - shift ));
#endif''')

def adapt_barrier(text):
    return replace_once(text, '#elif defined ( RED_COMPILER_CLANG )', '''#elif defined( RED_PLATFORM_WEB )
# include <atomic>
# define RED_THREADS_MEMORY_BARRIER() std::atomic_thread_fence( std::memory_order_seq_cst )
#elif defined ( RED_COMPILER_CLANG )''')

def adapt_pthread_selection(text):
    # Reuse the existing pthread implementation, without changing the OS macro
    # to Linux. Its platform-specific implementation calls still need porting.
    import re
    return re.sub(r'defined\s*\(\s*RED_PLATFORM_LINUX\s*\)',
                  '( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )', text)

def adapt_atomic_libc(text):
    text = text.replace('#ifdef RED_ARCH_X64', '#if defined( RED_ARCH_X64 ) || defined( RED_ARCH_WASM64 )')
    text = replace_once(text, '#define RED_THREADS_ATOMIC_MEMORDER __ATOMIC_RELAXED', '''#ifdef RED_PLATFORM_WEB
#define RED_THREADS_ATOMIC_MEMORDER __ATOMIC_SEQ_CST
#else
#define RED_THREADS_ATOMIC_MEMORDER __ATOMIC_RELAXED
#endif''')
    # A volatile load alone is not an inter-worker atomic load in the C++ model.
    for width in (8, 16, 32, 64):
        old = f'return *( volatile TAtomic{width}* )( destination );'
        new = f'''#ifdef RED_PLATFORM_WEB
                return __atomic_load_n( destination, RED_THREADS_ATOMIC_MEMORDER );
#else
                {old}
#endif'''
        # The source uses one-line functions, so preprocessor directives must
        # start on separate lines rather than inside that single physical line.
        text = replace_once(text, old, '\n' + new + '\n')
    text = replace_once(text, 'return *( volatile TAtomicPtr* )( destination );', '''#ifdef RED_PLATFORM_WEB
                return __atomic_load_n( destination, RED_THREADS_ATOMIC_MEMORDER );
#else
                return *( volatile TAtomicPtr* )( destination );
#endif''')
    return text

def adapt_timer_selection(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '#elif defined( RED_PLATFORM_WEB )\n# include "timerWeb.h"\n#elif defined( RED_PLATFORM_LINUX )')

def adapt_crash_frame(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX )', '''#elif defined( RED_PLATFORM_WEB )
#include <emscripten/stack.h>
#define DEBUG_FRAME_ADDR() static_cast<Uint64>( emscripten_stack_get_current() )
#elif defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX )''')

def adapt_light_mutex(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '''#elif defined( RED_PLATFORM_WEB )
        const Uint32 currentThreadId = red::ThreadId::CurrentThread().AsNumber();
#elif defined( RED_PLATFORM_LINUX )''')

def adapt_memory_includes(text):
    return replace_once(text, '#ifndef RED_PLATFORM_LINUX\n#\tinclude <xstddef>', '#if !defined( RED_PLATFORM_LINUX ) && !defined( RED_PLATFORM_WEB )\n#\tinclude <xstddef>')

def adapt_memory_thread_id(text):
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', '''#elif defined( RED_PLATFORM_WEB )
        const ThreadId id = static_cast<ThreadId>( ::red::ThreadId::CurrentThread().AsNumber() );
#elif defined( RED_PLATFORM_LINUX )''')

def adapt_crash_path(text):
    return replace_once(text, 'using StorageType = char[MAX_PATH];', '''#ifdef RED_PLATFORM_WEB
    using StorageType = char[260]; // Same crash-record capacity as Win32 MAX_PATH.
#else
    using StorageType = char[MAX_PATH];
#endif''')

def adapt_engine_time(text):
    text = adapt_pthread_selection(text)
    text = text.replace('#ifdef RED_PLATFORM_LINUX', '#if defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')
    if '# define ENGINE_TIME_CLOCK CLOCK_MONOTONIC_RAW' in text:
        text = replace_once(text, '# define ENGINE_TIME_CLOCK CLOCK_MONOTONIC_RAW', '''#ifdef RED_PLATFORM_WEB
# define ENGINE_TIME_CLOCK CLOCK_MONOTONIC
#else
# define ENGINE_TIME_CLOCK CLOCK_MONOTONIC_RAW
#endif''')
    return text

def adapt_hashmap_alignment(text):
    return replace_once(text, 'Max( std::alignment_of<TKey>(), std::alignment_of<TValue>() )', '( alignof(TKey) > alignof(TValue) ? alignof(TKey) : alignof(TValue) )')

def adapt_system_file(text):
    text = adapt_pthread_selection(text)
    # read() returns a signed count or -1; preserve actual failures on Web.
    text = replace_once(text, 'const size_t readSize = ::read(', 'const ssize_t readSize = ::read(')
    text = replace_once(text, '// TODO\n\t\treturn 0;', '''#ifdef RED_PLATFORM_WEB
        return static_cast<Uint32>( errno );
#else
        // TODO
        return 0;
#endif''')
    return replace_once(text, 'return ( stat.st_mtime / 100 ) - epochDifference;', '''#ifdef RED_PLATFORM_WEB
            // POSIX stat uses seconds plus nanoseconds, FILETIME uses 100 ns ticks.
            const Int64 unixTicks = static_cast<Int64>(stat.st_mtim.tv_sec) * 10000000ll
                + static_cast<Int64>(stat.st_mtim.tv_nsec) / 100ll;
            return static_cast<Uint64>(unixTicks + static_cast<Int64>(epochDifference));
#else
            return ( stat.st_mtime / 100 ) - epochDifference;
#endif''')

def adapt_dynamic_alignment(text):
    return replace_once(text, 'AlignUp( offset, __alignof( void* ) )', 'AlignUp( offset, static_cast<Uint64>( alignof(void*) ) )')

def adapt_locale_conversion(text):
    text = adapt_pthread_selection(text)
    # Emscripten's mbstate_t is a C struct, not the native Clang SDK class.
    text = text.replace('mbstate_t mbst; // has a ctor for C++', 'mbstate_t mbst{}; // Explicitly initialize conversion state.')
    text = text.replace('mbstate_t mbst;\n', 'mbstate_t mbst{};\n')
    text = '#ifdef __EMSCRIPTEN__\n#include "utf16-runtime.h"\n#define CPWEB_CONVERT(name) ::cpweb::name\n#else\n#define CPWEB_CONVERT(name) ::name\n#endif\n' + text
    import re
    for name in ['mbsrtowcs', 'wcsrtombs']:
        text = re.sub(r'(?<![A-Za-z_])' + name + r'\s*\(', 'CPWEB_CONVERT(' + name + ')(', text)
    return text


def adapt_crt_source(text):
    for native, web, source_type in [('wcstombs', 'wcsrtombs', 'UniChar'), ('mbstowcs', 'mbsrtowcs', 'AnsiChar')]:
        old = '#ifdef RED_PLATFORM_LINUX\n\t\tsize_t errval = ::' + native + '( dest, source, destSize );'
        new = '#ifdef RED_PLATFORM_WEB\n        if (destSize == 0) return 0;\n        const ' + source_type + '* cursor = source;\n        mbstate_t state{};\n        charsWritten = cpweb::' + web + '( dest, &cursor, destSize - 1, &state );\n        if (charsWritten == size_t(-1)) { dest[0] = 0; return 0; }\n        dest[charsWritten] = 0;\n        size_t errval = 0;\n#elif defined( RED_PLATFORM_LINUX )\n\t\tsize_t errval = ::' + native + '( dest, source, destSize );'
        text = replace_once(text, old, new)
    return text

def adapt_file_text(text):
    text = adapt_pthread_selection(text)
    return replace_once(text, '::fputws( buffer, handle );', '''#ifdef RED_PLATFORM_WEB
        const UniChar* cursor = buffer;
        mbstate_t state{};
        size_t size = cpweb::wcsrtombs(nullptr, &cursor, 0, &state);
        if (size == size_t(-1)) return;
        std::string encoded(size + 1, '\0');
        cpweb::wcsrtombs(&encoded[0], &cursor, encoded.size(), &state);
        ::fwrite(encoded.data(), 1, size, handle);
#else
        ::fputws( buffer, handle );
#endif''').replace('#include <stdio.h>', '#include <stdio.h>\n#ifdef RED_PLATFORM_WEB\n#include <string>\n#endif')

def adapt_log(text):
    return text.replace('defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX )', 'defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')

def adapt_logger(text):
    return text.replace('defined( RED_PLATFORM_LINUX )', 'defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')

def adapt_error_web_hooks(text):
    text = replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n# include "errorHandlerImplErrorHooksLinux.h"', '#elif defined( RED_PLATFORM_WEB )\n# include "error-hooks-web.h"\n#elif defined( RED_PLATFORM_LINUX )\n# include "errorHandlerImplErrorHooksLinux.h"')
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n\t\t\tresult = RegisterErrorHooksOnceLinux', '#elif defined( RED_PLATFORM_WEB )\n            result = RegisterErrorHooksOnceWeb(errorHandlerFlags, APP_VERSION_NUMBER, scriptCallstackVisitor);\n\t#elif defined( RED_PLATFORM_LINUX )\n\t\t\tresult = RegisterErrorHooksOnceLinux')

def adapt_thread_source(text):
    text = '''#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#include <atomic>
#include <cstdio>
namespace {
void ReportBrowserAffinityLimit()
{
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set(std::memory_order_relaxed))
        std::fputs("CP2077: CPU affinity hints are unavailable; browser workers use host scheduling.\\n", stderr);
}
}
#endif
''' + text
    for affinity_call in [
        'REDTHR_PTHREAD_CHECK( ::pthread_setaffinity_np( pthread_self(), sizeof( cpuset ), &cpuset ) );',
        'REDTHR_PTHREAD_CHECK( ::pthread_setaffinity_np( m_thread, sizeof( cpuset ), &cpuset ) );',
    ]:
        text = replace_once(text, affinity_call, '#ifdef RED_PLATFORM_WEB\n                ReportBrowserAffinityLimit();\n#else\n                ' + affinity_call + '\n#endif')
    text = replace_once(text, 'REDTHR_PTHREAD_CHECK( ::pthread_setname_np( pthread_self(), truncatedThreadName ) );', '''#ifdef RED_PLATFORM_WEB
            emscripten_set_thread_name(pthread_self(), threadName);
#else
            REDTHR_PTHREAD_CHECK( ::pthread_setname_np( pthread_self(), truncatedThreadName ) );
#endif''')
    text = replace_once(text, '::pthread_yield();', '''#ifdef RED_PLATFORM_WEB
            ::sched_yield();
#else
            ::pthread_yield();
#endif''')
    return replace_once(text, '_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);\n\t\t\t_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);', '''#ifndef RED_PLATFORM_WEB
            _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
            _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
            // WebAssembly has no mutable x86 floating-point control register.
            // Numerical parity around subnormal values remains unverified.''')


def adapt_startup_fp(text):
    for call in ['_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);', '_MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);']:
        text = replace_once(text, call, '#ifndef RED_PLATFORM_WEB\n        ' + call + '\n#endif\n        // WebAssembly has no mutable x86 floating-point control register.')
    text = '#ifdef RED_PLATFORM_WEB\n#include <cstdio>\n#endif\n' + text
    text = replace_once(text, 'if ( !gameApp.Initialize( appSettings ) )', '''#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: app initialization begin\\n", stderr);
#endif
        if ( !gameApp.Initialize( appSettings ) )''')
    text = replace_once(text, 'red::SetMemoryBudgets();', '''#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: app initialization returned; setting memory budgets\\n", stderr);
#endif
        red::SetMemoryBudgets();''')
    text = replace_once(text, 'InitializeBaseEngineSystems( gameApp, commandLine );', '''#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: base engine systems begin\\n", stderr);
#endif
        InitializeBaseEngineSystems( gameApp, commandLine );
#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: base engine systems returned\\n", stderr);
#endif''')
    text = replace_once(text, 'const EBaseEngineState state = GEngine->GetState();', '''const EBaseEngineState state = GEngine->GetState();
#ifdef RED_PLATFORM_WEB
        static Int32 lastReportedState = -1;
        if (lastReportedState != static_cast<Int32>(state)) {
            lastReportedState = static_cast<Int32>(state);
            std::fprintf(stderr, "CP2077 native base engine state: %d\\n", lastReportedState);
        }
#endif''')
    return text

def adapt_engine_init_trace(text):
    text = replace_once(text, '#include "build.h"', '''#include "build.h"
#ifdef RED_PLATFORM_WEB
#include <cstdio>
#define CPWEB_INIT_BEGIN(name) std::fprintf(stderr, "CP2077 native initializer begin: %s\\n", name)
#define CPWEB_INIT_FAILURE(name) std::fprintf(stderr, "CP2077 native initializer failed: %s\\n", name)
#else
#define CPWEB_INIT_BEGIN(name) ((void)0)
#define CPWEB_INIT_FAILURE(name) ((void)0)
#endif''')
    before = ' if ( !(func) )\\'
    if text.count(before) != 4:
        raise RuntimeError('Native SAFE_INIT macro count changed')
    text = text.replace(before, ' CPWEB_INIT_BEGIN(name);\\\n' + before)
    failure = '::dd::GetEngineCrashData().safeInitName.Set( "FAILED - " name );\\'
    if text.count(failure) != 4:
        raise RuntimeError('Native initialization failure macro count changed')
    return text.replace(failure, 'CPWEB_INIT_FAILURE(name);\\\n\t' + failure)

def adapt_memory_reporter(text):
    text = adapt_pthread_selection(text)
    text = text.replace('#ifdef RED_PLATFORM_LINUX', '#if defined(RED_PLATFORM_LINUX) || defined(RED_PLATFORM_WEB)')
    text = text.replace('#ifndef RED_PLATFORM_LINUX', '#if !defined(RED_PLATFORM_LINUX) && !defined(RED_PLATFORM_WEB)')
    text = text.replace('"/var/log/oom_report_', 'CPWEB_OOM_PREFIX "oom_report_')
    return '#ifdef __EMSCRIPTEN__\n#define CPWEB_OOM_PREFIX "/"\n#else\n#define CPWEB_OOM_PREFIX "/var/log/"\n#endif\n' + text

def adapt_memory_documentation(text):
    return '#ifdef __EMSCRIPTEN__\n#include <malloc.h>\n#endif\n' + text

def adapt_browser_locale(text):
    text = replace_once(text, '#include "languageRegion.h"', '#include "languageRegion.h"\n#ifdef RED_PLATFORM_WEB\n#include <emscripten.h>\n#endif')
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )', "#elif defined( RED_PLATFORM_WEB )\nstatic String GetUserLanguageCodeName()\n{\n    char localeName[128]{};\n    EM_ASM({ stringToUTF8(navigator.language || 'en-US', Number($0), Number($1)); }, localeName, sizeof(localeName));\n    String result{ localeName };\n    result.ToLower();\n    return result;\n}\n#elif defined( RED_PLATFORM_LINUX )")

def adapt_profiler_text(text):
    text = adapt_logger(text)
    text = text.replace('fwprintf( ', 'CPWEB_PROFILE_PRINT( ')
    return '#ifdef __EMSCRIPTEN__\n#include "utf16-runtime.h"\n#define CPWEB_PROFILE_PRINT ::cpweb::fwprintf\n#else\n#define CPWEB_PROFILE_PRINT ::fwprintf\n#endif\n' + text


def adapt_memory_types(text):
    text = text.replace('#if defined( RED_PLATFORM_WINPC )', '#if defined( RED_PLATFORM_WEB )\n#include "memory-web.h"\n#elif defined( RED_PLATFORM_WINPC )',1)
    name = 'SystemPageAllocatorWeb' if 'PAGE_SYSTEM_ALLOCATOR' in text or 'PlatformSystemPageAllocator' in text else 'SystemAllocatorWeb'
    pos=text.index('namespace red')
    head,tail=text[:pos],text[pos:]
    aliases='typedef SystemPageAllocatorWeb PlatformSystemPageAllocator;' if name=='SystemPageAllocatorWeb' else 'typedef SystemAllocatorWeb PlatformSystemAllocator;\n    typedef SystemAllocatorWeb PlatformFlexibleAllocator;'
    tail=tail.replace('#if defined( RED_PLATFORM_WINPC )', '#if defined( RED_PLATFORM_WEB )\n    '+aliases+'\n#elif defined( RED_PLATFORM_WINPC )',1)
    return head+tail

def adapt_callstack_types(text):
    text=text.replace('#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )', '#if defined( RED_PLATFORM_WEB )\n#include "callstack-web.h"\n#elif defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )',1)
    pos=text.index('namespace red');head,tail=text[:pos],text[pos:]
    tail=tail.replace('#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )', '#if defined( RED_PLATFORM_WEB )\n    typedef CallstackCollectorWeb CallstackCollector;\n#elif defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )',1)
    return head+tail


def adapt_memory_metrics(text):
    text=text.replace('#if defined( RED_PLATFORM_WINPC )', '#if defined( RED_PLATFORM_WEB )\n#include "metrics-web.h"\n#elif defined( RED_PLATFORM_WINPC )',1)
    pos=text.index('namespace red');head,tail=text[:pos],text[pos:]
    tail=tail.replace('#if defined( RED_PLATFORM_WINPC )', '#if defined( RED_PLATFORM_WEB )\n    typedef MetricsCaptureWeb MetricsCapture;\n#elif defined( RED_PLATFORM_WINPC )',1)
    return head+tail

def adapt_rapidjson_assignment(text):
    text = '#include <new>\n' + text
    return replace_once(text,
        'GenericStringRef& operator=(const GenericStringRef& rhs) { s = rhs.s; length = rhs.length; }',
        'GenericStringRef& operator=(const GenericStringRef& rhs) { if (this != &rhs) new (this) GenericStringRef(rhs); return *this; }')

def adapt_script_integer_abi(text):
    text = '#include <type_traits>\n' + text
    text = replace_once(text, '(*m_saver) << data;', '''#if defined(RED_PLATFORM_WEB)
            if constexpr (std::is_same<T, intptr_t>::value) {
                static_assert(sizeof(T) == sizeof(Int64), "Serialized pointer-width operand must remain 64-bit");
                (*m_saver) << static_cast<Int64>(data);
            } else
#endif
            { (*m_saver) << data; }''')
    return replace_once(text, '(*m_loader) >> ret;', '''#if defined(RED_PLATFORM_WEB)
            if constexpr (std::is_same<T, intptr_t>::value) {
                static_assert(sizeof(T) == sizeof(Int64), "Serialized pointer-width operand must remain 64-bit");
                Int64 diskValue;
                (*m_loader) >> diskValue;
                ret = static_cast<T>(diskValue);
            } else
#endif
            { (*m_loader) >> ret; }''')

def adapt_lexer_include(text):
    text = text.replace('../../../internal/RedLexer/', '../../../../internal/RedLexer/')
    return text.replace('#include "scriptExpressionParser_bison.cxx.h"', '#include "scriptExpressionParserInternal.h"\n#include "scriptExpressionParser_bison.cxx.h"')

def adapt_bison_lexer_call(text):
    return text + '\n#if defined(RED_PLATFORM_WEB)\n#define yylex(value, stream) script::expression::parsertime(*(value), stream)\n#endif\n'

def adapt_math_debug_guards(text):
    # Final already disables these diagnostic macros. Do not claim x86 FP
    # control register emulation or change actual math algorithms.
    text = replace_once(text, '\tstruct ScopedDenormGuard', '#if !defined(RED_PLATFORM_WEB)\n\tstruct ScopedDenormGuard')
    text = replace_once(text, '\t\tUint32 m_prevState;\n\t};', '\t\tUint32 m_prevState;\n\t};\n#endif')
    return adapt_pthread_selection(text)

def adapt_unused_lexer_intrinsics(text):
    text = text.replace('#ifdef _WINDOWS', '#if defined(__EMSCRIPTEN__)\n// Use portable bit operations for the browser.\n#elif defined(_WINDOWS)', 1)
    return replace_once(text, '#ifdef _WINDOWS', '#if defined(__EMSCRIPTEN__)\n    const unsigned int mask32 = static_cast<unsigned int>(mask);\n    index = mask32 ? 31u - __builtin_clz(mask32) : 0u;\n#elif defined(_WINDOWS)')

def adapt_browser_filepaths(text):
    text = replace_once(text, '#ifdef RED_PLATFORM_LINUX', '#if defined(RED_PLATFORM_LINUX) || defined(RED_PLATFORM_WEB)')
    return replace_once(text, 'getenv( "HOME" )', '''
#if defined(RED_PLATFORM_WEB)
        "/userdata"
#else
        getenv("HOME")
#endif
    ''')

def adapt_file_path_limit(text):
    return replace_once(text, 'path.Length() > MAX_PATH', '''path.Length() >
#if defined(RED_PLATFORM_WEB)
        260u // Preserve the original PC filename validation limit.
#else
        MAX_PATH
#endif
    ''')

def adapt_internal_io_paths(text):
    return text.replace('"../redContainers/include/', '"../../redContainers/include/').replace('"../redMemory/include/', '"../../redMemory/include/')

def adapt_kraken_include(text):
    return text.replace('../../../../dev/external/oodle/', '../../../../external/oodle/')

def adapt_gpu_cpu_staging_pools(text):
    # The PC path here allocates CPU backing/upload buffers through the real
    # default allocator. GPU resources still require the WebGPU backend.
    return text.replace('defined( RED_PLATFORM_WINPC )', '(defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_WEB ))')

def adapt_speedtree_depot_paths(text):
    return '#include <cassert>\n' + text.replace('../../../external/speedtree_v8/', '../../../../external/speedtree_v8/')

def adapt_oodle_wasm_declarations(text):
    # Define the Wasm ABI for declarations only. No codec implementation is
    # supplied by this header, and missing Oodle functions remain link errors.
    insertion = '''#if defined(__EMSCRIPTEN__)
  #define __RADWASM__ 19
  #define __RADDETECTED__ __RADWASM__
  #define __RADDETECTEDPROC__ 7
  #define __RADLITTLEENDIAN__
  #if defined(__wasm64__)
    #define __RAD64__
    #define __RAD64REGS__
  #endif
  #define RADRESTRICT __restrict
  #define RADSTRUCT struct __attribute__((__packed__))
  #define RADLINK
  #define RADEXPLINK
  #define RADDLLEXPORTDLL __attribute__((visibility("default")))
  #define RADDLLIMPORTDLL
#endif

'''
    return replace_once(text, '#if !__RADDETECTED__', insertion + '#if !__RADDETECTED__')

def adapt_app_browser_paths(text):
    for signature, result in [('static red::AbsolutePath GetBaseDirectory()', '/game/'),
                              ('static red::AbsolutePath GetUserDirectory()', '/userdata/')]:
        start = text.index(signature)
        opening = text.index('{', start)
        closing = text.index('\n\t\t\t}', opening)
        body = text[opening+1:closing]
        text = text[:opening+1] + '\n#if defined(RED_PLATFORM_WEB)\n                return red::AbsolutePath::CreateDirPath("' + result + '");\n#else' + body + '\n#endif' + text[closing:]
    return text

def adapt_app_cooked_platform(text):
    return replace_once(text, 'red::String CurrentPlatformDirectory()\n{\n#if defined( RED_PLATFORM_WINPC )', 'red::String CurrentPlatformDirectory()\n{\n#if defined( RED_PLATFORM_WEB )\n    return "pc"; // Original PC asset formats.\n#elif defined( RED_PLATFORM_WINPC )')

def adapt_datetime_web(text):
    marker = '#elif defined( RED_PLATFORM_LINUX )'
    start = text.rindex(marker)
    finish = text.index('#else', start)
    body = text[start + len(marker):finish]
    # Browser uses the portable UTC conversion, with the correct tick/ms scale.
    body = body.replace(') * ms;', ') / ms;')
    return text[:start].replace(marker, '#elif defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )') + '#elif defined( RED_PLATFORM_WEB )' + body + text[start:]

def adapt_resource_cooking(text):
    return replace_once(text, '#if defined( RED_PLATFORM_LINUX )',
                        '#if defined( RED_PLATFORM_WEB )\n        return PLATFORM_PC; // Read the original PC-cooked resource format.\n#elif defined( RED_PLATFORM_LINUX )')

def adapt_default_allocator(text):
    text=replace_once(text, '#include "../redSystem/include/unitTestMode.h"', '#include "../../redSystem/include/unitTestMode.h"')
    return text.replace('#ifdef RED_PLATFORM_CONSOLE', "#if defined(RED_PLATFORM_WEB)\n    // Browser platform budget, not the native editor's 112-GiB VA reservation.\n    // Reservations are backed by Wasm linear memory and may fail at runtime.\n    const u64 c_defaultAllocatorSlabVirtualRange = RED_GIGA_BYTE(1);\n    const u64 c_defaultAllocatorTLSFVirtualRange = RED_GIGA_BYTE(4);\n    const u64 c_defaultAllocatorBigSizeVirtualRange = RED_GIGA_BYTE(2);\n    const u64 c_maxDefaultAllocation = RED_GIGA_BYTE(2);\n#elif defined(RED_PLATFORM_CONSOLE)", 1)

def adapt_legacy_address(text):
    return replace_once(text, 'LegacyGpuAllocator::MakeFreeRegion( u64 address, u64 size )', 'LegacyGpuAllocator::MakeFreeRegion( red::MemUint address, u64 size )')

def adapt_icu_embedded_package(text):
    text = replace_once(text, '#include "build.h"', '#include "build.h"\n#if defined(RED_PLATFORM_WEB)\n#include "icu-data-web.h"\n#endif')
    return replace_once(text, 'void ICUInternationalization::PreloadDataPackage()\n\t{', '''void ICUInternationalization::PreloadDataPackage()
    {
#if defined(RED_PLATFORM_WEB)
        // The exact original locale package is embedded by compile-icu-data.py.
        // Keep the engine's allocator, ICU file wrappers, initialization and
        // cache/event lifecycle; only delivery of this boot asset changes.
        m_icuDataPackage = red::CreateUniqueBuffer< text::PoolText_ICU_Data_Packagae >( cpweb_icu_data_size, 16 );
        red::Memcpy( m_icuDataPackage.Get(), icudt65_dat, cpweb_icu_data_size );
        InternalICUInit();
        text::RegexPatternCache::Initialize();
        if (m_dataLoadedSyncEvt)
        {
            m_dataLoadedSyncEvt->SetEvent();
            m_dataLoadedSyncEvt = nullptr;
        }
        return;
#endif''')

def adapt_browser_file_operations(text):
    text = adapt_pthread_selection(text)
    text = replace_once(text, 'Bool CSystemIO::CreatePath( const char* pathName )\n{', '''Bool CSystemIO::CreatePath( const char* pathName )
{
#if defined(RED_PLATFORM_WEB)
    if (!pathName || !*pathName) return false;
    char buffer[4096];
    const size_t length = red::Strlen(pathName);
    if (length >= sizeof(buffer)) return false;
    red::Memcpy(buffer, pathName, length + 1);
    for (size_t index = 0; index < length; ++index) {
        if (buffer[index] == 92) buffer[index] = '/';
        if (buffer[index] != '/' || index == 0) continue;
        buffer[index] = 0;
        if (::mkdir(buffer, S_IRWXU) != 0) {
            struct stat existing;
            if (errno != EEXIST || ::stat(buffer, &existing) != 0 || !S_ISDIR(existing.st_mode)) {
                LogLastError("Browser parent-directory creation", buffer);
                return false;
            }
        }
        buffer[index] = '/';
    }
    return true;
#else''')
    text = replace_once(text, '// Path created\n\treturn true;\n}', '// Path created\n\treturn true;\n#endif\n}')
    return replace_once(text, '\t\treturn ( stats.st_mtime / 100 ) - epochDifference;', '''#if defined(RED_PLATFORM_WEB)
        const Int64 seconds = static_cast<Int64>(stats.st_mtime);
        const Int64 fraction = static_cast<Int64>(stats.st_mtim.tv_nsec) / 100;
        const Int64 ticks = seconds * 10000000ll + fraction + static_cast<Int64>(epochDifference);
        return ticks >= 0 ? static_cast<Uint64>(ticks) : 0ull;
#else
\t\treturn ( stats.st_mtime / 100 ) - epochDifference;
#endif''')

def adapt_physics_paths(text):
    text = replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n\t\treturn red::AbsolutePath::CreateDirPath( "/tmp/" );', '#elif defined( RED_PLATFORM_WEB ) || defined( RED_PLATFORM_LINUX )\n\t\treturn red::AbsolutePath::CreateDirPath( "/tmp/" );')
    return replace_once(text, '#elif defined( RED_PLATFORM_LINUX )\n\t\tconst char* path = "../linux64.metaData";', '#elif defined( RED_PLATFORM_WEB )\n        const char* path = "/userdata/physx-wasm64.metaData";\n#elif defined( RED_PLATFORM_LINUX )\n\t\tconst char* path = "../linux64.metaData";')

records = [stage("games/cpLauncher/src/mainRed.cpp", adapt_launcher),
           stage("common/engine/src/gameApp.cpp", adapt_loop),
           stage("common/redSystem/include/architecture.h", adapt_architecture),
           stage("common/redSystem/include/os.h", adapt_os),
           stage("common/redSystem/include/crt.hpp", adapt_crt),
           stage("common/redSystem/include/threads.h", adapt_thread_id),
           stage("common/redSystem/include/dbgUtils.h", adapt_debug_attachment),
           stage("common/redSystem/include/compilerExtensions.h", adapt_debug_macros),
           stage("common/redSystem/include/bitUtils.h", adapt_bit_utils),
           stage("common/redSystem/include/redThreadsRedSystem.h", adapt_barrier),
           stage("common/redSystem/include/redThreadsPlatform.h", adapt_pthread_selection),
           stage("common/redSystem/include/redThreadsTypes.h", adapt_pthread_selection),
           stage("common/redSystem/include/redThreadsAtomic.h", adapt_pthread_selection),
           stage("common/redSystem/include/redThreadsAtomicLinuxAPI.inl", adapt_atomic_libc),
           stage("common/redSystem/include/timer.h", adapt_timer_selection),
           stage("common/redSystem/include/crashData.inl", adapt_crash_frame),
           stage("common/redSystem/include/redThreadsThread.inl", adapt_light_mutex),
           stage("common/redMemory/include/redMemoryInternal.h", adapt_memory_includes),
           stage("common/redMemory/include/threadIdProvider.hpp", adapt_memory_thread_id)]
for relative in ['common/redIO/include/redIOCommon.h', 'common/redIO/include/redIOFile.h',
                 'common/redIO/include/redIOSystemFileLinuxAPI.h', 'common/redNetwork/include/redNetworkPublic.h']:
    records.append(stage(relative, adapt_pthread_selection))
def adapt_static_shader_cache(text):
    declaration = '\nnamespace cp2077::web { int ConvertStaticShaderEntry(const char*, StaticShaderEntry&); }\n'
    text = replace_once(text, '#include "staticShaderCache.h"', '#include "staticShaderCache.h"'+declaration)
    head, separator, body = text.partition('Bool CStaticShaderCacheReadonly::Initialize(')
    if not separator: raise ValueError('Original readonly shader-cache loader changed')
    anchor = '\n\treturn true;\n}\n'
    insertion = '''
    // Preserve the full original metadata/entry maps. Converted exact compute
    // variants replace only their corresponding binary payloads.
    for (const auto& item : m_metaEntries)
    {
        const auto& meta = item.Value();
        if (!meta->HasShaderHash(GpuWrapApi::ComputeShader)) continue;
        red::SharedPtr<StaticShaderEntry> entry;
        if (!GetShader(meta->m_entryHashes[GpuWrapApi::ComputeShader], entry)) return false;
        const int converted = cp2077::web::ConvertStaticShaderEntry(meta->m_variableName.AsChar(), *entry);
        if (converted < 0) return false;
    }
'''
    body = replace_once(body, anchor, insertion+anchor)
    return head+separator+body

for relative, adapter in [
    ('common/redSystem/src/crt.cpp', adapt_crt_source),
    ('common/redSystem/src/file.cpp', adapt_file_text),
    ('common/redSystem/src/log.cpp', adapt_log),
    ('common/redSystem/src/logger.cpp', adapt_logger),
    ('common/redSystem/src/errorHandler.cpp', adapt_error_web_hooks),
    ('common/redSystem/src/redThreadsThreadLinuxAPI.cpp', adapt_thread_source),
]:
    records.append(stage(relative, adapter))
for relative, adapter in [
    ('common/engine/src/gameAppBaseInitializationState.cpp', adapt_startup_fp),
    ('common/engine/src/baseEngineInit.cpp', adapt_engine_init_trace),
    ('common/renderer/src/renderInterface.cpp', adapt_browser_viewport),
    ('common/shaderCache/src/staticShaderCache.cpp', adapt_static_shader_cache),
    ('common/engine/src/engineWatchdog.cpp', adapt_pthread_selection),
    ('common/redCore/src/absolutePath.cpp', adapt_pthread_selection),
    ('common/redCore/src/languageRegion.cpp', adapt_browser_locale),
    ('common/redCore/src/redProfiler.cpp', adapt_profiler_text),
    ('common/redNetwork/include/network.h', adapt_pthread_selection),
    ('common/redNetwork/include/redNetworkPublic.h', adapt_pthread_selection),
    ('common/renderData/include/renderSimpleTypes.h', adapt_pthread_selection),
    ('common/renderData/include/gpuApiMemory.h', adapt_gpu_cpu_staging_pools),
    ('common/renderData/src/gpuApiMemory.cpp', adapt_gpu_cpu_staging_pools),
    ('common/renderData/src/renderMultilayerMaskBlob.cpp', adapt_pthread_selection),
    ('common/renderData/src/renderTextureBlob.cpp', adapt_pthread_selection),
    ('common/renderData/include/renderTextureBlob.h', adapt_pthread_selection),
    ('common/renderData/include/renderMultilayerMaskBlob.h', adapt_pthread_selection),
    ('common/renderData/include/speedTreeV8Sdk.h', adapt_speedtree_depot_paths),
    ('common/renderData/src/speedTreeV8Sdk.cpp', adapt_speedtree_depot_paths),
    ('common/redMemory/src/threadMonitor.cpp', adapt_pthread_selection),
    ('common/redMemory/src/systemAllocatorType.h', adapt_memory_types),
    ('common/redMemory/src/systemPageAllocatorType.h', adapt_memory_types),
    ('common/redMemory/src/callstackCollector.h', adapt_callstack_types),
    ('common/redMemory/src/metricsCapture.h', adapt_memory_metrics),
    ('common/redMemory/src/defaultAllocator.cpp', adapt_default_allocator),
    ('common/redMemory/src/dynamicLinearAllocator.cpp', lambda text: (ROOT / 'src/dynamic-linear-allocator-web.cpp').read_text(encoding='utf-8')),
    ('common/redMemory/src/legacyGpuAllocator.cpp', adapt_legacy_address),
    ('common/redMemory/src/mutex.cpp', adapt_pthread_selection),
    ('common/redMemory/src/mutex.h', adapt_pthread_selection),
    ('common/redConfig/src/configVarSystem.cpp', adapt_pthread_selection),
    ('common/redReflection/src/datetime.cpp', adapt_datetime_web),
    ('common/redReflection/src/resourceLoaderScheduler.cpp', adapt_resource_cooking),
    ('common/redReflection/src/resourceMetricsBank.cpp', adapt_pthread_selection),
    ('common/redReflection/src/scriptDataObject.cpp', adapt_script_integer_abi),
    ('common/redReflection/src/scriptExpressionParser.cpp', adapt_lexer_include),
    ('common/gameTweakDB/src/tweakDBLexer.cpp', lambda text: replace_once(text, 'defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX )', 'defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')),
    ('common/game/include/influenceLayer.h', lambda text: replace_once(text, '#include "../../worldNavigation/include/influenceMapCell.h"', '#include "../../worldNavigation/include/influenceMapCell.h"\n#include "../../worldNavigation/include/influenceMapHelpers.h"')),
    ('common/gameStateMachine/include/validator.h', lambda text: replace_once(text, 'm_errorMessage << message;', 'm_errorMessage += message;')),
    ('../external/bink2/include/radtypes.h', lambda text: replace_once(text, '#if defined(__ppc64__) || defined(__aarch64__) || defined(_M_X64)', '#if defined(__wasm64__) || defined(__ppc64__) || defined(__aarch64__) || defined(_M_X64)')),
    ('common/renderer/src/renderReflectionProbeManager.h', lambda text: replace_once(text, 'return red::GetHash( m_val );', 'return red::GetHash( static_cast<Uint64>( m_val ) );')),
    ('common/renderer/src/renderProxyLightVolume.cpp', lambda text: replace_once(replace_once(text, 'red::RoundUpToPowerOf2( RED_KILO_BYTE( 256 ) / sizeof( Vector3 ) )', 'red::RoundUpToPowerOf2( static_cast<Uint64>( RED_KILO_BYTE( 256 ) / sizeof( Vector3 ) ) )'), 'red::RoundUpToPowerOf2( RED_KILO_BYTE( 128 ) / sizeof( Int16 ) )', 'red::RoundUpToPowerOf2( static_cast<Uint64>( RED_KILO_BYTE( 128 ) / sizeof( Int16 ) ) )')),
    ('common/renderData/include/renderInGameSettings.h', lambda text: replace_once(text, '#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )', '#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO ) || defined( RED_PLATFORM_WEB )')),
    ('common/renderData/src/renderInGameSettings.cpp', lambda text: replace_once(text, '#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )', '#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO ) || defined( RED_PLATFORM_WEB )')),
    ('common/redReflection/src/scriptExpressionParserPath.cpp', adapt_lexer_include),
    ('common/redReflection/src/scriptExpressionParserInternal.h', adapt_bison_lexer_call),
    ('common/redMath/include/mathUtils.h', adapt_math_debug_guards),
    ('../internal/RedLexer/native/src/flexSupplimentary.h', adapt_unused_lexer_intrinsics),
    ('common/redFileSystem/include/system.h', adapt_pthread_selection),
    ('common/redFileSystem/src/systemLinux.cpp', adapt_browser_file_operations),
    ('common/redFileSystem/src/filePathsLinux.cpp', adapt_browser_filepaths),
    ('common/redFileSystem/src/fileUtils.cpp', adapt_file_path_limit),
    ('common/redIO/src/serializationMemoryAllocator.h', adapt_internal_io_paths),
    ('common/redIO/src/redIOWorkerGeneric.h', adapt_internal_io_paths),
    ('common/redIO/src/redIOWorkerWin32.h', adapt_internal_io_paths),
    ('common/redIO/src/serializationMemoryAllocator.cpp', adapt_internal_io_paths),
    ('common/redIO/src/redIOSettings.h', adapt_pthread_selection),
    ('common/redJobs2/src/jobDispatcher.cpp', adapt_pthread_selection),
    ('common/redJobs2/src/jobDebugger.cpp', adapt_internal_io_paths),
    ('common/core/include/cacheFilenames.h', lambda text: replace_once(text, 'defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_LINUX )', 'defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')),
    ('common/gameFramework/src/gameEngine.cpp', lambda text: replace_once(replace_once(text, 'Bool CGameEngine::InitializeInput()\n{\n#if defined RED_PLATFORM_WINPC', 'Bool CGameEngine::InitializeInput()\n{\n#if defined RED_PLATFORM_WEB\n    CName class_name = RED_NAME_CONSTEXPR("inputInputSystemWeb");\n#elif defined RED_PLATFORM_WINPC'), 'Bool CGameEngine::InitializeServices()\n{\n#if defined( RED_PLATFORM_LINUX )', 'Bool CGameEngine::InitializeServices()\n{\n#if defined( RED_PLATFORM_WEB )\n    CName servicesClassName = RED_NAME_CONSTEXPR("servicesGameServicesWeb");\n#elif defined( RED_PLATFORM_LINUX )')),
    ('common/core/include/platform.h', lambda text: replace_once(text, '\tNull\n};', '\tNull,\n\tWeb\n};')),
    ('common/gameServices/src/gameServices.cpp', lambda text: text.replace('#if defined( RED_PLATFORM_WINPC )\n\tUint32 GameServices::', '#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_WEB )\n\tUint32 GameServices::')),
    ('common/redConfig/include/inGameConfigSystem.h', lambda text: replace_once(text, '\t\tstatic red::AbsolutePath GetUserSettingsPath();\n#endif', '\t\tstatic red::AbsolutePath GetUserSettingsPath();\n#endif\n#if defined(RED_PLATFORM_WEB)\n        static red::AbsolutePath GetUserSettingsPath();\n#endif')),
    ('common/redConfig/src/inGameConfigSystem.cpp', lambda text: replace_once(replace_once(text, '#ifdef RED_PLATFORM_WINPC\n\tred::AbsolutePath System::GetUserSettingsPath()', '#if defined(RED_PLATFORM_WINPC) || defined(RED_PLATFORM_WEB)\n\tred::AbsolutePath System::GetUserSettingsPath()'), '\t\treturn red::paths::GetLocalAppDataDirectory().AddFilePath( c_inGameConfigFilename );', '#if defined(RED_PLATFORM_WEB)\n        return red::paths::GetSaveDirectory().AddFilePath(c_inGameConfigFilename);\n#else\n\t\treturn red::paths::GetLocalAppDataDirectory().AddFilePath( c_inGameConfigFilename );\n#endif')),
    ('common/inkRendering/src/inkRenderingInit.cpp', lambda text: replace_once(text, '#include "inkRenderingMemoryPools.h"\n#include "build.h"', '#include "build.h"\n#include "inkRenderingMemoryPools.h"')),
    ('../external/harfbuzz_new/src/hb-ft.cc', lambda text: replace_once(text, 'hb_ft_face_finalize (FT_Face ft_face)\n{', 'hb_ft_face_finalize (void* object)\n{\n  FT_Face ft_face = static_cast<FT_Face>(object);').replace('(FT_Generic_Finalizer) hb_ft_face_finalize', 'hb_ft_face_finalize').replace('_release_blob (FT_Face ft_face)\n{', '_release_blob (void* object)\n{\n  FT_Face ft_face = static_cast<FT_Face>(object);').replace('(FT_Generic_Finalizer) _release_blob', '_release_blob')),
    ('common/world/include/worldPersistencyCache.h', lambda text: replace_once(text, 'static const Uint32 c_invalidPageIndex = c_invalidPageIndex;', 'static const Uint32 c_invalidPageIndex = PersistencyCache::c_invalidPageIndex;')),
    ('common/redMath/include/numericalUtils.h', lambda text: replace_once(text, 'return std::pow<T>( a, b );', 'return static_cast<T>( std::pow( a, b ) );')),
    ('common/textSystem/src/icuInternationalization.cpp', adapt_icu_embedded_package),
    ('common/physics/src/physXNatives.cpp', adapt_physics_paths),
    ('common/redHTTP/src/httpImplementation.h', lambda text: replace_once(text, '#if defined( RED_PLATFORM_WINPC )', '#if defined( RED_PLATFORM_WEB )\n#include \"http-web.h\"\nnamespace http { using Impl = WebImpl; }\n#elif defined( RED_PLATFORM_WINPC )')),
    ('common/galaxy/src/galaxyInit.cpp', lambda text: replace_once(replace_once(text, '../../../../temp/gen/rtti/galaxy/typeRegistryFuncImpl.h', '../rtti/galaxy/typeRegistryFuncImpl.h'), '#ifdef RED_PLATFORM_WINPC', '#if defined(RED_PLATFORM_WINPC) || defined(RED_PLATFORM_WEB)')),
    ('common/triggers/src/clipper.cpp', lambda text: replace_once(text, '#include <algorithm>', '#include <algorithm>\n#include <ostream>')),
    ('common/textSystem/include/settings.h', lambda text: replace_once(text, '#if defined( RED_PLATFORM_WIN32 ) || defined ( RED_PLATFORM_WIN64 )', '#if defined( RED_PLATFORM_WEB )\n\t#define RED_USE_FREE_TYPE\n\t#define RED_USE_HARFBUZZ\n\n#elif defined( RED_PLATFORM_WIN32 ) || defined ( RED_PLATFORM_WIN64 )')),
    ('common/inkCore/src/animationUtils.cpp', lambda text: replace_once(text, '#include "animationUtils.h"\n#include "build.h"', '#include "build.h"\n#include "animationUtils.h"')),
    ('common/interop/src/interopInterpreter.cpp', lambda text: replace_once(text, '"../redReflection/include/standardTextWriter.h"', '"../../redReflection/include/standardTextWriter.h"')),
    ('common/redCompression/src/wrapperKraken.cpp', adapt_kraken_include),
    ('../external/oodle/include/oodle2.h', adapt_oodle_wasm_declarations),
    ('common/app/src/appPlatformLinux.cpp', adapt_app_browser_paths),
    ('common/app/src/appPath.cpp', adapt_app_cooked_platform),
    ('../external/rapidjson/include/rapidjson/document.h', adapt_rapidjson_assignment),
    ('common/redMemory/src/buddyAllocatorUtils.h', adapt_pthread_selection),
    ('common/redMemory/src/reporter.cpp', adapt_memory_reporter),
    ('common/redMemory/src/documentation.cpp', adapt_memory_documentation),
]:
    records.append(stage(relative, adapter))
records.append(stage('../external/PhysX342/PxShared/src/foundation/include/PsHash.h', lambda text: replace_once(text, '#if PX_APPLE_FAMILY\n// hash for size_t', '#if PX_APPLE_FAMILY || defined(__wasm64__)\n// hash for size_t')))
records.append(stage('../external/PhysX342/PhysX_3.4/Source/GeomUtils/src/gjk/GuGJKType.h', lambda text: replace_once(text, 'Ps::aos::PsMatTransformV& getRelativeTransform(){ return mAToB; }', 'const Ps::aos::PsMatTransformV& getRelativeTransform() const { return mAToB; }')))
records.append(stage('../external/PhysX342/PxShared/include/foundation/PxPreprocessor.h', lambda text: replace_once(text, '#if defined(__x86_64__) || defined(_M_X64)', '#if defined(__wasm64__) || defined(__x86_64__) || defined(_M_X64)')))
records.append(stage('common/redCore/include/absolutePath.hpp', adapt_crash_path))
records.append(stage('common/redReflection/include/engineTime.h', adapt_engine_time))
records.append(stage('common/redReflection/src/engineTime.cpp', adapt_engine_time))
records.append(stage('common/redContainers/include/hashMap.hpp', adapt_hashmap_alignment))
records.append(stage('common/redIO/src/redIOSystemFileLinuxAPI.cpp', adapt_system_file))
records.append(stage('common/redContainers/src/dynamicBuffer.cpp', adapt_dynamic_alignment))
records.append(stage('common/redContainers/src/string/stringLocale.cpp', adapt_locale_conversion))
new_timer = OVERLAY / 'common/redSystem/include/timerWeb.h'
records.append({"source": str(SOURCE / 'common/redSystem/include/timerWeb.h'),
                "sourceSHA256": None, "originalFileExists": False,
                "overlay": str(new_timer), "overlaySHA256": hashlib.sha256(new_timer.read_bytes()).hexdigest()})
def adapt_audio_os_char(text, expected):
    guard = '!defined( RED_PLATFORM_ORBIS ) && !defined( RED_PLATFORM_LINUX )'
    if text.count(guard) != expected:
        raise RuntimeError('Original audio OS-character guards changed')
    return text.replace(guard, guard + ' && !defined( RED_PLATFORM_WEB )')
records.append(stage('common/functionalTestsEngine/src/functionalTestsEngineInit.cpp', lambda text:
    replace_once(text, '../../rtti/functionalTestsEngine/typeRegistryFuncImpl.h',
                       '../rtti/functionalTestsEngine/typeRegistryFuncImpl.h')))
records.append(stage('common/functionalTestsService/src/functionalTestsServiceInit.cpp', lambda text:
    replace_once(text, '../../rtti/functionalTestsService/typeRegistryFuncImpl.h',
                       '../rtti/functionalTestsService/typeRegistryFuncImpl.h')))

def adapt_browser_memory_query(text):
    # This protocol requests physical host CPU/GPU memory, not Wasm heap size.
    # Browser APIs cannot provide those metrics. Preserve the original error
    # response instead of reporting invented values or equating heap with RAM.
    return replace_once(text,
        'return GetMemoryInfoInternal( errorInfo, memoryCPU, memoryGPU );',
        '''#ifdef RED_PLATFORM_WEB
    errorInfo = "Physical CPU/GPU memory statistics are unavailable in the browser";
    return false;
#else
    return GetMemoryInfoInternal( errorInfo, memoryCPU, memoryGPU );
#endif''')
records.append(stage('common/functionalTestsService/src/memoryStats.cpp', adapt_browser_memory_query))
for audio_source, count in [('soundEngine.cpp', 2), ('wwiseLowLevelIO.cpp', 1)]:
    records.append(stage('common/soundEngine/src/' + audio_source,
                         lambda text, expected=count: adapt_audio_os_char(text, expected)))
def adapt_audio_streaming(text):
    for old, new in [
        ('static int wwiseContainerOpusSeek( void* stream, long offset, signed int whence )',
         'static int wwiseContainerOpusSeek( void* stream, opus_int64 offset, signed int whence )'),
        ('static int wwiseContainerOpusSeek( void* stream, long long offset, signed int whence )',
         'static int wwiseContainerOpusSeek( void* stream, opus_int64 offset, signed int whence )'),
        ('static signed long wwiseContainerOpusTell( void* stream )',
         'static opus_int64 wwiseContainerOpusTell( void* stream )'),
        ('static signed long long wwiseContainerOpusTell( void* stream )',
         'static opus_int64 wwiseContainerOpusTell( void* stream )'),
    ]:
        text = replace_once(text, old, new)
    return text.replace('defined( RED_PLATFORM_LINUX )',
                        'defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')
records.append(stage('common/soundEngine/src/wwiseContainerStreaming.cpp', adapt_audio_streaming))
def adapt_wwise_wide_strings(text):
    # SDK interfaces retain two-byte wchar_t in the source-built game. Route
    # selected wide helpers explicitly; never bind them to four-byte libc.
    text = '#ifdef __EMSCRIPTEN__\n#include "utf16-runtime.h"\n#define CPWEB_AK_WIDE(name) ::cpweb::name\n#else\n#define CPWEB_AK_WIDE(name) ::name\n#endif\n' + text
    for name in ['wcslen', 'wcstombs', 'mbstowcs']:
        text = text.replace(name + '(', 'CPWEB_AK_WIDE(' + name + ')(')
    return text
for audio_header in ['include/AK/Tools/Common/AkString.h', 'include/AK/Tools/POSIX/AkPlatformFuncs.h']:
    records.append(stage('../external/wwise/' + audio_header, adapt_wwise_wide_strings))
records.append(stage('common/gameAudio/src/dynamicReverbSystem.cpp', lambda text:
    replace_once(text, 'Bool operator<( const Candidate& other )',
                       'Bool operator<( const Candidate& other ) const')))
records.append(stage('common/sound/src/advertSystem.cpp', lambda text:
    replace_once(replace_once(text,
        'Bool operator< (const AdvertPoint& other);',
        'Bool operator< (const AdvertPoint& other) const;'),
        'Bool AdvertPoint::operator < ( const AdvertPoint& other )',
        'Bool AdvertPoint::operator < ( const AdvertPoint& other ) const')))
def adapt_network_posix_selection(text):
    # Keep the original ENet/Galaxy/cloud choices and real POSIX socket code.
    # Emscripten socket operations still require browser-compatible peers/relay;
    # this selection does not claim direct browser UDP or disable any backend.
    return text.replace('defined( RED_PLATFORM_LINUX )',
                        'defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB )')
for network_file in ['src/networkConfig.h','src/sockets.h','src/selector.cpp']:
    records.append(stage('common/network/' + network_file, adapt_network_posix_selection))

def adapt_recording_loader(text):
    return replace_once(text, 'namespace srt\n{', '#ifdef RED_PLATFORM_WEB\n#include "recording-loader-web.h"\nnamespace srt { namespace platform = web; }\n#endif\n\nnamespace srt\n{')
records.append(stage('common/sessionRecordingTool/src/common/recordingToolCoreWrapper/platform/recordingToolCoreLoader.h', adapt_recording_loader))

def adapt_recording_api(text):
    return text + '\n#ifdef RED_PLATFORM_WEB\n#define REC_API\n#endif\n'
records.append(stage('../internal/RecordingToolCore/include/recCore_recApi.h', adapt_recording_api))

def adapt_recording_platform(text):
    return replace_once(text, '#else\n#error  "Unsuported platform!"', '#elif defined(RED_PLATFORM_WEB)\n#include "recording-file-web.h"\nnamespace recCore { namespace ioLayer { namespace platformAPI = web; }}\n#else\n#error  "Unsuported platform!"')
records.append(stage('../internal/RecordingToolCore/src/ioLayer/asyncIO/platform/platformSelector.h', adapt_recording_platform))

def adapt_recording_assertions(text):
    start=text.index('// Assertion')
    end=text.index('// Offset of')
    return text[:start] + '''// Browser failure diagnostics avoid the Windows-only wide CRT.
#include <cstdio>
#include <cstdlib>
#undef RED_FATAL_ASSERT
#undef RED_ASSERT
#define RED_FATAL_ASSERT(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "[REC CORE ASSERT] %s:%d: %s\\n", __FILE__, __LINE__, #cond); std::abort(); }} while (false);
#define RED_ASSERT(cond, ...) RED_FATAL_ASSERT(cond, __VA_ARGS__)
''' + text[end:]
records.append(stage('../internal/RecordingToolCore/src/common/platformMacros.h', adapt_recording_assertions))

def adapt_recording_deprecated_io(text):
    return text.replace('fopen_s( &file, filename, "ab" ) == 0', '(file = fopen(filename, "ab")) != nullptr').replace('fopen_s( &file, filename, "rb+" ) == 0', '(file = fopen(filename, "rb+")) != nullptr').replace('fopen_s( &file, filename, "rb" ) == 0', '(file = fopen(filename, "rb")) != nullptr')
records.append(stage('../internal/RecordingToolCore/src/ioLayer/deprecated/io.cpp', adapt_recording_deprecated_io))

for registration in (ROOT / 'generated/rtti').glob('*/typeRegistryFuncImpl.h'):
    module = registration.parent.name
    source_directory = 'renderer' if module == 'render' else module
    project_directory = f'games/{module}' if module == 'cp' else f'common/{source_directory}'
    records.append({"source": str(SOURCE / f'{project_directory}/rtti/{module}/typeRegistryFuncImpl.h'),
                    "sourceSHA256": None, "originalFileExists": False,
                    "overlay": str(registration), "overlaySHA256": hashlib.sha256(registration.read_bytes()).hexdigest(),
                    "provenance": "Original RTTIGenerator built from source; portable engine source list"})
generated_lexer = ROOT / 'generated/redReflection/scripts.cxx'
if generated_lexer.exists():
    records.append({"source": str(SOURCE.parent / 'internal/RedLexer/native/gen/scripts.cxx'),
                    "sourceSHA256": None, "originalFileExists": False,
                    "overlay": str(generated_lexer), "overlaySHA256": hashlib.sha256(generated_lexer.read_bytes()).hexdigest(),
                    "provenance": "Flex-generated original scripts.flex scanner"})
# Clang's filesystem overlay redirects original paths to adapted copies. This
generated_tweakdb_lexer = ROOT / 'generated/gameTweakDB/tdb_flex.cxx'
if generated_tweakdb_lexer.exists():
    records.append({"source": str(SOURCE.parent / 'temp/gen/gameTweakDB/tdb_flex.cxx'),
                    "sourceSHA256": None, "originalFileExists": False,
                    "overlay": str(generated_tweakdb_lexer),
                    "overlaySHA256": hashlib.sha256(generated_tweakdb_lexer.read_bytes()).hexdigest(),
                    "provenance": "Flex-generated original tdb.flex scanner"})
# also catches quoted relative includes without copying or modifying the depot.
staged_roots = [{"type": "file", "name": r["source"].replace("\\", "/"),
                 "external-contents": r["overlay"].replace("\\", "/")} for r in records] + [
    {"type": "file", "name": (SOURCE.parent / 'engine/shaders' / shader.relative_to(SOURCE.parents[1] / 'engine/shaders')).as_posix(),
     "external-contents": shader.as_posix()}
    for shader in (SOURCE.parents[1] / 'engine/shaders').rglob('*') if shader.is_file()
]
# Other build scripts register their own overlays in this file. Keep every entry staging does not produce.
staged_names = {e["name"].casefold() for e in staged_roots}
vfs_file = ROOT / "clang-vfs.json"
kept_roots = []
if vfs_file.exists():
    kept_roots = [e for e in json.loads(vfs_file.read_text(encoding='utf-8-sig')).get("roots", [])
                  if e.get("name", "").casefold() not in staged_names]
write_changed(vfs_file, json.dumps({
    "version": 0, "case-sensitive": False, "use-external-names": False,
    "roots": staged_roots + kept_roots,
}, indent=2))
(ROOT / "src").mkdir(exist_ok=True)
(ROOT / "src/mainBrowser.cpp").write_text('''// Browser entry into the original cpLauncher, not a replacement game.
#include "build.h"
#include "mainRed.h"
#include "../../../common/redCore/include/commandline.h"

int main( int argc, char** argv )
{
    red::CommandLine::Init( argc, argv );
    // Browser Run schedules the state machine and returns without quick_exit.
    return mainRed();
}
''', encoding="utf-8")
(ROOT / "source-manifest.json").write_text(json.dumps({
    "sourceRoot": str(SOURCE), "adaptedFiles": records,
    "scope": "Original launcher, browser scheduling and platform/ABI bring-up",
    "gameBuilt": False, "gameRunning": False,
    "remaining": ["Web platform and ABI", "Engine dependency build graph and RTTI generation",
                  "WebGPU renderer and shader conversion", "Browser filesystem and resource streaming",
                  "Input, threading, audio and physics middleware", "Original menu and game startup"],
}, indent=2), encoding="utf-8")
print("Staged original game bootstrap and browser loop in", ROOT)


