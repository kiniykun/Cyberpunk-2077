"""[nailfix] Pack flat DXIL varyings that share one register (SPIR-V Location + Component) into one vec4<u32>.

DXIL packs several flat interpolants into one register (e.g. uint in .x and float in .y). dxil-spirv emits one
Input/Output variable per element with Location L / Component c; naga ignores Component ("Unknown decoration
Component") and then rejects the shader ("Multiple bindings at location L"). WGSL has no component-level locations,
so every such register is rewritten as ONE flat vec4<u32> varying at Location L that carries the original 32-bit
words bit-exactly (OpBitcast), register channel c -> .c, unused channels 0:
  * the original per-element variables become Private variables (all original loads/stores/access chains unchanged);
  * vertex/output side: before every OpReturn of the entry point the private values are bitcast and stored;
  * pixel/input side: at entry the packed word vector is loaded, split and bitcast into the private variables.
Only applied when every element of the register is Flat, 32-bit, and the channels fit in 4. VS and PS of a pair are
rewritten the same way (channel order comes from the native register), so the browser interface matches.
"""
import struct
from collections import defaultdict

OpName, OpEntryPoint, OpDecorate, OpTypeInt, OpTypeFloat, OpTypeVector = 5, 15, 71, 21, 22, 23
OpTypePointer, OpConstant, OpFunction, OpFunctionEnd, OpVariable = 32, 43, 54, 56, 59
OpLoad, OpStore, OpAccessChain, OpCompositeConstruct, OpCompositeExtract, OpBitcast = 61, 62, 65, 80, 81, 124
OpLabel, OpReturn, OpReturnValue = 248, 253, 254
INPUT, OUTPUT, PRIVATE = 1, 3, 6
LOCATION, COMPONENT, FLAT = 30, 31, 14


def _parse(data):
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    ops, p = [], 5
    while p < len(w):
        n, op = w[p] >> 16, w[p] & 0xFFFF
        if not n or p + n > len(w):
            raise ValueError('Invalid SPIR-V instruction')
        ops.append([op, list(w[p + 1:p + n])])
        p += n
    return w[:5], ops


def _string_words(a, start):
    p = start
    while p < len(a):
        word = a[p]; p += 1
        if any(((word >> (8 * i)) & 255) == 0 for i in range(4)):
            break
    return p


