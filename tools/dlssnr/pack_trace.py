#!/usr/bin/env python3
"""Convert a captured, fixed-extent graph to a relocatable native execution plan.
Only the recovered spatial RGBA8 ABI is supported; no address guessing at runtime.
"""
import argparse
import json
import struct
from pathlib import Path

POINTERS = {
 '_Z8k_import12ImportParams': (0,32),
 '_Z7k_ffwd211Ffwd2Params': (0,8,16,24),
 '_Z11k_conv_res211Conv2Params': (0,8,16,24,32,40),
 '_Z11k_qkv_attn210AttnParams': (0,8,16),
 '_Z16k_conv_res_views12ConvPlParams': (0,16,24,40,56),
 '_Z12k_final_head10HeadParams': (0,8,16),
 '_Z8k_repack12RepackParams': (0,8),
 '_Z9k_expand212ExpandParams': (0,8,16),
 '_Z11k_contract212ConvParams1d': (0,8,16,24),
 '_Z6k_qkv29QkvParams': (0,8,16,24,32),
 '_Z12k_attention212AttnParams1d': (0,8,16,24),
 '_Z14k_dec_upsample11DecUpParams': (0,8,16,24),
 '_Z8k_export12ExportParams': (0,32,48),
}
for width,flag in [(32,1),(32,0),(64,0),(128,0),(256,0)]:
 POINTERS[f'_Z10k_swin_varILi{width}ELb{flag}EEv9VarParams']=(0,8,16,48,56,64,112,120,152,160)

def pack(trace):
 rows=[json.loads(s) for s in trace.read_text().splitlines()]
 allocations=[r for r in rows if r['op']=='alloc']
 if len(allocations)>128:raise ValueError('too many buffers')
 def ref(address,size=1):
  for i,a in enumerate(allocations):
   if a['address']<=address and address+size<=a['address']+a['size']:
    return i,address-a['address']
  raise ValueError(f'unbound captured pointer {address:x}')
 frames=[i for i,r in enumerate(rows) if r['op']=='begin_frame']
 if len(frames)!=2 or rows[-1]['op']!='end_frame':raise ValueError('two complete frames required')
 frame=rows[frames[1]];width,height=frame['width'],frame['height']
 if not 1<=width<=3840 or not 1<=height<=2160:raise ValueError('unsupported extent')
 uploads=[r for r in rows if r['op']=='copy' and r['kind']==1]
 if len(uploads)!=1:raise ValueError('expected one packed weight upload')
 weight=ref(uploads[0]['dst'],uploads[0]['size'])
 inp,out=ref(frame['input'],width*height*4),ref(frame['output'],width*height*4)
 if weight[1] or inp[1] or out[1]:raise ValueError('expected allocation bases')
 operations=[]
 for r in rows[frames[1]+1:-1]:
  if r['op']=='copy' and r['kind']==3:
   dst,src=ref(r['dst'],r['size']),ref(r['src'],r['size'])
   operations.append(struct.pack('<IIQIQ',2,*dst,*src)+struct.pack('<Q',r['size']))
  elif r['op']=='launch':
   name=r['kernel'].encode('ascii');data=bytearray.fromhex(r['args']);fixups=[]
   for offset in POINTERS[r['kernel']]:
    address=struct.unpack_from('<Q',data,offset)[0]
    if address:
     index,byte_offset=ref(address)
     fixups.append(struct.pack('<IIQ',offset,index,byte_offset))
     struct.pack_into('<Q',data,offset,0)
   # Padding belongs to the captured ABI, never interpreted as a pointer.
   operations.append(struct.pack('<II',3,len(name))+name+struct.pack('<7I',*r['grid'],*r['block'],r['shared'])+
                     struct.pack('<I',len(data))+data+struct.pack('<I',len(fixups))+b''.join(fixups))
  else:raise ValueError('unsupported steady-frame operation: '+str(r))
 header=b'NSNRPLAN'+struct.pack('<8I',1,width,height,len(allocations),len(operations),inp[0],out[0],weight[0])
 result=header+b''.join(struct.pack('<Q',a['size'])for a in allocations)+b''.join(operations)
 return result,dict(width=width,height=height,buffers=len(allocations),operations=len(operations),weight_bytes=uploads[0]['size'])

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('trace',type=Path);parser.add_argument('output',type=Path)
 args=parser.parse_args();data,info=pack(args.trace);args.output.write_bytes(data);print(json.dumps(info,indent=2))
