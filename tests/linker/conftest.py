"""Shared pytest configuration for the linker coverage layer."""

from pathlib import Path

import pytest

LINKER_DIR = Path(__file__).parent
TINYCC_DIR = LINKER_DIR / "../.."


def _find_compiler(compiler_override=None):
    """Resolve the cross compiler using the requested fallback chain."""
    if compiler_override is not None:
        p = Path(compiler_override)
        if not p.exists():
            raise FileNotFoundError(f"--compiler not found: {p}")
        return p

    candidates = [
        TINYCC_DIR / "bin" / "armv8m-tcc",
        TINYCC_DIR / "armv8m-tcc",
    ]
    for cand in candidates:
        if cand.exists():
            return cand
    raise FileNotFoundError(
        "No armv8m-tcc cross compiler found. "
        "Build one with `make cross` in libs/tinycc, or pass --compiler."
    )


def pytest_configure(config):
    """Register custom markers used by the linker test layers."""
    config.addinivalue_line("markers", "linker: linker coverage test")
    config.addinivalue_line("markers", "linker_reloc: relocation test")
    config.addinivalue_line("markers", "linker_section: section layout test")
    config.addinivalue_line("markers", "linker_yaff: YAFF output test")


@pytest.fixture(scope="session")
def linker_compiler(pytestconfig):
    return _find_compiler(pytestconfig.getoption("compiler"))


@pytest.fixture(scope="session")
def tinycc_root():
    return TINYCC_DIR
