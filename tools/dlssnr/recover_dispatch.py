#!/usr/bin/env python3
"""Map HIP registrations to host call sites; output is NOT an executable graph.
All recovered edges are static: loops/branches and argument values still need
reconstruction. Inputs are an objdump -d -M intel listing and its exact PE file.
"""
import argparse,bisect,hashlib,json,re,struct
from pathlib import Path

def recover(data,assembly):
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ValueError('expected PE')
    opt=pe+24;base=struct.unpack_from('<Q',data,opt+24)[0]
    sections={}
    for i in range(struct.unpack_from('<H',data,pe+6)[0]):
        o=opt+struct.unpack_from('<H',data,pe+20)[0]+40*i
        name=data[o:o+8].rstrip(b'\0').decode()
        virtual,rva,size,raw=struct.unpack_from('<IIII',data,o+8)
        sections[name]=(rva,raw,size,virtual)
    def offset(va):
        for rva,raw,size,_ in sections.values():
            if rva<=va-base<rva+size:return raw+va-base-rva
        raise ValueError('VA outside file')
    def string(va):
        try:
            o=offset(va);end=data.index(b'\0',o)
            return data[o:end].decode('ascii') if end-o<256 else ''
        except (ValueError,UnicodeDecodeError):return ''
    _,o,_,size=sections['.pdata']
    functions=sorted((base+s,base+e) for s,e,_ in struct.iter_unpack('<III',data[o:o+size]))
    starts=[s for s,_ in functions]
    def function(va):
        i=bisect.bisect_right(starts,va)-1
        return functions[i][0] if i>=0 and va<functions[i][1] else None
    rows=[]
    for line in assembly.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+\s*(.*)',line)
        if m:rows.append((int(m[1],16),m[2],line))
    registrations={};rdx=None
    for addr,ins,_ in rows:
        m=re.search(r'lea\s+rdx,.*# 0x([0-9a-f]+)',ins)
        if m:rdx=(addr,int(m[1],16))
        m=re.search(r'lea\s+r8,.*# 0x([0-9a-f]+)',ins)
        if m and rdx and addr-rdx[0]<32:
            name=string(int(m[1],16))
            if name.startswith('_Z') and 'k_' in name:registrations[rdx[1]]=name
    stubs={};launch_sites=[];candidates={}
    for addr,ins,_ in rows:
        m=re.search(r'lea\s+rcx,.*# 0x([0-9a-f]+)',ins)
        if m and int(m[1],16) in registrations:
            fn=function(addr)
            if fn:
                name=registrations[int(m[1],16)]
                launch_sites.append(dict(caller=hex(fn-base),site=hex(addr-base),kernel=name))
                candidates.setdefault(fn,set()).add(name)
    for start,end in functions:
        names=candidates.get(start,set())
        if end-start<=512 and len(names)==1:
            stubs[start]=next(iter(names))
    edges=[]
    for addr,ins,_ in rows:
        m=re.fullmatch(r'call\s+0x([0-9a-f]+)',ins)
        if m:
            src=function(addr);dst=int(m[1],16)
            if src:edges.append(dict(caller=hex(src-base),site=hex(addr-base),callee=hex(dst-base),kernel=stubs.get(dst)))
    return dict(source_sha256=hashlib.sha256(data).hexdigest(),assembly_sha256=hashlib.sha256(assembly.encode()).hexdigest(),image_base=hex(base),executable_graph=False,registrations=[dict(marker=hex(k-base),name=v) for k,v in registrations.items()],stubs=[dict(rva=hex(k-base),name=v) for k,v in stubs.items()],kernel_launch_sites=launch_sites,calls=edges)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('dll',type=Path);p.add_argument('assembly',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    report=recover(a.dll.read_bytes(),a.assembly.read_text())
    a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(registrations=len(report['registrations']),stubs=len(report['stubs']),kernel_launch_sites=len(report['kernel_launch_sites']),kernel_call_sites=sum(e['kernel'] is not None for e in report['calls']),executable_graph=False)))
if __name__=='__main__':main()
