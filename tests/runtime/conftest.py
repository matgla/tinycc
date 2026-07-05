"""Shared pytest configuration for the runtime-library coverage layer."""

from pathlib import Path

import pytest

RUNTIME_DIR = Path(__file__).parent
TINYCC_DIR = RUNTIME_DIR / "../.."


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


def pytest_addoption(parser):
    # --compiler is normally provided by the parent tests/conftest.py, but that
    # conftest is not loaded when pytest is invoked from inside tests/runtime/
    # (as `make test-runtime` does). Register it here too, tolerating the
    # duplicate when both conftests are active (running from tests/).
    try:
        parser.addoption(
            "--compiler",
            action="store",
            default=None,
            help="Path to the armv8m-tcc cross compiler",
        )
    except ValueError:
        pass


def pytest_configure(config):
    """Register custom markers used by the runtime test layers."""
    config.addinivalue_line("markers", "runtime: runtime-library coverage test")
    config.addinivalue_line("markers", "runtime_host: host-native runtime test")
    config.addinivalue_line("markers", "runtime_cross: cross-compiled runtime test")


@pytest.fixture(scope="session")
def runtime_compiler(pytestconfig):
    return _find_compiler(pytestconfig.getoption("compiler", default=None))


@pytest.fixture(scope="session")
def tinycc_root():
    return TINYCC_DIR
