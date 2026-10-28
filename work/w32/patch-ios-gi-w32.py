#!/usr/bin/env python3
"""[w32-iosgi] mem=ios: smaller global-illumination GPU pools via the engine's own GI settings defaults
(GlobalRenderingSettings constructor, so every copy agrees with the buffers): maxSectors 256 -> 96 (lit probes
112 -> 42 MiB; the nearest sectors keep slots, farther ones go without GI, as the slot code already handles) and
tetraWorkspaceBudget 48 -> 24 MiB (tetra lookup 48 -> 24 MiB + interpolation 60 -> 30 MiB)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
orig = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderData\src\renderSettingsVars.cpp'
subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
assert 'build-w32' in str(p), p
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw; s = raw.replace('\r\n', '\n')
if '[w32-iosgi]' in s: print('already'); raise SystemExit
for a, b in [
 ('#include "renderSettingsVars.h"\n', '#include "renderSettingsVars.h"\nextern "C" int CpWebMemIos(); // [w32-iosgi]\n'),
 ('''		globalIllumination.tetraWorkspaceBudget = 48 * 1024 * 1024;
''', '''		globalIllumination.tetraWorkspaceBudget = 48 * 1024 * 1024;
		if ( ::CpWebMemIos() ) // [w32-iosgi] iPhone memory profile: smaller GI pools
		{
			globalIllumination.maxSectors = 24;
			globalIllumination.tetraWorkspaceBudget = 8 * 1024 * 1024;
		}
''')]:
    assert s.count(a) == 1, a; s = s.replace(a, b)
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)
