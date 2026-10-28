"""Inventory original multilayer descriptor access, without changing bytecode."""
from pathlib import Path
import hashlib, json, struct

ROOT = Path(__file__).resolve().parents[1]
grammar = json.loads((ROOT/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
names = {i['opcode']: i['opname'] for i in grammar['instructions']}
definitions = {i['opcode']: i for i in grammar['instructions']}
hashes = ('3325971195118690692', '858400793762759017', '10497871587094582139')
rows = []
for shader in hashes:
    path = ROOT/f'build/shaders/native-observed-material-converted/{shader}-pixel.spv'
    data = path.read_bytes()
    words = struct.unpack(f'<{len(data)//4}I', data)
    assert words[0] == 0x07230203
    ops = []
    pos = 5
    while pos < len(words):
        count, opcode = words[pos] >> 16, words[pos] & 65535
        assert count > 0 and pos + count <= len(words)
        ops.append((opcode, list(words[pos+1:pos+count])))
        pos += count
    types = {a[0]: (op, a[1:]) for op,a in ops if 19 <= op <= 33}
    constants = {a[1]: a[2:] for op,a in ops if op == 43}
    decorations = {}
    for op,a in ops:
        if op == 71:
            decorations.setdefault(a[0], {})[a[1]] = a[2:]
    arrays, resource_ids = [], set()
    for op,a in ops:
        if op != 59 or 33 not in decorations.get(a[1], {}):
            continue
        pointer, variable, storage = a[:3]
        pointee = types[pointer][1][1]
        kind, args = types[pointee]
        if kind != 28:
            continue
        resource_ids.add(variable)
        arrays.append({'Variable':variable, 'Storage':storage,
                       'Binding':decorations[variable][33][0],
                       'ElementType':names[types[args[0]][0]],
                       'ElementCount':constants[args[1]][0]})
    accesses = []
    loaded_images, sampled_images = set(), set()
    # SPIR-V access chains precede their uses in the same basic block.
    pointer_ids = set(resource_ids)
    for op,a in ops:
        if op in (65,66,67) and a[2] in pointer_ids:
            pointer_ids.add(a[1])
            accesses.append({'Opcode':names[op], 'Result':a[1], 'Base':a[2],
                             'Indices':[{'Id':i,'Constant':constants.get(i)} for i in a[3:]]})
        elif op == 61 and a[2] in pointer_ids:
            loaded_images.add(a[1])
        elif op == 86 and a[2] in loaded_images:
            sampled_images.add(a[1])
    samples = [{'Opcode':names[op], 'Operands':a} for op,a in ops
               if names[op].startswith('OpImage') and len(a)>2
               and a[2] in (loaded_images | sampled_images)]
    assert arrays and accesses and samples, shader
    # Follow only grammar-declared IDs; literal offsets are not SSA references.
    dependencies, producers = {}, {}
    for op,a in ops:
        cursor, result, refs = 0, None, []
        for operand in definitions[op].get('operands', []):
            kind = operand['kind']
            width = 2 if kind.startswith('Pair') else 1
            repeats = (len(a)-cursor)//width if operand.get('quantifier') == '*' else 1
            for _ in range(repeats):
                if cursor >= len(a): break
                if kind == 'LiteralString':
                    while cursor < len(a):
                        word=a[cursor];cursor+=1
                        if 0 in struct.pack('<I', word): break
                    continue
                chunk=a[cursor:cursor+width];cursor+=width
                if kind == 'IdResult': result=chunk[0]
                elif kind in ('IdRef','IdScope','IdMemorySemantics'): refs.extend(chunk)
                elif kind == 'PairIdRefIdRef': refs.extend(chunk)
                elif kind == 'PairIdRefLiteralInteger': refs.append(chunk[0])
                elif kind == 'PairLiteralIntegerIdRef' and len(chunk)==2: refs.append(chunk[1])
        if result is not None:
            dependencies[result]=refs;producers[result]=(op,a)
    variables={a[1]:a[2] for op,a in ops if op==59}
    index_sources=[]
    for access in accesses:
        pending=[i['Id'] for i in access['Indices']];seen=set()
        while pending:
            ident=pending.pop()
            if ident in seen:continue
            seen.add(ident);pending.extend(dependencies.get(ident, []))
        roots=[{'Variable':v,'Storage':variables[v],
                'Binding':decorations.get(v,{}).get(33),
                'Location':decorations.get(v,{}).get(30)} for v in sorted(seen & variables.keys())]
        index_sources.append({'AccessResult':access['Result'],'Sources':roots,
                              'InputDependent':any(r['Storage']==1 for r in roots)})
    rows.append({'Hash':shader,'SPIRVSHA256':hashlib.sha256(data).hexdigest(),
                 'DescriptorArrays':arrays, 'DescriptorAccesses':accesses,
                 'ImageOperations':samples,'IndexDependencies':index_sources})
report = {'Status':'Inventory only; no shader translation or runtime fix claimed',
          'NativeTextureLookup':'AcquireIndirectGpuTextureView(uint16_t, TextureRef&, WGPUTextureView&)',
          'NativeLayerStrideBytes':128,
          'NativeLayerTextureWords':{'Microblend':112,'DiffuseNormal':116,'RoughnessMetalness':120},
          'Shaders':rows}
target = ROOT/'build/shaders/multilayer-bindless-access-audit.json'
target.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'Report':str(target),'Shaders':[
    {'Hash':r['Hash'],'Arrays':r['DescriptorArrays'],'Accesses':len(r['DescriptorAccesses']),
     'Samples':len(r['ImageOperations'])} for r in rows]},indent=2))
