"""Audit native DXIL CBV sizes and scalar reads before omitting translated padding."""
import re
def cbv_sizes(text):
    nodes=dict(re.findall(r'^!(\d+) = !\{(.*)\}$',text,re.M))
    root=re.search(r'^!dx\.resources = !\{!(\d+)\}',text,re.M)
    if not root:return {}
    groups=nodes[root[1]].split(', ')
    if len(groups)!=4:raise ValueError('Unknown DXIL resource table')
    if groups[2]=='null':return {}
    result={}
    for node in re.findall(r'!(\d+)',nodes[groups[2][1:]]):
        row=nodes[node]
        match=re.fullmatch(r'i32 (\d+), .+? undef, !"[^"]*", i32 (\d+), i32 (\d+), i32 (\d+), i32 (\d+), (?:null|!\d+)',row)
        if not match:raise ValueError('Unknown native CBV descriptor: '+row)
        index,space,binding,count,size=map(int,match.groups())
        if count!=1 or size<=0:raise ValueError('Unsupported native CBV size/count')
        result[(space,binding)]={'RangeID':index,'Bytes':size}
    return result

def verify_scalar_reads(text,range_id,size):
    if len(re.findall(r'^define ',text,re.M))!=1:raise ValueError('Multiple native functions need scoped scalar-use audit')
    handles=set(re.findall(r'(%\w+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 '+str(range_id)+r', i32 \d+, i1 (?:false|true)\)',text))
    if not handles:raise ValueError('Native CBV handle not audited')
    for handle in handles:
        for line in text.splitlines():
            if not re.search(re.escape(handle)+r'\b',line):continue
            if re.search(re.escape(handle)+r' = call %dx\.types\.Handle @dx\.op\.createHandle\(',line):continue
            if '@dx.op.cbufferLoad' not in line:raise ValueError('Native CBV handle alias/escape not audited')
    reads=[]
    for line in text.splitlines():
        if '@dx.op.cbufferLoad' not in line or not any(re.search(re.escape(h)+r'\b',line) for h in handles):continue
        match=re.search(r'(%\w+) = call %dx\.types\.CBufRet\.(f32|i32|f64|i64) @dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\w+), i32 (\d+)\)',line)
        if not match:raise ValueError('Dynamic/other CBV load cannot prove padding unread')
        value,kind,handle,register=match.groups();width=8 if kind.endswith('64') else 4
        for use in text.splitlines():
            if use==line or not re.search(re.escape(value)+r'\b',use):continue
            extract=re.search(r'extractvalue %dx\.types\.CBufRet\.\w+ '+re.escape(value)+r', (\d+)\s*$',use)
            if not extract:raise ValueError('Whole native CBV load escapes scalar audit')
            offset=int(register)*16+int(extract[1])*width
            if offset+width>size:raise ValueError('Original scalar read extends beyond CBV declaration')
            reads.append((offset,width))
    if not reads:raise ValueError('Native CBV scalar reads not identified')
    return sorted(set(reads))
