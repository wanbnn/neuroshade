#!/usr/bin/env python3
"""Verify instruction encodings before creating an experimental gfx1200 object.
Does not execute code. Qualification of numerical output remains a separate gate.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path


def prepare(source, output, llvm):
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError('unrecognized gfx1201 image; fixed revision only')
    if struct.unpack_from('<I', data, 48)[0] != 0x4e:
        raise ValueError('unexpected AMDGPU machine flags')
    decodes = []
    binaries = []
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for arch in ('gfx1200', 'gfx1201'):
            text = subprocess.check_output([str(llvm/'llvm-objdump'), '-d', '--mcpu='+arch, str(source)], text=True)
            if '<unknown>' in text:
                raise ValueError('unknown instruction')
            instructions = [line.split('//')[0].strip() for line in text.splitlines() if re.search(r'// [0-9A-F]+: ', line)]
            if not instructions:
                raise ValueError('no instructions decoded')
            decodes.append(instructions)
            assembly = root/(arch+'.s')
            assembly.write_text('.text\n'+'\n'.join(instructions)+'\n')
            obj = root/(arch+'.o')
            raw = root/(arch+'.text')
            subprocess.run([str(llvm/'llvm-mc'), '-triple=amdgcn-amd-amdhsa', '-mcpu='+arch, '-filetype=obj', str(assembly), '-o', str(obj)], check=True)
            subprocess.run([str(llvm/'llvm-objcopy'), '--dump-section', '.text='+str(raw), str(obj)], check=True)
            binaries.append(raw.read_bytes())
    if decodes[0] != decodes[1] or binaries[0] != binaries[1]:
        raise ValueError('RDNA4 target instruction encodings differ')
    result = bytearray(data.replace(b'gfx1201', b'gfx1200'))
    struct.pack_into('<I', result, 48, 0x48)
    # No executable instruction bytes are modified.
    output.write_bytes(result)
    report = dict(source_sha256=EXPECTED, output_sha256=hashlib.sha256(result).hexdigest(),
                  instructions=len(decodes[0]), identical_dual_target_assembly=True,
                  gpu_execution_qualified=False, numerical_equivalence_qualified=False)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


EXPECTED = '3163ee5edc8d201d804acdd16c63dbdcda102f21aeb1d5ad3e5e93ea3174f797'
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--llvm', type=Path, default=Path('/opt/rocm/llvm/bin'))
    args = parser.parse_args()
    print(json.dumps(prepare(args.source, args.output, args.llvm), indent=2))
