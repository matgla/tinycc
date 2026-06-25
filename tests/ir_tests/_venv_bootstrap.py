"""Local venv bootstrap for `tests/ir_tests` helper scripts.

Goal:
- Make it easy to run scripts like `profile_suite.py` / `profile_compare.py` directly
  without manually creating a virtualenv.

Behavior:
- If not running inside a virtualenv, create `tests/ir_tests/.venv` if needed and
  re-exec the current script under that interpreter.
- Once running inside the venv, install `tests/ir_tests/requirements.txt` if the
  content hash changed since last install.

This module is intentionally stdlib-only.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path


def _is_venv() -> bool:
    # Standard venv detection.
    if getattr(sys, "real_prefix", None) is not None:
        return True
    return sys.prefix != sys.base_prefix


def _venv_python(venv_dir: Path) -> Path:
    if os.name == "nt":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def _requirements_hash(requirements_path: Path) -> str:
    content = requirements_path.read_bytes()
    return hashlib.sha256(content).hexdigest()


def _install_requirements_if_needed(venv_dir: Path, requirements_path: Path) -> None:
    if not requirements_path.exists():
        return

    marker = venv_dir / ".requirements.sha256"
    desired = _requirements_hash(requirements_path)
    current = marker.read_text().strip() if marker.exists() else ""

    if current == desired:
        return

    subprocess.check_call(
        [sys.executable, "-m", "pip", "install", "-r", str(requirements_path)]
    )
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(desired + "\n")


def ensure_venv(
    *,
    project_dir: Path | None = None,
    venv_dir: Path | None = None,
    requirements_path: Path | None = None,
) -> None:
    """Ensure we're running under a local venv with requirements installed."""

    if project_dir is None:
        # `tests/ir_tests`
        project_dir = Path(__file__).resolve().parent

    if venv_dir is None:
        venv_dir = project_dir / ".venv"

    if requirements_path is None:
        requirements_path = project_dir / "requirements.txt"

    venv_python = _venv_python(venv_dir)

    if not _is_venv():
        if not venv_python.exists():
            venv_dir.mkdir(parents=True, exist_ok=True)
            subprocess.check_call([sys.executable, "-m", "venv", str(venv_dir)])

        # Re-exec this script using the venv interpreter.
        os.execv(str(venv_python), [str(venv_python), *sys.argv])

    # We are inside the venv.
    _install_requirements_if_needed(venv_dir, requirements_path)
