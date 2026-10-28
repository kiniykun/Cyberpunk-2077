#!/usr/bin/env python3
"""[w32] Smaller static image: wasm32 copies of the three graphics shader packages (material, static, ui).

  * Each native DXIL copy (used only to identify a shader by its exact native bytecode) becomes its 32-byte SHA-256;
    the table keeps the original byte length. FindConvertedGraphicsShaderBytecode hashes the incoming bytecode once
    and compares length + SHA-256 (gpu-shader-web.cpp w32 patch).
  * Each WGSL raw string becomes a zlib blob [u32 compressedBytes][zlib stream]; the table keeps the raw length.
    The lookup returns a lazily materialized mirror entry whose wgsl is the inflated text (stable pointer).
WGSL text is the raw string as clang compiles it (CRLF -> LF). Copies are written into build-w32/src-w32 via the
outer overlay; the wasm64 .inc files are not touched. Prints the before/after initialized-data estimate.
"""
import hashlib, json, re, subprocess, zlib
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
SRC = Path(r'D:\2077\src')
ARR = re.compile(r'static const unsigned char (k\w*DXIL\w*)\[\] = \{(.*?)\};', re.S)
STR = re.compile(r'static const char (k\w*WGSL\w*)\[\] = R"(\w*)\((.*?)\)\2";', re.S)
def carray(b):
    return ',\n'.join(','.join(str(x) for x in b[i:i + 40]) for i in range(0, len(b), 40))
total_before = total_after = 0
for name in ('gpu-material-graphics-package.inc', 'gpu-static-graphics-package.inc', 'gpu-ui-graphics-package.inc'):
    src = SRC / name
    text = src.read_bytes().decode('utf-8')
    dx_len, wg_len = {}, {}
    def dx(m):
        global total_before, total_after
        data = bytes(int(v) for v in m.group(2).replace('\r', ' ').replace('\n', ' ').split(',') if v.strip())
        dx_len[m.group(1)] = len(data); total_before += len(data); total_after += 32
        return f'static const unsigned char {m.group(1)}[] = {{ // [w32] SHA-256 of the {len(data)}-byte native DXIL\n{carray(hashlib.sha256(data).digest())}\n}};'
    def wg(m):
        global total_before, total_after
        raw = m.group(3).replace('\r\n', '\n').encode('utf-8')
        z = zlib.compress(raw, 9)
        blob = len(z).to_bytes(4, 'little') + z
        wg_len[m.group(1)] = len(raw); total_before += len(raw) + 1; total_after += len(blob)
        return f'static const unsigned char {m.group(1)}[] = {{ // [w32] zlib WGSL, {len(raw)} raw bytes\n{carray(blob)}\n}};'
    text = ARR.sub(dx, text)
    text = STR.sub(wg, text)
    n1 = n2 = 0
    for k, l in dx_len.items():
        a = f'{k}, sizeof({k})'; n1 += text.count(a); text = text.replace(a, f'{k}, {l}')
    for k, l in wg_len.items():
        a = f'{k}, sizeof({k})-1'; n2 += text.count(a); text = text.replace(a, f'reinterpret_cast<const char*>({k}), {l}')
    assert n1 == len(dx_len) and n2 == len(wg_len), (name, n1, len(dx_len), n2, len(wg_len))
    assert 'sizeof(k' not in text or name != 'x'
    subprocess.run([PY, OV, 'add', str(src)], capture_output=True, check=True)
    dst = Path(subprocess.run([PY, OV, 'resolve', str(src)], capture_output=True, text=True, check=True).stdout.strip())
    dst.write_bytes(text.encode('utf-8'))
    print(f'{name}: {len(dx_len)} DXIL -> SHA-256, {len(wg_len)} WGSL -> zlib -> {dst}')
print(f'graphics package initialized data: {total_before / 2**20:.1f} MiB -> {total_after / 2**20:.1f} MiB')
