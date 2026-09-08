#!/usr/bin/env python3
"""Exclusive GPU qualification of NR style, preset resolution and intensity.

Style plans are captured from the pinned companion with the pre-dispatch style
constant changed to the NGX value (style / 128). This is not an RTX image oracle.
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
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--style1-plan', type=Path, required=True)
    parser.add_argument('--style2-plan', type=Path, required=True)
    args = parser.parse_args()
    with open(f'/run/user/{os.getuid()}/neuroshade-gpu-qualification-{os.getuid()}.lock', 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lib = C.CDLL(str(args.library.resolve()))
        lib.ns_nr_create.argtypes = [C.c_char_p] * 4
        lib.ns_nr_create.restype = C.c_void_p
        lib.ns_nr_run.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_int]
        lib.ns_nr_controls_v3.argtypes = [C.c_void_p, C.c_float, C.c_float, C.c_float, C.c_uint, C.c_uint, C.c_uint, C.c_float]
        lib.ns_nr_destroy.argtypes = [C.c_void_p]
        lib.ns_nr_error.restype = C.c_char_p
        size = 1920 * 1080 * 4
        source = bytes(range(256)) * (size // 256)
        def check(ok):
            if not ok:
                raise RuntimeError(lib.ns_nr_error().decode())
        def execute(plan, style=None, preset=0, intensity=1.):
            handle = lib.ns_nr_create(*[str(p.resolve()).encode() for p in
                [args.model / 'kernels.hsaco', plan, args.model / 'weights.bin', args.model / 'lookup.bin']])
            check(handle)
            try:
                if style is not None:
                    check(lib.ns_nr_controls_v3(handle, 0, 1, 1, 1, style, preset, intensity) == 0)
                    for bad_style, bad_preset, bad_intensity in [(3, 0, 1), (0, 4, 1), (0, 0, float('nan')), (0, 0, 2.01)]:
                        if lib.ns_nr_controls_v3(handle, 0, 1, 1, 1, bad_style, bad_preset, bad_intensity) == 0:
                            raise RuntimeError('invalid NR mode accepted')
                output = C.create_string_buffer(size)
                frames = []
                for _ in range(3):
                    check(lib.ns_nr_run(handle, source, output, size, 0) == 0)
                    frames.append(output.raw)
                if style == 1 and preset == 0 and intensity == 1.:
                    check(lib.ns_nr_controls_v3(handle, 0, 1, 1, 1, 2, 3, 2.) == 0)
                    check(lib.ns_nr_run(handle, source, output, size, 0) == 0)
                    if output.raw == frames[0]:
                        raise RuntimeError('in-place mode update did not change pixels')
                    check(lib.ns_nr_controls_v3(handle, 0, 1, 1, 1, style, preset, intensity) == 0)
                    restored = []
                    for _ in range(3):
                        check(lib.ns_nr_run(handle, source, output, size, 0) == 0)
                        restored.append(output.raw)
                    if restored != frames:
                        raise RuntimeError('in-place update did not reset/restore history')
                return frames
            finally:
                lib.ns_nr_destroy(handle)
        baseline = execute(args.model / 'graph.bin')
        styles = {0: baseline}
        for style, plan in [(1, args.style1_plan), (2, args.style2_plan)]:
            reference = execute(plan)
            actual = execute(args.model / 'graph.bin', style)
            if actual != reference:
                raise RuntimeError(f'style={style}: differs from companion dispatch reference')
            styles[style] = actual
        if len({hashlib.sha256(frames[0]).digest() for frames in styles.values()}) != 3:
            raise RuntimeError('styles did not produce three distinct images')
        for preset in (1, 2, 3):
            if execute(args.model / 'graph.bin', 1, preset) != styles[1]:
                raise RuntimeError('preset did not resolve to the shipping weights')
        intensities = {1.: baseline}
        for intensity in (0., .5, 1.38, 2.):
            intensities[intensity] = execute(args.model / 'graph.bin', 0, 0, intensity)
        if any(frame != source for frame in intensities[0.]):
            raise RuntimeError('intensity zero did not preserve input pixels')
        if len({hashlib.sha256(frames[0]).digest() for frames in intensities.values()}) != 5:
            raise RuntimeError('intensity levels did not change images')
        # Directly qualify the new FP32 residual kernel, before any UNORM clamp.
        hip = C.CDLL('/opt/rocm/lib/libamdhip64.so')
        hip.hipMalloc.argtypes = [C.POINTER(C.c_void_p), C.c_size_t]
        hip.hipFree.argtypes = [C.c_void_p]
        hip.hipMemcpy.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_int]
        lib.ns_nr_intensify.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.c_float, C.c_void_p]
        a = (C.c_float * 6)(.25, .5, .75, 0, 1, .5)
        b = (C.c_float * 8)(.5, .25, 1.25, 1, -.5, 2, .25, 0)
        output = (C.c_float * 8)()
        buffers = []
        try:
            for data in (a, b, output):
                pointer = C.c_void_p()
                assert hip.hipMalloc(C.byref(pointer), C.sizeof(data)) == 0
                buffers.append(pointer)
                assert hip.hipMemcpy(pointer, data, C.sizeof(data), 1) == 0
            for intensity in (0., .5, 1., 1.38, 2.):
                assert lib.ns_nr_intensify(*buffers, 2, intensity, None) == 0
                assert hip.hipMemcpy(output, buffers[2], C.sizeof(output), 2) == 0
                for i in range(8):
                    j = (i // 4) * 3 + i % 4
                    expected = 1. if i % 4 == 3 else a[j] + C.c_float(intensity).value * (b[i] - a[j])
                    assert abs(output[i] - expected) < 1e-6, (intensity, i, output[i], expected)
        finally:
            for pointer in buffers:
                hip.hipFree(pointer)
        print(json.dumps(dict(styles_reference_bitwise_equal=True, distinct_styles=3,
            temporal_frames=3, in_place_update_and_restore=True, presets_resolve_to=1, intensity_zero_identity=True,
            intensity_levels=list(intensities), intensity_fp32_residual_kernel=True,
            rtx_equivalence_tested=False,
            style_sha256={str(k): hashlib.sha256(v[0]).hexdigest() for k, v in styles.items()},
            intensity_sha256={str(k): hashlib.sha256(v[0]).hexdigest() for k, v in intensities.items()}), indent=2))


if __name__ == '__main__':
    main()
