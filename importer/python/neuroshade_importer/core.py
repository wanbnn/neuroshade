"""Pipeline orchestration for the importer (SPEC §19).

The CLI module wraps these functions; unit tests invoke them directly. None
of this code requires GPU, Vulkan, or MIGraphX Python at import time.
"""

from __future__ import annotations

import datetime as _dt
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from .adapter import (
    AdapterError,
    ArchitectureAdapter,
    ManifestFields,
    TemporalSemantics,
    adapter_from_path,
    load_state_dict_strict,
)
from .package import (
    PackageResult,
    copy_package,
    package_hash_matches,
    write_package,
)
from .preflight import PreflightError, PreflightResult, preflight_existing_package
from .verifier import VerificationError, VerificationResult, verify


class PackageError(RuntimeError):
    """Raised when an .nsmodel cannot be read, written, or installed."""


# ---------------- internal helpers ----------------


def _load_state_dict(pth_path: Path, *, weights_only: bool):
    import torch  # type: ignore[import-not-found]

    try:
        state = torch.load(pth_path, map_location="cpu", weights_only=weights_only)
    except TypeError:
        # Older PyTorch (no weights_only kwarg) — fall back to default.
        state = torch.load(pth_path, map_location="cpu")
        weights_only = False  # we couldn't honour the flag
    if not isinstance(state, Mapping):
        raise AdapterError(
            f"top-level object in {pth_path} must be a state-dict (mapping), "
            f"got {type(state).__name__}"
        )
    return state, weights_only


def _export_onnx(adapter: ArchitectureAdapter, model, specs) -> bytes:
    """Dispatch through ``adapter.export_onnx`` so sentinel adapters can
    inject hand-rolled ONNX bytes (used by IMP-005). Each adapter owns
    its own export policy — the importer never reaches into ``torch.onnx``.
    """

    import torch  # type: ignore[import-not-found]

    model.eval()
    with torch.no_grad():
        example_tensors = [
            torch.zeros(spec.shape, dtype=_torch_dtype_for(spec.dtype))
            for spec in specs
        ]
        return adapter.export_onnx(model, list(example_tensors))


def _torch_dtype_for(name: str):
    import torch  # type: ignore[import-not-found]

    if name == "fp32":
        return torch.float32
    if name == "fp16":
        return torch.float16
    raise ValueError(f"unsupported dtype {name!r}")


def _manifest(
    adapter: ArchitectureAdapter,
    sem: TemporalSemantics,
    *,
    verification: VerificationResult | None,
    preflight: PreflightResult | None,
    content_hash: str | None,
    architecture_source: str,
) -> dict[str, Any]:
    fields = adapter.manifest_fields()
    manifest: dict[str, Any] = {
        "schema": 1,
        "id": fields.id,
        "name": fields.name,
        "version": fields.version,
        "runtime": fields.runtime,
        "inputs": [
            {
                "semantic": "Color.Final",
                "tensor": spec.name,
                "dtype": spec.dtype,
                "layout": fields.layout,
            }
            for spec in adapter.example_inputs()
        ],
        "output": {
            "semantic": "Output.Color",
            "tensor": fields.output_tensor,
            "dtype": fields.output_dtype,
            "layout": fields.layout,
        },
        "history": sem.history,
        "scale": {"x": sem.scale_x, "y": sem.scale_y},
        "first_frame": sem.first_frame,
        "shapes": {
            "kind": sem.shape_kind,
            "buckets": [
                {"input": list(sem.input_shape), "output": list(sem.output_shape)}
            ],
        },
    }
    return manifest


def _signature(adapter: ArchitectureAdapter, fields: ManifestFields, onnx_bytes: bytes) -> dict[str, Any]:
    import onnx

    graph_model = onnx.load_from_string(onnx_bytes)
    inputs = {}
    for entry in graph_model.graph.input:
        shape = []
        for dim in entry.type.tensor_type.shape.dim:
            shape.append(int(dim.dim_value) if dim.dim_value else -1)
        inputs[entry.name] = {"dtype": fields.input_dtype, "shape": shape}
    outputs = {}
    for entry in graph_model.graph.output:
        shape = []
        for dim in entry.type.tensor_type.shape.dim:
            shape.append(int(dim.dim_value) if dim.dim_value else -1)
        outputs[entry.name] = {"dtype": fields.output_dtype, "shape": shape}
    return {
        "schema": 1,
        "inputs": inputs,
        "outputs": outputs,
    }


