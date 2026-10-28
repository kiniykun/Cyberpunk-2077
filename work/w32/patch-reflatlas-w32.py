#!/usr/bin/env python3
"""[w32-reflatlas] mem=ios: world reflection-probe atlas with a quarter of the slots (like PREVIEW scenes):
512x256x32 mips6 R11G11B10 (21 MiB) -> 8 slots (5.3 MiB), 2 relight stages."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
orig = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\renderReflectionProbeManager.cpp'
subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip()); assert 'build-w32' in str(p)
raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw; s = raw.replace('\r\n', '\n')
if '[w32-reflatlas]' in s: raise SystemExit('already')
a = '''		case IRenderScene::Type::WORLD:			numProbes = CReflectionProbeManager::MAX_NUM_PROBES;
			numStages = NUM_PARALLEL_STAGES;
			createSubSceneAtlas = true;
			break;'''
b = '''		case IRenderScene::Type::WORLD:			numProbes = CReflectionProbeManager::MAX_NUM_PROBES;
			numStages = NUM_PARALLEL_STAGES;
			createSubSceneAtlas = true;
			if ( ::CpWebMemIos() ) { numProbes = CReflectionProbeManager::MAX_NUM_PROBES / 4; numStages = 2; } // [w32-reflatlas]
			break;'''
assert s.count(a) == 1; s = s.replace(a, b)
a2 = '''void SceneCustomData_ReflectionAtlas::Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData )'''
assert s.count(a2) == 1; s = s.replace(a2, 'extern "C" int CpWebMemIos(); // [w32-reflatlas]\n' + a2)
p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)
