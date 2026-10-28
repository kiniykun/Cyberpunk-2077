"""Exact IEEE binary32 NaN and infinity classification lowering for the browser shader converter."""
import json,struct
from pathlib import Path

def lower_isnan(data,root):
    grammar=json.loads((root/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
    ops={x['opname']:x['opcode'] for x in grammar['instructions']}
    result_ops={x['opcode'] for x in grammar['instructions'] if [o['kind'] for o in x.get('operands',[])[:2]]==['IdResultType','IdResult']}
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5];instructions=[];cursor=5
    while cursor<len(words):
        n,op=words[cursor]>>16,words[cursor]&65535
        if not n or cursor+n>len(words):raise ValueError('Invalid SPIR-V instruction')
        instructions.append((op,words[cursor+1:cursor+n]));cursor+=n
    if not any(op in (ops['OpIsNan'],ops['OpIsInf']) for op,a in instructions):return data,[]
    types={a[0]:(op,tuple(a[1:])) for op,a in instructions if ops['OpTypeVoid']<=op<=ops['OpTypeForwardPointer']}
    value_types={a[1]:a[0] for op,a in instructions if op in result_ops}
    declarations=[];audits=[];constants={}
    def fresh():
        value=header[3];header[3]+=1;return value
    def type_id(op,args):
        match=next((k for k,v in types.items() if v==(op,tuple(args))),None)
        if match is not None:return match
        result=fresh();types[result]=(op,tuple(args));declarations.append((op,[result,*args]));return result
    uint=type_id(ops['OpTypeInt'],[32,0])
    def literal(n):
        if n not in constants:
            constants[n]=fresh();declarations.append((ops['OpConstant'],[uint,constants[n],n]))
        return constants[n]
    transformed=[]
    for op,a in instructions:
        if op not in (ops['OpIsNan'],ops['OpIsInf']):transformed.append((op,a));continue
        boolean,result,operand=a;floating=value_types[operand];float_op,args=types[floating];lanes=1
        if float_op==ops['OpTypeVector']:
            scalar,lanes=args;float_op,args=types[scalar]
        if float_op!=ops['OpTypeFloat'] or args!=(32,):raise ValueError('NaN lowering supports only proven binary32 values')
        boolean_op,boolean_args=types[boolean]
        expected_bool=boolean
        if lanes>1:
            if boolean_op!=ops['OpTypeVector'] or boolean_args[1]!=lanes:raise ValueError('NaN result shape mismatch')
            expected_bool=boolean_args[0]
        if types[expected_bool][0]!=ops['OpTypeBool']:raise ValueError('NaN result is not boolean')
        unsigned=uint if lanes==1 else type_id(ops['OpTypeVector'],[uint,lanes])
        mask,exponent=literal(0x7fffffff),literal(0x7f800000)
        if lanes>1:
            m,e=fresh(),fresh();declarations.extend([(ops['OpConstantComposite'],[unsigned,m,*([mask]*lanes)]),(ops['OpConstantComposite'],[unsigned,e,*([exponent]*lanes)])]);mask,exponent=m,e
        bits,magnitude=fresh(),fresh()
        transformed.extend([(ops['OpBitcast'],[unsigned,bits,operand]),(ops['OpBitwiseAnd'],[unsigned,magnitude,bits,mask]),(ops['OpUGreaterThan'] if op==ops['OpIsNan'] else ops['OpIEqual'],[boolean,result,magnitude,exponent])])
        audits.append({'OriginalResultID':result,'FloatTypeID':floating,'Lanes':lanes,'Operator':'OpIsNan' if op==ops['OpIsNan'] else 'OpIsInf','Predicate':'(bits & 0x7fffffff) '+('>' if op==ops['OpIsNan'] else '==')+' 0x7f800000','Scope':'Exact binary32 classification; no other floating instructions altered'})
    first_function=next(i for i,(op,a) in enumerate(transformed) if op==ops['OpFunction']);transformed[first_function:first_function]=declarations
    output=list(header)
    for op,a in transformed:output.extend([(len(a)+1)<<16|op,*a])
    return struct.pack('<%dI'%len(output),*output),audits
