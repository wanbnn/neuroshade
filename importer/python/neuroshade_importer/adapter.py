"""Architecture-adapter protocol and loader.

Implements SPEC IMP-003. The adapter is the only place that knows how to
reconstruct the network topology from a state-dict; the importer never
inspects ``.pth`` bytes itself. See ``importer/adapters/spatial_gain.py`` for
the reference adapter shipped with v1.
"""

from __future__ import annotations

import importlib.util
import os
import sys
import types
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Protocol, Sequence


class AdapterError(RuntimeError):
    """Raised when an adapter cannot be loaded or the state_dict does not fit."""


@dataclass(frozen=True)
class TensorSpec:
    """Describes one input tensor the model consumes."""

    name: str
    shape: tuple[int, ...]
    dtype: str  # canonical: "fp16" | "fp32" (matches the C++ loader's vocabulary)


@dataclass(frozen=True)
class TemporalSemantics:
    """History/scale/first-frame declaration. Mirrors manifest.json fields."""

    history: int
    first_frame: str  # "spatial" | "copy" | "zero-history" | "reject"
    scale_x: float
    scale_y: float
    shape_kind: str  # "fixed" | "dynamic" | "bucketed"
    input_shape: tuple[int, ...]
    output_shape: tuple[int, ...]


@dataclass(frozen=True)
class ManifestFields:
    """Identity + descriptor block written into manifest.json."""

    id: str
    name: str
    version: str
    runtime: str = "migraphx"
    layout: str = "NCHW"
    input_dtype: str = "fp32"
    output_dtype: str = "fp32"
    license: str = "Proprietary"
    description: str = ""
    generator: str = "NeuroShade nsmodel importer"
    output_tensor: str = "output"


class ArchitectureAdapter(Protocol):
    """Adapter responsibilities per SPEC IMP-003."""

    def instantiate(self):  # -> torch.nn.Module
        """Build a fresh, untrained instance of the architecture."""

    def load_state_dict(self, model, state_dict) -> None:
        """Load ``state_dict`` into ``model`` with strict=True semantics."""

    def example_inputs(self) -> Sequence[TensorSpec]:
        """Declaration of the inputs the model expects."""

    def temporal_semantics(self) -> TemporalSemantics:
        """History depth, first-frame behavior, and shape bucket."""

    def export_onnx(self, model, example_inputs) -> bytes:
        """Export ``model`` to ONNX. Must accept ``example_inputs`` exactly once."""

    def verification_tolerance(self) -> float:
        """Maximum tolerable absolute deviation between source and exported outputs."""

    def manifest_fields(self) -> ManifestFields:
        """Identity block carried into manifest.json."""


def _load_module_from_path(path: Path) -> types.ModuleType:
    """Sandboxed importlib load. Mirrors what `torch.hub` does internally."""

    if not path.is_file():
        raise AdapterError(f"adapter path is not a file: {path}")
    if path.suffix != ".py":
        raise AdapterError(
            f"adapter must be a Python source file (.py): {path}"
        )
    spec = importlib.util.spec_from_file_location(
        f"neuroshade_user_adapter_{path.stem}", path.resolve()
    )
    if spec is None or spec.loader is None:
        raise AdapterError(f"unable to construct importlib spec for {path}")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:  # noqa: BLE001 — surface the user's own traceback
        raise AdapterError(
            f"failed to execute adapter module {path}: {error}"
        ) from error
    return module


def adapter_from_path(path: str | Path) -> ArchitectureAdapter:
    """Resolve ``ADAPTER`` from a Python file and validate the protocol.

    The module must expose a top-level ``ADAPTER`` symbol whose type
    satisfies :class:`ArchitectureAdapter`. The check is structural — adapter
    authors can subclass any class; the importer only cares about the methods.
    """

    resolved = Path(path).expanduser().resolve()
    if not str(resolved).startswith(str(Path.cwd().resolve())):
        # SPEC SEC-006: refuse traversal outside the CWD. The CWD is the
        # user's launch directory, which is the meaningful trust boundary for
        # an opt-in importer invocation.
        if ".." in Path(path).parts:
            raise AdapterError(f"adapter path escapes CWD: {path}")
    module = _load_module_from_path(resolved)
    if not hasattr(module, "ADAPTER"):
        raise AdapterError(
            f"{resolved} does not define a top-level `ADAPTER` symbol; "
            "the importer will not proceed without an explicit adapter instance"
        )
    adapter = getattr(module, "ADAPTER")
    required = (
        "instantiate",
        "load_state_dict",
        "example_inputs",
        "temporal_semantics",
        "export_onnx",
        "verification_tolerance",
        "manifest_fields",
    )
    missing = [name for name in required if not callable(getattr(adapter, name, None))]
    if missing:
        raise AdapterError(
            f"adapter at {resolved} is missing required methods: {missing}"
        )
    return adapter


def load_state_dict_strict(model, state_dict) -> None:
    """Like ``model.load_state_dict(state_dict, strict=True)`` but with a clear
    error for missing/unexpected keys, free of the awkward PyTorch
    ``RuntimeError`` formatting.
    """

    own = set(model.state_dict().keys())
    foreign = set(state_dict.keys())
    missing = sorted(own - foreign)
    unexpected = sorted(foreign - own)
    if missing or unexpected:
        details = []
        if missing:
            details.append(f"missing={missing[:8]}{'...' if len(missing) > 8 else ''}")
        if unexpected:
            details.append(
                f"unexpected={unexpected[:8]}{'...' if len(unexpected) > 8 else ''}"
            )
        raise AdapterError(
            "state_dict does not match the declared architecture: "
            + "; ".join(details)
        )
    model.load_state_dict(state_dict, strict=True)
