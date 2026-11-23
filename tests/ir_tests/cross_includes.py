"""The include setup for tests that run armv8m-tcc on a C file themselves.

Mirrors the QEMU Makefile's compile step: the newlib headers checked out with
the tests, then tcc's own include/.  -nostdinc drops the compiler's default
search path -- the configured install dir and the build host's /usr/include --
so a fixture never compiles against a stale `make install` or host glibc
headers that exist on one machine and not on another (CI).  -isystem: they are
the target's system headers, quiet as a real sysroot's (and newlib's
`static __inline` helpers become plain statics without __GNUC__).
"""
from pathlib import Path

IR_TESTS = Path(__file__).resolve().parent
ROOT = IR_TESTS.parents[1]  # libs/tinycc
LIBC_INCLUDES = IR_TESTS / "libc_includes"

INCLUDE_FLAGS = (
    "-nostdinc",
    f"-isystem{LIBC_INCLUDES}",
    f"-isystem{IR_TESTS / 'libc_imports'}",
    f"-isystem{LIBC_INCLUDES / 'newlib'}",
    f"-isystem{ROOT / 'include'}",
)
