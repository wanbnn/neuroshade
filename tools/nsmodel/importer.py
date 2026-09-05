#!/usr/bin/env python3
"""NeuroShade ``nsmodel`` importer — user-facing CLI entry.

Wrapper that injects ``importer/python`` onto ``sys.path`` and forwards to
``neuroshade_importer.cli.main``. Mirrors the ``tools/nsmodel/generate_*.py``
style: a single-file Python entry invoked by the user and by CTest.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REPO_IMPORTER_PYTHON = REPO_ROOT / "importer" / "python"
INSTALLED_IMPORTER_PYTHON = Path(__file__).resolve().parent / "python"
IMPORTER_PYTHON = Path(
    os.environ.get(
        "NEUROSHADE_IMPORTER_PYTHON",
        REPO_IMPORTER_PYTHON
        if (REPO_IMPORTER_PYTHON / "neuroshade_importer").is_dir()
        else INSTALLED_IMPORTER_PYTHON,
    )
)
sys.path.insert(0, str(IMPORTER_PYTHON))


def main() -> int:
    from neuroshade_importer.cli import main as cli_main

    return cli_main()


if __name__ == "__main__":
    raise SystemExit(main())
