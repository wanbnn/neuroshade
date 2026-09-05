#!/usr/bin/env python3
"""Generate the bundled ``temporal_sr_2x.nsmodel`` package.

The reference 2x temporal-SR model combines three inputs:

    output = Resize(0.5 * Color.LowRes + 0.1 * Motion.Screen) + History.Output.Color

The two elementwise scales are baked in as initializers; the resize is the
nearest-neighbour op that takes 32×32 → 64×64. The graph is hand-rolled so
CMake does not depend on an external ``onnx`` or ``torch`` to produce it.

The model is build tooling only. The runtime never imports Python.
"""

from __future__ import annotations

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


def string_attribute(name: str, value: str) -> bytes:
    # ONNX AttributeProto: name=1, s=4, type=20 (STRING=3).
    return text(1, name) + text(4, value) + integer(20, 3)


def node(
    op_type: str,
    inputs: list[str],
    outputs: list[str],
    node_name: str = "",
    attributes: list[bytes] | None = None,
) -> bytes:
    body = b""
    for inp in inputs:
        body += text(1, inp)
    for outp in outputs:
        body += text(2, outp)
    if node_name:
        body += text(3, node_name)
    body += text(4, op_type)
    for attribute in attributes or []:
        body += message(5, attribute)
    return message(1, body)


def initializer(name: str, value: float) -> bytes:
    payload = struct.pack("<f", value)
    return (
        integer(1, 1)
        + integer(2, 1)
        + text(8, name)
        + field(9, 2, payload)
    )


def vector_initializer(name: str, values: list[float]) -> bytes:
    payload = struct.pack(f"<{len(values)}f", *values)
    return (
        integer(1, len(values))
        + integer(2, 1)
        + text(8, name)
        + field(9, 2, payload)
    )


def make_onnx() -> bytes:
    """Build the 3-input temporal-SR graph with real 2x upsampling.

    Layout:
        inp_low_res  fp32 (1,4,32,32)   Color.LowRes
        inp_motion   fp32 (1,4,32,32)   Motion.Screen
        inp_history  fp32 (1,4,64,64)   History.Output.Color
        constants    gain_low=0.5  gain_motion=0.1
        output       fp32 (1,4,64,64)   Output.Color
    """

    shape = [1, 4, 32, 32]
    output_shape = [1, 4, 64, 64]

    mul_low = node("Mul", inputs=["inp_low_res", "gain_low"], outputs=["scaled_low"])
    mul_motion = node("Mul", inputs=["inp_motion", "gain_motion"], outputs=["scaled_motion"])
    add_low_motion = node(
        "Add",
        inputs=["scaled_low", "scaled_motion"],
        outputs=["partial"],
    )
    resize = node(
        "Resize",
        inputs=["partial", "", "scales_2x"],
        outputs=["upscaled"],
        attributes=[
            string_attribute("coordinate_transformation_mode", "asymmetric"),
            string_attribute("mode", "nearest"),
            string_attribute("nearest_mode", "floor"),
        ],
    )
    add_history = node(
        "Add",
        inputs=["upscaled", "inp_history"],
        outputs=["output"],
    )

    graph = b""
    graph += mul_low
    graph += mul_motion
    graph += add_low_motion
    graph += resize
    graph += add_history

    graph += message(5, initializer("gain_low", 0.5))
    graph += message(5, initializer("gain_motion", 0.1))
    graph += message(5, vector_initializer("scales_2x", [1.0, 1.0, 2.0, 2.0]))

    graph += message(11, value_info("inp_low_res", shape))
    graph += message(11, value_info("inp_motion", shape))
    graph += message(11, value_info("inp_history", output_shape))
    graph += message(12, value_info("output", output_shape))

    opset = integer(2, 13)
    model = integer(1, 13)
    model += text(2, "NeuroShade")
    model += text(3, "0.1.0")
    model += message(7, graph)
    model += message(8, opset)
    return model


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schema": 1,
        "id": "org.neuroshade.bundled.temporal_sr_2x",
        "name": "Temporal Super-Resolution Reference",
        "version": "1.0.0",
        "runtime": "migraphx",
        "inputs": [
            {
                "semantic": "Color.LowRes",
                "tensor": "inp_low_res",
                "dtype": "fp32",
                "layout": "NCHW",
            },
            {
                "semantic": "Motion.Screen",
                "tensor": "inp_motion",
                "dtype": "fp32",
                "layout": "NCHW",
            },
            {
                "semantic": "History.Output.Color",
                "tensor": "inp_history",
                "dtype": "fp32",
                "layout": "NCHW",
            },
        ],
        "output": {
            "semantic": "Output.Color",
            "tensor": "output",
            "dtype": "fp32",
            "layout": "NCHW",
        },
        "history": 1,
        "scale": {"x": 2.0, "y": 2.0},
        "first_frame": "spatial",
        "missing_motion": "spatial",
        "shapes": {
            "kind": "fixed",
            "buckets": [
                {"input": [1, 4, 32, 32], "output": [1, 4, 64, 64]},
            ],
        },
    }
    signature = {
        "schema": 1,
        "inputs": {
            "inp_low_res":  {"dtype": "fp32", "shape": [1, 4, 32, 32]},
            "inp_motion":   {"dtype": "fp32", "shape": [1, 4, 32, 32]},
            "inp_history":  {"dtype": "fp32", "shape": [1, 4, 64, 64]},
        },
        "outputs": {
            "output": {"dtype": "fp32", "shape": [1, 4, 64, 64]},
        },
    }
    metadata = {
        "schema": 1,
        "license": "MIT",
        "description": (
            "Deterministic 2x temporal super-resolution. Combines "
            "Color.LowRes + Motion.Screen + History.Output.Color with constant "
            "gains, then Resizes to 2x via nearest-neighbour. Used to qualify "
            "the motion + low-res scene path."
        ),
        "generator": "NeuroShade build tooling",
        "history_depth": 1,
        "scale_x": 2.0,
        "scale_y": 2.0,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "signature.json").write_text(json.dumps(signature, indent=2) + "\n")
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (args.output / "model.onnx").write_bytes(make_onnx())
    preview = base64.b64decode(
        "UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEALmk0mk0iIiIiIgBoSygABc6zbAAA"
    )
    (args.output / "preview.webp").write_bytes(preview)


if __name__ == "__main__":
    main()
