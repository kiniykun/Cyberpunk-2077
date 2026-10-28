#!/usr/bin/env python3
"""[w32-menupup] mem=ios: log which menu controller activates puppet scenes (class + scene names) and, with
?menuPuppets=0, keep them deferred (no puppet entities / meshes / facial rigs for that menu).
Keyed by the ORIGINAL depot path (the w32 copy starts from the engine-overlay version the wasm64 build sees)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
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

patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\gameUI\src\baseMenuGameController.cpp', [
 ('''            if(isVisible && CheckInitializationFlags(InitializationFlags::PlayerAttached) &&
                !CheckInitializationFlags(InitializationFlags::PuppetsInitialized)) {
                std::fprintf(stderr,"CP2077 puppet demand probe: phase=activate scenes=%llu\\n",static_cast<unsigned long long>(m_puppetSceneInfos.Size()));
                InitializePuppets();
            }''', '''            if(isVisible && CheckInitializationFlags(InitializationFlags::PlayerAttached) &&
                !CheckInitializationFlags(InitializationFlags::PuppetsInitialized)) {
                std::fprintf(stderr,"CP2077 puppet demand probe: phase=activate scenes=%llu\\n",static_cast<unsigned long long>(m_puppetSceneInfos.Size()));
                { // [w32-menupup] which controller / scenes; ?menuPuppets=0 keeps them deferred under mem=ios
                    std::string names; for ( const auto& info : m_puppetSceneInfos ) { names += info.m_sceneName.AsChar(); names += ","; }
                    std::fprintf(stderr,"CP2077 w32 menu puppets: class=%s scenes=%s\\n", GetClass() ? GetClass()->GetName().AsChar() : "?", names.c_str());
                    static const int cpSkip = ::CpWebMemIos() && MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'menuPuppets' ) === '0' ? 1 : 0; } catch ( e ) { return 0; } } );
                    if ( cpSkip ) { std::fprintf(stderr,"CP2077 w32 menu puppets: skipped (?menuPuppets=0)\\n"); return; }
                }
                InitializePuppets();
            }'''),
 ('''#include <cstdio>
#include <atomic>
''', '''#include <cstdio>
#include <atomic>
#include <string>
#include <emscripten/emscripten.h>
extern "C" int CpWebMemIos(); // [w32-menupup]
'''),
], '[w32-menupup]')
