"""Lower the original multilayered pixel shaders' subgroup waterfalls to per-lane execution.

dxil-spirv lowers NonUniformResourceIndex(t130[id]) into a "waterfall": ballot the lanes, broadcast the
first lane's id, run the sample for lanes whose id matches, clear them from the mask, repeat. Executed
with every lane as its own subgroup (ballot -> own bit, broadcastFirst -> own value, helper -> false,
subgroup lane id -> 0) each waterfall runs exactly once per pixel with that pixel's own index, which is
the exact per-pixel result the original computes. No subgroup feature is then required.
"""
import struct

OP_LOAD, OP_VARIABLE, OP_DECORATE, OP_ENTRY, OP_CAPABILITY = 61, 59, 71, 15, 17
OP_COPY, OP_SELECT, OP_CONSTRUCT = 83, 169, 80
OP_BROADCAST_FIRST, OP_BALLOT = 338, 339
BUILTIN_HELPER, BUILTIN_LANE = 23, 41
SUBGROUP_CAPS = {61, 62, 63, 64, 65, 66, 67, 68, 69, 4423}  # GroupNonUniform*, SubgroupBallotKHR


def parse(data):
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    assert words[0] == 0x07230203
    ops = []; pos = 5
    while pos < len(words):
        count, op = words[pos] >> 16, words[pos] & 65535
        ops.append([op, words[pos + 1:pos + count]]); pos += count
    return words[:5], ops


def emit(header, ops):
    words = list(header)
    for op, a in ops: words += [(len(a) + 1) << 16 | op, *a]
    return struct.pack('<%dI' % len(words), *words)


def lower_waterfalls(data):
    header, ops = parse(data)
    def new_id():
        value = header[3]; header[3] += 1; return value
    builtins = {a[0]: a[2] for op, a in ops if op == OP_DECORATE and a[1] == 11}
    removed = {v for v, b in builtins.items() if b in (BUILTIN_HELPER, BUILTIN_LANE)}
    kind = {v: builtins[v] for v in removed}
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    bool_t = next(i for i, (op, _) in types.items() if op == 20)
    uint_t = next(i for i, (op, a) in types.items() if op == 21 and a == [32, 0])
    constants = {}
    for op, a in ops:
        if op == 43 and a[0] == uint_t: constants.setdefault(a[2], a[1])
    added = []
    def uconst(value):
        if value not in constants:
            ident = new_id(); constants[value] = ident; added.append((43, [uint_t, ident, value]))
        return constants[value]
    false_id = next((a[1] for op, a in ops if op == 42 and a[0] == bool_t), None)
    if false_id is None:
        false_id = new_id(); added.append((42, [bool_t, false_id]))
    u0, u1 = uconst(0), uconst(1)
    audit = {'HelperLoads': 0, 'LaneLoads': 0, 'BroadcastFirst': 0, 'Ballot': 0}
    out = []
    for op, a in ops:
        if op == OP_CAPABILITY and a[0] in SUBGROUP_CAPS: continue
        if op == 10 and b'SPV_KHR_shader_ballot' in struct.pack('<%dI' % len(a), *a): continue
        if op in (OP_DECORATE, 5) and a[0] in removed: continue
        if op == OP_VARIABLE and a[1] in removed: continue
        if op == OP_ENTRY:
            end = 2
            while not any(b == 0 for b in struct.pack('<I', a[end])): end += 1
            end += 1
            a = a[:end] + [i for i in a[end:] if i not in removed]
        elif op == OP_LOAD and a[2] in removed:
            helper = kind[a[2]] == BUILTIN_HELPER
            audit['HelperLoads' if helper else 'LaneLoads'] += 1
            op, a = OP_COPY, [a[0], a[1], false_id if helper else u0]
        elif op == OP_BROADCAST_FIRST:
            audit['BroadcastFirst'] += 1
            op, a = OP_COPY, [a[0], a[1], a[3]]
        elif op == OP_BALLOT:
            audit['Ballot'] += 1
            bit = new_id()
            out.append([OP_SELECT, [uint_t, bit, a[3], u1, u0]])
            op, a = OP_CONSTRUCT, [a[0], a[1], bit, u0, u0, u0]
        elif 349 <= op <= 361 and len(a) == 5 and a[3] in (0, 1):
            # GroupNonUniform arithmetic/bitwise Reduce or InclusiveScan over a one-lane subgroup = the lane's value.
            audit['LaneReductions'] = audit.get('LaneReductions', 0) + 1
            op, a = OP_COPY, [a[0], a[1], a[4]]
        elif op == 337 and len(a) == 5:
            # GroupNonUniformBroadcast from the only lane (id 0, the only valid id) = own value.
            audit['LaneBroadcasts'] = audit.get('LaneBroadcasts', 0) + 1
            op, a = OP_COPY, [a[0], a[1], a[3]]
        elif op in (OP_BROADCAST_FIRST,) or 333 <= op <= 366:
            raise ValueError('Unhandled subgroup operation %d' % op)
        out.append([op, a])
    first_function = next(i for i, (op, _) in enumerate(out) if op == 54)
    out[first_function:first_function] = [list(x) for x in added]
    return emit(header, out), audit


