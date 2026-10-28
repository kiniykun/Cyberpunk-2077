"""Preserve native sampler LOD bias in converted WGSL sampling operations."""
import re

DIRECTIVE_LINE=re.compile(r'[ \t]*(?:(?:enable|requires)[ \t]+[\w \t,]+|diagnostic[ \t]*\([ \t]*\w+[ \t]*,[ \t]*[\w.]+[ \t]*\))[ \t]*;[ \t]*(?:\r?\n|$)')
BLANK_OR_COMMENT_LINE=re.compile(r'[ \t]*(?://[^\r\n]*)?\r?\n')
def directive_prefix_length(text):
    """End of the last leading WGSL directive line (blank/comment lines may sit between them); 0 if none."""
    pos=end=0
    while True:
        m=DIRECTIVE_LINE.match(text,pos)
        if m: pos=end=m.end(); continue
        m=BLANK_OR_COMMENT_LINE.match(text,pos)
        if m: pos=m.end(); continue
        return end

def lower_sampling_bias(text, resources, binding):
    samplers={}
    for resource in resources:
        if resource['Kind']!='Sampler': continue
        pattern=r'@group\('+str(resource['Group'])+r'\)\s+@binding\('+str(resource['Binding'])+r'\)\s+var\s+(\w+)\s*:\s*sampler(?:_comparison)?\s*;'
        match=re.search(pattern,text)
        if not match: raise ValueError('Sampler declaration not identified for bias lowering')
        samplers[match[1]]=resource['OriginalSlot']
    array_textures=set(re.findall(r'var\s+(\w+)\s*:\s*texture_(?:2d|cube)_array\s*<',text))
    pattern=re.compile(r'\b(textureSampleBias|textureSampleLevel|textureSampleGrad|textureSample)\s*\(')
    replacements=[]
    for match in pattern.finditer(text):
        start=match.end();pos=start;depth=1
        while depth and pos<len(text):
            if text[pos]=='(':depth+=1
            elif text[pos]==')':depth-=1
            pos+=1
        if depth:raise ValueError('Malformed WGSL sampling call')
        args=[];last=start;level=0
        for index in range(start,pos-1):
            if text[index]=='(':level+=1
            elif text[index]==')':level-=1
            elif text[index]==',' and not level:args.append(text[last:index].strip());last=index+1
        args.append(text[last:pos-1].strip())
        if len(args)<3 or args[1] not in samplers:raise ValueError('Sampler alias requires explicit bias lowering')
        bias='cp2077_sampler_bias.values[%du].x'%samplers[args[1]]
        kind=match[1]
        lod_index=4 if args[0] in array_textures else 3
        if args[0] not in array_textures and not re.search(r'var\s+'+re.escape(args[0])+r'\s*:\s*texture_',text): raise ValueError('Texture alias requires dimension-aware bias lowering')
        if kind=='textureSample':kind='textureSampleBias';args.insert(lod_index,bias)
        elif kind in ('textureSampleBias','textureSampleLevel'):args[lod_index]='('+args[lod_index]+' + '+bias+')'
        elif kind=='textureSampleGrad':
            args[lod_index]='('+args[lod_index]+' * exp2('+bias+'))';args[lod_index+1]='('+args[lod_index+1]+' * exp2('+bias+'))'
        replacements.append((match.start(),pos,kind+'('+', '.join(args)+')'))
    for start,end,replacement in reversed(replacements):text=text[:start]+replacement+text[end:]
    if replacements:
        if any(r['Group']==0 and r['Binding']==binding for r in resources):raise ValueError('Bias uniform binding collision')
        # WGSL directives (enable/requires/diagnostic) must precede every global declaration, so the
        # bias declarations go after any leading directive lines (offset 0 when there are none).
        prefix='struct CP2077SamplerBias { values: array<vec4<f32>, 16>, }\n@group(0) @binding(%d) var<uniform> cp2077_sampler_bias: CP2077SamplerBias;\n'%binding
        at=directive_prefix_length(text)
        text=text[:at]+prefix+text[at:]
        resources.append({'Group':0,'Binding':binding,'OriginalSlot':0,'Kind':'SamplerBias','WebGPUMinBytes':256})
    return text,len(replacements)
