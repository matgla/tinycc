"""A function that does not return is not worth inlining.

Nothing runs after it, so the call costs nothing that matters and the expansion is
all loss: the arguments of the call inside it, at every site, in the hot function.
The kernel's bounds and overflow checks call helpers of this kind thousands of times,
which was 51 KB of its .text.
"""

import re
import subprocess

import pytest

from conftest import _find_compiler


@pytest.fixture
def tcc(request):
    return str(_find_compiler(request.config.getoption("--compiler")).resolve())


def calls_in(tcc, tmp_path, source, flags=("-O2",)):
    """{function: [functions it calls]} of the compiled source."""
    src, obj = tmp_path / "t.c", tmp_path / "t.o"
    src.write_text(source)
    r = subprocess.run([tcc, *flags, "-fno-pic", "-nostdlib", "-c", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    dump = subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)], capture_output=True, text=True, check=True).stdout
    calls, current = {}, None
    for line in dump.splitlines():
        header = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if header:
            current = header.group(1)
            calls.setdefault(current, [])
            continue
        target = re.search(r"R_ARM_THM_(?:CALL|JUMP24|PC22)\s+(\S+)", line)
        if target and current:
            calls[current].append(target.group(1))
    assert any(calls.values()), dump  # a parse that finds nothing would pass every test
    return calls


CHECKS = """
extern void sink(const char *message, int code, int more) __attribute__((noreturn));
%s
int first(int *p, int n) { if (n > 4) fail(); return p[n]; }
int second(int *p, int n) { if (n < 0) fail(); return p[n] + 1; }
int third(int *p, int n) { if (n == 7) fail(); return p[n] + 2; }
"""


@pytest.mark.parametrize("level", ["-O1", "-O2", "-Os"])
def test_declared_noreturn_helper_stays_out_of_line(tcc, tmp_path, level):
    helper = '__attribute__((noreturn)) static void fail(void) { sink("message", 1, 2); }'
    calls = calls_in(tcc, tmp_path, CHECKS % helper, (level,))
    for function in ("first", "second", "third"):
        assert "sink" not in calls[function], calls


def test_always_inline_is_still_obeyed(tcc, tmp_path):
    helper = (
        "__attribute__((noreturn, always_inline)) static inline void fail(void)"
        ' { sink("message", 1, 2); }'
    )
    calls = calls_in(tcc, tmp_path, CHECKS % helper)
    assert "sink" in calls["first"], calls


def test_a_small_function_that_returns_is_still_inlined(tcc, tmp_path):
    source = """
    static int twice(int x) { return x * 2; }
    int first(int n) { return twice(n) + 1; }
    int second(int n) { return twice(n) + 2; }
    """
    calls = calls_in(tcc, tmp_path, source + "extern void other(void); void keep(void) { other(); }\n")
    assert "twice" not in calls["first"] and "twice" not in calls["second"], calls