# Original multilayered pixel shaders (sha256 of the DXBC/DXIL container) handled by this lowering.
MULTILAYER_SHA256 = {
    'c023c4c9a8b7e4c22a23137de17f0d2cae8712a641fc915d2220ddc71fdf7254',
    'f6e06190d6c8cd16734a143b2e812a3df35164367e47b0eda37545965146c68c',
    'd5b3d9bcbdf34b23bae0db26ee235d683e616573c50d16094aa18af1720f38eb',
}


ML_POOL_SLOTS = 40          # texture_2d_array pool bindings (see src/gpu-api-texture-web.cpp multilayer pools)
ML_POOL_BINDING_BASE = 640  # pixel-stage free range (texture SRVs end at 639, samplers start at 768)
ML_SAMPLED_TEXTURE_LIMIT = 48  # [npcblack] maxSampledTexturesPerShaderStage requested by src/gpu-device-web.cpp
ML_TABLE_BINDING = 764     # 64K x u32: bindless index -> (pool slot + 1) << 16 | layer; 0 = unmapped
ML_SAMPLE_OPS = ('textureSampleGrad', 'textureSampleLevel')


def bindless_table_in_dump(dump):
    """True when the original DXIL binds the 32768-entry bindless texture table t130."""
    import re
    return re.search(r'\bt130\s+32768\s*$', dump, re.M) is not None


def _call_args(text, open_paren):
    """Return (end, [args]) for the call whose '(' is at open_paren."""
    pos = open_paren + 1
    depth = 1
    while depth and pos < len(text):
        depth += {'(': 1, ')': -1}.get(text[pos], 0)
        pos += 1
    if depth:
        raise ValueError('Malformed WGSL call')
    args = []
    last = open_paren + 1
    level = 0
    for i in range(open_paren + 1, pos - 1):
        c = text[i]
        if c in '([':
            level += 1
        elif c in ')]':
            level -= 1
        elif c == ',' and not level:
            args.append(text[last:i].strip())
            last = i + 1
    args.append(text[last:pos - 1].strip())
    return pos, args


def lower_bindless_wgsl(text, resources):
    """Stage 1 (before sampler-bias lowering): t130 binding_array -> texture_2d_array placeholder.

    Every NAME[idx] sampling call becomes OP(NAME, sampler, uv, cp2077_ml_index(idx), ...), so the bias
    lowering sees an ordinary array texture (lod/grad positions shift by one, exactly as for a real array).
    finalize_bindless_wgsl() then replaces each call with a per-sampler pool helper.
    """
    import re
    binding = next(r['Binding'] for r in resources if r.get('MultilayerBindless'))
    match = re.search(r'@group\(0\)\s*@binding\(' + str(binding) +
                      r'\)\s*var\s+(\w+)\s*:\s*binding_array<texture_2d<f32>,\s*32768>;', text)
    if not match:
        raise ValueError('Multilayer bindless declaration not found')
    name = match[1]
    head = match[0][:match[0].index('binding_array')]
    text = text[:match.start()] + head + 'texture_2d_array<f32>;' + text[match.end():]
    text = re.sub(r'^\s*enable\s+wgpu_binding_array\s*;\s*$\n?', '', text, flags=re.M)
    ops = '|'.join(ML_SAMPLE_OPS)
    accesses = 0
    while True:
        call = re.search(r'\b(' + ops + r')\s*\(\s*' + name + r'\s*\[', text)
        if not call:
            break
        paren = text.index('(', call.start())
        end, args = _call_args(text, paren)
        first = args[0]
        index = first[first.index('[') + 1:first.rindex(']')].strip()
        args = [name, args[1], args[2], 'cp2077_ml_index(' + index + ')'] + args[3:]
        text = text[:call.start()] + call[1] + '(' + ', '.join(args) + ')' + text[end:]
        accesses += 1
    if re.search(r'\b' + name + r'\s*\[', text):
        raise ValueError('Unsupported multilayer bindless access (only %s)' % ops)
    if 'binding_array' in text:
        raise ValueError('Residual binding_array after multilayer lowering')
    return text, [{'Binding': binding, 'DynamicAccessesRedirected': accesses, 'RuntimeTexture': 'texture-array pools'}]


