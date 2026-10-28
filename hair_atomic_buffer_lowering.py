"""Lower the two audited original hair UAV programs to bounded buffer atomics.

Transport must preserve the original R32Uint array before and after each draw.
This pass alone does not integrate the shader into the native renderer.
"""
import hashlib,struct

IDENTITIES={
    'c6a5056963455cb0da0005df101189c2b397d63383feca516301a4c83c4549f0',
    'c5938921c31376b7353dc8f45a18596b84e82bac8b04064a1aeed28ca38a847a',
}
def lower_hair_atomics(data,resources,bytecode):
    bytecode_sha=hashlib.sha256(bytecode).hexdigest()
    if bytecode_sha not in IDENTITIES:raise ValueError('Unaudited original hair bytecode')
    words=list(struct.unpack('<%dI'%(len(data)//4),data));header=words[:5]
    if header[0]!=0x07230203:raise ValueError('Not SPIR-V')
    ops=[];p=5
    while p<len(words):
        size,op=words[p]>>16,words[p]&65535
        if not size or p+size>len(words):raise ValueError('Malformed SPIR-V')
        ops.append((op,words[p+1:p+size]));p+=size
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    variables={a[1]:(a[0],a[2]) for op,a in ops if op==59}
    uavs=[r for r in resources if r['Kind']=='StorageTextureUAV']
    if len(uavs)!=1 or uavs[0]['OriginalSlot']!=1:raise ValueError('Expected exact native hair u1')
    resource=uavs[0];image_variable=resource['Variable'];pointer,storage=variables[image_variable]
    image=types[pointer][1][1];op,args=types[image]
    if storage!=0 or op!=25 or args[1:]!=[1,0,1,0,2,33]:raise ValueError('Expected R32Uint 2D array UAV')
    uint=args[0]
    if types[uint]!=(21,[32,0]):raise ValueError('Unsigned 32-bit UAV required')
    atomics=[a for o,a in ops if o==239] # OpAtomicUMax
    reads=[a for o,a in ops if o==98]
    pointers=[a for o,a in ops if o==60]
    if len(atomics)!=3 or len(reads)!=1 or len(pointers)!=3:raise ValueError('Original hair operation count changed')
    if any(a[2]!=image_variable for a in pointers):raise ValueError('Other image atomic resource')
    loads=[a for o,a in ops if o==61 and a[2]==image_variable]
    if len(loads)!=1 or reads[0][2]!=loads[0][1]:raise ValueError('Unexpected image-read producer')
    image_load=loads[0][1]
    read_result=reads[0][1]
    observed_read_uses=[(o,a) for o,a in ops if o not in (5,98) and read_result in a]
    if not observed_read_uses or any(o!=81 or len(a)!=4 or a[2]!=read_result or a[3]!=0 for o,a in observed_read_uses):
        raise ValueError('Original image read observes channels other than r')
    if any(image_load in a for o,a in ops if not(o==61 and a[1]==image_load) and o!=98):raise ValueError('Image has another use')
    if any(o in range(227,243) and o!=239 for o,a in ops):raise ValueError('Unexpected atomic operation')
    new_id=header[3]
    def fresh():
        nonlocal new_id
        result=new_id;new_id+=1;return result
    boolean=next(t for t,v in types.items() if v==(20,[]))
    vec4,array,block,block_ptr,word_ptr,dims_block,dims_ptr,dims_word_ptr,dims= [fresh() for _ in range(9)]
    zero,one,two,three,scope,semantics=[fresh() for _ in range(6)]
    added=[(23,[vec4,uint,4]),(29,[array,uint]),(30,[block,array]),(32,[block_ptr,12,block]),
        (32,[word_ptr,12,uint]),(30,[dims_block,vec4]),(32,[dims_ptr,2,dims_block]),
        (32,[dims_word_ptr,2,uint]),(43,[uint,zero,0]),(43,[uint,one,1]),(43,[uint,two,2]),
        (43,[uint,three,3]),(43,[uint,scope,1]),(43,[uint,semantics,0]),(59,[dims_ptr,dims,2])]
    annotations=[(71,[array,6,4]),(71,[block,2]),(72,[block,0,35,0]),
        (71,[dims_block,2]),(72,[dims_block,0,35,0]),(71,[dims,34,0]),(71,[dims,33,995])]
    result_types={a[1]:a[0] for o,a in ops if len(a)>1 and a[0] in types and o>=41 and o not in (71,72)}
    pointer_info={a[1]:a for a in pointers}
    original_phi_ids={a[1] for o,a in ops if o==245}
    last_labels={};current=None;out=[];address_count=0
    def address(coords):
        nonlocal address_count
        coordinate_type=result_types[coords]
        if types[coordinate_type][0]!=23 or types[coordinate_type][1][1]!=3:raise ValueError('Expected three image coordinates')
        scalar=types[coordinate_type][1][0]
        if types[scalar][0]!=21 or types[scalar][1][0]!=32:raise ValueError('Expected integer texel coordinates')
        components=[];dimensions=[];checks=[];emitted=[]
        for index in range(3):
            component=fresh();emitted.append((81,[scalar,component,coords,index]))
            if scalar!=uint:
                converted=fresh();emitted.append((124,[uint,converted,component]));component=converted
            components.append(component)
        for constant in (zero,one,two,three):
            ptr,val=fresh(),fresh();emitted.extend([(65,[dims_word_ptr,ptr,dims,zero,constant]),(61,[uint,val,ptr])]);dimensions.append(val)
        for component,dimension in zip(components,dimensions):
            check=fresh();emitted.append((176,[boolean,check,component,dimension]));checks.append(check)
        xy,valid=fresh(),fresh();emitted.extend([(167,[boolean,xy,checks[0],checks[1]]),(167,[boolean,valid,xy,checks[2]])])
        zy,zyy,row,index,ptr=[fresh() for _ in range(5)]
        emitted.extend([(132,[uint,zy,components[2],dimensions[1]]),(128,[uint,zyy,zy,components[1]]),
            (132,[uint,row,zyy,dimensions[3]]),(128,[uint,index,row,components[0]]),
            (65,[word_ptr,ptr,image_variable,zero,index])])
        address_count+=1
        return emitted,ptr,valid
    def guarded(coords,result_type,result_id,operation):
        emitted,ptr,valid=address(coords);run,merge,value=fresh(),fresh(),fresh()
        predecessor=last_labels.get(current,current)
        emitted.extend([(247,[merge,0]),(250,[valid,run,merge]),(248,[run]),operation(ptr,value),
            (249,[merge]),(248,[merge]),(245,[result_type,result_id,value,run,zero,predecessor])])
        last_labels[current]=merge
        return emitted
    annotation_inserted=False;types_inserted=False
    for o,a in ops:
        if not annotation_inserted and 19<=o<=33:out.extend(annotations);annotation_inserted=True
        if not types_inserted and o==21 and a[0]==uint:
            out.append((o,a));out.extend(added);types_inserted=True;continue
        if o==248:current=a[0]
        if o==32 and a[0]==pointer:
            # Preserve the original variable's ID and binding decoration.
            out.append((32,[pointer,12,block]));continue
        if o==32 and a[1]==11:
            # Image texel pointer type becomes a storage-buffer scalar pointer.
            out.append((32,[a[0],12,uint]));continue
        if o==59 and a[1]==image_variable:out.append((59,[pointer,image_variable,12]));continue
        if o==61 and a[1]==image_load:continue
        if o==60:continue
        if o==239:
            info=pointer_info.get(a[2]);
            if not info:raise ValueError('Atomic pointer not mapped')
            out.extend(guarded(info[3],uint,a[1],lambda ptr,value:(239,[uint,value,ptr,scope,semantics,a[5]])));continue
        if o==98:
            if a!=reads[0] or len(a)!=5 or a[4]!=0:raise ValueError('Unexpected image-read operands')
            scalar_result=fresh()
            out.extend(guarded(a[3],uint,scalar_result,lambda ptr,value:(227,[uint,value,ptr,scope,semantics])))
            # Original R32Uint view has r,0,0,1; the original shader reads r.
            out.append((80,[a[0],a[1],scalar_result,zero,zero,one]));continue
        out.append((o,a))
    if not annotation_inserted or not types_inserted or address_count!=4:raise ValueError('Incomplete lowering')
    # Splitting an original predecessor block requires its successor phi edges
    # to reference the final inserted merge label, rather than its old entry.
    fixed=[]
    for o,a in out:
        if o==245 and a[1] in original_phi_ids:
            a=a[:]
            for i in range(3,len(a),2):a[i]=last_labels.get(a[i],a[i])
        fixed.append((o,a))
    header[3]=new_id;packed=header[:]
    for o,a in fixed:packed.extend([((len(a)+1)<<16)|o,*a])
    result=struct.pack('<%dI'%len(packed),*packed)
    return result,dict(OriginalBytecodeSHA256=bytecode_sha,InputSHA256=hashlib.sha256(data).hexdigest(),OutputSHA256=hashlib.sha256(result).hexdigest(),
        NativeSlot=1,Binding=resource['Binding'],DimensionsBinding=995,AtomicUMaxOperations=3,
        AtomicReadOperations=1,BoundsChecks=4,TransportRequired=True,RuntimeIntegrated=False)
