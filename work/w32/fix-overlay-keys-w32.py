#!/usr/bin/env python3
"""The w32 VFS overlay is keyed by the ORIGINAL depot path that the compile jobs use. A few phase-4 patches were
applied to copies keyed by the engine-overlay path, which the compiler never sees. Map the original paths to the
patched contents (copy the patched file over the original-keyed w32 copy)."""
import shutil, subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
O = 'D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline\\dev\\src\\common\\'
W = 'D:\\2077\\build-w32\\src-w32\\port\\engine-overlay\\common\\'
pairs = [('redIO\\src\\redIOSystemFileLinuxAPI.cpp', 'redio\\src\\rediosystemfilelinuxapi.cpp'),
         ('gameFramework\\src\\gameEngine.cpp', 'gameframework\\src\\gameengine.cpp'),
         ('renderer\\src\\renderSkinManager.cxx', 'renderer\\src\\renderskinmanager.cxx')]
for orig, patched in pairs:
    src = O + orig
    subprocess.run([PY, OV, 'add', src], capture_output=True, check=True)
    dst = Path(subprocess.run([PY, OV, 'resolve', src], capture_output=True, text=True, check=True).stdout.strip())
    assert 'build-w32' in str(dst), dst
    shutil.copyfile(W + patched, dst)
    print(dst, open(dst, 'rb').read().count(b'[w32-'))
