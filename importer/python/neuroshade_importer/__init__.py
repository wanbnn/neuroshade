"""NeuroShade `.pth` -> `.nsmodel` importer library.

This library implements SPEC §19 (IMP-001..IMP-005). It is invoked from
``tools/nsmodel/importer.py`` (the user-facing CLI) and from the CTest
wrappers under ``tests/importer/``.

Public surface::

    ArchitectureAdapter     # protocol (SPEC IMP-003)
    TemporalSemantics       # adapter-supplied history/scale/first_frame
    ManifestFields          # adapter-supplied identity/header fields

    AdapterError            # raised on bad adapter or shape mismatch
    VerificationError       # raised on PyTorch/ONNX output disagreement
    PreflightError          # raised when MIGraphX refuses to compile
    PackageError            # raised when filesystem writes fail

    run_import              # full .pth -> .nsmodel pipeline
    run_preflight           # MIGraphX compile preflight of an .nsmodel
    run_install             # copy an .nsmodel into the user-local models dir
    run_verify              # re-run numerical verification against an .nsmodel

The library never imports migraphx at module load time — MIGraphX availability
is probed lazily inside :func:`run_preflight` / :func:`importer.preflight`.
"""

from .adapter import (
    ArchitectureAdapter,
    ManifestFields,
    TemporalSemantics,
    TensorSpec,
    adapter_from_path,
    AdapterError,
)
from .package import PackageResult, write_package, stable_hash
from .verifier import VerificationError, VerificationResult, verify
from .preflight import PreflightError, PreflightResult, preflight
from .core import (
    PackageError,
    run_import,
    run_install,
    run_preflight,
    run_verify,
)

__all__ = [
    # protocol
    "ArchitectureAdapter",
    "ManifestFields",
    "TemporalSemantics",
    "TensorSpec",
    # exceptions
    "AdapterError",
    "VerificationError",
    "PreflightError",
    "PackageError",
    # results
    "PackageResult",
    "VerificationResult",
    "PreflightResult",
    # operations
    "adapter_from_path",
    "stable_hash",
    "write_package",
    "verify",
    "preflight",
    "run_import",
    "run_install",
    "run_preflight",
    "run_verify",
]
