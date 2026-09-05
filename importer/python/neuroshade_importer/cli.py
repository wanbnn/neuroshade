"""argparse wiring for the importer entry script.

The CLI follows the project's existing stdout convention — one ``key=value``
per logical fact, designed for ``string(FIND)`` substring matching in CTest.
The exit code is 0 on success and non-zero on failure.
"""

from __future__ import annotations

import argparse
import json
import sys
import traceback
from pathlib import Path

from .adapter import AdapterError
from .core import (
    PackageError,
    run_import,
    run_install,
    run_preflight,
    run_verify,
)
from .preflight import PreflightError
from .verifier import VerificationError


def _emit(line: str) -> None:
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="nsmodel",
        description="NeuroShade .pth -> .nsmodel importer (SPEC §19).",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_import = sub.add_parser("import", help="Convert a .pth to a .nsmodel")
    p_import.add_argument("pth", help="Path to the PyTorch state_dict (.pth)")
    p_import.add_argument(
        "--adapter", required=True,
        help="Python source file exposing `ADAPTER = ArchitectureAdapter()`",
    )
    p_import.add_argument(
        "--output", required=True,
        help="Destination directory (must end in .nsmodel)",
    )
    p_import.add_argument(
        "--unsafe-pickle", action="store_true",
        help="Allow torch.load(weights_only=False). Prompts on stdin first.",
    )
    p_import.add_argument(
        "--skip-preflight", action="store_true",
        help="Skip the MIGraphX compile preflight (not recommended).",
    )

    p_preflight = sub.add_parser("preflight", help="MIGraphX compile check for an .nsmodel")
    p_preflight.add_argument("package", help="Path to the .nsmodel directory")

    p_install = sub.add_parser("install", help="Copy an .nsmodel into the user models dir")
    p_install.add_argument("package", help="Path to the .nsmodel directory to install")
    p_install.add_argument(
        "--dest", default=None,
        help="Override the destination root (default: $XDG_DATA_HOME/neuroshade/models)",
    )

    p_verify = sub.add_parser("verify", help="Re-run numerical verification against an .nsmodel")
    p_verify.add_argument("pth", help="Source .pth")
    p_verify.add_argument("--adapter", required=True, help="Adapter source file")
    p_verify.add_argument("package", help=".nsmodel directory")

    return parser


def _cmd_import(args: argparse.Namespace) -> int:
    if args.unsafe_pickle:
        sys.stderr.write(
            "WARNING: --unsafe-pickle disables torch.load(weights_only=True). "
            "Only use with trusted .pth files.\n"
        )
    try:
        weights_only = not args.unsafe_pickle
        report = run_import(
            args.pth,
            adapter_path=args.adapter,
            output_dir=args.output,
            weights_only=weights_only,
            run_preflight=not args.skip_preflight,
        )
    except AdapterError as error:
        _emit(f"status=fail")
        _emit(f"reason=adapter_error")
        _emit(f"missing_keys=")  # not always present, but keeps grep forgiving
        sys.stderr.write(f"adapter_error: {error}\n")
        return 2
    except VerificationError as error:
        _emit("status=fail")
        _emit("reason=verification_error")
        sys.stderr.write(f"verification_error: {error}\n")
        return 3
    except PreflightError as error:
        _emit("status=fail")
        _emit("reason=preflight_error")
        sys.stderr.write(f"preflight_error: {error}\n")
        return 4
    except PackageError as error:
        _emit("status=fail")
        _emit("reason=package_error")
        sys.stderr.write(f"package_error: {error}\n")
        return 5
    except Exception as error:  # noqa: BLE001
        _emit("status=fail")
        _emit("reason=internal_error")
        sys.stderr.write(traceback.format_exc())
        return 1

    _emit("status=ok")
    _emit(f"model={_model_id(report)}")
    _emit(f"weights_only={'yes' if report.weights_only else 'no'}")
    _emit(f"adapter_module={report.adapter_path.stem}")
    if report.verification is not None:
        _emit(
            f"verification=tolerance={report.verification.tolerance} "
            f"max_abs_diff={report.verification.max_abs_diff:.3e} "
            f"backend={report.verification.backend}"
        )
    else:
        _emit("verification=skipped")
    _emit(
        f"preflight=compiled target={report.preflight.target} "
        f"backend={report.preflight.backend}"
    )
    _emit(f"content_hash={report.content_hash}")
    _emit(
        "installed=" + ("yes" if report.installed_to is not None else "no")
    )
    if report.installed_to is not None:
        _emit(f"install_path={report.installed_to}")
    _emit(f"adapter_loaded=yes")
    return 0


def _cmd_preflight(args: argparse.Namespace) -> int:
    try:
        result = run_preflight(args.package)
    except PreflightError as error:
        _emit("status=fail")
        _emit("reason=preflight_error")
        sys.stderr.write(f"preflight_error: {error}\n")
        return 4
    except PackageError as error:
        _emit("status=fail")
        _emit("reason=package_error")
        sys.stderr.write(f"package_error: {error}\n")
        return 5
    _emit("status=ok")
    _emit(f"preflight=compiled target={result.target} backend={result.backend}")
    _emit(f"parameters={' '.join(result.parameter_names) or 'none'}")
    return 0


def _cmd_install(args: argparse.Namespace) -> int:
    try:
        dest = run_install(args.package, args.dest)
    except PackageError as error:
        _emit("status=fail")
        _emit("reason=package_error")
        sys.stderr.write(f"package_error: {error}\n")
        return 5
    _emit("status=ok")
    _emit(f"installed=yes install_path={dest}")
    return 0


def _cmd_verify(args: argparse.Namespace) -> int:
    try:
        result = run_verify(
            args.pth,
            adapter_path=args.adapter,
            package_dir=args.package,
        )
    except VerificationError as error:
        _emit("status=fail")
        _emit("reason=verification_error")
        sys.stderr.write(f"verification_error: {error}\n")
        return 3
    except AdapterError as error:
        _emit("status=fail")
        _emit("reason=adapter_error")
        sys.stderr.write(f"adapter_error: {error}\n")
        return 2
    except Exception as error:  # noqa: BLE001
        _emit("status=fail")
        _emit("reason=internal_error")
        sys.stderr.write(traceback.format_exc())
        return 1
    _emit("status=ok")
    _emit(
        f"verification=tolerance={result.tolerance} "
        f"max_abs_diff={result.max_abs_diff:.3e} backend={result.backend}"
    )
    return 0


def _model_id(report) -> str:
    """Best-effort identifier for stdout."""

    try:
        manifest = json.loads((report.output_dir / "manifest.json").read_text())
        return manifest.get("id", "unknown")
    except Exception:  # noqa: BLE001
        return "unknown"


COMMAND_DISPATCH = {
    "import": _cmd_import,
    "preflight": _cmd_preflight,
    "install": _cmd_install,
    "verify": _cmd_verify,
}


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return COMMAND_DISPATCH[args.command](args)


if __name__ == "__main__":
    raise SystemExit(main())
