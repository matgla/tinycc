"""No warning in a system header, as gcc: every warning, not only the checks
that ask tcc_in_sys_header() -- the older -Wall set too (int-conversion,
incompatible-pointer-types, discarded-qualifiers,
implicit-function-declaration, return-type, ...).  error1 decides it, for
every warning, before -Werror or the safe default make one an error: an error
written as such is still given there.  A system header is one found on a
system include path (-isystem, the default ones), one after its #pragma GCC
system_header, and a body replayed from one (inline expansion, deferred
function).  -Wsystem-headers gives them again.  #warning is given there too.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

# the older -Wall warnings, each on its own line
BAD = """\
static inline int bad(int x)
{
  int *p = x;                       /* 3: int-conversion */
  char *q = p;                      /* 4: incompatible-pointer-types */
  const char *c = "x"; char *d = c; /* 5: discarded-qualifiers */
  (void)q; (void)d;
  return undeclared_fn();           /* 7: implicit-function-declaration */
}
int noret(int x) { if (x) return 1; }  /* 9: return-type */
"""

MAIN = '#include <bad.h>\nint main(void) { return bad(1) + bad(2) + noret(0); }\n'

WARNED_LINES = ["bad.h:3", "bad.h:4", "bad.h:5", "bad.h:7", "bad.h:9"]


def _run(tmp_path, *flags, header=BAD, main=MAIN, how="-isystem"):
    inc = tmp_path / "inc"
    inc.mkdir(exist_ok=True)
    (inc / "bad.h").write_text(header)
    src = tmp_path / "m.c"
    src.write_text(main)
    return subprocess.run(
        [str(TCC), "-c", how, str(inc), *flags, str(src), "-o", str(tmp_path / "m.o")],
        capture_output=True,
        text=True,
    )


def _places(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error"):
            out.append(parts[0].rsplit("/", 1)[-1])
    return out


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize(
    "flags",
    [
        ["-ccompat", "-Wall"],
        ["-ccompat", "-Wall", "-Werror"],
        ["-ccompat", "-Wall", "-Werror=int-conversion"],
        [],  # the safe default: these are errors there
    ],
    ids=["wall", "werror", "werror-one", "safe"],
)
def test_quiet_in_system_header(tmp_path, flags, opt):
    proc = _run(tmp_path, *flags, opt)
    assert proc.returncode == 0, proc.stderr
    assert _places(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_given_with_I(tmp_path, opt):
    """The same header found through -I is not a system header."""
    proc = _run(tmp_path, "-ccompat", "-Wall", opt, how="-I")
    assert proc.returncode == 0, proc.stderr
    # (each expansion of bad() at -O1+ gives them again: the open "once"
    # item of static_analysis_todo.md, not this one)
    assert sorted(set(_places(proc))) == WARNED_LINES, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_wsystem_headers(tmp_path, opt):
    proc = _run(tmp_path, "-ccompat", "-Wall", "-Wsystem-headers", opt)
    assert proc.returncode == 0, proc.stderr
    assert sorted(set(_places(proc))) == WARNED_LINES, proc.stderr
    proc = _run(tmp_path, "-ccompat", "-Wall", "-Wsystem-headers", "-Wno-system-headers", opt)
    assert _places(proc) == [], proc.stderr


def test_wsystem_headers_safe_default(tmp_path):
    """-Wsystem-headers: the safe default's errors are errors there again."""
    proc = _run(tmp_path, "-Wsystem-headers")
    assert proc.returncode != 0
    assert "bad.h:9: error: function might return no value" in proc.stderr


def test_errors_still_given(tmp_path):
    proc = _run(tmp_path, "-ccompat", header="int f(void) { return }\n", main="#include <bad.h>\n")
    assert proc.returncode != 0
    assert _places(proc) == ["bad.h:1"], proc.stderr


def test_warning_directive_given(tmp_path):
    proc = _run(tmp_path, "-ccompat", "-Wall", header="#warning from a system header\nint *p = 1;\n", main="#include <bad.h>\n")
    assert proc.returncode == 0, proc.stderr
    assert "bad.h:1: warning: #warning from a system header" in proc.stderr
    assert _places(proc) == ["bad.h:1"], proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_from_the_pragma_on(tmp_path, opt):
    """#pragma GCC system_header: the lines after it, not before."""
    header = "int *before(void) { return 1; }\n#pragma GCC system_header\n" + BAD
    main = '#include "bad.h"\nint main(void) { return !!before() + bad(1) + bad(2) + noret(0); }\n'
    proc = _run(tmp_path, "-ccompat", "-Wall", opt, header=header, main=main, how="-I")
    assert proc.returncode == 0, proc.stderr
    assert _places(proc) == ["bad.h:1"], proc.stderr


def test_main_file_still_warned(tmp_path):
    main = "#include <bad.h>\nint *g(void) { return 3; }\n"
    proc = _run(tmp_path, "-ccompat", "-Wall", main=main)
    assert _places(proc) == ["m.c:2"], proc.stderr


def test_pragma_diagnostic_does_not_reopen(tmp_path):
    """#pragma GCC diagnostic warning in the header does not give it there."""
    header = '#pragma GCC diagnostic warning "-Wint-conversion"\nint *p = 1;\n'
    proc = _run(tmp_path, "-ccompat", header=header, main="#include <bad.h>\n")
    assert _places(proc) == [], proc.stderr


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_objects_unchanged(tmp_path, opt):
    """Quiet or not, the code is the same."""
    out = []
    for flags in (["-Wsystem-headers"], ["-w"], []):
        proc = _run(tmp_path, "-ccompat", "-Wall", opt, *flags)
        assert proc.returncode == 0, proc.stderr
        out.append((tmp_path / "m.o").read_bytes())
    assert out[0] == out[1] == out[2]