def pack_component_varyings(data):
    header, ops = _parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 39}
    variables = {a[1]: a for op, a in ops if op == OpVariable}
    decorations = defaultdict(list)
    for op, a in ops:
        if op == OpDecorate and a[0] in variables:
            decorations[a[0]].append(tuple(a[1:]))

    def shape(t):  # (scalar type id, lanes) for 32-bit scalars/vectors, else None
        op, a = types.get(t, (None, None))
        if op == OpTypeVector:
            s = types.get(a[0])
            if s and s[0] in (OpTypeInt, OpTypeFloat) and s[1][0] == 32:
                return a[0], a[1]
            return None
        if op in (OpTypeInt, OpTypeFloat) and a[0] == 32:
            return t, 1
        return None

    groups = defaultdict(list)
    for v, ds in decorations.items():
        storage = variables[v][2]
        if storage not in (INPUT, OUTPUT):
            continue
        loc = [d[1] for d in ds if d[0] == LOCATION]
        if not loc:
            continue
        comp = [d[1] for d in ds if d[0] == COMPONENT]
        groups[(storage, loc[0])].append((v, comp[0] if comp else 0))
    packs = []
    for (storage, loc), members in groups.items():
        if len(members) < 2 and not any(c for _, c in members):
            continue
        ok, used = True, set()
        info = []
        for v, c in members:
            ptr = types[variables[v][0]]
            sh = shape(ptr[1][1])
            if not sh or (FLAT,) not in decorations[v]:
                ok = False; break
            if any(d[0] not in (LOCATION, COMPONENT, FLAT) for d in decorations[v]):
                ok = False; break
            lanes = set(range(c, c + sh[1]))
            if max(lanes) > 3 or lanes & used:
                ok = False; break
            used |= lanes
            info.append((v, c, ptr[1][1], sh))
        if ok and len(info) >= 2:
            packs.append((storage, loc, sorted(info, key=lambda x: x[1])))
    if not packs:
        return data, []

    bound = [header[3]]

    def new_id():
        bound[0] += 1
        return bound[0] - 1

    new_types = []

    def find_type(op, args):
        for t, (o, a) in types.items():
            if o == op and a == args:
                return t
        t = new_id(); types[t] = (op, args); new_types.append([op, [t] + args])
        return t

    u32 = find_type(OpTypeInt, [32, 0])
    uvec4 = find_type(OpTypeVector, [u32, 4])
    const_zero = next((a[1] for op, a in ops if op == OpConstant and a[0] == u32 and a[2:] == [0]), None)
    new_consts = []
    if const_zero is None:
        const_zero = new_id(); new_consts.append([OpConstant, [u32, const_zero, 0]])

    def uvec(lanes):
        return u32 if lanes == 1 else find_type(OpTypeVector, [u32, lanes])

    # Retype: original variables (and access chains rooted in them) become Private.
    private_ptr = {}

    def to_private(ptr_type):
        _, (sc, pointee) = types[ptr_type]
        if ptr_type not in private_ptr:
            private_ptr[ptr_type] = find_type(OpTypePointer, [PRIVATE, pointee])
        return private_ptr[ptr_type]

    packed_vars = {}
    member_vars = set()
    for storage, loc, info in packs:
        ptr = find_type(OpTypePointer, [storage, uvec4])
        packed_vars[(storage, loc)] = (new_id(), ptr)
        for v, _, _, _ in info:
            member_vars.add(v)
    roots = set(member_vars)
    # Pre-create every type the rewrite needs so they are declared before the first function.
    for op, a in ops:
        if op == OpVariable and a[1] in member_vars:
            to_private(a[0])
        elif op == OpAccessChain and a[2] in roots:
            roots.add(a[1]); to_private(a[0])
    for _, _, info in packs:
        for _, _, _, (_, lanes) in info:
            uvec(lanes)
    roots = set(member_vars)
    entry = next(a for op, a in ops if op == OpEntryPoint)
    entry_fn = entry[1]
    audits = []
    out = []
    emitted = [False]
    moved_vars = []
    in_entry = False
    after_label = False
    version = header[1]
    for op, a in ops:
        if op == OpName and a[0] in member_vars:
            out.append([op, a]); continue
        if op == OpDecorate and a[0] in member_vars:
            continue  # Location/Component/Flat now belong to the packed variable
        if op == OpEntryPoint:
            p = _string_words(a, 2)
            iface = [x for x in a[p:] if x not in member_vars] + [vid for vid, _ in packed_vars.values()]
            if version >= 0x10400:
                iface += sorted(member_vars)
            out.append([op, a[:p] + iface]); continue
        if op == OpFunction and not emitted[0]:
            # all new declarations, and the retyped member variables, go right before the first function
            emitted[0] = True
            out.extend(new_types); out.extend(new_consts)
            for (storage, loc), (vid, ptr) in sorted(packed_vars.items(), key=lambda x: x[1][0]):
                out.append([OpVariable, [ptr, vid, storage]])
            out.extend(moved_vars)
        if op == OpVariable and a[1] in member_vars:
            moved_vars.append([op, [to_private(a[0]), a[1], PRIVATE]]); continue
        if op == OpAccessChain and a[2] in roots:
            roots.add(a[1])
            out.append([op, [to_private(a[0])] + a[1:]]); continue
        if op == OpFunction:
            in_entry = a[1] == entry_fn
            out.append([op, a]); continue
        if in_entry and op == OpLabel and not after_label:
            out.append([op, a]); after_label = True; continue
        if in_entry and after_label and op != OpVariable and after_label is True:
            # first non-OpVariable instruction of the entry block: unpack inputs
            for storage, loc, info in packs:
                if storage != INPUT:
                    continue
                vid, _ = packed_vars[(storage, loc)]
                word = new_id(); out.append([OpLoad, [uvec4, word, vid]])
                for v, c, value_type, (scalar, lanes) in info:
                    parts = []
                    for i in range(lanes):
                        x = new_id(); out.append([OpCompositeExtract, [u32, x, word, c + i]]); parts.append(x)
                    packed = parts[0]
                    if lanes > 1:
                        packed = new_id(); out.append([OpCompositeConstruct, [uvec(lanes), packed] + parts])
                    value = packed
                    if value_type != uvec(lanes):
                        value = new_id(); out.append([OpBitcast, [value_type, value, packed]])
                    out.append([OpStore, [v, value]])
                audits.append({'Location': loc, 'Direction': 'input', 'Channels': [(c, lanes) for _, c, _, (_, lanes) in info]})
            after_label = 'done'
        if in_entry and op in (OpReturn, OpReturnValue):
            for storage, loc, info in packs:
                if storage != OUTPUT:
                    continue
                vid, _ = packed_vars[(storage, loc)]
                lanes_out = [const_zero] * 4
                for v, c, value_type, (scalar, lanes) in info:
                    value = new_id(); out.append([OpLoad, [value_type, value, v]])
                    if value_type != uvec(lanes):
                        cast = new_id(); out.append([OpBitcast, [uvec(lanes), cast, value]]); value = cast
                    for i in range(lanes):
                        if lanes == 1:
                            lanes_out[c] = value
                        else:
                            x = new_id(); out.append([OpCompositeExtract, [u32, x, value, i]]); lanes_out[c + i] = x
                word = new_id(); out.append([OpCompositeConstruct, [uvec4, word] + lanes_out])
                out.append([OpStore, [vid, word]])
                audits.append({'Location': loc, 'Direction': 'output', 'Channels': [(c, lanes) for _, c, _, (_, lanes) in info]})
        if op == OpFunctionEnd:
            in_entry = False
        out.append([op, a])
    # decorations for packed variables (after the last OpDecorate)
    last_dec = max(i for i, (op, _) in enumerate(out) if op == OpDecorate)
    extra = []
    for (storage, loc), (vid, _) in packed_vars.items():
        extra += [[OpDecorate, [vid, LOCATION, loc]], [OpDecorate, [vid, FLAT]]]
    out[last_dec + 1:last_dec + 1] = extra
    header = list(header); header[3] = bound[0]
    words = list(header)
    for op, a in out:
        words.extend([((len(a) + 1) << 16) | op, *a])
    for a in audits:
        a['Scope'] = 'flat 32-bit words carried bit-exactly in one vec4<u32> per native register'
    return struct.pack('<%dI' % len(words), *words), audits
