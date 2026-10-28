#!/usr/bin/env python3
"""[w32-streammax] ?streamMax=<metres> caps every world node's streaming radius (fewer meshes/textures/nodes resident:
the iPhone memory budget). Read once from location.search, which pthread workers also carry (their script URL gets
the page query), so no main-thread call is needed. 0/absent = unchanged."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
orig = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\world\src\worldNodeStreamingGrid.cpp'
subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip()); assert 'build-w32' in str(p), p
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw; s = raw.replace('\r\n', '\n')
if '[w32-streammax]' in s: raise SystemExit('already')
a = '''		return Clamp<Float>( streamingRange + Config::cvStreamingPrecacheDistance.Get(), minRadius, maxRadius );'''
b = '''#ifdef __EMSCRIPTEN__
		{ // [w32-streammax]
			static const Float cpStreamMax = Float( EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'streamMax' ); const n = v === null ? 0 : parseFloat( v ); return n > 0 ? n : 0; } catch ( e ) { return 0; } } ) );
			if ( cpStreamMax > 0.0f )
				return Clamp<Float>( std::min<Float>( streamingRange + Config::cvStreamingPrecacheDistance.Get(), cpStreamMax ), minRadius, maxRadius );
		}
#endif
''' + a
assert s.count(a) == 1, s.count(a); s = s.replace(a, b)
if '#include <emscripten.h>' not in s: s = '#ifdef __EMSCRIPTEN__\n#include <emscripten.h>\n#endif\n' + s
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
