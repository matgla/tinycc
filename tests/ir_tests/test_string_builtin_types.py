"""String builtins keep the declared result type (frontend/gen/builtin/string.c).

tcc redirects strchr, strrchr, strstr, strpbrk, strcpy and friends to
`__tcc_*` helpers and folds memchr of a constant string.  The call's value
must still be what the declaration says: in C, strchr returns `char *` even
for a `const char *` argument (no "discards qualifiers"), and in yasc the
result keeps its qualifiers, so `char * view maybe strchr(...)` is followed
for null as with -fno-builtin.  The -Wlifetime record of the call (what may
be null, which arguments may not) goes to the helper call too.
"""

import re
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

YASC_DECLS = """\
typedef unsigned size_t;
char * view maybe strchr(const char * view s, int c);
char * view maybe strrchr(const char * view s, int c);
char * view maybe strstr(const char * view s, const char * view n);
char * view maybe strpbrk(const char * view s, const char * view a);
char * view maybe index(const char * view s, int c);
void * view maybe memchr(const void * view s, int c, size_t n);
size_t strlen(const char * view s);
"""
YASC_LINES = YASC_DECLS.count("\n")

# each line marked HIT warns, no other line does
YASC_USES = """\
int a(const char * view s) { const char * view p = strchr(s, 'x'); return *p; } /* HIT store */
int b(const char * view s) { char * view maybe p = strrchr(s, 'x'); return *p; } /* HIT deref */
int c(const char * view s) { const char * view p = strstr(s, "ab"); return p != 0; } /* HIT store */
int d(const char * view s) { char * view maybe p = strpbrk(s, "ab"); return *p; } /* HIT deref */
int e(const char * view s) { char * view maybe p = index(s, 'x'); return *p; } /* HIT deref */
int f(const char * view s) { const char * view maybe p = memchr(s, 'x', 4); return *p; } /* HIT deref */
int g(const char * view s) { const char * view maybe p = strchr(s, 'x'); return p ? *p : 0; }
int h(const char * view s) { char * view maybe p = strstr(s, "ab"); if (!p) return 0; return *p; }
int i(char * view maybe s) { return strlen(s) != 0; } /* HIT maybe arg */
int j(const char * view s) { return strlen(s) != 0; }
"""


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True, text=True,
    )


def _warned_lines(proc, first=0):
    lines = set()
    for line in proc.stderr.splitlines():
        if "warning: " in line or "error: " in line:
            lines.add(int(line.split(":")[1]) - first)
    return sorted(lines)


def _hit_lines(body):
    return [n for n, line in enumerate(body.splitlines(), 1) if "HIT" in line]


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("builtins", [(), ("-fno-builtin",)], ids=["builtin", "no-builtin"])
def test_yasc_qualifiers_survive_the_redirect(tmp_path, level, builtins):
    # -ccompat: the flow checks' reports stay warnings (errors in safe mode)
    proc = _compile(tmp_path, YASC_DECLS + YASC_USES, "-ccompat", "-std=yasc", "-Wlifetime", "-Wall", level, *builtins)
    assert proc.returncode == 0, proc.stderr
    assert _warned_lines(proc, YASC_LINES) == _hit_lines(YASC_USES), proc.stderr


# regular C: the declared result, not the type of the converted argument
C_DECLS = """\
typedef unsigned size_t;
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *index(const char *s, int c);
char *strstr(const char *s, const char *n);
char *strpbrk(const char *s, const char *a);
char *strcpy(char *d, const char *s);
void *memchr(const void *s, int c, size_t n);
"""
C_USES = """\
char *f1(char *s, int c) { char *p = strchr(s, c); return p; }
char *f2(char *s, int c) { char *p = strrchr(s, c); return p; }
char *f3(char *s) { char *p = index(s, 'x'); return p; }
char *f4(char *s) { char *p = strstr(s, "ab"); return p; }
char *f5(char *s) { char *p = strpbrk(s, "ab"); return p; }
char *f6(char *d, const char *s) { char *p = strcpy(d, s); return p; }
char *f7(char *s) { char *p = memchr("abc", 'b', 3); return p ? p : s; }
char *f8(char *s) { char *p = memchr("abc", 'z', 3); return p ? p : s; }
int f9(const char *s) { return strchr(s, 'x') == s; }
"""


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("builtins", [(), ("-fno-builtin",)], ids=["builtin", "no-builtin"])
def test_c_result_is_the_declared_type(tmp_path, level, builtins):
    proc = _compile(tmp_path, C_DECLS + C_USES, "-ccompat", "-Wall", level, *builtins)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_c_result_in_the_safe_default(tmp_path):
    # discarded-qualifiers is an error there; a regular C header's pointers
    # are unchecked: the uses take them as views with unsafe(...) (S2)
    (tmp_path / "decls.h").write_text(C_DECLS)
    body = '#include "decls.h"\n'
    # (not f8: its memchr folds to a null constant, which -Wlifetime reports
    # stored in a pointer that is not maybe)
    # unsafe(...) takes its whole line: one statement per line; a parameter
    # written through or handed back as `char * view` says inout
    uses = "".join(re.sub(r"char \*p = (.*?); return", r"char *p = unsafe(\1); return", line)
                   .replace("(char *", "(inout char *")
                   .replace("char *", "char * view ").replace("void *", "void * view ")
                   .replace("{ ", "{\n  ").replace("; ", ";\n  ").replace(" }", "\n}") + "\n"
                   for line in C_USES.splitlines() if not line.startswith("char *f8"))
    proc = _compile(tmp_path, body + uses)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
