"""Split a native SRV that is sampled both with and without depth comparison into two WGSL bindings.

DXIL allows one Texture2D (typically a D32 shadow/depth map) to be read by SampleCmp* through a comparison
sampler and by Sample*/SampleLevel through an ordinary sampler. naga infers the WGSL image type from its uses
(texture_depth_2d for Dref sampling, texture_2d<f32> otherwise) and rejects the mix with
"inconsistent comparison sampling". The plain sampling uses are redirected to a second variable decorated with
the marker binding ALIAS_MARKER + slot; prepare-native-static-graphics.py maps it to its own WebGPU binding
(DEPTH_ALIAS_SHIFT + slot) with the same native SRV slot, so the runtime binds the same engine texture twice
(the depth-float bridge covers a depth texture bound as texture_2d<f32>). Original arithmetic is unchanged.
"""
import struct

ALIAS_MARKER = 1000          # Binding decoration of the plain-sampling alias variable = 1000 + native slot.
DEPTH_ALIAS_SHIFT = 168      # pixel 512 + 168 + slot = 680..743 (free range between pools and samplers).
OP_LOAD, OP_VARIABLE, OP_DECORATE, OP_ENTRY, OP_SAMPLED_IMAGE = 61, 59, 71, 15, 86
DREF_OPS = {89, 90, 93, 94, 97}
PLAIN_OPS = {87, 88, 91, 92, 96}


def _parse(data):
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != 0x07230203:
        raise ValueError('Not SPIR-V')
    ops = []
    pos = 5
    while pos < len(words):
        count = words[pos] >> 16
        if not count:
            raise ValueError('Malformed SPIR-V')
        ops.append([words[pos] & 65535, words[pos + 1:pos + count]])
        pos += count
    return words[:5], ops


def split_comparison_images(data):
    header, ops = _parse(data)
    load_var = {}
    for op, a in ops:
        if op == OP_LOAD:
            load_var[a[1]] = a[2]
    sampled = {}
    for op, a in ops:
        if op == OP_SAMPLED_IMAGE:
            sampled[a[1]] = (a[2], load_var.get(a[2]))
    dref_vars, plain_sampled = set(), {}
    for op, a in ops:
        if op in DREF_OPS or op in PLAIN_OPS:
            image_load, var = sampled.get(a[2], (None, None))
            if var is None:
                continue
            if op in DREF_OPS:
                dref_vars.add(var)
            else:
                plain_sampled.setdefault(var, set()).add(a[2])
    split = sorted(v for v in dref_vars if v in plain_sampled)
    if not split:
        return data, []
    decorations = {}
    for op, a in ops:
        if op == OP_DECORATE:
            decorations.setdefault(a[0], {})[a[1]] = a[2:]
    variables = {a[1]: a for op, a in ops if op == OP_VARIABLE}
    load_types = {a[1]: a[0] for op, a in ops if op == OP_LOAD}

    def fresh():
        value = header[3]
        header[3] += 1
        return value

    alias = {}
    audits = []
    for var in split:
        deco = decorations.get(var, {})
        if 33 not in deco or deco.get(34, [0]) != [0]:
            raise ValueError('Comparison image without a space-0 native register')
        alias[var] = fresh()
        audits.append({'Variable': var, 'Alias': alias[var], 'NativeSlot': deco[33][0],
                       'PlainSampledImages': len(plain_sampled[var])})
    redirect = {}  # plain OpSampledImage result -> var
    for var in split:
        for si in plain_sampled[var]:
            redirect[si] = var
    out = []
    for op, a in ops:
        if op == OP_VARIABLE and a[1] in alias:
            out.append([op, a])
            out.append([OP_VARIABLE, [a[0], alias[a[1]], a[2]]])
            continue
        if op == OP_DECORATE and a[0] in alias:
            out.append([op, a])
            if a[1] == 33:
                out.append([OP_DECORATE, [alias[a[0]], 33, ALIAS_MARKER + a[2]]])
            elif a[1] == 34:
                out.append([OP_DECORATE, [alias[a[0]], 34, a[2]]])
            continue
        if op == OP_ENTRY and header[1] >= 0x00010400:
            extra = [alias[v] for v in split if v in a]
            out.append([op, a + extra])
            continue
        if op == OP_SAMPLED_IMAGE and a[1] in redirect:
            var = redirect[a[1]]
            loaded = fresh()
            out.append([OP_LOAD, [load_types[a[2]], loaded, alias[var]]])
            out.append([op, [a[0], a[1], loaded, a[3]]])
            continue
        out.append([op, a])
    words = list(header)
    for op, a in out:
        words += [(len(a) + 1) << 16 | op, *a]
    return struct.pack('<%dI' % len(words), *words), audits
