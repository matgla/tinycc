"""Shared pytest configuration for the self-host bootstrap gate."""

from pathlib import Path

import pytest

SELFHOST_DIR = Path(__file__).parent
TINYCC_DIR = SELFHOST_DIR / "../.."

# YasOS is expected to live one directory above libs/tinycc, i.e. two levels
# above the tinycc root.
YASOS_DIR = TINYCC_DIR / "../.."


def _find_compiler(compiler_override=None):
    """Resolve the armv8m-tcc cross compiler."""
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


def _find_yasos_env():
    """Return the YasOS repo root if the FAT-drive runner is available.

    The full self-host round-trip needs the YasOS kernel and the
    qemu_fatdisk_run.py script.  In the standalone libs/tinycc checkout these
    are absent; the FAT tests skip gracefully.
    """
    yasos_root = YASOS_DIR.resolve()
    kernel = yasos_root / "zig-out" / "bin" / "yasos_kernel"
    runner = yasos_root / "scripts" / "qemu_fatdisk_run.py"
    if kernel.is_file() and runner.is_file():
        return yasos_root
    return None


def _find_native_tcc(yasos_root):
    """Locate a native tcc binary built for the YasOS guest."""
    if yasos_root is None:
        return None
    candidates = [
        # Stage-1 native bootstrap binary produced by build_rootfs.sh
        yasos_root / "libs" / "tinycc" / "bin" / "armv8m-tcc.elf",
        # Installed native compiler inside the rootfs
        yasos_root / "rootfs" / "usr" / "bin" / "tcc",
    ]
    for cand in candidates:
        if cand.is_file():
            return cand
    return None


def pytest_addoption(parser):
    # --compiler is normally provided by the parent tests/conftest.py, but that
    # conftest is not loaded when pytest is invoked from inside tests/selfhost/
    # (as `make test-selfhost` does). Register it here too, tolerating the
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

    parser.addoption(
        "--native-tcc",
        action="store",
        default=None,
        help="Path to the native YasOS tcc binary (optional)",
    )
    parser.addoption(
        "--yasos-root",
        action="store",
        default=None,
        help="Path to the YasOS repository root (optional)",
    )


def pytest_configure(config):
    config.addinivalue_line("markers", "selfhost: self-host bootstrap gate")
    config.addinivalue_line(
        "markers", "selfhost_compile: compile-only self-host smoke test"
    )
    config.addinivalue_line(
        "markers", "selfhost_fat: FAT-drive native-vs-cross round-trip test"
    )


@pytest.fixture(scope="session")
def selfhost_compiler(pytestconfig):
    return _find_compiler(pytestconfig.getoption("compiler"))


@pytest.fixture(scope="session")
def yasos_root(pytestconfig):
    override = pytestconfig.getoption("yasos_root")
    if override is not None:
        return Path(override).resolve()
    return _find_yasos_env()


@pytest.fixture(scope="session")
def native_tcc(pytestconfig, yasos_root):
    override = pytestconfig.getoption("native_tcc")
    if override is not None:
        p = Path(override)
        if not p.exists():
            raise FileNotFoundError(f"--native-tcc not found: {p}")
        return p.resolve()
    return _find_native_tcc(yasos_root)


@pytest.fixture(scope="session")
def qemu_fatdisk_runner(yasos_root):
    if yasos_root is None:
        return None
    runner = yasos_root / "scripts" / "qemu_fatdisk_run.py"
    if runner.is_file():
        return runner
    return None
