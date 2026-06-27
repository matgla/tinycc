"""Shared pytest configuration for the frontend coverage layer."""

from pathlib import Path

import pytest


def pytest_addoption(parser):
    parser.addoption(
        "--update",
        action="store_true",
        default=False,
        help="Regenerate golden files from current compiler output",
    )


def _find_compiler(compiler_override=None):
    """Resolve the cross compiler using the requested fallback chain."""
    if compiler_override is not None:
        p = Path(compiler_override)
        if not p.exists():
            raise FileNotFoundError(f"--compiler not found: {p}")
        return p

    tinycc = Path(__file__).parent.parent.parent
    candidates = [
        tinycc / "bin" / "armv8m-tcc",
        tinycc / "armv8m-tcc",
    ]
    for cand in candidates:
        if cand.exists():
            return cand
    raise FileNotFoundError(
        "No armv8m-tcc cross compiler found. "
        "Build one with `make cross` in libs/tinycc, or pass --compiler."
    )


def pytest_configure(config):
    """Register custom markers used by the frontend test layers."""
    config.addinivalue_line("markers", "frontend: frontend coverage test")
    config.addinivalue_line("markers", "frontend_pp: preprocessor/lexer test")
    config.addinivalue_line("markers", "frontend_types: type-system / semantic test")
    config.addinivalue_line(
        "markers", "frontend_diagnostics: expected-error diagnostic test"
    )


@pytest.fixture(scope="session")
def frontend_compiler(pytestconfig):
    return _find_compiler(pytestconfig.getoption("compiler"))
