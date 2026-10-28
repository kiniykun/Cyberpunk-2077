"""Turn point-sampled reads of unfilterable (depth / 32-bit float) SRVs into textureLoad in converted compute WGSL.

WebGPU's auto layout marks a texture_2d<f32> that is used with textureGather / textureSample* plus a 'sampler' as
sampleType 'float' (filterable). The engine binds some such SRVs with a depth (or R32 float) view, which only fits
'unfilterable-float' (or 'depth'), so the runtime check (gpu-api-command-web.cpp ComputeSampledViewMismatch) logs
"compute SRV mismatch zero fallback ... reason=view not filterable float" and binds a zero view instead. The engine
reads those SRVs through a POINT sampler, so the filtering is not needed: gathers become 4 textureLoads and point
textureSampleLevel calls become one textureLoad. The sampler global stays in the auto layout through a phony
statement '_ = <sampler>;' before each rewritten line (a phony use of a sampler FUNCTION PARAMETER breaks Tint's
HLSL backend: "let: value type, 'sampler', must be a concrete constructible type"), so helpers take no sampler. Only the (shader, binding) pairs listed in UNFILTERABLE_SRVS are touched; every other module is
returned unchanged.
"""
import re

# Evidence: full.log of build ca81cbc7 ("compute SRV mismatch zero fallback: ... reason=view not filterable float",
# viewFormat=48 = depth-only view of the d32s8 scene depth). Both read t1 = scene depth through s8 = samplPoint, which
# CRenderInterface::BindGlobalSamplers (tiledDeferred.cpp) sets to SAMPSTATEPRESET_WrapPointNoMip (wrap, point, no
# mip) - hence 'wrap' addressing and level 0. Calls: exact number of rewritten calls per kind (a changed shader
# raises instead of silently diverging).
UNFILTERABLE_SRVS = {
    # classifyMaterials.fx: tDepth.GatherRed(samplPoint, fpCoord)
    'm_classifyMaterials': {65: {'Sampler': 264, 'Filter': 'point', 'Address': 'wrap', 'Calls': {'textureGather': 1}}},
    # histogramCompute.fx (DOWNSCALE): depth SampleLevel(samplPoint, uv, 0[, offset]) x3 taps, depth > 0 test.
    'm_histogramDownscale': {65: {'Sampler': 264, 'Filter': 'point', 'Address': 'wrap', 'Calls': {'textureSampleLevel': 3}}},
}

HELPERS = '''
fn cp2077_unfilterable_texel_wrap(t: texture_2d<f32>, p: vec2<i32>) -> vec4<f32> {
    let d = vec2<i32>(textureDimensions(t, 0));
    return textureLoad(t, ((p % d) + d) % d, 0);
}

fn cp2077_unfilterable_gather_wrap(t: texture_2d<f32>, uv: vec2<f32>, c: u32, o: vec2<i32>) -> vec4<f32> {
    let b = vec2<i32>(floor(uv * vec2<f32>(textureDimensions(t, 0)) - vec2<f32>(0.5f))) + o;
    let x = cp2077_unfilterable_texel_wrap(t, b + vec2<i32>(0i, 1i));
    let y = cp2077_unfilterable_texel_wrap(t, b + vec2<i32>(1i, 1i));
    let z = cp2077_unfilterable_texel_wrap(t, b + vec2<i32>(1i, 0i));
    let w = cp2077_unfilterable_texel_wrap(t, b);
    return vec4<f32>(x[c], y[c], z[c], w[c]);
}

fn cp2077_unfilterable_point_wrap(t: texture_2d<f32>, uv: vec2<f32>, o: vec2<i32>) -> vec4<f32> {
    return cp2077_unfilterable_texel_wrap(t, vec2<i32>(floor(uv * vec2<f32>(textureDimensions(t, 0)))) + o);
}
'''
ZERO_LEVEL = {'0f', '0.0f', '0.0', '0', '0i', '0u'}
CALL = re.compile(r'\b(texture[A-Z]\w*)\s*\(')

def _split_call(text, open_paren):
    pos, depth = open_paren + 1, 1
    while depth and pos < len(text):
        if text[pos] in '([': depth += 1
        elif text[pos] in ')]': depth -= 1
        pos += 1
    if depth: raise ValueError('Malformed WGSL texture call')
    args, last, level = [], open_paren + 1, 0
    for index in range(open_paren + 1, pos - 1):
        c = text[index]
        if c in '([': level += 1
        elif c in ')]': level -= 1
        elif c == ',' and not level: args.append(text[last:index].strip()); last = index + 1
    args.append(text[last:pos - 1].strip())
    return pos, args

