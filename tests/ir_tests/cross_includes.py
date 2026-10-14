"""The include setup for tests that run armv8m-tcc on a C file themselves.

Mirrors the QEMU Makefile's compile step: the newlib headers checked out with
the tests, then tcc's own include/.  -nostdinc drops the compiler's default
search path -- the configured install dir and the build host's /usr/include --
so a fixture never compiles against a stale `make install` or host glibc
headers that exist on one machine and not on another (CI).
"""
from pathlib import Path

IR_TESTS = Path(__file__).resolve().parent
ROOT = IR_TESTS.parents[1]  # libs/tinycc
LIBC_INCLUDES = IR_TESTS / "libc_includes"

INCLUDE_FLAGS = (
    "-nostdinc",
    f"-I{LIBC_INCLUDES}",
    f"-I{IR_TESTS / 'libc_imports'}",
    f"-I{LIBC_INCLUDES / 'newlib'}",
    f"-I{ROOT / 'include'}",
)
