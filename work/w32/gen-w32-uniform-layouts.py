#!/usr/bin/env python3
"""[w32-unilayout] wasm32 overlay copies of the generated uniform-layout tables with smaller static data:
each UniformScalarCopy array is merged into runs (adjacent in BOTH native and web layouts, the same merge the
consumers already do at runtime, see gpu-api-command-web.cpp RepackNativeGpuConstant) and identical arrays are
shared through #define aliases. RepackUniform/validation semantics are unchanged (same bytes copied, padding still
zeroed, every run still 4-byte aligned)."""
import re, subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
ARR = re.compile(r'static const UniformScalarCopy (\w+)\[\] = \{\n(.*?)\n\};\n', re.S)
ROW = re.compile(r'\{(\d+), (\d+), (\d+)\}')
for name in ('gpu-material-graphics-uniform-layouts.inc', 'gpu-static-graphics-uniform-layouts.inc',
             'gpu-uniform-layouts.inc', 'gpu-graphics-uniform-layouts.inc'):
    orig = 'D:\\2077\\src\\' + name
    src = Path(orig).read_text(encoding='utf-8')
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    dst = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
    assert 'build-w32' in str(dst), dst
    canon, aliases, before, after = {}, [], 0, 0
    def merge(m):
        global before, after
        rows = [tuple(map(int, r)) for r in ROW.findall(m.group(2))]
        assert rows, m.group(1)
        before += len(rows)
        runs = []
        for n, w, b in rows:
            if runs and runs[-1][0] + runs[-1][2] == n and runs[-1][1] + runs[-1][2] == w: runs[-1][2] += b
            else: runs.append([n, w, b])
        key = tuple(map(tuple, runs))
        if key in canon:
            aliases.append(f'#define {m.group(1)} {canon[key]}')
            return ''
        canon[key] = m.group(1); after += len(runs)
        return (f'static const UniformScalarCopy {m.group(1)}[] = {{\n' +
                ',\n'.join(f'    {{{n}, {w}, {b}}}' for n, w, b in runs) + '\n};\n')
    out, count = ARR.subn(merge, src)
    # aliases must precede the layout rows that use them: put them right before the first layout table
    first = out.find('static const UniformLayout ')
    assert first >= 0, name
    out = out[:first] + '// [w32-unilayout] identical field arrays shared\n' + '\n'.join(aliases) + '\n' + out[first:]
    dst.write_text('// [w32-unilayout] merged/deduplicated copy of ' + name + '\n' + out, encoding='utf-8')
    print(f'{name}: arrays {count} unique {len(canon)} rows {before} -> {after}, bytes {len(src)} -> {len(out)}')
