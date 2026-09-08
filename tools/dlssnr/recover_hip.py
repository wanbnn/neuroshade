#!/usr/bin/env python3
"""Recover HIP code objects and ABI metadata locally; never execute/retarget them.
Requires llvm-readobj from the installed ROCm toolchain. Does not build a graph.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

MAGIC = b'__CLANG_OFFLOAD_BUNDLE__'

def bundles(data):
    start = data.find(MAGIC)
    if start < 0:
        raise ValueError('no Clang offload bundle found')
    def words(offset, count):
        if offset > len(data) or count*8 > len(data)-offset:
            raise ValueError('truncated bundle descriptor')
        return struct.unpack_from('<'+'Q'*count, data, offset)
    offset = start + len(MAGIC)
    count, = words(offset, 1)
    offset += 8
    if not 1 <= count <= 64:
        raise ValueError('invalid bundle count')
    result = []
    seen = set()
    for _ in range(count):
        rel, size, n = words(offset, 3)
        offset += 24
        if not 1 <= n <= 256 or n > len(data)-offset:
            raise ValueError('invalid target name')
        target = data[offset:offset+n].decode('ascii')
        offset += n
        if rel > len(data)-start or size > len(data)-start-rel:
            raise ValueError('bundle out of file bounds')
        if not size:
            continue  # Empty host placeholder, not a GPU image.
        match = re.fullmatch(r'hipv4-amdgcn-amd-amdhsa--(gfx[0-9a-f]+)', target)
        if not match:
            raise ValueError('unsupported target: '+target)
        architecture = match[1]
        if architecture in seen:
            raise ValueError('duplicate GPU image')
        seen.add(architecture)
        payload = data[start+rel:start+rel+size]
        if payload[:6] != b'\x7fELF\x02\x01' or len(payload)<64 or struct.unpack_from('<H',payload,18)[0]!=224:
            raise ValueError('expected little-endian ELF64 AMDGPU code object')
        result.append((architecture, start+rel, payload))
    if any(pos < offset for _, pos, _ in result):
        raise ValueError('bundle overlaps descriptor table')
    intervals = sorted((pos, pos+len(payload)) for _,pos,payload in result)
    if any(a[1]>b[0] for a,b in zip(intervals,intervals[1:])):
        raise ValueError('overlapping code objects')
    if not result:
        raise ValueError('no HIP GPU images')
    return result

def kernel_metadata(notes):
    result=[]
    for block in notes.split('  - .args:')[1:]:
        name = re.search(r'^    \.name:\s+(\S+)', block, re.M)
        if not name:
            raise ValueError('missing kernel name')
        args=[]
        # Pointer arguments also have .address_space / .access fields, and
        # llvm-readobj may print .name before .offset. Parse each argument map.
        arg_text = block.split('    .group_segment_fixed_size:')[0]
        for entry in re.split(r'\n      - ', '\n      - ' + arg_text.lstrip())[1:]:
            fields = dict(re.findall(r'\.(\w+):\s+([^\s]+)', entry))
            if not {'offset', 'size', 'value_kind'} <= fields.keys():
                raise ValueError('incomplete argument metadata')
            kind = fields['value_kind']
            if not kind.startswith('hidden_'):
                args.append(dict(offset=int(fields['offset']), size=int(fields['size']), kind=kind))
        if not args:
            raise ValueError('kernel argument metadata unavailable')
        item=dict(name=name[1],arguments=args)
        for key in ['kernarg_segment_size','group_segment_fixed_size','private_segment_fixed_size','wavefront_size','max_flat_workgroup_size']:
            value=re.search(r'^    \.'+key+r':\s+(\d+)',block,re.M)
            if not value:
                raise ValueError('missing '+key)
            item[key]=int(value[1])
        result.append(item)
    if not result:
        raise ValueError('no kernel metadata decoded')
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--llvm-readobj',default='/opt/rocm/llvm/bin/llvm-readobj')
    args=parser.parse_args()
    data=args.dll.read_bytes()
    images=bundles(data)
    args.output.mkdir(parents=True,exist_ok=False)
    report=dict(source_sha256=hashlib.sha256(data).hexdigest(),execution_qualified=False,images=[])
    for arch,offset,payload in images:
        image=args.output/(arch+'.hsaco')
        image.write_bytes(payload)
        notes=subprocess.run([args.llvm_readobj,'--notes',str(image)],check=True,capture_output=True,text=True,timeout=30).stdout
        (args.output/(arch+'-notes.txt')).write_text(notes)
        kernels=kernel_metadata(notes)
        report['images'].append(dict(architecture=arch,offset=offset,size=len(payload),sha256=hashlib.sha256(payload).hexdigest(),file=image.name,kernels=kernels))
    (args.output/'hip-inventory.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(images=[dict(architecture=i['architecture'],kernels=len(i['kernels'])) for i in report['images']],execution_qualified=False),indent=2))
if __name__=='__main__':
    main()
