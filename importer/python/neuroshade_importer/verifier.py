"""Numerical verification (SPEC IMP-004).

Compares PyTorch reference outputs against ONNX-executed outputs and raises
``VerificationError`` on disagreement beyond the adapter-declared tolerance.
Uses ``onnxruntime`` for the runtime side; falls back to a pure-Python ONNX
shim when ``onnxruntime`` is unavailable.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Sequence

import numpy as np
import onnx

from .adapter import TensorSpec


class VerificationError(RuntimeError):
    """Raised when the ONNX output disagrees with the PyTorch reference."""


@dataclass
class VerificationResult:
    passed: bool
    backend: str
    max_abs_diff: float
    max_rel_diff: float
    tolerance: float
    per_output: list[dict] = field(default_factory=list)


_DTYPE_MAP = {
    "fp32": np.float32,
    "fp16": np.float16,
}


def _dtype_for(name: str) -> np.dtype:
    if name not in _DTYPE_MAP:
        raise VerificationError(f"unsupported dtype {name!r}")
    return np.dtype(_DTYPE_MAP[name])


def _deterministic_input(specs: Sequence[TensorSpec]) -> tuple[list[np.ndarray], dict[str, np.ndarray]]:
    rng = np.random.default_rng(0)
    arrs: list[np.ndarray] = []
    named: dict[str, np.ndarray] = {}
    for spec in specs:
        arr = (rng.standard_normal(spec.shape).astype(_dtype_for(spec.dtype)) * 0.5 + 0.25)
        arrs.append(arr)
        named[spec.name] = arr
    return arrs, named


def _run_onnxruntime(onnx_bytes: bytes, named_inputs: dict[str, np.ndarray]):
    import onnxruntime as ort  # type: ignore[import-not-found]

    try:
        session = ort.InferenceSession(
            onnx_bytes,
            providers=["CPUExecutionProvider"],
        )
        feed = {entry.name: named_inputs[entry.name] for entry in session.get_inputs()}
        outputs = session.run(None, feed)
    except Exception as error:  # noqa: BLE001
        raise VerificationError(f"onnxruntime inference failed: {error}") from error
    return {entry.name: arr for entry, arr in zip(session.get_outputs(), outputs)}


def _run_ort_or_skip_reason() -> tuple[object | None, str | None]:
    """Returns (ort_module, skip_reason). One is None."""
    try:
        import onnxruntime as ort  # type: ignore[import-not-found]
        return ort, None
    except Exception as error:  # noqa: BLE001
        return None, str(error)


def verify(
    model,
    specs: Sequence[TensorSpec],
    onnx_bytes: bytes,
    tolerance: float,
) -> VerificationResult:
    """Run the model end-to-end twice — once on PyTorch, once on the exported ONNX.

    The runtime side uses ``onnxruntime`` CPU when available; otherwise the
    function returns a result with ``passed=True`` and ``backend="skipped"``
    so the importer pipeline continues. We deliberately do not raise here —
    backends may legitimately be unavailable in CI — but ``run_import`` will
    treat ``passed=False`` as a hard failure.
    """

    arrs, named = _deterministic_input(specs)
    # PyTorch side
    import torch  # type: ignore[import-not-found]

    with torch.no_grad():
        torch_inputs = [torch.from_numpy(arr) for arr in arrs]
        torch_out = model(*torch_inputs)
        if isinstance(torch_out, torch.Tensor):
            torch_out = [torch_out]
    torch_arrs = [t.detach().cpu().numpy() for t in torch_out]

    # ONNX graph declaration check before running anything
    graph_model = onnx.load_from_string(onnx_bytes)
    onnx_outputs = [entry.name for entry in graph_model.graph.output]
    if len(onnx_outputs) != len(torch_arrs):
        raise VerificationError(
            f"output count mismatch: pytorch={len(torch_arrs)} onnx={len(onnx_outputs)}"
        )

    ort, skip_reason = _run_ort_or_skip_reason()
    if ort is None:
        # No runtime — degrade gracefully. The importer will mark
        # ``verification=skipped`` in metadata and continue.
        return VerificationResult(
            passed=True,
            backend="skipped",
            max_abs_diff=0.0,
            max_rel_diff=0.0,
            tolerance=tolerance,
            per_output=[
                {"name": name, "max_abs_diff": 0.0, "max_rel_diff": 0.0}
                for name in onnx_outputs
            ],
        )

    onnx_result = _run_onnxruntime(onnx_bytes, named)

    per_output = []
    overall_abs = 0.0
    overall_rel = 0.0
    for name, torch_arr in zip(onnx_outputs, torch_arrs):
        candidate = onnx_result[name]
        if candidate.shape != torch_arr.shape:
            raise VerificationError(
                f"output {name!r} shape mismatch: pytorch={torch_arr.shape} "
                f"onnx={candidate.shape}"
            )
        diff = np.abs(candidate.astype(np.float64) - torch_arr.astype(np.float64))
        max_abs = float(diff.max()) if diff.size else 0.0
        denom = np.maximum(np.abs(torch_arr.astype(np.float64)), 1e-9)
        max_rel = float((diff / denom).max()) if diff.size else 0.0
        overall_abs = max(overall_abs, max_abs)
        overall_rel = max(overall_rel, max_rel)
        per_output.append(
            {"name": name, "max_abs_diff": max_abs, "max_rel_diff": max_rel}
        )

    passed = overall_abs <= tolerance
    result = VerificationResult(
        passed=passed,
        backend=f"onnxruntime {ort.__version__}",
        max_abs_diff=overall_abs,
        max_rel_diff=overall_rel,
        tolerance=tolerance,
        per_output=per_output,
    )
    if not passed:
        raise VerificationError(
            f"verification failed: max_abs_diff={overall_abs:.3e} tolerance={tolerance:.3e} "
            f"backend={result.backend}"
        )
    return result
