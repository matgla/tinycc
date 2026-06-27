"""Shared pytest configuration for the debug-info coverage layer."""

from pathlib import Path

import pytest

DEBUG_DIR = Path(__file__).parent
TINYCC_DIR = DEBUG_DIR / "../.."


def _find_compiler(compiler_override=None):
    """Resolve the cross compiler using the requested fallback chain."""
    if compiler_override is not None:
        p = Path(compiler_override)
        if not p.exists():
            raise FileNotFoundError(f"--compiler not found: {p}")
        return p

    candidates = [
        TINYCC_DIR / "armv8m-tcc",
        TINYCC_DIR / "bin" / "armv8m-tcc",
    ]
    for cand in candidates:
        if cand.exists():
            return cand
    raise FileNotFoundError(
        "No armv8m-tcc cross compiler found. "
        "Build one with `make cross` in libs/tinycc, or pass --compiler."
    )


def pytest_configure(config):
    """Register custom markers used by the debug test layers."""
    config.addinivalue_line("markers", "debug: debug-info coverage test")
    config.addinivalue_line("markers", "debug_dwarf: DWARF debug-info test")
    config.addinivalue_line("markers", "debug_stab: STAB debug-info test")


@pytest.fixture(scope="session")
def debug_compiler(pytestconfig):
    return _find_compiler(pytestconfig.getoption("compiler"))
