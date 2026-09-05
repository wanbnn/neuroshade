"""MIGraphX compile preflight (SPEC IMP-005).

The same flag set the C++ runtimes use is encoded here so the convention
cannot drift. When MIGraphX Python is unavailable we fall back to a
syntax/structural check (parse the graph and look for ops whose domain
MIGraphX is known to refuse) — this is weaker than a real compile but
gives the importer a deterministic fail-fast behaviour in environments
without ``pymigraphx``.
"""

from __future__ import annotations

import importlib.util
import io
from dataclasses import dataclass
from typing import Optional

import onnx


class PreflightError(RuntimeError):
    """Raised when MIGraphX cannot compile the exported graph."""


@dataclass
class PreflightResult:
    passed: bool
    backend: str
    target: str
    parameter_names: list[str]
    unsupported_ops: list[str]


# OpType strings MIGraphX is known to refuse at v2.15 — used only as a
# fallback signature check when the MIGraphX Python binding is absent.
_KNOWN_UNSUPPORTED_OPS = frozenset(
    {
        "Optional",
        "GridSample",  # in older versions; safe to keep as a trip-wire
    }
)


def _try_migraphx():
    spec = importlib.util.find_spec("migraphx")
    if spec is None:
        return None
    return importlib.import_module("migraphx")


def _fallback_structural_check(onnx_bytes: bytes) -> PreflightResult:
    """A best-effort structural preflight when MIGraphX Python is missing.

    The function only flags operators MIGraphX definitely rejects — it is a
    defensive trip-wire, not a substitute for the real ``migraphx::compile``
    call performed by the C++ runtime.
    """

    graph = onnx.load_from_string(onnx_bytes).graph
    unsupported = sorted({node.op_type for node in graph.node if node.op_type in _KNOWN_UNSUPPORTED_OPS})
    parameters = [entry.name for entry in graph.input]
    passed = not unsupported
    return PreflightResult(
        passed=passed,
        backend="fallback_structural",
        target="gpu",
        parameter_names=parameters,
        unsupported_ops=unsupported,
    )


def preflight(onnx_bytes: bytes, *, target: str = "gpu") -> PreflightResult:
    """Run MIGraphX compile on ``onnx_bytes`` or fall back to a structural scan.

    Raises ``PreflightError`` only when the real compile fails. When MIGraphX
    Python is missing we still return a ``PreflightResult`` whose
    ``passed`` field reflects the structural trip-wire.
    """

    mx = _try_migraphx()
    if mx is None:
        result = _fallback_structural_check(onnx_bytes)
        if not result.passed:
            raise PreflightError(
                f"structural preflight flagged unsupported ops: {result.unsupported_ops}"
            )
        return result

    # Mirror the C++ runtimes exactly (see src/neural/runtime/spatial_runtime.cpp:88-91)
    try:
        program = mx.parse_onnx(io.BytesIO(onnx_bytes))
    except Exception as error:  # noqa: BLE001
        raise PreflightError(f"migraphx.parse_onnx failed: {error}") from error

    options = mx.compile_options()
    options.set_offload_copy(False)
    try:
        program.compile(mx.target(target), options)
    except Exception as error:  # noqa: BLE001
        raise PreflightError(
            f"migraphx.compile(target={target}) failed: {error}"
        ) from error

    try:
        params = list(program.get_parameter_names())
    except Exception:  # noqa: BLE001
        params = []
    return PreflightResult(
        passed=True,
        backend=f"migraphx.{getattr(mx, '__version__', 'unknown')}",
        target=target,
        parameter_names=params,
        unsupported_ops=[],
    )


def preflight_existing_package(package_dir) -> PreflightResult:
    """Convenience wrapper: extract ``model.onnx`` from an ``.nsmodel`` and preflight."""

    onnx_path = package_dir / "model.onnx"
    if not onnx_path.is_file():
        raise PreflightError(f"no model.onnx in {package_dir}")
    return preflight(onnx_path.read_bytes())
