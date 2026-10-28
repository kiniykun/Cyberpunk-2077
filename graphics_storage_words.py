"""Consolidate native read-only SSBO word views without changing byte addresses."""
import re

def native_storage_strides(dxil):
    nodes=dict(re.findall(r'^!(\d+) = !\{(.*)\}$',dxil,re.M))
    root=re.search(r'^!dx\.resources = !\{!(\d+)\}',dxil,re.M)
    if not root:return {}
    group=nodes[root[1]].split(', ')[0]
    if group=='null':return {}
    result={}
    for node in re.findall(r'!(\d+)',nodes[group[1:]]):
        row=nodes[node]
        match=re.fullmatch(r'i32 (\d+), .+? undef, !"[^"]*", i32 (\d+), i32 (\d+), i32 (\d+), i32 (\d+), i32 (\d+), (null|!\d+)',row)
        if not match:raise ValueError('Unknown native SRV metadata')
        _,space,slot,count,kind,_,props=match.groups()
        if int(kind) not in (11,12):continue
        if int(space)!=0 or int(count)!=1:raise ValueError('Unsupported native storage register space/count')
        stride=4
        if int(kind)==12:
            values=list(map(int,re.findall(r'i32 (\d+)',nodes[props[1:]])))
            pairs=dict(zip(values[::2],values[1::2]))
            stride=pairs.get(1,0)
            if not stride or stride%4:raise ValueError('Unknown native structured element stride')
        result[int(slot)]=stride
    return result

def consolidate_storage_words(text,resources):
    audits=[]
    declaration=re.compile(r'@group\((\d+)\)\s*@binding\((\d+)\)\s*var<storage(?:,\s*read)?>\s+(\w+)\s*:\s*(\w+)\s*;')
    for resource in resources:
        if resource['Kind']!='StorageBuffer':continue
        variables=[m for m in declaration.finditer(text) if (int(m[1]),int(m[2]))==(resource['Group'],resource['Binding'])]
        if len(variables)!=len(resource['StorageWordViews']):raise ValueError('Native storage aliases changed')
        canonical=f'cp2077_storage_words_{resource["Binding"]}'
        accesses=[]
        declarations=[]
        for variable,view in zip(variables,resource['StorageWordViews']):
            structure=re.search(r'\bstruct\s+'+re.escape(variable[4])+r'\s*\{\s*(\w+)\s*:\s*array<(u32|vec([234])<u32>)>\s*,\s*\}',text)
            if not structure:raise ValueError('Only flat native uint32 storage views are supported')
            width=int(structure[3] or 1)
            if width!=view['Width'] or view['Stride']!=width*4:raise ValueError('Native word-view byte stride changed')
            name=variable[3];member=structure[1]
            prefix=re.compile(r'\b'+re.escape(name)+r'\.'+re.escape(member)+r'\[')
            patches=[]
            for access in prefix.finditer(text):
                begin=access.end();end=begin;depth=1
                while end<len(text) and depth:
                    if text[end]=='[':depth+=1
                    elif text[end]==']':depth-=1
                    end+=1
                if depth:raise ValueError('Unterminated native storage index')
                index=text[begin:end-1]
                if not re.fullmatch(r'[\w\s()+*/%&|^<>.-]+',index):raise ValueError('Native storage index expression needs further audit')
                suffix=text[end:].lstrip()
                if suffix.startswith('=') and not suffix.startswith('=='):raise ValueError('Native storage write is unsupported')
                words=[f'{canonical}.words[((({index}) * {width}u) + {component}u)]' for component in range(width)]
                replacement=words[0] if width==1 else f'vec{width}<u32>('+', '.join(words)+')'
                patches.append((access.start(),end,replacement))
                accesses.append({'Variable':name,'Width':width,'NativeByteStride':width*4,'NativeIndexExpression':index})
            # Replacements never overlap within one flat array view.
            for begin,end,replacement in reversed(patches):text=text[:begin]+replacement+text[end:]
            if re.search(r'\b'+re.escape(name)+r'\b',text.replace(variable[0],'')):raise ValueError('Native storage pointer escapes word lowering')
            text=text.replace(variable[0],'',1)
        if not accesses:
            # Every alias was checked above for escaping uses. A declaration
            # whose only consumer was removed dead code has no runtime reads.
            resource['UnusedAfterLowering']=True
        binding=resource['Binding']
        header=f'struct Cp2077StorageWords_{binding} {{ words: array<u32>, }}\n@group({resource["Group"]}) @binding({binding}) var<storage, read> {canonical}: Cp2077StorageWords_{binding};\n'
        text=header+text
        audits.append({'Binding':binding,'OriginalSlot':resource['OriginalSlot'],'NativeElementStride':resource['StorageStride'],'Accesses':accesses,'UnusedAfterLowering':not accesses,'Scope':'Each native view index is multiplied by its original 4-byte word width; uint32 bits and original arithmetic retained. Empty access lists require all aliases to have no escaping uses.'})
    return text,audits
