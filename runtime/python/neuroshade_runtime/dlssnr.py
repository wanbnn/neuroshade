"""Host protocol adapter for NeuroShade's native HIP graph runtime."""
from __future__ import annotations
import ctypes
import json
import os
import struct
from pathlib import Path

class DlssnrEngine:
    def __init__(self, path: Path, manifest: dict, config: dict):
        if config.get('device', 'cuda:0') not in ('cuda:0', 'hip:0'):
            raise ValueError('DLSSNR native requires the qualified gfx1200 GPU at device 0')
        if manifest['scale'] != {'x': 1, 'y': 1}:
            raise ValueError('DLSSNR native is a 1:1 spatial renderer')
        header = (path/'graph.bin').read_bytes()[:40]
        if len(header) != 40 or header[:8] != b'NSNRPLAN':
            raise ValueError('invalid native graph header')
        version, self.width, self.height, *_ = struct.unpack('<8I', header[8:])
        if version != 1 or not 1 <= self.width <= 3840 or not 1 <= self.height <= 2160:
            raise ValueError('unsupported native graph extent')
        # The library is installed with NeuroShade, never loaded from a model package.
        library = Path(os.environ.get('NEUROSHADE_DLSSNR_LIBRARY',
                       str(Path.home()/'.local/share/neuroshade/runtime/lib/libneuroshade_dlssnr.so')))
        self.lib = ctypes.CDLL(str(library))
        self.lib.ns_nr_create.argtypes = [ctypes.c_char_p]*4
        self.lib.ns_nr_create.restype = ctypes.c_void_p
        self.lib.ns_nr_error.restype = ctypes.c_char_p
        self.lib.ns_nr_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        self.lib.ns_nr_run.restype = ctypes.c_int
        self.lib.ns_nr_reset.argtypes = [ctypes.c_void_p]
        self.lib.ns_nr_reset.restype = ctypes.c_int
        self.lib.ns_nr_destroy.argtypes = [ctypes.c_void_p]
        self.lib.ns_nr_destroy.restype = None
        self.handle = self.lib.ns_nr_create(*(os.fsencode(path/name) for name in ('kernels.hsaco','graph.bin','weights.bin','lookup.bin')))
        if not self.handle:
            raise RuntimeError(self.lib.ns_nr_error().decode())
        metadata=json.loads((path/'metadata.json').read_text())
        plan=(path/'graph.bin').read_bytes();count=struct.unpack_from('<I',plan,20)[0]
        resident_bytes=sum(struct.unpack_from('<'+str(count)+'Q',plan,40))
        self.info = dict(model_version=manifest.get('version'),resident_bytes=resident_bytes,
                         controls=metadata.get('dlssnr_controls',{}),backend='NeuroShade/HIP native', architecture='DLSSNR spatial FP8',
                         device='gfx1200', scale=1, precision='FP8 weights / FP16 activations',
                         inference_extent=[self.width,self.height], fixed_extent=True,
                         external_motion_depth=False)

    def process_rgba(self, raw: bytes, width: int, height: int, bgra: bool) -> bytes:
        if (width,height) != (self.width,self.height):
            raise ValueError(f'native model requires {self.width}x{self.height}; received {width}x{height}')
        result = ctypes.create_string_buffer(len(raw))
        if self.lib.ns_nr_run(self.handle,raw,result,len(raw),int(bgra)):
            raise RuntimeError(self.lib.ns_nr_error().decode())
        return result.raw

    def reset(self):
        if self.lib.ns_nr_reset(self.handle):
            raise RuntimeError(self.lib.ns_nr_error().decode())

    def close(self):
        if getattr(self,'handle',None):
            self.lib.ns_nr_destroy(self.handle)
            self.handle = None

    def __del__(self):
        self.close()
