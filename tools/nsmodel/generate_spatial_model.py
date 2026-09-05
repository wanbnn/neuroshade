#!/usr/bin/env python3
"""Generate NeuroShade's tiny deterministic ONNX spatial model package.

This script is build tooling only. The installed model and runtime do not require Python.
"""

import argparse
import base64
import json
import pathlib
import struct


def varint(value: int) -> bytes:
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def field(number: int, wire: int, value: bytes) -> bytes:
    tag = varint((number << 3) | wire)
    return tag + (varint(len(value)) + value if wire == 2 else value)


def text(number: int, value: str) -> bytes:
    return field(number, 2, value.encode("utf-8"))


def integer(number: int, value: int) -> bytes:
    return field(number, 0, varint(value))


def message(number: int, value: bytes) -> bytes:
    return field(number, 2, value)


def value_info(name: str, dimensions: list[int]) -> bytes:
    shape = b"".join(message(1, integer(1, dimension)) for dimension in dimensions)
    tensor_type = integer(1, 1) + message(2, shape)  # ONNX FLOAT
    return text(1, name) + message(2, message(1, tensor_type))


def make_onnx() -> bytes:
    dimensions = [1, 4, 64, 64]
    node = text(1, "color") + text(1, "gain") + text(2, "output")
    node += text(3, "spatial_gain") + text(4, "Mul")
    gain = integer(1, 1) + integer(2, 1) + text(8, "gain")
    gain += field(9, 2, struct.pack("<f", 0.75))
    graph = message(1, node) + text(2, "neuroshade_spatial_gain")
    graph += message(5, gain)
    graph += message(11, value_info("color", dimensions))
    graph += message(12, value_info("output", dimensions))
    opset = integer(2, 13)
    return integer(1, 8) + text(2, "NeuroShade") + text(3, "0.1.0") + \
        message(7, graph) + message(8, opset)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schema": 1,
        "id": "org.neuroshade.bundled.spatial_gain",
        "name": "Spatial Gain Reference",
        "version": "1.0.0",
        "runtime": "migraphx",
        "inputs": [{
            "semantic": "Color.Final", "tensor": "color",
            "dtype": "fp32", "layout": "NCHW"
        }],
        "output": {
            "semantic": "Output.Color", "tensor": "output",
            "dtype": "fp32", "layout": "NCHW"
        },
        "history": 0,
        "scale": {"x": 1.0, "y": 1.0},
        "first_frame": "spatial",
        "shapes": {
            "kind": "fixed",
            "buckets": [{"input": [1, 4, 64, 64], "output": [1, 4, 64, 64]}]
        }
    }
    signature = {
        "schema": 1,
        "inputs": {"color": {"dtype": "fp32", "shape": [1, 4, 64, 64]}},
        "outputs": {"output": {"dtype": "fp32", "shape": [1, 4, 64, 64]}}
    }
    metadata = {
        "schema": 1,
        "license": "MIT",
        "description": "Deterministic spatial gain model used to qualify MIGraphX.",
        "generator": "NeuroShade build tooling",
        "expected_gain": 0.75
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "signature.json").write_text(json.dumps(signature, indent=2) + "\n")
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (args.output / "model.onnx").write_bytes(make_onnx())
    preview = base64.b64decode(
        "UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEALmk0mk0iIiIiIgBoSygABc6zbAAA")
    (args.output / "preview.webp").write_bytes(preview)


if __name__ == "__main__":
    main()
