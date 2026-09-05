"""Sentinel adapter exercising IMP-005 (unsupported MIGraphX ops fail before launch).

The forward graph is constructed from raw ONNX protobuf bytes containing
``onnx::Optional`` (opset ≥ 15), which MIGraphX 2.15 is known to reject
during ``migraphx::parse_onnx``. Used by
``tests/importer/unsupported_op_test.cmake.in``.
"""

from __future__ import annotations

import io
import struct
import sys
from pathlib import Path

_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[2]
_IMPORTER_PYTHON = _REPO_ROOT / "importer" / "python"
if _IMPORTER_PYTHON.is_dir() and str(_IMPORTER_PYTHON) not in sys.path:
    sys.path.insert(0, str(_IMPORTER_PYTHON))

from typing import Sequence  # noqa: E402

import torch  # noqa: E402
import torch.nn as nn  # noqa: E402

from neuroshade_importer.adapter import (  # noqa: E402
    ManifestFields,
    TemporalSemantics,
    TensorSpec,
)


INPUT_SHAPE: tuple[int, ...] = (1, 4, 64, 64)


class OptionalModule(nn.Module):
    """Wrap ``x`` in a tuple so the ``Optional`` sequence-construction op has work to do."""

    def forward(self, color: torch.Tensor):
        # ``optional_get_element`` is a placeholder; the real unsupported op
        # is added at ONNX-export time by the adapter below.
        return color


def _varint(value: int) -> bytes:
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def _field(number: int, wire: int, value: bytes) -> bytes:
    tag = _varint((number << 3) | wire)
    return tag + (_varint(len(value)) + value if wire == 2 else value)


def _text(number: int, value: str) -> bytes:
    return _field(number, 2, value.encode("utf-8"))


def _integer(number: int, value: int) -> bytes:
    return _field(number, 0, _varint(value))


def _message(number: int, value: bytes) -> bytes:
    return _field(number, 2, value)


def _value_info(name: str, dimensions) -> bytes:
    shape = b"".join(_message(1, _integer(1, dimension)) for dimension in dimensions)
    tensor_type = _integer(1, 1) + _message(2, shape)  # FLOAT
    return _text(1, name) + _message(2, _message(1, tensor_type))


def _shape_node(op_type: str, inputs: list[str], outputs: list[str]) -> bytes:
    body = b""
    for inp in inputs:
        body += _text(1, inp)
    for outp in outputs:
        body += _text(2, outp)
    body += _text(4, op_type)
    return _message(1, body)


def build_unsupported_onnx() -> bytes:
    """Hand-rolled ONNX graph containing an op MIGraphX 2.15 rejects.

    Uses ``Optional`` (opset ≥ 15) so MIGraphX's parser aborts with an
    "unsupported op" error. Combined with the bundled importer preflight,
    this exercises IMP-005 without needing the MIGraphX Python bindings:
    the structural fallback also flags ``Optional``.
    """

    graph = b""
    # `_shape_node(...)` already returns a fully-tagged NodeProto message
    # (field 1 wire 2), so concatenating with the rest of the graph is the
    # correct encoding — there is no outer `_message(1, ...)` wrapper.
    graph += _shape_node("Identity", ["color"], ["optional_input"])
    graph += _shape_node("Optional", ["optional_input"], ["optional_output"])
    graph += _shape_node("Identity", ["optional_output"], ["output"])
    graph += _message(11, _value_info("color", INPUT_SHAPE))
    graph += _message(12, _value_info("output", INPUT_SHAPE))

    opset = _integer(2, 17)
    model = _integer(1, 13)
    model += _text(2, "NeuroShade")
    model += _text(3, "0.1.0")
    model += _message(7, graph)
    model += _message(8, opset)
    return model


class UnsupportedOpAdapter:
    """Adapter whose ``export_onnx`` returns raw bytes with the ``Optional`` op."""

    def instantiate(self) -> nn.Module:
        return OptionalModule()

    def load_state_dict(self, model: nn.Module, state_dict) -> None:
        model.load_state_dict(state_dict, strict=True)

    def example_inputs(self) -> Sequence[TensorSpec]:
        return (TensorSpec(name="color", shape=INPUT_SHAPE, dtype="fp32"),)

    def temporal_semantics(self) -> TemporalSemantics:
        return TemporalSemantics(
            history=0, first_frame="spatial",
            scale_x=1.0, scale_y=1.0,
            shape_kind="fixed",
            input_shape=INPUT_SHAPE, output_shape=INPUT_SHAPE,
        )

    def export_onnx(self, model: nn.Module, example_inputs) -> bytes:
        return build_unsupported_onnx()

    def verification_tolerance(self) -> float:
        return 1.0e-3

    def manifest_fields(self) -> ManifestFields:
        return ManifestFields(
            id="org.neuroshade.bundled.unsupported_op",
            name="Unsupported Operator Sentinel",
            version="1.0.0",
            runtime="migraphx",
            input_dtype="fp32",
            output_dtype="fp32",
            output_tensor="output",
            description="Sentinel fixture for IMP-005.",
            generator="NeuroShade nsmodel importer",
        )


ADAPTER = UnsupportedOpAdapter()
