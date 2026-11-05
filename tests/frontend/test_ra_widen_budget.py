"""Register allocator liveness: only vregs that cross a block boundary cost rows.

ra_widen_intervals_by_liveness keeps per-block bit matrices that are among the
largest allocations of a compile, and past a budget it falls back to a far
coarser analysis.  Block-local temporaries need no row, so they must count
against neither.  The budget is the observable: at a cap the old row count
exceeds, the code has to stay what the precise analysis produces.
"""

import os
import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def source():
    blocks = "".join(
        f"    if (x & {1 << (i % 16)}) {{ int a{i} = x + {i}; int b{i} = a{i} * 3; "
        f"int c{i} = b{i} ^ a{i}; acc += c{i} + b{i}; }}\n"
        for i in range(200)
    )
    return f"int f(int x, int n) {{\n  int acc = 0;\n  for (int i = 0; i < n; i++) {{\n    x += acc;\n{blocks}  }}\n  return acc;\n}}\n"


def compile_text(tcc, tmp_path, cap):
    src, obj = tmp_path / "f.c", tmp_path / f"f{cap}.o"
    src.write_text(source())
    env = dict(os.environ)
    env.pop("TCC_RA_WIDEN_MAX_WORDS", None)
    if cap is not None:
        env["TCC_RA_WIDEN_MAX_WORDS"] = str(cap)
    r = subprocess.run([tcc, "-O1", "-nostdlib", "-c", str(src), "-o", str(obj)], capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    return obj.read_bytes()


def test_block_local_temporaries_do_not_count_against_the_budget(tcc, tmp_path):
    precise = compile_text(tcc, tmp_path, None)
    if compile_text(tcc, tmp_path, 1) == precise:
        pytest.skip("this compiler has no TCC_RA_WIDEN_MAX_WORDS knob, or the fallback gave the same code")
    # 200 blocks with 600+ temporaries, all local: 3 matrices of ~400 blocks by
    # the few rows that cross a block fit here; a row per temporary does not.
    assert compile_text(tcc, tmp_path, 8192) == precise
