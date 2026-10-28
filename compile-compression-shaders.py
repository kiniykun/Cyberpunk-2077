"""Compile original UI compute shaders; record CPU-to-Web uniform layout maps."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent
SHADERS = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\engine\shaders')
DXC = ROOT / 'tools/dxc-1.9.2609/bin/x64/dxc.exe'
NAGA = ROOT / 'tools/naga-30.0.1/bin/naga.exe'
OUT = ROOT / 'build/shaders'
OUT.mkdir(parents=True, exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def reflect(path, structure_layouts=False, indexed=False, root_spans=False):
    data = path.read_bytes()
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    if words[0] != 0x07230203:
        raise ValueError('Not SPIR-V')
    types, names, member_names, decorations, members, constants, variables = {}, {}, {}, {}, {}, {}, []
    cursor = 5
    def string(values):
        return struct.pack('<%dI' % len(values), *values).split(b'\0', 1)[0].decode('utf-8')
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if not count or cursor + count > len(words):
            raise ValueError('Malformed SPIR-V instruction')
        args = words[cursor+1:cursor+count]
        if opcode == 5: names[args[0]] = string(args[1:])
        elif opcode == 6: member_names[args[0], args[1]] = string(args[2:])
        elif opcode == 71: decorations.setdefault(args[0], {})[args[1]] = args[2:]
        elif opcode == 72: members.setdefault((args[0], args[1]), {})[args[2]] = args[3:]
        elif 20 <= opcode <= 32: types[args[0]] = (opcode, args[1:])
        elif opcode == 43: constants[args[1]] = args[2]
        elif opcode == 59: variables.append(args)
        cursor += count

    def leaves(type_id, offset, path, matrix_info=None):
        opcode, args = types[type_id]
        if opcode in (21, 22):
            yield path, offset, args[0] // 8
        elif opcode == 23:
            width = types[args[0]][1][0] // 8
            for index in range(args[1]):
                yield from leaves(args[0], offset + index*width, path+'.'+'xyzw'[index])
        elif opcode == 24:
            if not matrix_info or 7 not in matrix_info:
                raise ValueError('Matrix stride missing')
            column_type, columns = args
            _, (scalar, rows) = types[column_type]
            width = types[scalar][1][0] // 8
            stride = matrix_info[7][0]
            row_major = 4 in matrix_info
            for column in range(columns):
                for row in range(rows):
                    relative = row*stride + column*width if row_major else column*stride + row*width
                    yield from leaves(scalar, offset+relative, f'{path}[{column}][{row}]')
        elif opcode == 28:
            element, length_id = args
            stride = decorations[type_id][6][0]
            length = constants[length_id]
            if length > 65536: raise ValueError('Unexpected uniform array length')
            for index in range(length):
                yield from leaves(element, offset + index*stride, f'{path}[{index}]', matrix_info)
        elif opcode == 30:
            for index, field_type in enumerate(args):
                metadata = members[type_id, index]
                field_name = str(index) if indexed else member_names.get((type_id, index), str(index))
                yield from leaves(field_type, offset + metadata[35][0], path+'.'+field_name, metadata)
        else:
            raise ValueError(f'Unsupported uniform type opcode {opcode}')

    buffers = {}
    spans = {}
    uniform_structures = {}
    def collect_structures(type_id):
        opcode, args = types[type_id]
        if opcode == 30:
            if type_id in uniform_structures: return
            fields = list(leaves(type_id, 0, names.get(type_id, str(type_id))))
            end = max((offset+width for _, offset, width in fields), default=0)
            uniform_structures[type_id] = {
                'Name': names.get(type_id), 'Members': len(args),
                'LastOffset': members[type_id, len(args)-1][35][0],
                'Size': (end+15)//16*16,
            }
            for child in args: collect_structures(child)
        elif opcode == 28: collect_structures(args[0])
    for pointer, variable, storage, *_ in variables:
        if storage != 2: continue
        _, (_, pointee) = types[pointer]
        if 2 not in decorations.get(pointee, {}): continue
        deco = decorations[variable]
        key = (deco[34][0], deco[33][0])
        # Naga's SPIR-V emitter adds an unnamed single-member Block wrapper
        # around the WGSL uniform type. It contributes no data or offsets.
        while types[pointee][0] == 30 and len(types[pointee][1]) == 1:
            child = types[pointee][1][0]
            if (pointee, 0) in member_names or types[child][0] != 30 or members[pointee, 0][35][0] != 0:
                break
            pointee = child
        fields = list(leaves(pointee, 0, 'root' if indexed else names.get(variable, str(variable))))
        buffers[key] = {name: (offset, width) for name, offset, width in fields}
        collect_structures(pointee)
        spans[key] = uniform_structures[pointee]['Size']
    if root_spans: return spans
    return list(uniform_structures.values()) if structure_layouts else buffers

def preserve_uniform_struct_padding(web_path, wgsl_path):
    # Naga's SPIR-V writer path can omit uniform struct tail padding in WGSL.
    # Restore the reflected std140 span, without changing any member's value or
    # shader algorithm. The native-to-Web byte map remains the SPIR-V layout.
    text = wgsl_path.read_text(encoding='utf-8')
    edits = []
    for layout in reflect(web_path, structure_layouts=True):
        name = layout['Name']
        if not name: raise ValueError('Unnamed uniform structure cannot be matched')
        generated_name = name.replace('.', '_u002e_')
        pattern = re.compile(r'\bstruct\s+'+re.escape(generated_name)+r'\s*\{([^{}]*)\}')
        match = pattern.search(text)
        if not match: raise ValueError(f'WGSL structure name not found: {name}')
        lines = match.group(1).splitlines(keepends=True)
        fields = [index for index, line in enumerate(lines) if re.search(r'\w+\s*:\s*.+,', line)]
        if len(fields) != layout['Members']: raise ValueError('WGSL structure member count changed')
        last = fields[-1]
        size = layout['Size'] - layout['LastOffset']
        line = re.sub(r'@size\(\d+\)\s*', '', lines[last]).lstrip()
        lines[last] = f'    @size({size}) '+line
        text = text[:match.start(1)]+''.join(lines)+text[match.end(1):]
        edits.append({'Struct': generated_name, 'TailMemberSize': size, 'ReflectedSpan': layout['Size']})
    wgsl_path.write_text(text, encoding='utf-8')
    return edits

def layouts(native_path, web_path, padded_path):
    native, web = reflect(native_path, indexed=True), reflect(web_path, indexed=True)
    native_names = reflect(native_path)
    padded_spans = reflect(padded_path, root_spans=True)
    if native.keys() != web.keys(): raise ValueError('Uniform bindings changed')
    result = []
    for (group, binding), source in native.items():
        target = web[group, binding]
        if source.keys() != target.keys(): raise ValueError('Uniform members changed')
        fields = []
        names = dict(zip(source, native_names[group, binding]))
        for name, (offset, width) in source.items():
            web_offset, web_width = target[name]
            if width != web_width: raise ValueError('Uniform scalar width changed')
            fields.append({'Path': names[name], 'IndexedPath': name, 'NativeOffset': offset, 'WebOffset': web_offset, 'Bytes': width})
        source_size = max((f['NativeOffset']+f['Bytes'] for f in fields), default=0)
        web_size = max((f['WebOffset']+f['Bytes'] for f in fields), default=0)
        padded_size = padded_spans[group, binding]
        if web_size > padded_size: raise ValueError('WGSL fields exceed preserved uniform span')
        result.append({'Group': group, 'Binding': binding, 'NativeMinimumBytes': source_size,
                       'WebBytes': padded_size, 'Fields': fields})
    return result

def run(command, log):
    completed = subprocess.run([str(arg) for arg in command], capture_output=True)
    log.write_bytes(completed.stdout + completed.stderr)
    if completed.returncode:
        raise RuntimeError(f'Compiler failed: {log}')

def specialize_tile_storage(filename, wgsl_path):
    if filename == 'textureCompress.fx':
        text = wgsl_path.read_text(encoding='utf-8')
        if re.search(r'\btextureLoad\s*\(\s*Target\b', text):
            raise ValueError('Compression target is read; write-only narrowing is invalid')
        if not re.search(r'\btextureStore\s*\(\s*Target\b', text):
            raise ValueError('Original compression target store was not found')
        pattern = r'(\bvar\s+Target\s*:\s*texture_storage_2d<\s*(rg32uint|rgba32uint)\s*,\s*)read_write(\s*>)'
        formats = re.findall(pattern,text)
        text,count = re.subn(pattern,r'\1write\3',text)
        if count != 1: raise ValueError('Unexpected compression target format/access')
        wgsl_path.write_text(text,encoding='utf-8')
        evidence = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\gpuApi\src\gpuApiTextureCompressor.inl')
        return {'Format':formats[0][1],'Access':'write','Evidence':str(evidence),
                'EvidenceSHA256':sha(evidence),'RequiresDeviceFeature':'texture-formats-tier1' if formats[0][1]=='rg32uint' else None}
    if filename == 'hud.fx':
        text = wgsl_path.read_text(encoding='utf-8')
        if re.search(r'\btextureLoad\s*\(\s*OutputTexture\b', text):
            raise ValueError('HUD storage output reads require new access translation')
        if not re.search(r'\btextureStore\s*\(\s*OutputTexture\b', text):
            raise ValueError('HUD output store was not found')
        pattern = r'(\bvar\s+OutputTexture\s*:\s*)texture_storage_2d<\s*rgba32float\s*,\s*read_write\s*>'
        text, count = re.subn(pattern, r'\1texture_storage_2d<rgba8unorm,write>', text)
        if count != 1: raise ValueError('Unexpected HUD storage declaration')
        evidence = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\renderNode_Composition.cpp')
        if 'finalDesc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm_SRGB;' not in evidence.read_text(encoding='utf-8-sig'):
            raise ValueError('Original SDR output format changed')
        wgsl_path.write_text(text, encoding='utf-8')
        return {'Format': 'rgba8unorm', 'Access': 'write',
                'NativeFormat': 'TEXFMT_R8G8B8A8_Unorm_SRGB', 'OutputMode': 'SDR',
                'Evidence': str(evidence), 'EvidenceSHA256': sha(evidence),
                'RequiresDeviceFeature': None, 'HDRVariantsIntegrated': False}
    if filename not in ('prepareHudTiles.fx', 'dilateHudTiles.fx'): return None
    text = wgsl_path.read_text(encoding='utf-8')
    # Original renderNode_Composition.cpp creates these outputs as R8_Unorm.
    # The original kernels only write OutputTexture. Narrowing its access is
    # validated by Naga, rather than assuming all HLSL UAVs need read/write.
    if re.search(r'\btextureLoad\s*\(\s*OutputTexture\b', text):
        raise ValueError('Tile output now reads from storage: specialization must be reviewed')
    if not re.search(r'\btextureStore\s*\(\s*OutputTexture\b', text):
        raise ValueError('Tile output store was not found')
    pattern = r'(\bvar\s+OutputTexture\s*:\s*)texture_storage_2d<\s*r32float\s*,\s*read_write\s*>'
    text, count = re.subn(pattern, r'\1texture_storage_2d<r8unorm,write>', text)
    if count != 1: raise ValueError('Unexpected tile storage declaration')
    # Browser requirements are checked explicitly before module creation.
    # Naga 30 parses the format but not this newer optional requires directive.
    wgsl_path.write_text(text, encoding='utf-8')
    evidence = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\renderer\src\renderNode_Composition.cpp')
    if 'tileMapDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;' not in evidence.read_text(encoding='utf-8-sig'):
        raise ValueError('Original tile allocation format changed')
    return {'Format': 'r8unorm', 'Access': 'write', 'NativeFormat': 'TEXFMT_R8_Unorm',
            'Evidence': str(evidence), 'EvidenceSHA256': sha(evidence),
            'RequiresDeviceFeature': 'texture-formats-tier1'}

manifest = {'DxcVersion': '1.9.2609', 'DxcSHA256': sha(DXC), 'NagaVersion': '30.0.1',
            'NagaSHA256': sha(NAGA), 'RuntimeUniformRepackingIntegrated': False,
            'BrowserRenderingVerified': False,
            'ShaderIncludeSnapshot': {str(path.relative_to(SHADERS)): sha(path)
                for path in sorted((SHADERS/'include').rglob('*')) if path.is_file()},
            'Shaders': []}
registry = SHADERS/'staticShaders.xml'
manifest['StaticShaderRegistrySHA256'] = sha(registry)
selected = {'textureCompress.fx'}
specs = []
registry_text = registry.read_text(encoding='utf-8-sig')
# An unrelated original shader group has a duplicate XML attribute. Parse only
# selected complete groups without rewriting or normalizing the source depot.
for match in re.finditer(r'<ShaderGroup\b[^>]*>[\s\S]*?</ShaderGroup>', registry_text):
    opening = match.group(0).split('>', 1)[0]
    file_match = re.search(r'\bFileName="([^"]+)"', opening)
    if not file_match or file_match.group(1) not in selected: continue
    group = ET.fromstring(match.group(0))
    filename = group.attrib.get('FileName')
    if filename not in selected: continue
    for shader in group.findall('Shader'):
        if shader.attrib['Type'] != 'CS': raise ValueError('Expected original compute variant')
        runtime_name = shader.attrib['VariableName']
        defines = {entry.attrib['Name']: entry.attrib['Value'] for entry in shader.findall('Defines/Define')}
        specs.append((runtime_name.removeprefix('m_'), filename, runtime_name, defines))
if len(specs) != 4: raise ValueError('Original compression shader registry variants changed')
for name, filename, runtime_name, defines in specs:
    record = {'Name': name, 'RuntimeName': runtime_name, 'Defines': defines,
              'Source': str(SHADERS/filename),
              'SourceSHA256': sha(SHADERS/filename), 'Complete': False}
    manifest['Shaders'].append(record)
    try:
        common = [DXC, SHADERS/filename, '-spirv', '-HV', '2018', '-T', 'cs_6_0', '-E', 'cs_main_',
                  '-D', 'RED_PLATFORM_WINPC=1', '-D', 'COMPUTESHADER=1',
                  '-I', SHADERS/'include', '-I', SHADERS,
                  '-fvk-b-shift', '0', '0', '-fvk-t-shift', '64', '0',
                  '-fvk-s-shift', '256', '0', '-fvk-u-shift', '288', '0']
        for define, value in defines.items(): common.extend(['-D', f'{define}={value}'])
        native, web, wgsl = OUT/f'{name}-native.spv', OUT/f'{name}-web.spv', OUT/f'{name}-web.wgsl'
        run(common+['-fvk-use-dx-layout', '-Fo', native], OUT/f'{name}-native-dxc.log')
        run(common+['-fvk-use-gl-layout', '-Fo', web], OUT/f'{name}-web-dxc.log')
        run([NAGA, web, wgsl], OUT/f'{name}-web-naga.log')
        record['UniformTailPadding'] = preserve_uniform_struct_padding(web, wgsl)
        record['StorageOutputSpecialization'] = specialize_tile_storage(filename, wgsl)
        run([NAGA, wgsl], OUT/f'{name}-wgsl-validation.log')
        # Re-import generated WGSL and compare its actual layout through a
        # separately emitted SPIR-V module, rather than assuming writer offsets.
        audit = OUT/f'{name}-wgsl-layout-audit.spv'
        run([NAGA, '-g', wgsl, audit], OUT/f'{name}-wgsl-layout-audit.log')
        expected, actual = reflect(web, indexed=True), reflect(audit, indexed=True)
        if expected.keys() != actual.keys(): raise ValueError('WGSL roundtrip uniform bindings changed')
        for key in expected:
            if expected[key].keys() != actual[key].keys():
                raise ValueError('WGSL uniform scalar coordinates changed')
            if any(expected[key][name][1] != actual[key][name][1] for name in expected[key]):
                raise ValueError('WGSL uniform scalar width changed')
        record['WGSLUniformLayoutRoundtripVerified'] = True
        record.update({'WGSL': str(wgsl), 'WGSLSHA256': sha(wgsl), 'EntryPoint': 'cs_main_',
                       'UniformLayouts': layouts(native, audit, web), 'Complete': True,
                       'NativeSPIRVSHA256': sha(native), 'WebSPIRVSHA256': sha(web)})
    except Exception as error:
        record['Error'] = str(error)
    (OUT/'compression-shader-conversion.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'{name}: {"converted" if record["Complete"] else record["Error"]}')
if not all(record['Complete'] for record in manifest['Shaders']):
    raise SystemExit(1)
