"""-O0 generates each function once, unless it is short of registers.

The discovery walk is 15% of a self-host compile at -O0 on the board.  What it
found there -- the sizes of the scratch and nested-call save areas -- -O0 takes as
fixed.  Optimised levels keep the walk: their frames are sized exactly.
"""

import os
import re
import subprocess

import pytest

from conftest import _find_compiler

SOURCE = """
typedef unsigned long long u64;
extern u64 next(u64 a, int b);
extern int many(int a, int b, int c, int d, int e, int f);
int stack_args(int a, int b, int c, int d, int e, int f) { return many(a, b, c, d, e, f); }
u64 wide(u64 a, int b) { if (b) a <<= 3; return next(next(a, b), b) + 1; }
int branches(int a) { int s = 0; while (a--) if (a & 1) s += a; return s; }
int big_frame(int a) { volatile char p[300]; p[a] = 1; return p[0]; }
int divide(int a, int b) { return a / b; }
"""


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def walks(tcc, tmp_path, level):
    src = tmp_path / "t.c"
    src.write_text(SOURCE)
    env = dict(os.environ, TCC_PASS_TIMING="1", ASAN_OPTIONS="detect_leaks=0")
    r = subprocess.run([tcc, level, "-nostdlib", "-c", str(src), "-o", str(tmp_path / "t.o")],
                       capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    out = r.stdout + r.stderr
    found = {m[1]: int(m[2]) for m in re.finditer(r"PASS_TIME (cg:\w+)\s+\d+\s+\d+\s+(\d+)", out)}
    assert found.get("cg:emit") == 5, out  # a run that reports nothing would pass below
    return found


def test_o0_does_not_walk_twice(tcc, tmp_path):
    assert "cg:dry" not in walks(tcc, tmp_path, "-O0")


def test_o2_keeps_the_walk(tcc, tmp_path):
    assert walks(tcc, tmp_path, "-O2").get("cg:dry", 0) > 0
