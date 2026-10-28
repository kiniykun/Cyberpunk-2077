"""Normalize identical packed-scalar/vec4 views of one native constant buffer."""
import struct

def normalize_uniform_scalar_aliases(data,register=13):
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5]
    assert header[0]==0x07230203
    ops=[];pos=5
    while pos<len(words):
        size=words[pos]>>16;op=words[pos]&65535
        assert size and pos+size<=len(words)
        ops.append((op,words[pos+1:pos+size]));pos+=size
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    constants={a[1]:(a[0],a[2]) for op,a in ops if op==43 and len(a)==3}
    decorations={};members={};groups={};result_types={}
    for op,a in ops:
        if op==71:decorations.setdefault(a[0],{})[a[1]]=a[2:]
        if op==72:members.setdefault((a[0],a[1]),{})[a[2]]=a[3:]
        if op>=41 and op not in (71,72) and len(a)>=2 and a[0] in types:
            result_types[a[1]]=a[0]
    for op,a in ops:
        if op!=59 or a[2]!=2:continue
        pointer,var,_=a[:3];deco=decorations.get(var,{})
        if deco.get(33)!=[register] or deco.get(34,[0])!=[0]:continue
        structure=types[pointer][1][1];struct_op,fields=types[structure]
        if struct_op!=30 or len(fields)!=1 or members.get((structure,0),{}).get(35)!=[0]:
            raise ValueError('Aliased b13 is not a single offset-zero array')
        array=fields[0];array_op,array_args=types[array]
        if array_op!=28:raise ValueError('Aliased b13 array must have static length')
        element,length_id=array_args;length=constants[length_id][1]
        element_op,element_args=types[element]
        shape={'Variable':var,'Length':length,'Stride':decorations[array].get(6,[None])[0],
            'ElementOp':element_op,'ElementArgs':element_args}
        groups.setdefault(register,[]).append(shape)
    if len(groups.get(register,[]))<2:return data,[]
    views=groups[register]
    if len(views)!=2:raise ValueError('Expected exactly two native b13 views')
    scalar=next((v for v in views if v['ElementOp']==22 and v['ElementArgs']==[32] and v['Stride']==4),None)
    vector=next((v for v in views if v['ElementOp']==23 and v['ElementArgs'][1]==4 and v['Stride']==16),None)
    if not scalar or not vector or types[vector['ElementArgs'][0]]!=(22,[32]) or scalar['Length']!=4*vector['Length']:
        raise ValueError('Native b13 views are not byte-identical f32/vec4 arrays')
    old=scalar['Variable'];canonical=vector['Variable'];extra_constants=[];rewritten=[];accesses=[]
    def fresh():
        ident=header[3];header[3]+=1;return ident
    def constant(type_id,value):
        for ident,pair in constants.items():
            if pair==(type_id,value):return ident
        ident=fresh();constants[ident]=(type_id,value);result_types[ident]=type_id
        extra_constants.append((43,[type_id,ident,value]));return ident
    for op,a in ops:
        if op==59 and a[1]==old:continue
        if op in (5,71) and a[0]==old:continue
        if op==15:
            # Entry-point name is a zero-terminated SPIR-V string.
            begin=2
            while begin<len(a):
                chunk=struct.pack('<I',a[begin]);begin+=1
                if b'\0' in chunk:break
            interface=list(dict.fromkeys(canonical if x==old else x for x in a[begin:]))
            rewritten.append((op,a[:begin]+interface));continue
        if op==65 and a[2]==old:
            if len(a)!=5 or constants.get(a[3],(None,None))[1]!=0:
                raise ValueError('Native scalar uniform access shape changed')
            index=a[4];index_type=result_types.get(index)
            if index_type not in types or types[index_type][0]!=21 or types[index_type][1][0]!=32:
                raise ValueError('Native uniform index must be 32-bit integer')
            known=constants.get(index)
            if known:
                if known[1]>=scalar['Length']:raise ValueError('Native constant scalar index is out of bounds')
                row=constant(index_type,known[1]//4);component=constant(index_type,known[1]%4)
            else:
                if types[index_type][1]!=[32,0]:raise ValueError('Dynamic signed uniform index requires further proof')
                four=constant(index_type,4);row=fresh();component=fresh()
                rewritten.extend([(134,[index_type,row,index,four]),(137,[index_type,component,index,four])])
            rewritten.append((65,[a[0],a[1],canonical,a[3],row,component]))
            accesses.append({'OriginalIndexID':index,'ConstantIndex':known[1] if known else None,'ResultPointerID':a[1]})
            continue
        # Uniform pointers may not escape into a different access representation.
        # OpConstant (43) operand 2 is a literal value that may equal the variable id; it is not a use.
        # OpExtInst (12) operand 3 is the literal instruction number.
        # Type declarations (19-39) only carry type ids and literals (e.g. OpTypeFloat 32), never variables.
        if op not in (71,72,5,6,43) and not 19<=op<=39 and old in (a[:3]+a[4:] if op==12 else a):
            raise ValueError('Native scalar uniform variable has an unaudited use')
        rewritten.append((op,a))
    if not accesses:raise ValueError('Aliased scalar view has no accounted accesses')
    insert=next(i for i,(op,a) in enumerate(rewritten) if op==59)
    rewritten[insert:insert]=extra_constants
    output=header[:]
    for op,a in rewritten:output.extend([((len(a)+1)<<16)|op,*a])
    # Exhaustively prove all valid native scalar addresses, independent of values.
    assert all(i*4==(i//4)*16+(i%4)*4 for i in range(scalar['Length']))
    proof={'NativeBinding':register,'ScalarVariable':old,'CanonicalVariable':canonical,
        'ScalarCount':scalar['Length'],'VectorCount':vector['Length'],'NativeBytes':scalar['Length']*4,
        'Accesses':accesses,'AllValidAddressesIdentical':True,
        'Scope':'Uniform address representation only; original loads, arithmetic, control flow and native bytecode unchanged.'}
    return struct.pack('<%dI'%len(output),*output),[proof]


def has_uniform_b13_alias(data):
    """True when more than one Uniform-storage variable is bound to native register b13 (space 0)."""
    words=struct.unpack('<%dI'%(len(data)//4),data)
    ops=[];pos=5
    while pos<len(words):
        size=words[pos]>>16
        if not size:break
        ops.append((words[pos]&65535,words[pos+1:pos+size]));pos+=size
    bound={a[0] for op,a in ops if op==71 and len(a)>=3 and a[1]==33 and a[2]==13}
    spaced={a[0] for op,a in ops if op==71 and len(a)>=3 and a[1]==34 and a[2]!=0}
    return sum(1 for op,a in ops if op==59 and len(a)>=3 and a[2]==2 and a[1] in bound and a[1] not in spaced)>1


def uniform_alias_registers(data):
    """Native b registers (space 0) bound by more than one Uniform-storage variable."""
    words=struct.unpack('<%dI'%(len(data)//4),data)
    ops=[];pos=5
    while pos<len(words):
        size=words[pos]>>16
        if not size:break
        ops.append((words[pos]&65535,words[pos+1:pos+size]));pos+=size
    binding={a[0]:a[2] for op,a in ops if op==71 and len(a)>=3 and a[1]==33}
    spaced={a[0] for op,a in ops if op==71 and len(a)>=3 and a[1]==34 and a[2]!=0}
    counts={}
    for op,a in ops:
        if op==59 and len(a)>=3 and a[2]==2 and a[1] in binding and a[1] not in spaced:
            counts[binding[a[1]]]=counts.get(binding[a[1]],0)+1
    return sorted(r for r,c in counts.items() if c>1)
