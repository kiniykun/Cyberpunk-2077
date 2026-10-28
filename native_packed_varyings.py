"""Pack original DXIL register xy/zw interfaces into one WGSL location.

Only these two original bytecode identities are admitted. Scalar access chains
keep their native register channel; all original arithmetic remains unchanged.
"""
import hashlib,json,struct

PACKED_IDENTITIES={
 '10921253598648822861':'4d5e4a8172bddd1f9b90693f94d765af7218f239f17bd84225ad28a85fd7c11e',
 '15840787688720938348':'59be9f8cf5797377f7730b5f71313e0c770338208813138851648a58d99c03b2',
}

def pack_native_varyings(data,native_bytes,shader_hash,root):
    assert hashlib.sha256(native_bytes).hexdigest()==PACKED_IDENTITIES[shader_hash]
    grammar=json.loads((root/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
    codes={x['opname']:x['opcode'] for x in grammar['instructions']}
    w=list(struct.unpack('<%dI'%(len(data)//4),data));header=w[:5];ops=[];cursor=5
    while cursor<len(w):
        n,op=w[cursor]>>16,w[cursor]&65535
        assert n and cursor+n<=len(w)
        ops.append((op,w[cursor+1:cursor+n]));cursor+=n
    variables={a[1]:a for op,a in ops if op==codes['OpVariable']}
    decorations={v:[] for v in variables}
    for op,a in ops:
        if op==codes['OpDecorate'] and a[0] in decorations:decorations[a[0]].append(a[1:])
    ids=sorted(v for v,ds in decorations.items() if [30,1] in ds)
    assert len(ids)==2
    types={a[0]:(op,a[1:]) for op,a in ops if codes['OpTypeVoid']<=op<=codes['OpTypeForwardPointer']}
    scalar=None;storage=None;offsets={}
    for v in ids:
        pointer=variables[v][0];p_op,p_args=types[pointer]
        assert p_op==codes['OpTypePointer']
        v_op,v_args=types[p_args[1]]
        assert v_op==codes['OpTypeVector'] and v_args[1]==2
        assert types[v_args[0]]==(codes['OpTypeFloat'],[32])
        if scalar is None:scalar=v_args[0];storage=p_args[0]
        assert scalar==v_args[0] and storage==p_args[0] and storage in (1,3)
        assert all(d in ([30,1],[31,2]) for d in decorations[v])
        offsets[v]=2 if [31,2] in decorations[v] else 0
    assert sorted(offsets.values())==[0,2]
    vector=next(t for t,(op,a) in types.items() if op==codes['OpTypeVector'] and a==[scalar,4])
    pointer=next(t for t,(op,a) in types.items() if op==codes['OpTypePointer'] and a==[storage,vector])
    constants={a[1]:(a[0],a[2]) for op,a in ops if op==codes['OpConstant'] and len(a)==3 and types.get(a[0])==(codes['OpTypeInt'],[32,0])}
    indices={value:identifier for identifier,(t,value) in constants.items()}
    assert all(i in indices for i in range(4))
    keep=next(v for v in ids if offsets[v]==0);remove=next(v for v in ids if offsets[v]==2)
    # Reject every unsupported use of the original interface pointers. Numeric
    # literals and unrelated result IDs must not be interpreted as pointer uses.
    for op,a in ops:
        name=next(x['opname'] for x in grammar['instructions'] if x['opcode']==op)
        definition=next(x for x in grammar['instructions'] if x['opcode']==op)
        if name in ('OpName','OpDecorate','OpVariable','OpEntryPoint','OpAccessChain'):continue
        pos=0
        for operand in definition.get('operands',[]):
            kind=operand['kind'];quant=operand.get('quantifier')
            if kind=='LiteralString':
                while pos<len(a):
                    word=a[pos];pos+=1
                    if any(((word>>(8*i))&255)==0 for i in range(4)):break
                continue
            values=a[pos:] if quant=='*' else a[pos:pos+1]
            if kind.startswith('Id') and kind!='IdResult':assert not any(v in ids for v in values),(name,a)
            pos=len(a) if quant=='*' else pos+1
    output=[];channels=[]
    for op,a in ops:
        a=list(a)
        if op==codes['OpName'] and a[0]==remove:continue
        if op==codes['OpDecorate'] and a[0] in ids:
            if a[0]==remove or a[1]==31:continue
        if op==codes['OpVariable'] and a[1] in ids:
            if a[1]==remove:continue
            assert len(a)==3;a[0]=pointer
        if op==codes['OpEntryPoint']:
            p=2
            while p<len(a):
                word=a[p];p+=1
                if any(((word>>(8*i))&255)==0 for i in range(4)):break
            assert keep in a[p:] and remove in a[p:]
            a=a[:p]+[v for v in a[p:] if v!=remove]
        if op==codes['OpAccessChain'] and a[2] in ids:
            assert len(a)==4 and a[3] in constants
            t,index=constants[a[3]];native=index+offsets[a[2]]
            assert index in (0,1) and native in range(4)
            p_op,p_args=types[a[0]]
            assert p_op==codes['OpTypePointer'] and p_args==[storage,scalar]
            channels.append({'PointerID':a[1],'OriginalVariable':a[2],'OriginalElement':index,'NativeRegisterChannel':native})
            a[2]=keep;a[3]=indices[native]
        output.append((op,a))
    assert channels
    result=header[:]
    for op,a in output:result.extend([((len(a)+1)<<16)|op,*a])
    return struct.pack('<%dI'%len(result),*result),[{'Location':1,'OriginalComponents':'xy / zw','PackedComponents':'xyzw','ScalarAccesses':channels,'Scope':'Original register channels and scalar computations preserved; one browser varying per native register.'}]
