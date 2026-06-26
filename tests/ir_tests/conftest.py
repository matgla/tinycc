"""Pytest configuration for ir_tests."""


def pytest_addoption(parser):
    parser.addoption(
        "--update",
        action="store_true",
        default=False,
        help="Regenerate .expected files from current compiler output",
    )
    parser.addoption(
        "--compiler",
        action="store",
        default=None,
        help="Path to debug-enabled TCC binary",
    )


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "gcc_torture: GCC torture tests")
    config.addinivalue_line("markers", "gcc_compile: GCC compile-only tests")
    config.addinivalue_line("markers", "gcc_execute: GCC execute tests")
    config.addinivalue_line("markers", "slow: Slow tests (long timeout)")
    config.addinivalue_line("markers", "golden_ir: golden IR snapshot test")