def finalize_bindless_wgsl(text, resources):
    """Stage 2 (after sampler-bias lowering): pool bindings, the index table and per-sampler helpers."""
    import re
    binding = next(r['Binding'] for r in resources if r.get('MultilayerBindless'))
    decl = re.search(r'@group\(0\)\s*@binding\(' + str(binding) + r'\)\s*var\s+(\w+)\s*:\s*texture_2d_array<f32>;[ \t]*\n?', text)
    if not decl:
        raise ValueError('Multilayer placeholder declaration not found')
    name = decl[1]
    text = text[:decl.start()] + text[decl.end():]
    helpers = {}
    calls = 0
    while True:
        call = re.search(r'\b(' + '|'.join(ML_SAMPLE_OPS) + r')\s*\(\s*' + name + r'\s*,', text)
        if not call:
            break
        paren = text.index('(', call.start())
        end, args = _call_args(text, paren)
        op, sampler = call[1], args[1]
        if not re.fullmatch(r'\w+', sampler):
            raise ValueError('Multilayer sampler is not a global')
        layer = args[3]
        if not (layer.startswith('cp2077_ml_index(') and layer.endswith(')')):
            raise ValueError('Multilayer layer argument lost')
        index = layer[len('cp2077_ml_index('):-1]
        helper = 'cp2077_ml_%s_%s' % ('grad' if op == 'textureSampleGrad' else 'level', sampler)
        helpers[helper] = (op, sampler)
        text = text[:call.start()] + helper + '(u32(' + index + '), ' + ', '.join([args[2]] + args[4:]) + ')' + text[end:]
        calls += 1
    if re.search(r'\b' + name + r'\b', text):
        raise ValueError('Residual multilayer placeholder use')
    # [npcblack] WebGPU caps sampled textures per stage (48 requested by gpu-device-web.cpp on the test adapters).
    # A shader with N ordinary sampled textures declares only the first min(ML_POOL_SLOTS, limit - N) pools. Pool
    # slots are handed out in first-use order (gpu-texture-web.cpp), so only layers in the last, rarely used pools
    # fall back to the helper's neutral default. Shaders within the limit keep all 40 pools (byte-identical output).
    ordinary = len(re.findall(r'var\s+\w+\s*:\s*texture_(?!storage)', text))
    pool_slots = max(1, min(ML_POOL_SLOTS, ML_SAMPLED_TEXTURE_LIMIT - ordinary))
    pools = ''.join('@group(0) @binding(%d) var cp2077_ml_pool_%d: texture_2d_array<f32>;\n' % (ML_POOL_BINDING_BASE + k, k)
                    for k in range(pool_slots))
    pools += '@group(0) @binding(%d) var<storage, read> cp2077_ml_table: array<u32>;\n' % ML_TABLE_BINDING
    funcs = ''
    for helper, (op, sampler) in sorted(helpers.items()):
        if op == 'textureSampleGrad':
            params, extra = 'uv: vec2<f32>, dx: vec2<f32>, dy: vec2<f32>', 'dx, dy'
        else:
            params, extra = 'uv: vec2<f32>, lod: f32', 'lod'
        cases = ''.join('        case %du: { result = %s(cp2077_ml_pool_%d, %s, uv, layer, %s); }\n'
                        % (k + 1, op, k, sampler, extra) for k in range(pool_slots))
        funcs += ('fn %s(index: u32, %s) -> vec4<f32> {\n'
                  '    let entry = cp2077_ml_table[min(index, arrayLength(&cp2077_ml_table) - 1u)];\n'
                  '    let layer = entry & 0xffffu;\n'
                  '    var result = vec4<f32>(0.5, 0.5, 1.0, 1.0);\n'
                  '    switch (entry >> 16u) {\n%s        default: { }\n    }\n'
                  '    return result;\n}\n\n') % (helper, params, cases)
    first_global = re.search(r'^@group', text, re.M)
    if not first_global:
        raise ValueError('No binding declarations to anchor multilayer pools')
    text = text[:first_global.start()] + pools + text[first_global.start():]
    first_fn = re.search(r'^fn\s', text, re.M)
    if not first_fn:
        raise ValueError('No function to anchor multilayer helpers')
    text = text[:first_fn.start()] + funcs + text[first_fn.start():]
    resources[:] = [r for r in resources if not r.get('MultilayerBindless')]
    for k in range(pool_slots):
        resources.append({'Group': 0, 'Binding': ML_POOL_BINDING_BASE + k, 'OriginalSlot': k, 'Kind': 'MultilayerPool'})
    resources.append({'Group': 0, 'Binding': ML_TABLE_BINDING, 'OriginalSlot': 0, 'Kind': 'MultilayerTable', 'NativeRegister': 130})
    return text, {'PoolSlots': pool_slots, 'HelperCalls': calls, 'Helpers': sorted(helpers)}
