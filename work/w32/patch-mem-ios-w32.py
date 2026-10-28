#!/usr/bin/env python3
"""[w32] iPhone memory profile, switched on by ?mem=ios (the page adds it when it selects the wasm32 bundle on iOS).
Uses the engine's own presets/budgets only; allocator semantics are unchanged.
  * CpWebMemIos(): cached read of ?mem=ios (memory-web.cpp copy; browser main thread via MAIN_THREAD_EM_ASM).
  * IO memory cache budget: the engine's console value (62 MB instead of the PC 126 MB) - serializationMemoryAllocator.cpp.
  * Texture quality: the engine's SafeMode preset ("drop as many mips as possible ... low end") unless ?texq=<level>
    names another level - gameAppBaseInitializationState.cpp.
  * Rendering: the existing potato preset defaults to level 2 when ?potato is absent (renderInGameSettings.cpp,
    renderProxyMesh.cpp, gpu-device-web.cpp), and the render size defaults to 0.6 x 1280x720 (768x432) unless
    ?potatoScale is given (gpu-device-web.cpp).
Job workers already scale with hardwareConcurrency (4-core phone -> 2 workers), so they are unchanged.
"""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
C = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag='CpWebMemIos'):
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if '[w32-ios]' in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

patch(r'D:\2077\src\memory-web.cpp', [('''#if defined( __wasm32__ )
// [w32] On-demand memory probe''', '''#if defined( __wasm32__ )
// [w32-ios] ?mem=ios selects the iPhone memory profile (read once on the browser main thread, then cached).
extern "C" int CpWebMemIos() {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mem') === 'ios' ? 1 : 0; } catch (e) { return 0; } });
        cached.store(v, std::memory_order_release);
        std::fprintf(stderr, "CP2077 w32 memory profile: %s\\n", v ? "ios" : "default");
    }
    return v;
}
#endif
#if defined( __wasm32__ )
// [w32] On-demand memory probe''')])

patch(C + r'\redIO\src\serializationMemoryAllocator.cpp', [
 ('''#ifdef RED_PLATFORM_CONSOLE
const Uint32 c_ioAllocatorBudget = RED_MEGA_BYTE( 62 );
#else
const Uint32 c_ioAllocatorBudget = RED_MEGA_BYTE( 126 );
#endif''', '''#if defined( __wasm32__ )
extern "C" int CpWebMemIos();
// [w32-ios] iPhone profile uses the engine's console IO cache budget.
static Uint32 CpWebIoAllocatorBudget() { return CpWebMemIos() ? RED_MEGA_BYTE( 62 ) : RED_MEGA_BYTE( 126 ); }
#define c_ioAllocatorBudget CpWebIoAllocatorBudget()
#elif defined( RED_PLATFORM_CONSOLE )
const Uint32 c_ioAllocatorBudget = RED_MEGA_BYTE( 62 );
#else
const Uint32 c_ioAllocatorBudget = RED_MEGA_BYTE( 126 );
#endif''')])

patch(C + r'\engine\src\gameAppBaseInitializationState.cpp', [
 ('''		String textureQualityLevel;
		if ( commandLine.GetFirstOption( "texQualityLevel", textureQualityLevel ) )
		{
			params.m_textureQualityLevel = Config::TextureQualityLevelNameToEnum( textureQualityLevel );
		}''', '''		String textureQualityLevel;
		if ( commandLine.GetFirstOption( "texQualityLevel", textureQualityLevel ) )
		{
			params.m_textureQualityLevel = Config::TextureQualityLevelNameToEnum( textureQualityLevel );
		}
#if defined( __wasm32__ )
		{
			// [w32-ios] iPhone memory profile: the engine's SafeMode texture preset unless ?texq=<level> is given.
			extern "C" int CpWebMemIos();
			if ( CpWebMemIos() )
			{
				char texq[ 32 ] = { 0 };
				MAIN_THREAD_EM_ASM( { try { const v = new URLSearchParams( location.search ).get( 'texq' ) || ''; stringToUTF8( v, $0, 32 ); } catch ( e ) {} }, texq );
				params.m_textureQualityLevel = texq[ 0 ] ? Config::TextureQualityLevelNameToEnum( red::StringView( texq ) ) : Config::TextureQualityLevel::SafeMode;
				std::fprintf( stderr, "CP2077 w32 memory profile: texture quality %s\\n", texq[ 0 ] ? texq : "SafeMode" );
			}
		}
#endif''')])

POT = "get( 'potato' )"
POT_IOS = "get( 'potato' ) ?? ( new URLSearchParams( location.search ).get( 'mem' ) === 'ios' ? '2' : null ) /* [w32-ios] */"
patch(C + r'\renderData\src\renderInGameSettings.cpp', [(POT, POT_IOS)])
patch(C + r'\renderer\src\renderProxyMesh.cpp', [(POT, POT_IOS)])
patch(r'D:\2077\src\gpu-device-web.cpp', [
 ("        const p = q.get('potato');", "        const p = q.get('potato') ?? (q.get('mem') === 'ios' ? '2' : null); // [w32-ios] potato 2 by default on the iPhone profile"),
 ("        if (q.has('potatoScale') && Number.isFinite(o) && o >= 0.5 && o <= 1) scale = o;",
  "        if (q.has('potatoScale') && Number.isFinite(o) && o >= 0.5 && o <= 1) scale = o;\n        else if (q.get('mem') === 'ios') scale = 0.6; // [w32-ios] 768x432 internal render size on the phone (browser upscales)"),
])
