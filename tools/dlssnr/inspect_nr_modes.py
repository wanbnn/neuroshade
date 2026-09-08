#!/usr/bin/env python3
"""Read mode metadata from the exact locally qualified NGX NR binary (no execution)."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(path):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != 'dcc0dc2414aedec4a8e084647070383be068554042587180c20c784d4772d36f':
        raise ValueError('unqualified DLL revision; offsets must be re-established')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    optional = pe + 24
    base = struct.unpack_from('<Q', data, optional + 24)[0]
    sections = optional + struct.unpack_from('<H', data, pe + 20)[0]
    def offset(va):
        for i in range(struct.unpack_from('<H', data, pe + 6)[0]):
            _, rva, size, raw = struct.unpack_from('<IIII', data, sections + i * 40 + 8)
            if rva <= va - base < rva + size:
                return raw + va - base - rva
        raise ValueError('VA outside file')
    table = offset(0x1800b0d80)
    name_va, preset = struct.unpack_from('<QI', data, table)
    start = offset(name_va)
    name = data[start:data.index(b'\0', start)].decode('ascii')
    style_count = struct.unpack_from('<I', data, table + 0x24)[0]
    scale = struct.unpack_from('<f', data, offset(0x1800b0d6c))[0]
    return dict(sha256=digest, weight_name=name, weight_presets=[preset],
        requested_to_effective_preset={str(i): preset for i in range(4)},
        style_count=style_count, style_conditioning_scale=scale,
        style_labels=['Default', 'Natural', 'Cinematic'],
        evidence=dict(style_count='descriptor+0x24 -> 180021235 -> record+0x64',
            style_value='1800224e7..180022505: unsigned clamp, float conversion, scale',
            preset_fallback='180023a40..180023aba: lookup, fallback to preset 1'),
        intensity_formula_proven_from_this_inspection=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dll', type=Path)
    print(json.dumps(inspect(parser.parse_args().dll), indent=2))
