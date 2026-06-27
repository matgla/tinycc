"""Shared pytest options for the tinycc tests tree."""


def pytest_addoption(parser):
    parser.addoption(
        "--compiler",
        action="store",
        default=None,
        help="Path to the armv8m-tcc cross compiler",
    )
