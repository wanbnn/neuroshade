"""``.nsmodel`` packager.

Mirrors ``src/neural/model/package.cpp`` byte-for-byte so the runtime cache
key matches what the importer produces. The 5-file order and the FNV-1a
constants are part of the contract with the C++ loader.
"""

from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping

# FNV-1a 64-bit constants. MUST match ``src/neural/model/package.cpp``.
_FNV_OFFSET_BASIS = 0xCBF29CE484222325
_FNV_PRIME = 0x100000001B3
_FNV_MASK = 0xFFFFFFFFFFFFFFFF

PREVIEW_WEBP = (
    b"UklGRiIAAABXRUJQVlA4IBYAAAAwAQCdASoBAAEALmk0mk0iIiIiIgBoSygABc6zbAAA"
)


def stable_hash(blobs: Iterable[bytes]) -> str:
    """Hex-formatted FNV-1a 64-bit hash over a sequence of bytes.

    The format (``0x`` + 16 lowercase hex digits) is identical to
    :func:`neuroshade.neural.hash` in the spatial_runtime test.
    """

    h = _FNV_OFFSET_BASIS
    for blob in blobs:
        for byte in blob:
            h ^= byte
            h = (h * _FNV_PRIME) & _FNV_MASK
    return f"0x{h:016x}"


@dataclass
class PackageResult:
    out_dir: Path
    content_hash: str
    files: dict[str, int]  # name -> byte count


def _atomic_write(path: Path, body: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(body)
    os.replace(tmp, path)


def write_package(
    out_dir: Path,
    *,
    manifest: Mapping,
    signature: Mapping,
    metadata: Mapping,
    onnx_bytes: bytes,
    preview_webp: bytes = PREVIEW_WEBP,
) -> PackageResult:
    """Materialise a ``.nsmodel`` directory. Returns the resulting hash.

    All five files are written in the order the C++ loader reads them so the
    FNV-1a hash stays stable across the language boundary.
    """

    if not out_dir.suffix == ".nsmodel":
        raise ValueError(
            f"output directory must end in .nsmodel, got {out_dir.name}"
        )
    files = {
        "manifest.json": json.dumps(manifest, indent=2).encode("utf-8"),
        "model.onnx": onnx_bytes,
        "signature.json": json.dumps(signature, indent=2).encode("utf-8"),
        "metadata.json": json.dumps(metadata, indent=2).encode("utf-8"),
        "preview.webp": preview_webp,
    }
    for name, body in files.items():
        _atomic_write(out_dir / name, body)
    h = stable_hash(
        [files["manifest.json"], files["model.onnx"], files["signature.json"],
         files["metadata.json"], files["preview.webp"]]
    )
    return PackageResult(
        out_dir=out_dir,
        content_hash=h,
        files={name: len(body) for name, body in files.items()},
    )


def package_hash_matches(out_dir: Path) -> str | None:
    """Recompute the content_hash for an existing package directory.

    Returns the hex hash on success, ``None`` if any required file is missing.
    """

    blobs: list[bytes] = []
    for name in ("manifest.json", "model.onnx", "signature.json",
                 "metadata.json", "preview.webp"):
        path = out_dir / name
        if not path.is_file():
            return None
        blobs.append(path.read_bytes())
    return stable_hash(blobs)


def copy_package(src: Path, dest: Path) -> None:
    """Recursively copy a ``.nsmodel`` directory. Used by ``run_install``."""

    if src.is_symlink() or dest.is_symlink():
        # SEC-006: refuse symlinks for the install boundary.
        raise ValueError(
            f"symlinks are not allowed for .nsmodel install: src={src}, dest={dest}"
        )
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest)