def _sanitize_onnx(onnx_bytes: bytes) -> bytes:
    """Remove exporter diagnostics that embed checkout-specific source paths."""

    import onnx

    model = onnx.load_from_string(onnx_bytes)

    def clean(message) -> None:
        if hasattr(message, "doc_string"):
            message.doc_string = ""
        if hasattr(message, "metadata_props"):
            del message.metadata_props[:]
        for descriptor, value in message.ListFields():
            if descriptor.type != descriptor.TYPE_MESSAGE:
                continue
            if descriptor.is_repeated:
                for child in value:
                    clean(child)
            else:
                clean(value)

    clean(model)
    return model.SerializeToString()


def _metadata(
    fields: ManifestFields,
    *,
    verification: VerificationResult | None,
    preflight: PreflightResult | None,
    content_hash: str,
    architecture_source: str,
    adapter_path: Path,
    weights_only: bool,
    semantics: TemporalSemantics,
) -> dict[str, Any]:
    meta: dict[str, Any] = {
        "schema": 1,
        "license": fields.license,
        "description": fields.description,
        "generator": fields.generator,
        "architecture_source": architecture_source,
        "adapter": adapter_path.name,
        "weights_only_load": weights_only,
        "history_depth": semantics.history,
        "first_frame": semantics.first_frame,
        "shape_kind": semantics.shape_kind,
        "created_utc": _reproducible_utc_now(),
        "content_hash": content_hash,
    }
    if verification is not None:
        meta["verification"] = {
            "passed": verification.passed,
            "backend": verification.backend,
            "tolerance": verification.tolerance,
            "max_abs_diff": verification.max_abs_diff,
            "max_rel_diff": verification.max_rel_diff,
            "per_output": verification.per_output,
        }
    if preflight is not None:
        meta["preflight"] = {
            "passed": preflight.passed,
            "backend": preflight.backend,
            "target": preflight.target,
            "parameter_names": preflight.parameter_names,
            "unsupported_ops": preflight.unsupported_ops,
        }
    return meta


