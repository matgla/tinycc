"""-Wuse-after-free (-Wall): plain C's released pointers (ir/bounds.c).

The value-range walk of the raw IR also carries a "released on every path"
bit: a call of a deallocator (free, or one a malloc(dealloc, i) attribute
names; not realloc, whose argument lives on when it fails) sets it on the
vreg it passes and the variable that vreg copies; copies and p + k keep it,
any other assignment clears it.  A load or store through such a pointer, or
a second release, is reported -- on a path that certainly runs.  Only the
pointer itself is followed, not other copies made before the release
(`q = p; free(p); *q` is not seen), and a release on some paths only says
nothing.  yasc's -Wlifetime does this for owned pointers, so the check is
off where -Wlifetime is on.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
typedef unsigned long size_t;
void *malloc(size_t);
void *realloc(void *, size_t);
void free(void *);
typedef struct F F;
int fclose_(F *);
F *fopen_(const char *) __attribute__((malloc, malloc(fclose_, 1)));
int fgetc_(F *);
struct N { struct N *next; int v; };
int g(int);
"""
PL = PRELUDE.count("\n")


def _run(tmp_path, body, *flags, ccompat=True, prelude=PRELUDE):
    src = tmp_path / "t.c"
    src.write_text(prelude + body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / "t.o")]
    return subprocess.run(cmd, capture_output=True, text=True)


def _diags(proc):
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[1].isdigit() and parts[2].strip() != "note":
            out.append((int(parts[1]) - PL, parts[3].strip()))
    return out


HITS = """\
int h1(void) { int *p = malloc(16); p[0] = 1; free(p); return p[0]; }
int h2(void) { int *p = malloc(16); free(p); free(p); return 0; }
int h3(struct N *n) { free(n); return n->v; }
int h4(struct N *n) { free(n); n = n->next; return 0; }
int h5(int *p, int c) { free(p); if (c) return 0; return *p; }
int h6(void) { F *f = fopen_("x"); fclose_(f); return fgetc_(f) + ((char *)f)[0]; }
int h7(int *p) { free(p); for (int i = 0; i < 4; i++) g(i); p[1] = 2; return 0; }
int h8(char *s) { free(s); while (*s) s++; return 0; }
"""
HITS_EXPECT = [
    (1, "'p' is used after it was freed"),
    (2, "'p' is freed twice"),
    (3, "'n' is used after it was freed"),
    (4, "'n' is used after it was freed"),
    (5, "'p' is used after it was freed"),
    (6, "'f' is used after it was freed"),
    (7, "'p' is used after it was freed"),
    (8, "'s' is used after it was freed"),
]

MISSES = """\
int m1(struct N *n) { while (n) { struct N *nx = n->next; free(n); n = nx; } return 0; }
int m2(int c) { int *p = malloc(4); if (c) free(p); return *p; }
int m3(void) { int *p = malloc(4); free(p); p = malloc(4); return *p; }
int m4(int *p) { int *q = realloc(p, 8); if (!q) { free(p); return 0; } free(q); return 1; }
int m5(int *p) { free(p); p = 0; return p ? *p : 0; }
int m6(struct N *n) { int v = n->v; free(n); return v; }
int m7(int **pp) { free(*pp); *pp = 0; return 0; }
int m8(int *p, int c) { for (int i = 0; i < c; i++) { free(p); p = malloc(4); *p = i; } return *p; }
int m9(int *p) { if (p) free(p); return 0; }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_hits(tmp_path, opt):
    proc = _run(tmp_path, HITS, "-Wall", opt)
    assert _diags(proc) == HITS_EXPECT, proc.stderr


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_misses(tmp_path, opt):
    proc = _run(tmp_path, MISSES, "-Wall", opt)
    assert "freed" not in proc.stderr, proc.stderr


def test_switches(tmp_path):
    body = "int f(int *p) { free(p); return *p; }\n"
    msg = "'p' is used after it was freed"
    for flags in ([], ["-Wall", "-Wno-use-after-free"], ["-Wall", "-w"]):
        assert msg not in _run(tmp_path, body, *flags).stderr, flags
    for flags in (["-Wall"], ["-Wuse-after-free"]):
        assert "warning: " + msg in _run(tmp_path, body, *flags).stderr, flags
    proc = _run(tmp_path, body, "-Werror=use-after-free")
    assert proc.returncode != 0 and "error: " + msg in proc.stderr, proc.stderr


def test_lifetime_says_it_in_yasc(tmp_path):
    """yasc with -Wlifetime (the safe default) has its own words for it."""
    (tmp_path / "alloc.h").write_text("void free(void *p);\n"
                                      "void *malloc(unsigned long n) __attribute__((malloc, malloc(free, 1)));\n")
    body = ('#include "alloc.h"\n'
            "int f(void) { int *maybe owned p = malloc(4); if (!p) return 0; free(p); return *p; }\n")
    proc = _run(tmp_path, body, ccompat=False, prelude="")
    assert "used after it was freed" in proc.stderr, proc.stderr
    assert proc.stderr.count("used after it was freed") == 1, proc.stderr


def test_objects_unchanged(tmp_path):
    for opt in ("-O0", "-O2"):
        objs = []
        for flags in (["-w"], ["-Wall"]):
            out = tmp_path / ("o%d.o" % len(objs))
            src = tmp_path / "b.c"
            src.write_text(PRELUDE + HITS + MISSES)
            proc = subprocess.run([str(TCC), "-ccompat", opt, *flags, "-c", str(src), "-o", str(out)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            objs.append(out.read_bytes())
        assert objs[0] == objs[1], opt
