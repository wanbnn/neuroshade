"""Reference adapter for the M5 ``spatial_gain.nsmodel`` model.

A TinySpatialGain module whose forward returns ``0.75 * x``. The bundled
reference `.pth` for this adapter contains the matching ``state_dict``
emitted by ``tools/nsmodel/build_reference_model.py``.

This is the canonical IMP-003 adapter: a single ``nn.Module`` plus
declarations of input/tolerance/manifest fields. Every property the importer
writes into ``manifest.json``/``signature.json`` is set here so the schema
stays close to the C++ loader's source of truth.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Make ``neuroshade_importer`` importable when the importer's loader picks
# this file up without the package parent being on sys.path. We resolve the
# repository layout deterministically — there is no PYTHONPATH side-effect.
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


class TinySpatialGain(nn.Module):
    def forward(self, color: torch.Tensor) -> torch.Tensor:
        return 0.75 * color


INPUT_SHAPE: tuple[int, ...] = (1, 4, 64, 64)
INPUT_NAME = "color"
OUTPUT_NAME = "output"


class SpatialGainAdapter:
    """Adapter contract for the spatial-gain reference model."""

    def instantiate(self) -> nn.Module:
        return TinySpatialGain()

    def load_state_dict(self, model: nn.Module, state_dict) -> None:
        model.load_state_dict(state_dict, strict=True)

    def example_inputs(self) -> Sequence[TensorSpec]:
        return (TensorSpec(name=INPUT_NAME, shape=INPUT_SHAPE, dtype="fp32"),)

    def temporal_semantics(self) -> TemporalSemantics:
        return TemporalSemantics(
            history=0,
            first_frame="spatial",
            scale_x=1.0,
            scale_y=1.0,
            shape_kind="fixed",
            input_shape=INPUT_SHAPE,
            output_shape=INPUT_SHAPE,
        )

    def export_onnx(self, model: nn.Module, example_inputs) -> bytes:
        import io

        # The importer has already called model.eval() — re-asserting it is a
        # cheap safety net against adapters that mutate the graph further
        # outside of the forward path.
        model.eval()
        # `example_inputs` is a list of ``torch.Tensor`` instances the
        # importer materialised from ``TensorSpec.shape``; the spatial-gain
        # model has a single input. opset 18 is the lowest version PyTorch
        # 2.13's dynamo-based exporter supports without triggering an
        # auto-conversion that older conversions cannot complete.
        example_tensor = example_inputs[0]
        buffer = io.BytesIO()
        with torch.no_grad():
            torch.onnx.export(
                model,
                (example_tensor,),
                buffer,
                input_names=[INPUT_NAME],
                output_names=[OUTPUT_NAME],
                opset_version=18,
                do_constant_folding=True,
                dynamic_shapes={INPUT_NAME: {0: torch.export.Dim("N", min=1, max=64)}},
            )
        return buffer.getvalue()

    def verification_tolerance(self) -> float:
        return 1.0e-5

    def manifest_fields(self) -> ManifestFields:
        return ManifestFields(
            id="org.neuroshade.bundled.spatial_gain",
            name="Spatial Gain Reference",
            version="1.0.0",
            runtime="migraphx",
            layout="NCHW",
            input_dtype="fp32",
            output_dtype="fp32",
            license="MIT",
            description=(
                "Deterministic spatial gain model used to qualify MIGraphX. "
                "Built from a PyTorch state_dict via the M7 importer."
            ),
            generator="NeuroShade nsmodel importer",
        )

    @property
    def output_tensor(self) -> str:
        return OUTPUT_NAME


ADAPTER = SpatialGainAdapter()
