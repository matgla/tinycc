"""Pytest configuration for ir_tests."""


def pytest_addoption(parser):
    parser.addoption(
        "--update",
        action="store_true",
        default=False,
        help="Regenerate .expected files from current compiler output",
    )
    parser.addoption(
        "--require-dump-ir",
        action="store_true",
        default=False,
        help="Fail instead of skipping when -dump-ir-passes support is unavailable",
    )
    # --compiler is normally provided by the parent tests/conftest.py, but that
    # conftest is not loaded when pytest is invoked from inside tests/ir_tests/
    # (as `make test-golden-ir` does). Register it here too, tolerating the
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
    """Register custom markers."""
    config.addinivalue_line("markers", "gcc_torture: GCC torture tests")
    config.addinivalue_line("markers", "gcc_compile: GCC compile-only tests")
    config.addinivalue_line("markers", "gcc_execute: GCC execute tests")
    config.addinivalue_line("markers", "slow: Slow tests (long timeout)")
    config.addinivalue_line("markers", "golden_ir: golden IR snapshot test")