def lower_unfilterable_srvs(text, runtime_name):
    """Returns (text, rewritten call count). Unlisted shaders come back unchanged (same object)."""
    table = UNFILTERABLE_SRVS.get(runtime_name)
    if not table: return text, 0
    if 'cp2077_unfilterable_' in text: raise ValueError('Helper name collision')
    newline = '\r\n' if '\r\n' in text else '\n'
    replacements, phony, total = [], {}, 0
    for binding, spec in table.items():
        if spec['Filter'] != 'point' or spec['Address'] != 'wrap':
            raise ValueError('Only point/wrap unfilterable SRV lowering is implemented')
        texture = re.search(r'@group\(0\)\s+@binding\(%d\)\s+var\s+(\w+)\s*:\s*texture_2d<f32>\s*;' % binding, text)
        sampler = re.search(r'@group\(0\)\s+@binding\(%d\)\s+var\s+(\w+)\s*:\s*sampler\s*;' % spec['Sampler'], text)
        if not texture or not sampler: raise ValueError(f'{runtime_name}: unfilterable SRV {binding} / sampler not declared as expected')
        tex, samp = texture[1], sampler[1]
        counts, handled = {}, 0
        for match in CALL.finditer(text):
            end, args = _split_call(text, match.end() - 1)
            kind = match[1]
            if kind == 'textureGather' and len(args) in (4, 5) and args[1] == tex:
                if args[2] != samp or not re.fullmatch(r'[0-3][iu]?', args[0]): raise ValueError(f'{runtime_name}: unexpected gather shape')
                offset = args[4] if len(args) == 5 else 'vec2<i32>(0i)'
                new = f'cp2077_unfilterable_gather_wrap({tex}, {args[3]}, {args[0][0]}u, {offset})'
            elif kind == 'textureSampleLevel' and args and args[0] == tex:
                if len(args) not in (4, 5) or args[1] != samp or args[3] not in ZERO_LEVEL:
                    raise ValueError(f'{runtime_name}: unexpected textureSampleLevel shape')
                offset = args[4] if len(args) == 5 else 'vec2<i32>(0i)'
                new = f'cp2077_unfilterable_point_wrap({tex}, {args[2]}, {offset})'
            elif tex in args[:2]:
                if kind in ('textureLoad', 'textureDimensions', 'textureNumLevels'): handled += 1; continue
                raise ValueError(f'{runtime_name}: {kind} on unfilterable SRV {binding} is not lowered')
            else:
                continue
            counts[kind] = counts.get(kind, 0) + 1
            replacements.append((match.start(), end, new))
            # Keep the sampler global in the auto layout: phony use as its own statement before the call's line.
            line = text.rfind('\n', 0, match.start()) + 1
            if not re.fullmatch(r'[ \t]+let\s+\w+\s*=\s*', text[line:match.start()]):
                raise ValueError(f'{runtime_name}: rewritten call is not a plain let initializer')
            indent = re.match(r'[ \t]*', text[line:])[0]
            phony[line] = indent + f'_ = {samp};' + newline
        if counts != spec['Calls']: raise ValueError(f'{runtime_name}: unfilterable SRV {binding} call counts {counts} != {spec["Calls"]}')
        # Every other reference to the texture must be its declaration or an already-legal load/size query.
        if len(re.findall(r'\b%s\b' % re.escape(tex), text)) != 1 + sum(counts.values()) + handled:
            raise ValueError(f'{runtime_name}: unfilterable SRV {binding} has untracked references')
        total += sum(counts.values())
    replacements.sort()
    for (s0, e0, _), (s1, _, _) in zip(replacements, replacements[1:]):
        if s1 < e0: raise ValueError('Nested unfilterable SRV calls')
    edits = replacements + [(at, at, line) for at, line in phony.items()]
    edits.sort(key=lambda e: (e[0], e[1]))
    for start, end, new in reversed(edits): text = text[:start] + new + text[end:]
    if not text.endswith('\n'): text += newline
    return text + HELPERS.replace('\n', newline), total
