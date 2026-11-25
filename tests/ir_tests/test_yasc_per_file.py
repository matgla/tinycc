"""The yasc dialect is a file's property, not the unit's.

yasc applies to the main file under -std=yasc (and the safe default), and to
any file from its `#pragma yasc` on.  A header without the pragma -- every
system header -- is regular C whoever includes it: owned/view/maybe/out/drop
are identifiers there.  So is the expansion of a macro defined in regular C
(NULL's `(void *)0`, a parameter named `view`), and the body of a function
defined in regular C wherever the inliner replays it.  __YASC__ is defined
for the whole unit, so a header can ask whether yasc code includes it.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

C_HEADER = """\
/* regular C: the yasc words are identifiers */
int view(int owned);
struct maybe { int out; };
#define TAKE(view) ((view) + 1)
#define MKPTR(x) ((char *)(x))
static inline int helper(int maybe)
{
  int out = maybe + 1;
  return out;
}
#ifdef __YASC__
int included_from_yasc(void);
#endif
"""

YASC_HEADER = """\
#pragma yasc
char * owned yalloc(void);
void yfree(drop char * owned p);
"""

MAIN = """\
#include "c.h"
#include "y.h"
int f(void)
{
  char * owned p = yalloc();
  char * view maybe q = MKPTR(0);
  struct maybe m = {TAKE(1)};
  (void)q;
  yfree(p);
  return view(helper(m.out)) + included_from_yasc();
}
"""


def _run(tmp_path, main, *flags):
    (tmp_path / "c.h").write_text(C_HEADER)
    (tmp_path / "y.h").write_text(YASC_HEADER)
    src = tmp_path / "m.c"
    src.write_text(main)
    return subprocess.run(
        [str(TCC), "-c", f"-I{tmp_path}", *flags, str(src), "-o", str(tmp_path / "m.o")],
        capture_output=True,
        text=True,
    )


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("mode", ["std", "pragma", "safe"])
def test_regular_c_header_in_yasc(tmp_path, opt, mode):
    """-std=yasc, a #pragma yasc in the main file after its includes, the safe
    default: the C header compiles, its inline helper too at -O1+."""
    main, flags = MAIN, []
    if mode == "std":
        flags = ["-std=yasc"]
    elif mode == "pragma":
        main = MAIN.replace('#include "y.h"\n', '#include "y.h"\n#pragma yasc\n')
        flags = ["-ccompat"]
    proc = _run(tmp_path, main, *flags, opt)
    assert proc.returncode == 0, proc.stderr
    assert "error" not in proc.stderr, proc.stderr


def test_yasc_header_in_regular_c(tmp_path):
    """A plain-C file including a yasc header stays plain C after it."""
    main = '#include "y.h"\nint owned(int view) { return view; }\nvoid g(void) { char *p = yalloc(); (void)p; }\n'
    proc = _run(tmp_path, main, "-ccompat")
    assert proc.returncode == 0, proc.stderr


def test_pragma_ends_with_its_file(tmp_path):
    """A header's #pragma yasc does not reach the file including it."""
    main = '#include "y.h"\nint out = 1;\n'
    proc = _run(tmp_path, main, "-ccompat")
    assert proc.returncode == 0, proc.stderr


def test_pragma_still_reaches_the_end_of_its_file(tmp_path):
    main = '#include "c.h"\n#pragma yasc\nint g(void)\n{\n  int out = 1;\n  return out;\n}\n'
    proc = _run(tmp_path, main, "-ccompat")
    assert proc.returncode != 0
    assert "'out' is a yasc keyword" in proc.stderr


def test_yasc_macro_stays_yasc(tmp_path):
    """A macro defined in yasc code expands as yasc wherever it is used."""
    main = '#pragma yasc\n#define OWNED_PTR(T) T * owned\nOWNED_PTR(char) make(void);\n'
    proc = _run(tmp_path, main, "-ccompat")
    assert proc.returncode == 0, proc.stderr


@pytest.mark.parametrize("opt", LEVELS[1:])
def test_regular_c_header_code_unchanged(tmp_path, opt):
    """The markers around a regular-C macro's expansion change nothing: a
    plain-C main file and the same file in yasc compile to the same object."""
    body = '#include "c.h"\n#ifdef __YASC__\n#define VIEW view\n#else\n#define VIEW\n#endif\nint g(int a)\n{\n  char * VIEW q = MKPTR(a);\n  return TAKE(helper(a)) + (q != 0);\n}\n'
    (tmp_path / "c.h").write_text(C_HEADER.replace("#ifdef __YASC__", "#if 0"))
    out = {}
    for name, flags in (("plain", ["-ccompat"]), ("yasc", ["-ccompat", "-std=yasc", "-Wno-lifetime"])):
        d = tmp_path / name
        d.mkdir()
        (d / "t.c").write_text(body)
        proc = subprocess.run(
            [str(TCC), "-c", f"-I{tmp_path}", *flags, opt, "t.c", "-o", "t.o"],
            capture_output=True,
            text=True,
            cwd=d,
        )
        assert proc.returncode == 0, proc.stderr
        out[name] = (d / "t.o").read_bytes()
    assert out["plain"] == out["yasc"]