def _reproducible_utc_now() -> str:
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is not None:
        return _dt.datetime.fromtimestamp(int(epoch), tz=_dt.timezone.utc).replace(
            microsecond=0
        ).isoformat().replace("+00:00", "Z")
    return _dt.datetime.now(tz=_dt.timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


# ---------------- public surface ----------------


@dataclass
class ImportReport:
    output_dir: Path
    content_hash: str
    weights_only: bool
    adapter_path: Path
    verification: VerificationResult | None
    preflight: PreflightResult
    installed_to: Path | None


def run_import(
    pth_path: str | Path,
    *,
    adapter_path: str | Path,
    output_dir: str | Path,
    weights_only: bool = True,
    run_preflight: bool = True,
) -> ImportReport:
    """Full pipeline: load adapter, build model, export ONNX, verify, preflight, package."""

    pth_path = Path(pth_path).expanduser().resolve()
    adapter_path = Path(adapter_path).expanduser().resolve()
    output_dir = Path(output_dir).expanduser().resolve()
    if not pth_path.is_file():
        raise PackageError(f"input .pth not found: {pth_path}")
    if not adapter_path.is_file():
        raise PackageError(f"adapter path not found: {adapter_path}")

    adapter: ArchitectureAdapter = adapter_from_path(adapter_path)
    fields = adapter.manifest_fields()
    semantics = adapter.temporal_semantics()
    specs = list(adapter.example_inputs())

    state_dict, weights_only_actual = _load_state_dict(pth_path, weights_only=weights_only)
    model = adapter.instantiate()
    try:
        adapter.load_state_dict(model, state_dict)
    except AdapterError:
        raise
    except Exception as error:  # noqa: BLE001
        # Adapter loaders may use the strict-load fallback; surface a clearer message.
        raise AdapterError(
            f"adapter.load_state_dict failed: {error}"
        ) from error
    load_state_dict_strict(model, model.state_dict())  # confirms round-trip integrity

    onnx_bytes = _sanitize_onnx(_export_onnx(adapter, model, specs))

    # IMP-005 before IMP-004: a graph MIGraphX refuses to compile should
    # short-circuit the importer so the user never sees a confusion between
    # "verification failed" and "can't run on GPU at all".
    if run_preflight:
        from .preflight import preflight

        preflight_result = preflight(onnx_bytes)
    else:
        preflight_result = PreflightResult(
            passed=True,
            backend="skipped",
            target="gpu",
            parameter_names=[],
            unsupported_ops=[],
        )

    tolerance = adapter.verification_tolerance()
    verification = verify(model, specs, onnx_bytes, tolerance=tolerance)

    manifest = _manifest(
        adapter,
        semantics,
        verification=verification,
        preflight=preflight_result,
        content_hash=None,
        architecture_source=pth_path.name,
    )
    signature = _signature(adapter, fields, onnx_bytes)
    metadata = _metadata(
        fields,
        verification=verification,
        preflight=preflight_result,
        content_hash="placeholder",
        architecture_source=pth_path.name,
        adapter_path=adapter_path,
        weights_only=weights_only_actual,
        semantics=semantics,
    )

    result = write_package(
        output_dir,
        manifest=manifest,
        signature=signature,
        metadata=metadata,
        onnx_bytes=onnx_bytes,
    )

    # content_hash now known — write a final manifest+metadata that echoes it
    metadata["content_hash"] = result.content_hash
    manifest["content_hash"] = result.content_hash
    write_package(
        output_dir,
        manifest=manifest,
        signature=signature,
        metadata=metadata,
        onnx_bytes=onnx_bytes,
    )

    installed_to: Path | None = None
    if os.environ.get("NEUROSHADE_NO_INSTALL") != "1":
        share_root = _share_root()
        default_dest = share_root / pth_path.with_suffix(".nsmodel").name
        # Default to co-locating with the artefact's basename.
        default_dest = share_root / output_dir.name
        try:
            if default_dest.resolve() == output_dir.resolve():
                installed_to = output_dir
            else:
                copy_package(output_dir, default_dest)
                installed_to = default_dest
        except Exception:  # noqa: BLE001
            # Non-fatal: CI environments without $HOME or with read-only $HOME
            # simply leave the artefact where the user asked.
            installed_to = None

    return ImportReport(
        output_dir=output_dir,
        content_hash=result.content_hash,
        weights_only=weights_only_actual,
        adapter_path=adapter_path,
        verification=verification,
        preflight=preflight_result,
        installed_to=installed_to,
    )


def run_install(src: str | Path, dest_root: str | Path | None = None) -> Path:
    """Copy a ``.nsmodel`` into the user-local models directory."""

    src = Path(src).expanduser().resolve()
    if not src.is_dir():
        raise PackageError(f"package directory not found: {src}")
    if not src.suffix == ".nsmodel":
        raise PackageError(f"directory must end in .nsmodel: {src}")
    if package_hash_matches(src) is None:
        raise PackageError(
            f"package at {src} is missing one or more required files"
        )

    if dest_root is None:
        dest_root = _share_root()
    else:
        dest_root = Path(dest_root).expanduser().resolve()
    dest = dest_root / src.name
    copy_package(src, dest)
    return dest


def run_preflight(package_dir: str | Path) -> PreflightResult:
    package_dir = Path(package_dir).expanduser().resolve()
    if not package_dir.is_dir():
        raise PackageError(f"package directory not found: {package_dir}")
    if not package_dir.suffix == ".nsmodel":
        raise PackageError(f"directory must end in .nsmodel: {package_dir}")
    return preflight_existing_package(package_dir)


def run_verify(
    pth_path: str | Path,
    *,
    adapter_path: str | Path,
    package_dir: str | Path,
) -> VerificationResult:
    """Re-run numerical verification against an existing .nsmodel's ONNX.

    Skips the export step (uses the bytes already in the package), so this
    is the cheap fast-path that ``tools/nsmodel/importer.py verify`` wires.
    """

    pth_path = Path(pth_path).expanduser().resolve()
    adapter_path = Path(adapter_path).expanduser().resolve()
    package_dir = Path(package_dir).expanduser().resolve()
    onnx_bytes = (package_dir / "model.onnx").read_bytes()

    adapter = adapter_from_path(adapter_path)
    fields = adapter.manifest_fields()
    state_dict, _ = _load_state_dict(pth_path, weights_only=True)
    model = adapter.instantiate()
    adapter.load_state_dict(model, state_dict)
    return verify(
        model,
        list(adapter.example_inputs()),
        onnx_bytes,
        tolerance=adapter.verification_tolerance(),
    )


def _share_root() -> Path:
    """Resolve ``$XDG_DATA_HOME/neuroshade/models`` per SPEC §37."""

    base = os.environ.get("XDG_DATA_HOME")
    if base:
        return Path(base) / "neuroshade" / "models"
    home = os.environ.get("HOME")
    if home:
        return Path(home) / ".local" / "share" / "neuroshade" / "models"
    return Path.cwd() / "neuroshade" / "models"
