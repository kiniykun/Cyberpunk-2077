"""Remove unused pure subgroup OR results, preserving observable shader work."""
import hashlib,struct

def lower_dead_subgroup_or(data,root):
    def parse(blob):
        w=list(struct.unpack('<%dI'%(len(blob)//4),blob));ops=[];i=5
        if w[0]!=0x07230203:raise ValueError('Not SPIR-V')
        while i<len(w):
            count,op=w[i]>>16,w[i]&65535
            if not count or i+count>len(w):raise ValueError('Malformed SPIR-V')
            ops.append((op,w[i+1:i+count]));i+=count
        return w[:5],ops
    header,ops=parse(data);removed=[];kept=[]
    for index,(op,args) in enumerate(ops):
        # OpGroupNonUniformBitwiseOr is a pure value reduction, not a barrier.
        # Conservatively count every non-debug operand, including literals.
        unused=op==360 and len(args)==5 and not any(args[1] in a for j,(o,a) in enumerate(ops) if j!=index and o not in (5,71))
        if unused:removed.append({'ResultID':args[1],'Opcode':op,'ExecutableUses':0})
        else:kept.append((op,args))
    if not removed:return data,[]
    ids={r['ResultID'] for r in removed}
    # Remove only the dead helper-mask chain. General DCE may also discard
    # resource declarations which the native binding/reflection ABI preserves.
    definitions={a[1]:(op,a) for op,a in ops if op in (61,169)}
    dead=set(ids);helpers=set()
    for item in removed:
        reduction=next(a for op,a in ops if op==360 and a[1]==item['ResultID'])
        select=definitions.get(reduction[4])
        if not select or select[0]!=169:raise ValueError('Unexpected dead helper-mask producer')
        selection=select[1];load=definitions.get(selection[2])
        if not load or load[0]!=61:raise ValueError('Expected helper input load')
        variable=load[1][2]
        if (71,[variable,11,23]) not in ops:raise ValueError('Not a HelperInvocation mask')
        dead.update((selection[1],load[1][1]));helpers.add(variable)
    excluded={5,71,15}
    for op,args in ops:
        if op in (360,169,61) and len(args)>1 and args[1] in dead:continue
        if op==59 and args[1] in helpers:continue
        if op not in excluded and any(x in dead|helpers for x in args):raise ValueError('Helper-mask chain has another use')
    packed=header[:]
    for op,args in kept:
        if op in (5,71) and args[0] in dead|helpers:continue
        if op in (61,169) and args[1] in dead:continue
        if op==59 and args[1] in helpers:continue
        if op==15:
            end=2
            while end<len(args):
                word=struct.pack('<I',args[end]);end+=1
                if b'\0' in word:break
            args=args[:end]+[x for x in args[end:] if x not in helpers]
        packed.extend([((len(args)+1)<<16)|op,*args])
    result=struct.pack('<%dI'%len(packed),*packed)
    _,clean=parse(result)
    bindings=lambda rows:{tuple(a) for op,a in rows if op==71 and a[1] in (33,34)}
    if bindings(ops)!=bindings(clean):raise ValueError('Dead-code cleanup changed resource bindings')
    proof={'InputSHA256':hashlib.sha256(data).hexdigest(),'OutputSHA256':hashlib.sha256(result).hexdigest(),'RemovedPureUnusedReductions':removed,'RemovedDeadIDs':sorted(dead|helpers),'ResourceBindingDecorationsPreserved':True,'OutputCalculationsReplaced':False,'GPUParityVerified':False}
    return result,[proof]
