"""Expose original fixed-array vertex inputs as consecutive WGSL locations.

Rebuild the exact array at entry; preserve original arithmetic and access chains.
Reject unsupported pointer uses rather than discard an input.
"""
from pathlib import Path
import struct,json,hashlib,subprocess
r=Path(__file__).resolve().parent
grammar=json.loads((r/'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json').read_text())
C={x['opname']:x['opcode'] for x in grammar['instructions']}

def lower(data):
 w=list(struct.unpack('<%dI'%(len(data)//4),data));h=w[:5];ops=[];i=5
 while i<len(w):
  n=w[i]>>16;assert n and i+n<=len(w);ops.append((w[i]&65535,w[i+1:i+n]));i+=n
 types={a[0]:(op,a[1:]) for op,a in ops if C['OpTypeVoid']<=op<=C['OpTypeForwardPointer']}
 constants={a[1]:a[2] for op,a in ops if op==C['OpConstant'] and len(a)==3}
 locations={a[0]:a[2] for op,a in ops if op==C['OpDecorate'] and a[1]==30}
 variables={a[1]:a for op,a in ops if op==C['OpVariable']}
 # Input and output locations are separate namespaces. Only another input
 # can conflict with the locations occupied by an expanded input array.
 input_locations={loc for v,loc in locations.items() if v in variables and variables[v][2]==1}
 transforms={};decorations=[];decls=[];private_ptrs={};proof=[]
 def new():
  value=h[3];h[3]+=1;return value
 def private_ptr(t):
  if t not in private_ptrs:
   private_ptrs[t]=new();decls.append((C['OpTypePointer'],[private_ptrs[t],6,t]))
  return private_ptrs[t]
 for v,a in variables.items():
  if a[2]!=1 or v not in locations:continue
  po,pa=types[a[0]];assert po==C['OpTypePointer'];op,ta=types[pa[1]]
  if op!=C['OpTypeArray']:continue
  elem,length=ta;length=constants[length]
  eo,ea=types[elem];assert eo==C['OpTypeVector'] and 1<=length<=4
  scalar,components=ea;assert types[scalar]==(C['OpTypeFloat'],[32]) and 2<=components<=4
  location=locations[v];assert all(location+j not in input_locations for j in range(1,length))
  assert not any(op==C['OpDecorate'] and x[0]==v and x[1]!=30 for op,x in ops)
  rowptr=next((t for t,(o,x) in types.items() if o==C['OpTypePointer'] and x==[1,elem]),None)
  if rowptr is None:rowptr=new();decls.append((C['OpTypePointer'],[rowptr,1,elem]))
  rows=[new() for _ in range(length)];private=private_ptr(pa[1])
  transforms[v]={'rows':rows,'elem':elem,'array':pa[1],'pointer':private,'rowptr':rowptr}
  decorations.extend((C['OpDecorate'],[row,30,location+j]) for j,row in enumerate(rows))
  proof.append({'OriginalVariable':v,'OriginalLocation':location,'ArrayLength':length,'VectorComponents':components,'BrowserLocations':list(range(location,location+length))})
 if not transforms:return data,[]
 # Original interface variables become private arrays. Only direct access-chain
 # pointer derivation is currently admitted; dynamic indices remain unchanged.
 pointers={v:t['pointer'] for v,t in transforms.items()}
 rebuilt=[]
 for op,a in ops:
  a=list(a)
  if op in (C['OpAccessChain'],C['OpInBoundsAccessChain']) and a[2] in pointers:
   old=types[a[0]];assert old[0]==C['OpTypePointer'] and old[1][0]==1
   a[0]=private_ptr(old[1][1]);pointers[a[1]]=a[0]
  elif op in (C['OpCopyObject'],C['OpFunctionCall'],C['OpPhi'],C['OpSelect']) and any(x in pointers for x in a[2:]):
   raise ValueError('Unsupported original interface pointer use')
  rebuilt.append((op,a))
 # New pointer types must precede their global variables and all functions.
 # Emit originals, then new types/variables, then the original function bodies.
 globals_=[];functions=[];in_functions=False;entries=set()
 for op,a in rebuilt:
  if op==C['OpFunction']:in_functions=True
  if in_functions:functions.append((op,a));continue
  if op==C['OpDecorate'] and a[0] in transforms and a[1]==30:continue
  if op==C['OpVariable'] and a[1] in transforms:continue
  if op==C['OpEntryPoint']:
   assert a[0]==0,'Only original vertex array inputs supported';entries.add(a[1])
   at=2
   while True:
    value=a[at];at+=1
    if any(((value>>(8*j))&255)==0 for j in range(4)):break
   ids=[]
   for v in a[at:]:ids.extend(transforms[v]['rows'] if v in transforms else [v])
   a=a[:at]+ids
  globals_.append((op,a))
 at=next(i for i,(op,a) in enumerate(globals_) if op>=C['OpTypeVoid'] and op<=C['OpTypeForwardPointer'])
 globals_[at:at]=decorations
 globals_.extend(decls)
 for v,t in transforms.items():
  globals_.append((C['OpVariable'],[t['pointer'],v,6]))
  globals_.extend((C['OpVariable'],[t['rowptr'],row,1]) for row in t['rows'])
 output=globals_;fn=None;first=False
 for op,a in functions:
  if op==C['OpFunction']:fn=a[1];first=True
  output.append((op,a))
  if op==C['OpLabel'] and first:
   first=False
   if fn in entries:
    for v,t in transforms.items():
     values=[]
     for row in t['rows']:
      value=new();values.append(value);output.append((C['OpLoad'],[t['elem'],value,row]))
     array=new();output.append((C['OpCompositeConstruct'],[t['array'],array,*values]));output.append((C['OpStore'],[v,array]))
 packed=h[:]
 for op,a in output:packed.extend([((len(a)+1)<<16)|op,*a])
 return struct.pack('<%dI'%len(packed),*packed),proof

if __name__=='__main__':
 source=json.loads((r/'build/shaders/native-observed-material-conversion.json').read_text());out=r/'build/shaders/native-observed-material-ready';out.mkdir(exist_ok=True)
 rows=[]
 for s in source['Shaders']:
  if s['Stage']!='vertex' or s['WGSLValidated']:continue
  p=Path(s['SPIRV']);data,proof=lower(p.read_bytes())
  if not proof:continue
  target=out/p.name;target.write_bytes(data);wgsl=target.with_suffix('.wgsl')
  result=subprocess.run([str(r/'tools/naga-30.0.1/bin/naga.exe'),'--keep-coordinate-space',str(target),str(wgsl)],capture_output=True,text=True)
  target.with_suffix('.log').write_text(result.stdout+result.stderr)
  rows.append({'Hash':s['Hash'],'OriginalSPIRVSHA256':s['SPIRVSHA256'],'LoweredSPIRVSHA256':hashlib.sha256(data).hexdigest(),'Inputs':proof,'WGSLValidated':result.returncode==0,'Log':str(target.with_suffix('.log'))})
 (out/'array-input-proof.json').write_text(json.dumps(rows,indent=2))
 print(json.dumps({'Programs':len(rows),'WGSLValidated':sum(x['WGSLValidated'] for x in rows)}))
