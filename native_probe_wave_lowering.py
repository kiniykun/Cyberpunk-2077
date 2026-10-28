"""Remove a proved cross-pixel work-list optimization, preserving pixel work."""
import hashlib,re,struct

PROBE_SHADER_IDENTITIES={
 '1438046696210447442':'3f23b9425df43273fd3692eae3a7247ae075d83a7fd4bfaa6c5ba46656776105',
 '2175733990096522335':'1c47f3460bafc227f3b1bdd4afd42769c00d79b1543d058beeec18f55d60d71a',
 '4280642813451389880':'5e396741267579e8d084894bc78096b3d4a0df16627d0c4b34dce54d60086ae9',
 '10496203463145514412':'a63e425cbc9295a1b22a819ad69faf8388e0c7d83d3e0997d69be8369528fe2e',
 '12703885102664573402':'27d8b30ef7090632329f46b62ab438b2bcca3cf662680e9675472440773ffb98',
 '13436381369273847951':'1f34c0656422e121c386cf8be61228760d7f545a946f14db64bb05b1b8b91aa0',
 '16788563839102729027':'7ae3340eb2b71077bc1c7ca5eb8cacf04f362fbd96e4bcf90c60297b139ac96d',
 '16877832901189072552':'d99a74c4c049366176b76794f69aef4e041aefa1df664ac97622dcb40185fd16',
}

def verify_guarded_native_worklist(dxil):
    calls=re.findall(r'%(\d+) = call i32 @dx.op.waveActiveBit.i32\(i32 120, i32 %(\d+), i8 1\)',dxil)
    if len(calls)!=1:raise ValueError('Expected exactly one native probe wave OR')
    wave,pixel=calls[0]
    parts=re.split(r'^; <label>:(\d+)[^\n]*\n',dxil,flags=re.M)
    blocks={parts[i]:parts[i+1] for i in range(1,len(parts),2)}
    candidates=[]
    for block,text in blocks.items():
        for result,body in re.findall(r'%(\d+) = phi i32 ([^\n]+)',text):
            incoming=re.findall(r'\[ %(\d+), %(\d+) \]',body)
            if len(incoming)==2 and any(value==wave for value,label in incoming):
                candidates.append((block,text,result,next((value,label)for value,label in incoming if value!=wave)))
    if len(candidates)!=1:raise ValueError('Native wave result lacks unique loop phi')
    header,text,mask,(next_mask,skip)=candidates[0]
    first=re.search(r'%(\d+) = call i32 @dx.op.unaryBits.i32\(i32 32, i32 %'+mask+r'\)',text)
    decrement=re.search(r'%(\d+) = add i32 %'+mask+r', -1',text)
    if not first or not decrement or not re.search(r'%'+next_mask+r' = and i32 %'+decrement[1]+r', %'+mask+r'\b',text):
        raise ValueError('Work-list does not remove its least-significant bit')
    clamped=re.search(r'%(\d+) = and i32 %'+first[1]+r', 31',text)
    index=clamped[1] if clamped else first[1]
    bit=re.search(r'%(\d+) = shl i32 1, %'+index+r'\b',text)
    membership=re.search(r'%(\d+) = and i32 %'+(bit[1]if bit else '_missing_')+r', %'+pixel+r'\b',text)
    predicate=re.search(r'%(\d+) = icmp eq i32 %'+(membership[1]if membership else '_missing_')+r', 0',text)
    if not predicate or not re.search(r'br i1 %'+predicate[1]+r', label %'+skip+r', label %\d+',text):
        raise ValueError('Probe evaluation is not guarded by the original pixel mask')
    preserved=[]
    for current,kind,body in re.findall(r'%(\d+) = phi (float|i32) ([^\n]+)',text):
        if current==mask:continue
        incoming=re.findall(r'\[ %(\d+), %(\d+) \]',body)
        back=next((value for value,label in incoming if label==skip),None)
        match=re.search(r'%'+str(back)+r' = phi '+kind+r' ([^\n]+)',blocks[skip]) if back else None
        if not match or not re.search(r'\[ %'+current+r', %'+header+r' \]',match[1]):
            raise ValueError('Skipped membership changes a loop accumulator')
        preserved.append(dict(Header=current,Skip=back,Type=kind))
    if not preserved:raise ValueError('No preserved observable probe state')
    # The wave result only initializes this work-list and its zero check.
    if len(re.findall(r'%'+wave+r'\b',dxil))!=3:raise ValueError('Wave result has another observable use')
    return dict(Wave=wave,Pixel=pixel,Membership=membership[1],Predicate=predicate[1],
                LoopHeader=header,SkipBlock=skip,PreservedAccumulators=preserved)

