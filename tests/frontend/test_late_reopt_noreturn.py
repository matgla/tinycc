"""A function is compiled again at the end of the unit only for what its first
compile could not know.

A callee found not to return after its caller was compiled lets the second compile
drop the code behind the call.  A callee already known not to return gives the same
code twice, and every caller of tcc_error() in tinycc paid for that: about half of a
compile of its largest file.  Compiled twice, a function moves to the end of .text.
"""

import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def compile_and_dump(tcc, tmp_path, source):
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(source)
    r = subprocess.run([tcc, "-O2", "-fno-pic", "-nostdlib", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return obj, subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)], capture_output=True, text=True, check=True).stdout


def text_order(obj):
    out = subprocess.run(["arm-none-eabi-nm", "-n", str(obj)], capture_output=True, text=True, check=True).stdout
    return [line.split()[-1] for line in out.splitlines() if line.split()[-2] in "tT"]


def test_callee_found_noreturn_later_still_recompiles(tcc, tmp_path):
    _, dis = compile_and_dump(
        tcc,
        tmp_path,
        """
extern void after(void);
int sink[4];
void die(int x);
void caller(int x) { die(x); after(); }
__attribute__((noinline)) void die(int x) { for (;;) { sink[x & 3] = x; x++; } }
""",
    )
    assert "after" not in dis


def test_callee_known_noreturn_does_not_recompile(tcc, tmp_path):
    obj, dis = compile_and_dump(
        tcc,
        tmp_path,
        """
extern void die(void) __attribute__((noreturn));
extern void after(void);
extern void later(void);
void caller(int x) { if (x) { later(); die(); after(); } }
void other(void) { later(); }
""",
    )
    assert "after" not in dis
    assert text_order(obj) == ["caller", "other"]
