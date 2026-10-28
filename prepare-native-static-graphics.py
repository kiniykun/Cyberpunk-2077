"""Lower original static DXIL-derived SPIR-V to browser graphics interfaces.

Keep shader computations unchanged. Separate native b/t/s and vertex/pixel
register namespaces, and provide real draw parameters for Vulkan-only builtins.
Unsupported interfaces are recorded as failures, never replaced by shaders.
"""
from pathlib import Path
import argparse, ast, hashlib, json, re, struct, subprocess
from sampler_bias_lowering import lower_sampling_bias
from graphics_storage_words import native_storage_strides, consolidate_storage_words
ROOT=Path(__file__).resolve().parent
NAGA=ROOT/'tools/naga-scope-fixed/target/release/naga.exe'
_scope_proof=json.loads((ROOT/'tools/naga-scope-fixed/scope-fix-provenance.json').read_text())
assert hashlib.sha256(NAGA.read_bytes()).hexdigest()==_scope_proof['ExecutableSHA256']
assert hashlib.sha256((ROOT/'tools/naga-scope-fixed/naga/src/front/spv/next_block.rs').read_bytes()).hexdigest()==_scope_proof['PatchedNextBlockSHA256']
assert hashlib.sha256((ROOT/'tools/naga-scope-fixed/naga/src/back/wgsl/writer.rs').read_bytes()).hexdigest()==_scope_proof['PatchedWGSLWriterSHA256']
assert hashlib.sha256((ROOT/'tools/naga-scope-fixed/naga/src/front/spv/image.rs').read_bytes()).hexdigest()==_scope_proof['PatchedImageCoordinatesSHA256']
DXC=ROOT/'tools/dxc-1.9.2609/bin/x64/dxc.exe'
OUT=ROOT/'build/shaders/native-static-graphics-ready'
OUT.mkdir(exist_ok=True)

