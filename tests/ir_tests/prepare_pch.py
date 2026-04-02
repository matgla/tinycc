#!/usr/bin/env python3
"""
Prepare reusable explicit PCH artifacts for libc/common headers and libtcc.h.

By default this writes into a repo-local target-specific directory:
  pch/<target>/prepared/

Generated artifacts:
  - libc-common.c
  - libc-common.pch
  - libtcc-bench.c
  - libtcc-bench.pch
  - manifest.json

Example:
  python tests/ir_tests/prepare_pch.py
  python tests/ir_tests/prepare_pch.py --output-dir /tmp/pch --only libtcc
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

CURRENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CURRENT_DIR.parent.parent
DEFAULT_COMPILER = REPO_ROOT / "armv8m-tcc"
LIBC_HEADERS = ("stdio.h", "stdlib.h", "string.h")


def _run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def _compiler_base(compiler: Path) -> list[str]:
    return [str(compiler), f"-B{REPO_ROOT}"]


def _target_subdir_name(compiler: Path) -> str:
    name = compiler.name
    if name.endswith("-tcc"):
        return name[:-3]
    if name == "tcc":
        return "native"
    return f"{compiler.stem}-"


def _write_probe(path: Path, source: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source)


def _generate_pch(compiler: Path, header_path: Path, pch_path: Path, include_dirs: list[Path]) -> None:
    command = [*_compiler_base(compiler)]
    for include_dir in include_dirs:
        command.extend(["-I", str(include_dir)])
    command.extend(["-generate-pch", str(header_path), "-o", str(pch_path)])
    result = _run(command)
    if result.returncode != 0 or not pch_path.exists():
        raise RuntimeError(
            f"failed to generate PCH for {header_path}:\n{result.stderr}{result.stdout}"
        )


def _validate_pch(compiler: Path, pch_path: Path, probe_path: Path, include_dirs: list[Path]) -> None:
    object_path = probe_path.with_suffix(".validate.o")
    command = [*_compiler_base(compiler)]
    for include_dir in include_dirs:
        command.extend(["-I", str(include_dir)])
    command.extend(["-use-pch", str(pch_path), "-c", str(probe_path), "-o", str(object_path)])
    result = _run(command)
    combined = (result.stderr or "") + (result.stdout or "")
    if result.returncode != 0:
        raise RuntimeError(f"failed to validate PCH {pch_path}:\n{combined}")
    if "ignoring PCH" in combined:
        raise RuntimeError(f"PCH {pch_path} was ignored during validation:\n{combined}")
    if object_path.exists():
        object_path.unlink()


def _prepare_libc(compiler: Path, output_dir: Path, manifest: dict) -> None:
    source_path = output_dir / "libc-common.c"
    pch_path = output_dir / "libc-common.pch"

    source_text = "".join(f"#include <{header_name}>\n" for header_name in LIBC_HEADERS) + """
static int pch_prepare_libc_probe(void)
{
  return 0;
}
"""
    _write_probe(source_path, source_text)
    _generate_pch(compiler, source_path, pch_path, [output_dir])
    _validate_pch(compiler, pch_path, source_path, [output_dir])

    manifest["libc"] = {
        "mode": "translation-unit-snapshot",
        "source": str(source_path),
        "headers": list(LIBC_HEADERS),
        "pch_path": str(pch_path),
    }


def _prepare_libtcc(compiler: Path, output_dir: Path, manifest: dict) -> None:
    header_path = (REPO_ROOT / "libtcc.h").resolve()
    if not header_path.exists():
        raise FileNotFoundError(f"missing libtcc header: {header_path}")

    source_path = output_dir / "libtcc-bench.c"
    pch_path = output_dir / "libtcc-bench.pch"
    include_dirs = [REPO_ROOT, REPO_ROOT / "include"]

    source_text = """
#include "libtcc.h"

static int pch_prepare_libtcc_probe(void)
{
  return 0;
}
"""
    _write_probe(source_path, source_text)
    _generate_pch(compiler, source_path, pch_path, include_dirs + [output_dir])
    _validate_pch(compiler, pch_path, source_path, include_dirs + [output_dir])

    manifest["libtcc"] = {
        "header": "libtcc.h",
        "header_path": str(header_path),
        "mode": "translation-unit-snapshot",
        "source": str(source_path),
        "pch_path": str(pch_path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare repo-local explicit PCH artifacts for libc and libtcc")
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER, help="Path to compiler binary")
    parser.add_argument("--output-dir", type=Path, help="Output directory (default: repo pch/<target>/)")
    parser.add_argument(
        "--only",
        action="append",
        choices=["libc", "libtcc"],
        help="Prepare only selected artifacts (repeatable)",
    )
    args = parser.parse_args()

    compiler = args.compiler.resolve()
    if not compiler.exists():
        print(f"Compiler not found: {compiler}", file=sys.stderr)
        return 1

    target_subdir = _target_subdir_name(compiler)
    output_dir = (args.output_dir.resolve() if args.output_dir else (REPO_ROOT / "pch" / target_subdir / "prepared"))
    output_dir.mkdir(parents=True, exist_ok=True)

    selected = set(args.only or ["libc", "libtcc"])
    manifest = {
        "compiler": str(compiler),
        "target_subdir": target_subdir,
        "output_dir": str(output_dir),
    }

    if "libc" in selected:
        _prepare_libc(compiler, output_dir, manifest)
    if "libtcc" in selected:
        _prepare_libtcc(compiler, output_dir, manifest)

    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))

    print(f"Prepared PCH artifacts in {output_dir}")
    if "libc" in selected:
        print(f"  libc PCH:        {output_dir / 'libc-common.pch'}")
    if "libtcc" in selected:
        print(f"  libtcc PCH:      {output_dir / 'libtcc-bench.pch'}")
    print(f"  manifest:       {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
