"""Preserve unsigned 32-bit integer negation as modular integer subtraction."""
import json,struct

def lower_unsigned_negate(data,root):
    grammar=json.loads((root/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
    codes={x['opname']:x['opcode'] for x in grammar['instructions']}
    result_ops={x['opcode'] for x in grammar['instructions'] if [o['kind'] for o in x.get('operands',[])[:2]]==['IdResultType','IdResult']}
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5];ops=[];cursor=5
    while cursor<len(words):
        n,op=words[cursor]>>16,words[cursor]&65535
        if not n or cursor+n>len(words):raise ValueError('Invalid SPIR-V instruction')
        ops.append((op,words[cursor+1:cursor+n]));cursor+=n
    types={a[0]:(op,a[1:]) for op,a in ops if codes['OpTypeVoid']<=op<=codes['OpTypeForwardPointer']}
    value_types={a[1]:a[0] for op,a in ops if op in result_ops}
    output=[];declarations=[];zeros={a[0]:a[1] for op,a in ops if op==codes['OpConstant'] and a[2:]==[0] and types.get(a[0])==(codes['OpTypeInt'],[32,0])};audits=[]
    for op,a in ops:
        if op!=codes['OpSNegate']:output.append((op,a));continue
        type_id,result,operand=a;type_op,args=types[type_id];lanes=1
        if type_op==codes['OpTypeVector']:
            scalar,lanes=args;type_op,args=types[scalar]
        if type_op!=codes['OpTypeInt'] or args!=[32,0]:output.append((op,a));continue
        if value_types[operand]!=type_id:raise ValueError('Unsigned negate operand type mismatch')
        if type_id not in zeros:
            if lanes==1:
                zero=header[3];header[3]+=1;zeros[type_id]=zero
                declarations.append((codes['OpConstant'],[type_id,zero,0]))
            else:
                scalar=types[type_id][1][0]
                if scalar not in zeros:
                    zeros[scalar]=header[3];header[3]+=1
                    declarations.append((codes['OpConstant'],[scalar,zeros[scalar],0]))
                zero=header[3];header[3]+=1;zeros[type_id]=zero
                declarations.append((codes['OpConstantComposite'],[type_id,zero,*([zeros[scalar]]*lanes)]))
        output.append((codes['OpISub'],[type_id,result,zeros[type_id],operand]))
        audits.append({'OriginalResultID':result,'OriginalTypeID':type_id,'Lanes':lanes,'Expression':'0u-x modulo2^32','Scope':'Original result bit pattern retained; signed and floating negation unchanged'})
    if not audits:return data,[]
    first_function=next(i for i,(op,a) in enumerate(output) if op==codes['OpFunction']);output[first_function:first_function]=declarations
    words=list(header)
    for op,a in output:words.extend([(len(a)+1)<<16|op,*a])
    return struct.pack('<%dI'%len(words),*words),audits

def lower_signed_uint_ops(data,root):
    """Make signed SPIR-V operations on 32-bit uint-typed values explicit (graphics twin of convert-static-compute.py).

    dxil-spirv types every DXIL i32 as uint and relies on the signed opcode (OpConvertSToF, OpConvertFToS, OpSDiv,
    OpSRem, OpSMod, OpShiftRightArithmetic, GLSL SAbs/SSign/SMin/SMax/SClamp/FindSMsb) to carry signedness.
    naga's SPIR-V frontend uses the operand/result type instead and prints f32(u32) for SToF, u32(f) for FToS and
    unsigned forms of the others. Example (browser-verified): camera-relative fixed-point world offsets
    f32(bitcast<u32>(objectPos) - cameraPos) * 2^-17 become ~+32768 m whenever objectPos < cameraPos, which throws
    every skinned/static mesh west/south of the camera 32 km away. Each instruction is rewritten to compute in i32
    with OpBitcast in/out, identical to DXIL two's complement semantics. Signed opcodes this pass does not lower are
    reported in the audit (left unchanged) rather than failing the shader.
    """
    grammar=json.loads((root/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
    typed={x['opcode'] for x in grammar['instructions'] if [o['kind'] for o in x.get('operands',[])[:2]]==['IdResultType','IdResult']}
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5];ops=[];cursor=5
    while cursor<len(words):
        n,op=words[cursor]>>16,words[cursor]&65535
        if not n or cursor+n>len(words):raise ValueError('Invalid SPIR-V instruction')
        ops.append((op,list(words[cursor+1:cursor+n])));cursor+=n
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    value_type={a[1]:a[0] for op,a in ops if op in typed and len(a)>=2}
    glsl={a[0] for op,a in ops if op==11 and b''.join(struct.pack('<I',x) for x in a[1:]).rstrip(b'\0')==b'GLSL.std.450'}
    state={'bound':header[3]};new_globals=[];signed_of={};audits=[];unlowered=[];out=[]
    def new_id():
        state['bound']+=1;return state['bound']-1
    def lanes_of(t):
        if t not in types:return None
        op,args=types[t];lanes=1
        if op==23:t,lanes=args[0],args[1];op,args=types[t]
        if op!=21 or args[0]!=32:return None
        return lanes,args[1]==0
    def is_uint(t):
        info=lanes_of(t);return bool(info and info[1])
    def signed_type(t):
        lanes=lanes_of(t)[0]
        if lanes in signed_of:return signed_of[lanes]
        scalar=next((i for i,(op,a) in types.items() if op==21 and a==[32,1]),None)
        if scalar is None:
            scalar=new_id();types[scalar]=(21,[32,1]);new_globals.append((21,[scalar,32,1]))
        result=scalar
        if lanes>1:
            result=next((i for i,(op,a) in types.items() if op==23 and a==[scalar,lanes]),None)
            if result is None:
                result=new_id();types[result]=(23,[scalar,lanes]);new_globals.append((23,[result,scalar,lanes]))
        signed_of[lanes]=result;return result
    def as_signed(value):
        t=value_type.get(value)
        if not is_uint(t):return value
        cast=new_id();out.append((124,[signed_type(t),cast,value]));return cast
    def signed_result(op,a,operands):
        audits.append({'Opcode':op,'OriginalResultID':a[1]})
        if is_uint(a[0]):
            tmp=new_id();out.append((op,[signed_type(a[0]),tmp,*operands]));out.append((124,[a[0],a[1],tmp]))
        else:out.append((op,[a[0],a[1],*operands]))
    for op,a in ops:
        if op==110 and is_uint(a[0]):signed_result(op,a,a[2:]);continue  # OpConvertFToS -> uint
        if op==111 and is_uint(value_type.get(a[2])):  # OpConvertSToF(uint)
            audits.append({'Opcode':op,'OriginalResultID':a[1]});operand=as_signed(a[2]);out.append((op,[a[0],a[1],operand]));continue
        if op in (135,138,139) and (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[2:4])):  # SDiv/SRem/SMod
            signed_result(op,a,[as_signed(a[2]),as_signed(a[3])]);continue
        if op==195 and (is_uint(a[0]) or is_uint(value_type.get(a[2]))):  # ShiftRightArithmetic; shift keeps type
            signed_result(op,a,[as_signed(a[2]),a[3]]);continue
        if op==12 and a[2] in glsl and a[3] in (5,7,39,42,45,74) and (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[4:])):
            signed_result(op,a,[a[2],a[3],*[as_signed(x) for x in a[4:]]]);continue
        if op in (114,152,202,236,238,353,356,4450,4452) and (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[2:] if x in value_type)):
            unlowered.append({'Opcode':op,'ResultID':a[1]})
        out.append((op,a))
    if not audits:return data,[],unlowered
    first_function=next(i for i,(op,_) in enumerate(out) if op==54);out[first_function:first_function]=new_globals
    header[3]=state['bound'];words=list(header)
    for op,a in out:words.extend([(len(a)+1)<<16|op,*a])
    return struct.pack('<%dI'%len(words),*words),audits,unlowered
