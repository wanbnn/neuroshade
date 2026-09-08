#!/usr/bin/env python3
"""Package locally recovered DLSSNR artifacts; never download or publish weights."""
import argparse
import hashlib
import json
import shutil
import tempfile
from pathlib import Path
from pack_trace import pack

HASHES = {
 'nr-subject.dll':'d1f10165de2a328ac2962002c2775af0d8ad9c10030d60197ff53fd4783cff4c',
 'weights.bin':'6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab',
 'lookup.bin':'7cb230f4a456ffdd669842a257b4ffdb30f3ffa9179f0ecca10a831a3a907a84',
}
CODE_HASH='371f37ec3cbd0f521cdab38b341ca0871a9ae700cce208255517f3b4c5eb501f'

def package(trace, code, output):
 if output.exists() or output.suffix!='.nsmodel':raise ValueError('destination must be a new .nsmodel directory')
 for name,digest in HASHES.items():
  if hashlib.sha256((trace/name).read_bytes()).hexdigest()!=digest:raise ValueError('unrecognized artifact: '+name)
 if hashlib.sha256(code.read_bytes()).hexdigest()!=CODE_HASH:raise ValueError('code object has not passed the pinned RDNA4 conversion')
 rows=[json.loads(line) for line in (trace/'capture.jsonl').read_text().splitlines()]
 uploads=[r for r in rows if r['op']=='copy' and r['kind']==1]
 if len(uploads)!=1:raise ValueError('expected one GPU weight upload')
 uploaded=trace/f'upload_{uploads[0]["dst"]}.bin'
 if hashlib.sha256(uploaded.read_bytes()).hexdigest()!='c384f5938e792de8f95d1593382feb2b40cfbafeebe64ff5e4db13ce8cfcca9d':
  raise ValueError('weights were not reorganized with the default companion layout')
 import struct
 pre=[bytes.fromhex(r['args']) for r in rows if r['op']=='launch' and r['kernel']=='_Z10k_swin_varILi32ELb1EEv9VarParams' and struct.unpack_from('<I',bytes.fromhex(r['args']),40)[0]==20]
 if len(pre)!=2 or any(struct.unpack_from('<4f',args,88)!=(1.0,0.0,1.0,1.0) for args in pre):
  raise ValueError('the default model requires the companion visual controls, not a zero-control capture')
 graph,info=pack(trace/'capture.jsonl');w,h=info['width'],info['height']
 if uploaded.stat().st_size!=info['weight_bytes']:raise ValueError('GPU weight allocation mismatch')
 output.parent.mkdir(parents=True,exist_ok=True)
 staging=Path(tempfile.mkdtemp(prefix='.dlssnr-',dir=output.parent))
 try:
  (staging/'graph.bin').write_bytes(graph)
  shutil.copyfile(uploaded,staging/'weights.bin')
  shutil.copyfile(trace/'lookup.bin',staging/'lookup.bin')
  shutil.copyfile(code,staging/'kernels.hsaco')
  manifest=dict(schema=1,id=f'org.neuroshade.dlssnr.spatial.{w}x{h}',name=f'DLSSNR Native {w}x{h}',version='0.1.2',runtime='dlssnr_hip',
   inputs=[dict(semantic='Color.Final',tensor='color',dtype='fp32',layout='NCHW')],
   output=dict(semantic='Output.Color',tensor='output',dtype='fp32',layout='NCHW'),history=0,
   scale=dict(x=1,y=1),first_frame='spatial',shapes=dict(kind='fixed',buckets=[dict(input=[1,4,h,w],output=[1,4,h,w])]))
  metadata=dict(schema=1,dlssnr_controls=dict(local_tone=0,local_structure=1,skin_structure=1),architecture='DLSSNR spatial FP8',source_sha256=HASHES['nr-subject.dll'],
   weights_source_sha256='dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f',
   sha256={name:hashlib.sha256((staging/name).read_bytes()).hexdigest() for name in ['graph.bin','weights.bin','lookup.bin','kernels.hsaco']},
   qualification=dict(gpu='gfx1200',external_motion_depth=False,fixed_extent=[w,h],mod_performance_parity_verified=False))
  for name,value in [('manifest.json',manifest),('metadata.json',metadata),('signature.json',dict(schema=1,kind='local-recovery'))]:
   (staging/name).write_text(json.dumps(value,indent=2)+'\n')
  from PIL import Image
  Image.new('RGB',(32,32),(26,43,57)).save(staging/'preview.webp')
  staging.rename(output)
 except BaseException:
  shutil.rmtree(staging,ignore_errors=True);raise
 return info
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('trace',type=Path);p.add_argument('code',type=Path);p.add_argument('output',type=Path)
 a=p.parse_args();print(json.dumps(package(a.trace,a.code,a.output),indent=2))
