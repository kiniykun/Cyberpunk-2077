"""Read stencil SRVs from component .x in converted WGSL (D3D X24G8/X32G8 SRV layout -> WebGPU stencil-only view).

The original HLSL declares every stencil SRV as Texture2D<uint2> and reads it through GetStencilValue (include_utilities.fx:
'return fetchedValue.y;') or GatherStencilValues ('GatherGreen'): D3D's X24_TYPELESS_G8_UINT / X32_TYPELESS_G8X24_UINT views
put the stencil in .g. The runtime binds those SRVs as a WebGPU stencil-only view (gpu-api-texture-web.cpp aspect
StencilOnly), and WebGPU returns the stencil value in .x (.y reads 0; measured on the live device: textureLoad over the
scene depth-stencil -> max .x=149, max .y=0). Without this pass every converted shader decodes MaterialID/ObjectID 0 for
all pixels (no subsurface/hair/eye/foliage workflow, no ROT_NoLighting discard).

Which SRVs are stencil comes from the original DXIL resource metadata: every Texture2D<vector<unsigned int, 2> > SRV in the
engine shaders is a stencil SRV (all 'TEXTURE2D<uint2>' declarations under engine/shaders are named *Stencil*), while
uint4 histogram textures and uint (R8/R32_UINT) SRVs keep their other DXIL types and are never touched. For each such SRV
the WGSL binding (base + t#) must be a texture_2d<u32>; every textureLoad result must be read only as '.y' (rewritten to
'.x') and every textureGather must use component 1 (rewritten to 0). Any other use raises instead of guessing.
"""
import functools
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DXC = ROOT / 'tools/dxc-1.9.2609/bin/x64/dxc.exe'
# !{i32 id, %"class.Texture2D<vector<unsigned int, 2> >"* undef, !"name", i32 space, i32 lowerBound, i32 rangeSize, ...}
STENCIL_SRV = re.compile(r'%"class\.Texture2D<vector<unsigned int, 2> >"\* undef, !"[^"]*", i32 (\d+), i32 (\d+), i32 (\d+),')


@functools.lru_cache(maxsize=None)
def stencil_srv_registers(dxil_path):
    result = subprocess.run([str(DXC), '-dumpbin', str(dxil_path)], capture_output=True, text=True)
    if result.returncode != 0 or "!dx.entryPoints" not in result.stdout:
        raise ValueError(f'dxc -dumpbin failed for {dxil_path}')
    if '!dx.resources' not in result.stdout:
        return ()  # no resources at all
    registers = set()
    for space, lower, count in STENCIL_SRV.findall(result.stdout):
        if int(space) != 0 or int(count) != 1:
            raise ValueError(f'Unexpected stencil SRV range space={space} count={count} in {dxil_path}')
        registers.add(int(lower))
    return tuple(sorted(registers))


def lower_stencil_reads(text, dxil_path, srv_binding_base):
    """Returns (text, rewritten_reads). Unchanged text when the shader has no live stencil SRV."""
    if os.environ.get('CP2077_SKIP_STENCIL_LOWERING') == '1' or not dxil_path:
        return text, 0
    rewritten = 0
    for register in stencil_srv_registers(Path(dxil_path)):
        binding = srv_binding_base + register
        declaration = re.search(r'@group\(0\) @binding\(' + str(binding) + r'\)\s*var(?:<[^>]*>)? (\w+): ([^;]+);', text)
        if not declaration:
            continue  # SRV declared in DXIL but not live after conversion.
        var, wgsl_type = declaration.groups()
        if wgsl_type.strip() != 'texture_2d<u32>':
            raise ValueError(f'Stencil SRV t{register} (binding {binding}) is {wgsl_type}, expected texture_2d<u32>')
        uses = len(re.findall(r'\b' + var + r'\b', text)) - 1
        loads = re.findall(r'let (_e\d+) = textureLoad\(' + var + r', ', text)
        gathers = re.findall(r'textureGather\((\d+)u?, ' + var + r', ', text)
        dims = len(re.findall(r'textureDimensions\(' + var + r'\b', text))
        if uses != len(loads) + len(gathers) + dims:
            raise ValueError(f'Unrecognized use of stencil SRV {var} (binding {binding})')
        for load in loads:
            reads = re.findall(r'\b' + load + r'\b(\.[A-Za-z_]\w*)?', text)[1:]
            if not reads or any(r != '.y' for r in reads):
                raise ValueError(f'Stencil load {load} of {var} read as {sorted(set(reads))}, expected only .y')
            text, n = re.subn(r'\b' + load + r'\.y\b', load + '.x', text)
            rewritten += n
        if any(c != '1' for c in gathers):
            raise ValueError(f'Stencil gather of {var} uses components {gathers}, expected only 1 (GatherGreen)')
        text, n = re.subn(r'textureGather\(1(u?), ' + var + r', ', r'textureGather(0\1, ' + var + ', ', text)
        rewritten += n
    return text, rewritten
