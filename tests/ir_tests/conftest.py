"""Pytest configuration for ir_tests."""


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "gcc_torture: GCC torture tests")
    config.addinivalue_line("markers", "gcc_compile: GCC compile-only tests")
    config.addinivalue_line("markers", "gcc_execute: GCC execute tests")
    config.addinivalue_line("markers", "slow: Slow tests (long timeout)")
