#!/usr/bin/env python3
"""Rank wasm32 link-input objects by initialized static data (DATA / .data / .rodata sections, not .bss)."""
import json, re, subprocess
from pathlib import Path
OD = r'D:\Cyber punk\tools\emsdk\upstream\bin\llvm-objdump.exe'
li = json.loads(Path(r'D:\2077\build-w32\jobs\link-inputs.json').read_text())
objs = li['Objects'] + [o for L in li['Libraries'] for o in L['Objects']]
res = []
for o in objs:
    t = subprocess.run([OD, '-h', o], capture_output=True, text=True).stdout
    d = 0
    for l in t.splitlines():
        m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)', l)
        if m and (m.group(1) == 'DATA' or m.group(1).startswith(('.data', '.rodata'))): d += int(m.group(2), 16)
    res.append((d, o))
res.sort(reverse=True)
print('total initialized data MiB', round(sum(d for d, _ in res) / 2**20, 1))
for d, o in res[:15]: print(round(d / 2**20, 1), Path(o).parent.name + '/' + Path(o).name)
