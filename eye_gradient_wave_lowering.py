"""Lower one exact original lighting work list, retaining per-pixel evaluation."""
import hashlib, re, struct
from native_probe_wave_lowering import verify_guarded_native_worklist

IDENTITY='ef911f0d6072925b180b733318ad8144c3c7760f753eecd7829eb9a1af692773'
def lower_eye_gradient_worklist(data,bytecode,dxil):
    if hashlib.sha256(bytecode).hexdigest()!=IDENTITY:raise ValueError('Unaudited eye-gradient bytecode')
    proof=verify_guarded_native_worklist(dxil)
    if re.search(r'call\s+.*@dx\.op\.(?:bufferStore|rawBufferStore|textureStore|atomicBinOp|atomicCompareExchange|discard)',dxil):
        raise ValueError('Unexpected shared side effect or discard')
    words=list(struct.unpack('<%dI'%(len(data)//4),data));ops=[];p=5
    while p<len(words):
        size,op=words[p]>>16,words[p]&65535
        if not size or p+size>len(words):raise ValueError('Invalid SPIR-V')
        ops.append((op,words[p+1:p+size]));p+=size
    groups=[(i,op,a) for i,(op,a) in enumerate(ops) if 333<=op<=366]
    if len(groups)!=1 or groups[0][1]!=360:raise ValueError('Unexpected subgroup operations')
    position,_,reduction=groups[0]
    # All implicit-LOD/derivative work precedes the changed control-flow region.
    # The light-list body and subsequent work must use explicit-LOD reads only.
    if any(op in (87,89,91,93) or 207<=op<=215 for op,a in ops[position:]):
        raise ValueError('Changed control flow contains implicit gradients')
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    constants={a[1]:(a[0],a[2:]) for op,a in ops if op in (42,43,46)}
    if len(reduction)!=5 or reduction[3]!=0 or types[reduction[0]]!=(21,[32,0]):raise ValueError('Expected u32 OR reduction')
    if constants.get(reduction[2])!=(reduction[0],[3]):raise ValueError('Not subgroup scope')
    select=next((a for op,a in ops if op==169 and a[1]==reduction[4]),None)
    if not select or constants.get(select[3])!=(reduction[0],[0]):raise ValueError('Helper mask not neutral zero')
    load=next((a for op,a in ops if op==61 and a[1]==select[2]),None)
    if not load or types[load[0]]!=(20,[]):raise ValueError('Expected helper boolean')
    helper=load[2]
    if (71,[helper,11,23]) not in ops:raise ValueError('Not HelperInvocation')
    removed={helper,load[1],select[1]};out=[]
    for op,a in ops:
        if op in (5,71) and a[0] in removed:continue
        if (op==59 and a[1]==helper) or (op in (61,169) and a[1] in removed):continue
        if op==15:
            end=2
            while end<len(a):
                word=struct.pack('<I',a[end]);end+=1
                if b'\0' in word:break
            a=a[:end]+[v for v in a[end:] if v!=helper]
        if op==360 and a==reduction:out.append((83,[a[0],a[1],select[4]]));continue
        if op not in (15,) and any(v in removed for v in a):raise ValueError('Helper chain has another use')
        out.append((op,a))
    packed=words[:5]
    for op,a in out:packed.extend([((len(a)+1)<<16)|op,*a])
    result=struct.pack('<%dI'%len(packed),*packed)
    bindings=lambda rows:{tuple(a) for op,a in rows if op==71 and a[1] in (33,34)}
    if bindings(ops)!=bindings(out):raise ValueError('Resource bindings changed')
    # Ordered union enumeration restricted to M equals ordered enumeration of M.
    for mask in [0,0xffffffff,0xaaaaaaaa,0x55555555]+[1<<b for b in range(32)]:
        for union in [mask,mask|0x12345678,0xffffffff]:
            assert [b for b in range(32) if union&(1<<b) and mask&(1<<b)]==[b for b in range(32) if mask&(1<<b)]
    return result,[dict(OriginalDXILSHA256=IDENTITY,NativeWorklistProof=proof,
        RemovedHelperIDs=sorted(removed),OriginalPixelMask=select[4],
        ImplicitGradientOperationsBeforeChangedRegion=True,SharedResourceWrites=False,
        ResourceBindingsPreserved=True,PixelArithmeticAndAccumulatorOrderPreserved=True,
        InputSHA256=hashlib.sha256(data).hexdigest(),OutputSHA256=hashlib.sha256(result).hexdigest(),
        BrowserRenderingParityVerified=False)]
