#!/usr/bin/env python3
"""Compare native NR controls with independently captured companion graphs.

Qualification tool only; the game runtime is C++/HIP. Run exclusively, with the
AMD companion captures produced by trace/capture_main.cpp and pack_trace.py.
"""
import argparse
import ctypes as C
import fcntl
import hashlib
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--model', required=True, type=Path)
    parser.add_argument('--mask-on-plan', required=True, type=Path)
    parser.add_argument('--mask-off-plan', required=True, type=Path)
    args = parser.parse_args()
    with open(f'/run/user/{os.getuid()}/neuroshade-gpu-qualification-{os.getuid()}.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lib = C.CDLL(str(args.library.resolve()))
        lib.ns_nr_create.argtypes = [C.c_char_p] * 4
        lib.ns_nr_create.restype = C.c_void_p
        lib.ns_nr_run.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_int]
        lib.ns_nr_controls_v2.argtypes = [C.c_void_p, C.c_float, C.c_float, C.c_float, C.c_uint]
        lib.ns_nr_destroy.argtypes = [C.c_void_p]
        lib.ns_nr_error.restype = C.c_char_p
        size = 1920 * 1080 * 4
        source = bytes(range(256)) * (size // 256)

        def check(ok):
            if not ok:
                raise RuntimeError(lib.ns_nr_error().decode())

        def execute(plan, controls=None):
            handle = lib.ns_nr_create(*[str(p.resolve()).encode() for p in
                [args.model / 'kernels.hsaco', plan, args.model / 'weights.bin', args.model / 'lookup.bin']])
            check(handle)
            try:
                if controls is not None:
                    check(lib.ns_nr_controls_v2(handle, *controls) == 0)
                    for invalid in [(float('nan'), 2, -1, 1), (1.54, 2.01, -1, 1), (1.54, 2, -1, 2)]:
                        if lib.ns_nr_controls_v2(handle, *invalid) == 0:
                            raise RuntimeError('accepted invalid control')
                output = C.create_string_buffer(size)
                frames = []
                for _ in range(3):
                    check(lib.ns_nr_run(handle, source, output, size, 1) == 0)
                    frames.append(output.raw)
                return frames
            finally:
                lib.ns_nr_destroy(handle)

        outputs = {}
        for mask, plan in [(0, args.mask_off_plan), (1, args.mask_on_plan)]:
            reference = execute(plan)
            actual = execute(args.model / 'graph.bin', (1.54, 2, -1, mask))
            if actual != reference:
                raise RuntimeError(f'mask={mask}: pixels differ from captured graph')
            outputs[mask] = actual
        if outputs[0] == outputs[1]:
            raise RuntimeError('automatic mask switch did not change pixels')
        inherited = execute(args.model / 'graph.bin', (1.54, 2, 2, 1))
        if inherited != outputs[1]:
            raise RuntimeError('negative skin did not inherit local structure')
        print(json.dumps(dict(capture_reference_bitwise_equal=True, temporal_frames=3,
            mask_changes_pixels=True, negative_skin_inherits_structure=True,
            invalid_controls_preserve_configuration=True,
            output_sha256={str(mask): [hashlib.sha256(frame).hexdigest() for frame in frames]
                           for mask, frames in outputs.items()}), indent=2))


if __name__ == '__main__':
    main()
