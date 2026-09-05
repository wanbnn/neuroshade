#!/usr/bin/env python3
"""Generate the bundled reference ``.pth`` used by the M7 qualification tests.

The script saves ``tiny_spatial_gain.pth`` containing the state-dict of the
same ``TinySpatialGain`` module the spatial_gain adapter wraps. Tests that
need to exercise a full import pipeline will materialise this file via
CMake's ``add_custom_command`` and hand it to the importer CLI.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import torch

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def _load_spatial_gain_adapter():
    """Load the reference adapter via the importer's adapter loader.

    This exercises the same path the importer uses at runtime, so adapter
    errors surface uniformly.
    """

    sys.path.insert(0, str(REPO_ROOT / "importer" / "python"))
    from neuroshade_importer.adapter import adapter_from_path

    return adapter_from_path(REPO_ROOT / "importer" / "adapters" / "spatial_gain.py")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="Path to write the .pth file")
    args = parser.parse_args()

    adapter = _load_spatial_gain_adapter()
    input_shape = adapter.example_inputs()[0].shape

    model = adapter.instantiate()
    # TinySpatialGain has no parameters; emit the dict so torch.save writes
    # a real pickled mapping rather than a bare string.
    state = {k: v.detach().clone() for k, v in model.state_dict().items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    torch.save(state, args.output)
    sys.stdout.write(
        f"wrote reference .pth: {args.output} "
        f"size={args.output.stat().st_size} bytes keys={list(state.keys())} "
        f"input_shape={input_shape}\n"
    )


if __name__ == "__main__":
    main()
