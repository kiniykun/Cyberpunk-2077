#!/usr/bin/env python3
"""Byte split of the embedded shader packages: DXIL arrays (native bytecode copies) vs WGSL raw strings."""
import re, sys
from pathlib import Path
SRC = Path(r'D:\2077\src')
for name in ('gpu-material-graphics-package.inc', 'gpu-shader-package.inc', 'gpu-ui-graphics-package.inc', 'gpu-static-graphics-package.inc'):
    p = SRC / name
    if not p.exists(): continue
    t = p.read_text(encoding='utf-8', errors='replace')
    dx = 0; n = 0
    for m in re.finditer(r'static const unsigned char (\w+)\[\] = \{(.*?)\};', t, re.S):
        dx += m.group(2).count(',') + 1; n += 1
    wg = sum(len(m.group(1)) for m in re.finditer(r'R"CPUIWGSL\((.*?)\)CPUIWGSL"', t, re.S))
    wg2 = sum(len(m.group(1)) for m in re.finditer(r'R"(\w*)\((.*?)\)\1"', t, re.S)) if not wg else wg
    print(f'{name}: {n} byte arrays {dx/2**20:.1f} MiB, raw strings {wg2/2**20:.1f} MiB')
