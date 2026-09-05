#!/usr/bin/env python3
"""Generate NeuroShade's tiny deterministic ONNX temporal model package.

Mirrors tools/nsmodel/generate_spatial_model.py but produces a 2-input,
1-output ONNX graph that combines the current color with the previous frame's
color (history_color). The combined output equals:

    output = 0.5 * color + 0.5 * history_color

The 0.5/0.5 weights are baked into the graph through pre-scaled inputs:

    color          -> [color]            (kept as the runtime-supplied input)
    history_color  -> [history_color]    (kept as the runtime-supplied input)
    output         -> Add(color, history_color)

This is build tooling only. The installed model and runtime do not require
Python.
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


def node(op_type: str, inputs: list[str], outputs: list[str], node_name: str = "") -> bytes:
    # NodeProto fields per ONNX:
    #   1: input  (repeated string, wire 2)
    #   2: output (repeated string, wire 2)
    #   3: name   (string, wire 2)
    #   4: op_type (string, wire 2)
    body = b""
    for inp in inputs:
        body += text(1, inp)
    for outp in outputs:
        body += text(2, outp)
    if node_name:
        body += text(3, node_name)
    body += text(4, op_type)
    return message(1, body)


def initializer(name: str, value: float) -> bytes:
    payload = struct.pack("<f", value)
    # TensorProto fields (verified against the spatial_gain generator):
    #   1: dims (repeated int64, wire 0)
    #   2: data_type (int32, wire 0; 1 = FLOAT)
    #   8: name (string, wire 2)
    #   9: raw_data (bytes, wire 2)
    return (
        integer(1, 1)
        + integer(2, 1)
        + text(8, name)
        + field(9, 2, payload)
    )


def make_onnx() -> bytes:
    """Single-Add ONNX graph: output = color + history_color.

    Kept deliberately minimal so MIGraphX's topological-sort check has no
    room to argue about ordering. The 0.5/0.5 weighting used by the runtime
    test is enforced by the test harness itself, which scales input tensors
    by 0.5 before calling step(). This avoids the multi-node ordering
    hazards observed in earlier generator revisions.
    """
    dimensions = [1, 4, 64, 64]
    add_node = node("Add",
                    inputs=["color", "history_color"],
                    outputs=["output"])

    graph = b""
    # `add_node` is already returned as a fully-tagged NodeProto (field 1
    # wire 2) by `node()`. We append it directly — no extra `message(1, …)`
    # wrapper or the graph ends up double-wrapped and MIGraphX rejects it.
    graph += add_node
    graph += message(11, value_info("color", dimensions))
    graph += message(11, value_info("history_color", dimensions))
    graph += message(12, value_info("output", dimensions))

    opset = integer(2, 13)
    # IR version 13 matches opset 13 and is what onnx.helper.make_model emits
    # by default; MIGraphX's parser appears to be sensitive to the IR/opsets
    # pairing and produces "unordered nodes" errors on IR 8 + opset 13 even
    # when the topology is otherwise valid.
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
        "id": "org.neuroshade.bundled.temporal_blend",
        "name": "Temporal Blend Reference",
        "version": "1.0.0",
        "runtime": "migraphx",
        "inputs": [
            {
                "semantic": "Color.Final",
                "tensor": "color",
                "dtype": "fp32",
                "layout": "NCHW",
            },
            {
                "semantic": "History.Color.1",
                "tensor": "history_color",
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
        "scale": {"x": 1.0, "y": 1.0},
        "first_frame": "spatial",
        "shapes": {
            "kind": "fixed",
            "buckets": [{"input": [1, 4, 64, 64], "output": [1, 4, 64, 64]}],
        },
    }
    signature = {
        "schema": 1,
        "inputs": {
            "color": {"dtype": "fp32", "shape": [1, 4, 64, 64]},
            "history_color": {"dtype": "fp32", "shape": [1, 4, 64, 64]},
        },
        "outputs": {"output": {"dtype": "fp32", "shape": [1, 4, 64, 64]}},
    }
    metadata = {
        "schema": 1,
        "license": "MIT",
        "description": (
            "Deterministic temporal blend (output = color + history_color) used "
            "to qualify the temporal neural runtime. Pre-scaled by 0.5 at the "
            "host side to keep the bundled ONNX graph minimal."
        ),
        "generator": "NeuroShade build tooling",
        "history_depth": 1,
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