def instructions(data):
    words=list(struct.unpack('<%dI'%(len(data)//4),data))
    assert words[0]==0x07230203
    result=[]; pos=5
    while pos<len(words):
        count,op=words[pos]>>16,words[pos]&65535
        assert count and pos+count<=len(words)
        result.append((op,words[pos+1:pos+count]));pos+=count
    return words[:5],result

def lower(data,stage):
    header,ops=instructions(data)
    types={a[0]:(op,a[1:]) for op,a in ops if 19<=op<=33}
    variables={a[1]:(a[0],a[2]) for op,a in ops if op==59}
    decorations={}
    for op,a in ops:
        if op==71: decorations.setdefault(a[0],{})[a[1]]=a[2:]
    resources=[]; bindings={}; draw_variables={}
    stage_shift=512 if stage=='pixel' else 0
    for variable,(pointer,storage) in variables.items():
        deco=decorations.get(variable,{})
        if 11 in deco and deco[11][0] in (4424,4425,4426):
            if stage!='vertex': raise ValueError('Draw parameter builtin in non-vertex stage')
            pointee=types[pointer][1][1]
            if types[pointee][0]!=21 or types[pointee][1][0]!=32: raise ValueError('Non-32-bit draw builtin')
            draw_variables[variable]=(deco[11][0]-4424,pointee)
        if 33 not in deco: continue
        slot=deco[33][0]; group=deco.get(34,[0])[0]
        if group: raise ValueError('Native register space requires additional lowering')
        pointee=types[pointer][1][1]; type_op=types[pointee][0]
        if storage==2: kind,shift,limit='ConstantBuffer',0,14
        elif storage==0 and type_op==26: kind,shift,limit='Sampler',256,16
        elif storage==0 and type_op==25 and types[pointee][1][5]==1: kind,shift,limit='TextureSRV',64,64
        elif storage==0 and type_op==25 and types[pointee][1][5]==2 and stage=='pixel': kind,shift,limit='StorageTextureUAV',288,64
        elif storage==12 and type_op==30: kind,shift,limit='StorageBuffer',64,64
        elif storage==0 and type_op==28 and slot==130 and stage=='pixel' and types[types[pointee][1][0]][0]==25:
            # Multilayer bindless table t130 (see multilayer_bindless_lowering.py): the WGSL keeps one
            # sampled texture at the reserved SRV slot 63 (the original GNM bindless slot).
            kind,shift,limit,slot='TextureIndirection',64,64,63
        else: raise ValueError('Unsupported static graphics resource type/storage %s/%s'%(type_op,storage))
        depth_alias=False
        if kind=='TextureSRV' and slot>=1000:
            # comparison_image_split.py: plain-sampling alias of a depth-compared SRV, same native slot.
            from comparison_image_split import ALIAS_MARKER,DEPTH_ALIAS_SHIFT
            slot-=ALIAS_MARKER;shift=DEPTH_ALIAS_SHIFT;depth_alias=True
        if slot>=limit: raise ValueError('Resource slot beyond native graphics interface')
        binding=stage_shift+shift+slot
        bindings[variable]=binding
        resource={'Group':0,'Binding':binding,'OriginalSlot':slot,'Kind':kind,'Variable':variable}
        if depth_alias:resource['ComparisonSplitAlias']=True
        if kind=='TextureIndirection':resource.update(OriginalSlot=65535,NativeRegister=130,MultilayerBindless=True)
        if kind=='StorageBuffer':
            block=types[pointee][1]
            if len(block)!=1 or types[block[0]][0]!=29:raise ValueError('Unsupported native SSBO block shape')
            array=block[0];element=types[array][1][0];element_op,element_args=types[element]
            width=1
            if element_op==23:
                scalar,width=element_args
                scalar_op,scalar_args=types[scalar]
            else:scalar_op,scalar_args=element_op,element_args
            if scalar_op!=21 or scalar_args!=[32,0] or decorations[array].get(6)!=[width*4]:raise ValueError('Native SSBO word stride cannot be proved')
            resource['StorageWordViews']=[{'Width':width,'Stride':width*4}]
        existing=next((r for r in resources if r['Binding']==binding),None)
        if existing:
            if kind!='StorageBuffer' or existing['Kind']!=kind:raise ValueError('Duplicate native resource register')
            existing['StorageWordViews']+=resource['StorageWordViews']
        else:resources.append(resource)
    added_types=[]; added_decorations=[]; draw_pointer=draw_field_pointer=draw_i32=None; constants=[]
    if draw_variables:
        def new_id():
            value=header[3];header[3]+=1;return value
        draw_i32=next((i for i,(op,a) in types.items() if op==21 and a==[32,1]),None)
        if draw_i32 is None: draw_i32=new_id();added_types.append((21,[draw_i32,32,1]))
        u32=next((i for i,(op,a) in types.items() if op==21 and a==[32,0]),None)
        if u32 is None: u32=new_id();added_types.append((21,[u32,32,0]))
        block=new_id();block_pointer=new_id();draw_field_pointer=new_id();draw_pointer=new_id()
        added_types += [(30,[block,draw_i32,draw_i32,draw_i32]),(32,[block_pointer,2,block]),(32,[draw_field_pointer,2,draw_i32])]
        for i in range(3):
            ident=new_id();constants.append(ident);added_types.append((43,[u32,ident,i]))
        added_types.append((59,[block_pointer,draw_pointer,2]))
        added_decorations += [(71,[block,2]),*( (72,[block,i,35,i*4]) for i in range(3)),(71,[draw_pointer,34,0]),(71,[draw_pointer,33,511])]
        resources.append({'Group':0,'Binding':511,'OriginalSlot':0,'Kind':'DrawParameters','Variable':draw_pointer,'Builtins':sorted({x[0]+4424 for x in draw_variables.values()})})
    transformed=[]
    for op,args in ops:
        a=list(args)
        if draw_variables and op==17 and a==[4427]: continue
        if op==71 and a[0] in draw_variables: continue
        if op==59 and a[1] in draw_variables: continue
        if op==15 and draw_variables:
            end=2
            while not any(byte==0 for byte in struct.pack('<I',a[end])): end+=1
            end+=1
            a=a[:end]+[i for i in a[end:] if i not in draw_variables]
            # Modern SPIR-V entry interfaces include uniform globals too.
            if header[1]>=0x00010400: a.append(draw_pointer)
        if op==71 and a[1]==33 and a[0] in bindings: a[2]=bindings[a[0]]
        if op==61 and a[2] in draw_variables:
            field,type_id=draw_variables[a[2]]
            access=new_id(); transformed.append((65,[draw_field_pointer,access,draw_pointer,constants[field]]))
            if a[0]==draw_i32: a[2]=access
            else:
                loaded=new_id();transformed.append((61,[draw_i32,loaded,access,*a[3:]]));transformed.append((124,[a[0],a[1],loaded]));continue
        transformed.append((op,a))
    # Decorations precede all type declarations; new types follow dependencies
    # and precede functions. Do not reorder any original executable operations.
    first_type=next(i for i,(op,_) in enumerate(transformed) if 19<=op<=39)
    transformed[first_type:first_type]=added_decorations
    first_function=next(i for i,(op,_) in enumerate(transformed) if op==54)
    transformed[first_function:first_function]=added_types
    words=list(header)
    for op,a in transformed: words += [(len(a)+1)<<16|op,*a]
    return struct.pack('<%dI'%len(words),*words),resources

def run():
    global OUT
    parser=argparse.ArgumentParser()
    parser.add_argument('--material-ui',action='store_true')
    parser.add_argument('--material-observed',action='store_true')
    parser.add_argument('--only',nargs='+',help='Reprepare selected hashes; retain unchanged audited rows for others')
    options=parser.parse_args()
    if options.material_observed:
        OUT=ROOT/'build/shaders/native-observed-material-graphics-ready';OUT.mkdir(exist_ok=True)
    if options.material_ui:
        OUT=ROOT/'build/shaders/native-material-graphics-ready';OUT.mkdir(exist_ok=True)
    tree=ast.parse((ROOT/'compile-ui-shaders.py').read_text())
    reflector=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='reflect')
    ns={'Path':Path,'struct':struct};exec(compile(ast.Module(body=[reflector],type_ignores=[]),'native-layout-reflector','exec'),ns)
    reflect=ns['reflect']
    source=json.loads((ROOT/('build/shaders/native-observed-material-conversion.json' if options.material_observed else 'build/shaders/native-ui-conversion.json' if options.material_ui else 'build/shaders/native-cache-conversion.json')).read_text())
    previous={}
    if options.only:
        previous={r['Hash']:r for r in json.loads((ROOT/('build/shaders/native-observed-material-graphics-binding-preparation.json' if options.material_observed else 'build/shaders/native-material-graphics-binding-preparation.json' if options.material_ui else 'build/shaders/native-static-graphics-binding-preparation.json')).read_text())['Shaders']}
        assert set(options.only)<={s['Hash'] for s in source['Shaders']}
    rows=[]
    for original in source['Shaders']:
        if original['Stage'] not in ('vertex','pixel'): continue
        if options.only and original['Hash'] not in options.only:
            prior=previous[original['Hash']]
            assert prior['SHA256']==original['SHA256'] and prior['SPIRVSHA256']==original['SPIRVSHA256']
            rows.append(prior);continue
        row=dict(original,RegisterMappingVerified=False,UniformRoundtripVerified=False)
        try:
            for key,hashkey in [('Path','SHA256'),('SPIRV','SPIRVSHA256')]:
                assert hashlib.sha256(Path(original[key]).read_bytes()).hexdigest()==original[hashkey]
            dump=subprocess.run([str(DXC),'-dumpbin',original['Path']],capture_output=True,text=True)
            assert dump.returncode==0,dump.stderr
            storage_strides=native_storage_strides(dump.stdout)
            native_data=Path(original['SPIRV']).read_bytes()
            if original['Stage']=='pixel':
                # [nailfix] write-only RWTexture2D<uint> UAVs (glass, vehicle_glass, water) need an explicit R32ui format
                from storage_image_format_lowering import set_integer_uav_formats
                native_data,row['IntegerUavFormatAudits']=set_integer_uav_formats(native_data,dump.stdout)
            if options.material_observed and original['Stage']=='vertex':
                import runpy
                array_lower=runpy.run_path(str(ROOT/'flatten-native-array-inputs.py'))['lower']
                native_data,row['NativeArrayInputAudits']=array_lower(native_data)
            from native_packed_varyings import pack_native_varyings,PACKED_IDENTITIES
            if original['Hash'] in PACKED_IDENTITIES:
                native_data,row['NativePackedVaryingAudits']=pack_native_varyings(native_data,Path(original['Path']).read_bytes(),original['Hash'],ROOT)
            else:
                # [nailfix] flat DXIL elements sharing one register (Location+Component) -> one vec4<u32> varying
                from component_varying_lowering import pack_component_varyings
                native_data,_component_audits=pack_component_varyings(native_data)
                if _component_audits:row['ComponentVaryingAudits']=_component_audits
            from native_probe_wave_lowering import lower_probe_wave_worklist,PROBE_SHADER_IDENTITIES
            if original['Hash'] in PROBE_SHADER_IDENTITIES:
                native_data,row['NativeProbeWorklistAudits']=lower_probe_wave_worklist(native_data,Path(original['Path']).read_bytes(),dump.stdout,ROOT)
            row['UniformScalarAliasAudits']=[]
            is_captured_eye_gradient=original['SHA256']=='ef911f0d6072925b180b733318ad8144c3c7760f753eecd7829eb9a1af692773'
            from uniform_scalar_aliases import uniform_alias_registers
            alias_registers=uniform_alias_registers(native_data)
            if original['Hash'] in ('16576201090757737304','16524549184563482639','11526415310978580109','12784076727638384549') or is_captured_eye_gradient or alias_registers:
                from uniform_scalar_aliases import normalize_uniform_scalar_aliases
                for alias_register in (alias_registers or [13]):
                    native_data,alias_audits=normalize_uniform_scalar_aliases(native_data,alias_register)
                    row['UniformScalarAliasAudits']+=alias_audits
            if is_captured_eye_gradient:
                from eye_gradient_wave_lowering import lower_eye_gradient_worklist
                native_data,row['NativeEyeGradientWorklistAudits']=lower_eye_gradient_worklist(native_data,Path(original['Path']).read_bytes(),dump.stdout)
            from multilayer_bindless_lowering import MULTILAYER_SHA256,lower_waterfalls,lower_bindless_wgsl,finalize_bindless_wgsl,bindless_table_in_dump
            # Any pixel shader reading the t130 bindless table (multilayered, vehicle/decal/glass variants...).
            is_multilayer=original['SHA256'] in MULTILAYER_SHA256 or (original['Stage']=='pixel' and bindless_table_in_dump(dump.stdout))
            # dxil-spirv subgroup waterfalls / helper-lane tests (HelperInvocation=23, SubgroupLocalInvocationId=41)
            # also occur without t130 (forward/translucent materials): run them per lane as well.
            import struct as _struct
            _words=_struct.unpack('<%dI'%(len(native_data)//4),native_data)
            _has_lane_builtin=False;_pos=5
            while _pos<len(_words):
                _size=_words[_pos]>>16
                if not _size:break
                if (_words[_pos]&65535)==71 and _size>=4 and _words[_pos+2]==11 and _words[_pos+3] in (23,41):_has_lane_builtin=True;break
                _pos+=_size
            if is_multilayer or (original['Stage']=='pixel' and _has_lane_builtin):
                native_data,row['MultilayerWaterfallAudits']=lower_waterfalls(native_data)
            if original['Stage']=='pixel':
                from comparison_image_split import split_comparison_images
                native_data,row['ComparisonImageSplitAudits']=split_comparison_images(native_data)
            lowered,resources=lower(native_data,original['Stage'])
            is_hair_atomic=original['Hash'] in ('1147326969136257287','3595036683822271335')
            if is_hair_atomic:
                from hair_atomic_buffer_lowering import lower_hair_atomics
                lowered,row['HairAtomicBufferAudits']=lower_hair_atomics(lowered,resources,Path(original['Path']).read_bytes())
                for resource in resources:
                    if resource['Kind']=='StorageTextureUAV':resource['RuntimeKind']='StorageTextureAtomicBuffer'
            row['DeadSubgroupOperationAudits']=[]
            if original['Hash']=='12784076727638384549':
                from dead_subgroup_lowering import lower_dead_subgroup_or
                lowered,row['DeadSubgroupOperationAudits']=lower_dead_subgroup_or(lowered,ROOT)
            from spirv_nan_lowering import lower_isnan
            lowered,nan_audits=lower_isnan(lowered,ROOT)
            row['FloatClassificationAudits']=nan_audits
            from spirv_integer_lowering import lower_unsigned_negate
            integer_audits=[]
            if original['Hash'] in ('1235608198175133740','9561067772023104335','11535887795805672346','13089757599489257835'):
                lowered,integer_audits=lower_unsigned_negate(lowered,ROOT)
            row['UnsignedNegationAudits']=integer_audits
            # dxil-spirv keeps DXIL i32 as uint; naga then prints signed opcodes (sitofp etc.) as unsigned.
            from spirv_integer_lowering import lower_signed_uint_ops
            import os
            lowered,signed_audits,signed_unlowered=(lowered,[],[]) if os.environ.get('CP2077_SKIP_SIGNED_LOWERING') else lower_signed_uint_ops(lowered,ROOT)
            row['SignedIntegerOpAudits']=len(signed_audits);row['SignedIntegerOpsUnlowered']=signed_unlowered
            for resource in resources:
                if resource['Kind']=='StorageBuffer':
                    if resource['OriginalSlot'] not in storage_strides:raise ValueError('Storage binding lacks exact native read-only SRV metadata')
                    resource['StorageStride']=storage_strides[resource['OriginalSlot']]
            stem=original['Hash']+'-'+original['Stage']; spv=OUT/(stem+'.spv');spv.write_bytes(lowered)
            wgsl=OUT/(stem+'.wgsl')
            # The translator emits scalar/vec3/vec4 aliases for the same native
            # byte-addressed SRV. Emit an intermediate only, consolidate exact
            # word addresses, then require normal validation of final WGSL.
            intermediate_options=['--validate','0'] if storage_strides or is_multilayer else []
            process=subprocess.run([str(NAGA),*intermediate_options,'--keep-coordinate-space',str(spv),str(wgsl)],capture_output=True,text=True)
            (OUT/(stem+'-conversion.log')).write_text(process.stdout+process.stderr)
            if process.returncode: raise ValueError(process.stderr[:1800])
            intermediate_text=wgsl.read_text()
            if is_multilayer:intermediate_text,row['MultilayerBindlessAudits']=lower_bindless_wgsl(intermediate_text,resources)
            storage_text,storage_audits=consolidate_storage_words(intermediate_text,resources)
            row['ConstantPowerFoldAudits']=[]
            if original['Hash']=='12784076727638384549' or is_captured_eye_gradient:
                expression='exp2((log2(0f) * 4f))'
                assert storage_text.count(expression)==1,'Original zero-power expression changed'
                reference=ROOT/'build/shaders/zero-power-reference.dxil'
                reference_dump=subprocess.run([str(DXC),'-dumpbin',str(reference)],capture_output=True,text=True,check=True).stdout
                stores=[line for line in reference_dump.splitlines() if 'call void @dx.op.storeOutput.f32' in line]
                assert len(stores)==4 and all('float 0.000000e+00)' in line for line in stores)
                storage_text=storage_text.replace(expression,'0f')
                row['ConstantPowerFoldAudits']=[{'Expression':expression,'Replacement':'0f','ReferenceDXILSHA256':hashlib.sha256(reference.read_bytes()).hexdigest(),'HLSLCompilerZeroOutputsVerified':True,'OtherArithmeticChanged':False}]
            else:
                # [textures] Same pow(0, positive constant) fold as above (naga const-evaluates log2(0f) and rejects it);
                # the zero-power reference DXIL shows the HLSL compiler emits exactly 0 for these.
                folds=re.findall(r'exp2\(\(log2\(0f\) \* ([0-9.]+)f\)\)',storage_text)
                if folds and all(float(v)>0 for v in folds):
                    storage_text=re.sub(r'exp2\(\(log2\(0f\) \* ([0-9.]+)f\)\)','0f',storage_text)
                    row['ConstantPowerFoldAudits']=[{'Expression':'exp2((log2(0f) * %sf))'%v,'Replacement':'0f'} for v in folds]
            # [whitewall] The HLSL compiler folded some expressions to NaN literals (DXIL 'float 0x7FF8000000000000', e.g.
            # window_parallax_interior_proxy VS); naga prints them as the invalid identifier 'NaNf'. WGSL forbids NaN
            # const-expressions, so read one quiet NaN through a private variable at run time. Same IEEE value as native.
            row['NaNLiteralLowerings']=len(re.findall(r'\bNaNf\b',storage_text))
            if row['NaNLiteralLowerings']:
                storage_text=re.sub(r'\bNaNf\b','bitcast<f32>(cp2077_quiet_nan_bits)',storage_text)
                storage_text='var<private> cp2077_quiet_nan_bits: u32 = 0x7fc00000u;\n'+storage_text
            # [whitewall] r4: WebGPU has no subgroup operations in vertex shaders. Clustered-light vertex shaders
            # (blackbodyradiation*) scalarize with WaveActiveBitOr(tileMask) and then walk the set bits; the lane's own
            # mask is a subset of the wave OR, so each vertex still walks every light of its own tile (lights of
            # neighbouring lanes' tiles are skipped). Only subgroupOr is rewritten, only in vertex shaders.
            row['VertexSubgroupOrLowerings']=0
            if original['Stage']=='vertex' and 'subgroupOr(' in storage_text:
                row['VertexSubgroupOrLowerings']=storage_text.count('subgroupOr(')
                storage_text=storage_text.replace('subgroupOr(','(')
                storage_text=re.sub(r'^\s*enable\s+subgroups\s*;\s*$','',storage_text,flags=re.M)
            biased,sampling_calls=lower_sampling_bias(storage_text,resources,996 if original['Stage']=='pixel' else 500)
            if is_multilayer:biased,row['MultilayerPoolAudits']=finalize_bindless_wgsl(biased,resources)
            # Lowering helpers prepend declarations. WGSL feature directives
            # must precede those declarations; retain every requested feature.
            directives=re.findall(r'^\s*((?:enable|requires)\s+[^;]+;)\s*$',biased,re.M)
            if directives:
                biased=re.sub(r'^\s*(?:enable|requires)\s+[^;]+;\s*$','',biased,flags=re.M)
                biased='\n'.join(dict.fromkeys(directives))+'\n'+biased
            wgsl.write_text(biased)
            audit=OUT/(stem+'-audit.spv')
            process=subprocess.run([str(NAGA),'--keep-coordinate-space','-g',str(wgsl),str(audit)],capture_output=True,text=True)
            if process.returncode: raise ValueError(process.stderr[:1800])
            native=reflect(Path(original['SPIRV']),indexed=True); web=reflect(audit,indexed=True);sizes=reflect(audit,root_spans=True)
            shift=512 if original['Stage']=='pixel' else 0
            for key,fields in native.items():
                assert fields==web[(key[0],key[1]+shift)],'Native uniform scalar byte coordinates changed'
            for resource in resources:
                if resource['Kind']=='ConstantBuffer':resource['WebGPUMinBytes']=sizes[(0,resource['Binding'])]
            text=wgsl.read_text()
            dump=subprocess.run([str(DXC),'-dumpbin',original['Path']],capture_output=True,text=True)
            assert dump.returncode==0,dump.stderr
            dump_path=OUT/(stem+'-original-dxil.txt');dump_path.write_text(dump.stdout)
            if row.get('NaNLiteralLowerings'):assert '0x7FF8000000000000' in dump.stdout,'NaN literal lowering without a native NaN constant' # [whitewall]
            if row.get('VertexSubgroupOrLowerings'):assert 'waveActiveBit' in dump.stdout,'subgroupOr lowering without a native wave bit op' # [whitewall] r4
            expected=set()
            section=dump.stdout.split('; Resource Bindings:',1)[1].split('; ViewId',1)[0]
            for line in section.splitlines():
                match=re.search(r'\b(cbuffer|texture|sampler|UAV)\s+.*?\b(?:CB|T|S|U)\d+\s+(cb|t|s|u)(\d+)\s+(\d+)\s*$',line)
                if match:
                    _,kind,slot,count=match.groups()
                    if is_multilayer and kind=='t' and slot=='130':
                        assert int(count)==32768;expected.add(('MultilayerTable',0));continue
                    assert int(count)==1
                    resource_kind={'cb':'ConstantBuffer','t':'TextureSRV','s':'Sampler','u':'StorageTextureUAV'}[kind]
                    if kind=='t' and int(slot) in storage_strides:resource_kind='StorageBuffer'
                    expected.add((resource_kind,int(slot)))
            assert expected=={(r['Kind'],r['OriginalSlot']) for r in resources if r['Kind'] not in ('DrawParameters','SamplerBias','MultilayerPool')},'Native register reflection mismatch'
            if is_hair_atomic:
                resources.append({'Group':0,'Binding':995,'OriginalSlot':1,'Kind':'TextureAtomicDimensions','WebGPUMinBytes':16})
            inputs=[]
            if original['Stage']=='vertex':
                signature=dump.stdout.split('; Input signature:',1)[1].split('; Output signature:',1)[0]
                entry=re.search(r'@vertex\s+fn main\((.*?)\)\s*(?:->|\{)',text,re.S);assert entry
                locations={int(v) for v in re.findall(r'@location\((\d+)\)',entry[1])}
                for line in signature.splitlines():
                    match=re.match(r';\s+(\w+)\s+(\d+)\s+([xyzw]+)\s+(\d+)\s+(\w+)\s+(float|uint|sint)\b',line)
                    if match:
                        name,index,mask,reg,system,component=match.groups()
                        if system=='NONE' and int(reg) in locations:inputs.append({'Semantic':name,'Index':int(index),'Location':int(reg),'Components':len(mask),'ComponentType':component})
                assert {v['Location'] for v in inputs}==locations,'Native vertex input locations changed'
            row.update(RegisterMappingVerified=True,UniformRoundtripVerified=True,WGSLValidated=True,
                Resources=resources,VertexInputs=inputs,VertexInputLocationsVerified=True,
                LoweredSPIRV=str(spv),LoweredSPIRVSHA256=hashlib.sha256(lowered).hexdigest(),
                AuditSPIRV=str(audit),WGSL=str(wgsl),WGSLSHA256=hashlib.sha256(wgsl.read_bytes()).hexdigest(),
                OriginalDXILReflection=str(dump_path),NativeDrawParametersRequired=any(r['Kind']=='DrawParameters' for r in resources))
            row.update(SamplerBiasLowered=True,SamplerBiasSamplingCalls=sampling_calls)
            row.update(StorageWordAddressAudits=storage_audits)
        except Exception as error: row['PreparationError']=str(error)
        rows.append(row)
    result={'Candidates':len(rows),'Prepared':sum(r['RegisterMappingVerified'] and r['UniformRoundtripVerified'] for r in rows),
        'SourceSHA256':source['OriginalCacheSHA256'],'DrawParameterBuiltinsUseActualDrawUniform':True,
        'RuntimeIntegrated':False,'OriginalMenusVerified':False,'ConverterPath':str(NAGA),
        'ConverterSHA256':hashlib.sha256(NAGA.read_bytes()).hexdigest(),'ConverterValidationEnabled':True,'Shaders':rows}
    (ROOT/('build/shaders/native-observed-material-graphics-binding-preparation.json' if options.material_observed else 'build/shaders/native-material-graphics-binding-preparation.json' if options.material_ui else 'build/shaders/native-static-graphics-binding-preparation.json')).write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='Shaders'},indent=2))
    for r in rows:
        if r['Hash'] in ('5982978567936655361','5512477063305157692','17639438368405678707','17635279434789068189','7118783774257250113','8490209825382157974'):
            print(r['Hash'],r['RegisterMappingVerified'],r.get('PreparationError',''))
if __name__=='__main__': run()