def lower_probe_wave_worklist(data,bytecode,dxil,root):
    expected=hashlib.sha256(bytecode).hexdigest()
    if expected not in PROBE_SHADER_IDENTITIES.values():raise ValueError('Unaudited lighting bytecode')
    # Native DXIL: wave OR creates a superset work-list. Every actual probe
    # evaluation is guarded by the ORIGINAL per-pixel mask. Failed membership
    # leaves every accumulator unchanged through these native phi edges.
    native_proof=verify_guarded_native_worklist(dxil)
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5];ops=[];i=5
    while i<len(words):
        n=words[i]>>16;op=words[i]&65535
        if not n or i+n>len(words):raise ValueError('Invalid SPIR-V')
        ops.append((op,words[i+1:i+n]));i+=n
    groups=[(op,a) for op,a in ops if 333<=op<=366]
    if len(groups)!=1 or groups[0][0]!=360:raise ValueError('Unexpected subgroup operations')
    # Changing list-loop divergence must not change implicit texture LOD or
    # quad derivatives. This exact native variant uses explicit-LOD reads.
    if any(op in (87,89,91,93) or 207<=op<=215 for op,a in ops):
        raise ValueError('Probe shader needs derivative/divergence analysis')
    _,reduction=groups[0]
    if len(reduction)!=5 or reduction[3]!=0:raise ValueError('Expected native subgroup OR reduction')
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    if types[reduction[0]]!=(21,[32,0]):raise ValueError('Probe mask is not u32')
    constants={a[1]:(a[0],a[2:]) for op,a in ops if op in (42,43,46)}
    if constants.get(reduction[2])!=(reduction[0],[3]):raise ValueError('Not subgroup scope')
    select=next((a for op,a in ops if op==169 and a[1]==reduction[4]),None)
    if not select or constants.get(select[3])!=(reduction[0],[0]):raise ValueError('Helper mask is not OR-neutral zero')
    predicate=next((a for op,a in ops if op==166 and a[1]==select[2]),None)
    if not predicate or types[predicate[0]]!=(20,[]):raise ValueError('Unexpected helper predicate')
    load=next((a for op,a in ops if op==61 and a[1]==predicate[2]),None)
    discard_load=next((a for op,a in ops if op==61 and a[1]==predicate[3]),None)
    if not load or not discard_load:raise ValueError('Unexpected helper/discard predicate')
    discard_variable=next((a for op,a in ops if op==59 and a[1]==discard_load[2]),None)
    if not discard_variable or discard_variable[2]!=6 or types[discard_variable[0]]!=(32,[6,predicate[0]]):
        raise ValueError('Discard state is not the original private boolean')
    # Discarded/helper invocations may now evaluate their own list, but cannot
    # affect visible lanes: this exact shader has no resource writes or other
    # subgroup operations. Keep ALL original discard stores and final kill.
    if re.search(r'call\s+.*@dx\.op\.(?:bufferStore|rawBufferStore|textureStore|atomicBinOp|atomicCompareExchange)',dxil):
        raise ValueError('Probe shader has shared side effects')
    if not re.search(r'call void @dx\.op\.discard\(',dxil):raise ValueError('Original discard path missing')
    helper=load[2]
    if (71,[helper,11,23]) not in ops:raise ValueError('Missing helper-input identity')
    variable=next((a for op,a in ops if op==59 and a[1]==helper),None)
    if not variable or variable[2]!=1:raise ValueError('Unexpected helper variable storage')
    removed={helper,load[1],predicate[1],select[1]};output=[]
    for op,a in ops:
        if op in (5,71) and a[0] in removed:continue
        if (op==59 and a[1]==helper) or (op in (61,166,169) and a[1] in removed):continue
        if op==15:
            start=2
            while start<len(a):
                part=struct.pack('<I',a[start]);start+=1
                if b'\0' in part:break
            output.append((op,a[:start]+[x for x in a[start:] if x!=helper]));continue
        if op==360 and a==reduction:
            output.append((83,[a[0],a[1],select[4]]));continue
        if any(x in removed for x in a):raise ValueError('Helper mask has another unaudited use')
        output.append((op,a))
    packed=header[:]
    for op,a in output:packed.extend([((len(a)+1)<<16)|op,*a])
    # For every bit, a lane's M is a subset of the wave union W. Enumerating W
    # increasingly and accepting only M bits is the same sequence as enumerating
    # M directly, including zero masks. Thus all FLOAT operation order is kept.
    masks=[0,0xffffffff]+[1<<b for b in range(32)]+[0xaaaaaaaa,0x55555555,0x80000001]
    for mask in masks:
        for union in [mask,0xffffffff,mask|0x12345678]:
            assert [b for b in range(32) if union&(1<<b) and mask&(1<<b)]==[b for b in range(32) if mask&(1<<b)]
    proof=dict(OriginalDXILSHA256=expected,NativeWorklistProof=native_proof,
        SPIRVORResult=reduction[1],SPIRVOriginalPixelMask=select[4],RemovedHelperIDs=sorted(removed),
        InputSPIRVSHA256=hashlib.sha256(data).hexdigest(),
        OutputSPIRVSHA256=hashlib.sha256(struct.pack('<%dI'%len(packed),*packed)).hexdigest(),
        OriginalDiscardVariable=discard_load[2],OriginalDiscardPathPreserved=True,SharedResourceWrites=False,
        ImplicitGradientOperations=False,
        Scope='One exact original lighting shader: remove wave-union work-list and helper-neutral mask. Original per-pixel membership guards, probe-evaluation bodies, scalar arithmetic, accumulator order, discard path and bytecode identity remain unchanged. Helper/discarded work has no cross-lane or shared-write effects. Full rendering parity still unverified.')
    return struct.pack('<%dI'%len(packed),*packed),[proof]
