"""-Wcross-tu: a global symbol whose type differs between units, reported at
the link.

With -Wcross-tu each unit records its global symbols' types (normalised C,
frontend/gen/decl/typesig.c): with -c in a non-allocated ".tcc.typesig"
section (only then: objects without it stay byte-identical), kept by
`tcc -ar`, merged by `-r`, never in a linked output.  The link (with
-Wcross-tu) compares them: "'f' declared as 'int (int)' at api.h:1 in a.c,
defined as 'long (int)' at b.c:2", and the layouts of the named structs those
types reach: "'struct s' has different layouts at api.h:4 in a.c and at b.c:1
('mk' uses it): member 'a' is 'int' at offset 0 in a.c, 'long long' at offset
0 in b.c".  Off by default, not in -Wall.
"""

import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
BASE_TCC_ENV = "TCC_BASELINE"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

USE = """\
#include "api.h"
int use(void) { return f(1) + arr[0] + g("x") + mk()->a + (int)h(2); }
"""

HEADER = """\
int f(int);
extern int arr[];
int g(const char *, ...);
struct s { int a; };
struct s *mk(void);
long h();
"""

DEF_OK = """\
struct s { int a; };
int f(const int x) { return x; }
int arr[10];
int g(const char *s, ...) { return s[0]; }
struct s *mk(void) { return 0; }
long h(int x) { return x; }
"""

START = "void _start(void) {}\n"


def _tcc(tmp_path, *args):
    # the link's runtime libraries (libtcc1, fp/lib*.a) are in the tree, not
    # where the cross is configured to install them (test_yasc_error_names.py)
    libs = [] if args[0] == "-ar" else [*INCLUDE_FLAGS, f"-B{TCC.parent}", f"-L{TCC.parent}/lib", f"-L{TCC.parent}/lib/fp"]
    return subprocess.run([str(TCC), *libs, *map(str, args)], capture_output=True, text=True, cwd=tmp_path)


def _setup(tmp_path, defs, use=USE, header=HEADER):
    (tmp_path / "api.h").write_text(header)
    (tmp_path / "a.c").write_text(use)
    (tmp_path / "b.c").write_text(defs)
    (tmp_path / "st.c").write_text(START)


def _objects(tmp_path, *flags):
    for name in ("a", "b", "st"):
        proc = _tcc(tmp_path, "-ccompat", *flags, "-c", f"{name}.c", "-o", f"{name}.o")
        assert proc.returncode == 0, proc.stderr


def _link(tmp_path, *flags, inputs=("a.o", "b.o", "st.o")):
    return _tcc(tmp_path, "-ccompat", *flags, *inputs, "-nostdlib", "-o", "out")


def _warnings(proc):
    return [l for l in proc.stderr.splitlines() if "warning:" in l or "error:" in l]


def test_compatible_is_quiet(tmp_path):
    _setup(tmp_path, DEF_OK)
    _objects(tmp_path, "-Wcross-tu")
    proc = _link(tmp_path, "-Wcross-tu")
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == []


def test_return_type_mismatch(tmp_path):
    _setup(tmp_path, DEF_OK.replace("int f(const int x)", "long f(int x)"))
    _objects(tmp_path, "-Wcross-tu")
    proc = _link(tmp_path, "-Wcross-tu")
    assert proc.returncode == 0
    assert _warnings(proc) == [
        "tcc: warning: 'f' declared as 'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:2"
    ]


@pytest.mark.parametrize(
    "old,new,msg",
    [
        ("int arr[10];", "short arr[10];", "'arr' declared as 'int []' at api.h:2 in a.c, defined as 'short [10]' at b.c:3"),
        (
            "int g(const char *s, ...)",
            "int g(char *s, ...)",
            "'g' declared as 'int (const char *, ...)' at api.h:3 in a.c, defined as 'int (char *, ...)' at b.c:4",
        ),
        (
            "struct s *mk(void)",
            "struct s *mk(int n)",
            "'mk' declared as 'struct s *(void)' at api.h:5 in a.c, defined as 'struct s *(int)' at b.c:5",
        ),
        ("long h(int x)", "int h(int x)", "'h' declared as 'long ()' at api.h:6 in a.c, defined as 'int (int)' at b.c:6"),
    ],
)
def test_mismatches(tmp_path, old, new, msg):
    _setup(tmp_path, DEF_OK.replace(old, new))
    _objects(tmp_path, "-Wcross-tu")
    proc = _link(tmp_path, "-Wcross-tu")
    assert _warnings(proc) == ["tcc: warning: " + msg]


def test_two_declarations(tmp_path):
    _setup(tmp_path, DEF_OK)
    (tmp_path / "c.c").write_text("double f(double);\nint other(void) { return (int)f(1.0); }\n")
    _objects(tmp_path, "-Wcross-tu")
    assert _tcc(tmp_path, "-ccompat", "-Wcross-tu", "-c", "c.c", "-o", "c.o").returncode == 0
    proc = _link(tmp_path, "-Wcross-tu", inputs=("a.o", "c.o", "st.o"))
    assert "'f' declared as 'int (int)' at api.h:1 in a.c and as 'double (double)' at c.c:1" in proc.stderr
    proc = _link(tmp_path, "-Wcross-tu", inputs=("a.o", "b.o", "c.o", "st.o"))
    assert "'f' declared as 'double (double)' at c.c:1, defined as 'int (int)' at b.c:2" in proc.stderr


def test_one_command(tmp_path):
    _setup(tmp_path, DEF_OK.replace("int f(const int x)", "long f(int x)"))
    proc = _tcc(tmp_path, "-ccompat", "-Wcross-tu", "a.c", "b.c", "st.c", "-nostdlib", "-o", "out")
    assert _warnings(proc) == [
        "tcc: warning: 'f' declared as 'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:2"
    ]


def test_werror_and_wno(tmp_path):
    _setup(tmp_path, DEF_OK.replace("int f(const int x)", "long f(int x)"))
    _objects(tmp_path, "-Wcross-tu")
    proc = _link(tmp_path, "-Werror=cross-tu")
    assert proc.returncode == 1
    assert "error: 'f' declared as 'int (int)'" in proc.stderr
    proc = _link(tmp_path, "-Werror", "-Wcross-tu", "-Wno-error=cross-tu")
    assert proc.returncode == 0 and "warning: 'f'" in proc.stderr
    # the link decides: without -Wcross-tu nothing is compared
    proc = _link(tmp_path)
    assert proc.returncode == 0 and proc.stderr == ""
    proc = _link(tmp_path, "-Wcross-tu", "-Wno-cross-tu")
    assert proc.stderr == ""


def test_not_in_wall_or_default(tmp_path):
    _setup(tmp_path, DEF_OK.replace("int f(const int x)", "long f(int x)"))
    _objects(tmp_path, "-Wall")
    proc = _link(tmp_path, "-Wall", "-Wcross-tu")
    assert proc.stderr == ""  # the objects carry no signatures


def test_yasc_and_c_agree(tmp_path):
    header = "char *dup_name(const char *s);\n"
    use = '#include "api.h"\nchar *first(void) { return dup_name("a"); }\n'
    defs = (
        "#include <stdlib.h>\n"
        "char * owned dup_name(const char * view s) { char * owned p = malloc(2); p[0] = s[0]; p[1] = 0; return p; }\n"
    )
    _setup(tmp_path, defs, use=use, header=header)
    (tmp_path / "a.c").write_text(use)
    assert _tcc(tmp_path, "-ccompat", "-Wcross-tu", "-c", "a.c", "-o", "a.o").returncode == 0
    proc = _tcc(tmp_path, "-Wcross-tu", "-c", "b.c", "-o", "b.o")
    assert proc.returncode == 0, proc.stderr
    assert _tcc(tmp_path, "-c", "st.c", "-o", "st.o").returncode == 0
    proc = _link(tmp_path, "-Wcross-tu")
    assert "cross" not in proc.stderr and "declared as" not in proc.stderr


def test_archive_and_r(tmp_path):
    _setup(tmp_path, DEF_OK.replace("int f(const int x)", "long f(int x)"))
    _objects(tmp_path, "-Wcross-tu")
    assert _tcc(tmp_path, "-ar", "rcs", "libb.a", "b.o").returncode == 0
    proc = _link(tmp_path, "-Wcross-tu", inputs=("a.o", "st.o", "libb.a"))
    assert "'f' declared as 'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:2" in proc.stderr
    proc = _tcc(tmp_path, "-ccompat", "-Wcross-tu", "-r", "a.o", "b.o", "-o", "r.o")
    assert proc.returncode == 0 and proc.stderr == ""
    proc = _link(tmp_path, "-Wcross-tu", inputs=("r.o", "st.o"))
    assert "'f' declared as 'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:2" in proc.stderr


def test_output_unchanged(tmp_path):
    """the signatures never reach a linked output; -c without the flag is unchanged"""
    _setup(tmp_path, DEF_OK)
    _objects(tmp_path, "-Wcross-tu")
    assert _link(tmp_path, "-Wcross-tu").returncode == 0
    with_sigs = (tmp_path / "out").read_bytes()
    _objects(tmp_path)
    assert _link(tmp_path).returncode == 0
    assert (tmp_path / "out").read_bytes() == with_sigs
    for opt in ("-O0", "-O2"):
        _tcc(tmp_path, "-ccompat", opt, "-c", "b.c", "-o", "x.o")
        _tcc(tmp_path, "-ccompat", opt, "-Wno-cross-tu", "-c", "b.c", "-o", "y.o")
        assert (tmp_path / "x.o").read_bytes() == (tmp_path / "y.o").read_bytes()
        _tcc(tmp_path, "-ccompat", opt, "-Wcross-tu", "-c", "b.c", "-o", "z.o")
        assert b".tcc.typesig" in (tmp_path / "z.o").read_bytes()


# ---------------------------------------------------------------------------
# struct / union layouts: the named structs a global's type reaches, as each
# unit lays them out (C's compatible types across units)

LAYOUT_A = """\
struct t { int x; };
struct s { int a; struct t *n; char c; };
struct node { struct node *next; struct t v; };
struct opaque;
int use(struct s *p);
int walk(struct node *);
int peek(struct opaque *);
int a_main(void) { return use(0) + walk(0) + peek(0); }
"""

LAYOUT_B = """\
struct t { int x; };
struct s { int a; struct t *n; char c; };
struct node { struct node *next; struct t v; };
struct opaque { char secret[8]; };
int use(struct s *p) { return p->a; }
int walk(struct node *n) { return n != 0; }
int peek(struct opaque *o) { return o->secret[0]; }
int again(struct s *p) { return p->a; } /* b.c's own: compared with nothing */
"""


def _layout(tmp_path, a, b, *flags):
    (tmp_path / "a.c").write_text(a)
    (tmp_path / "b.c").write_text(b)
    (tmp_path / "st.c").write_text(START)
    _objects(tmp_path, "-Wcross-tu")
    return _link(tmp_path, "-Wcross-tu", *flags)


def test_layouts_equal_is_quiet(tmp_path):
    """the same layouts, a cycle (struct node), a struct incomplete in one unit"""
    proc = _layout(tmp_path, LAYOUT_A, LAYOUT_B)
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == []


@pytest.mark.parametrize(
    "old,new,msg",
    [
        (
            "struct s { int a; struct t *n; char c; };",
            "struct s { long long a; struct t *n; char c; };",
            "'struct s' has different layouts at b.c:2 and at a.c:2 ('use' uses it): "
            "member 'a' is 'long long' at offset 0 in b.c, 'int' at offset 0 in a.c",
        ),
        (
            "struct s { int a; struct t *n; char c; };",
            "struct s { int a; struct t *n; char d; };",
            "'struct s' has different layouts at b.c:2 and at a.c:2 ('use' uses it): "
            "member 3 is 'd' ('char' at offset 8) in b.c, 'c' ('char' at offset 8) in a.c",
        ),
        (
            "struct s { int a; struct t *n; char c; };",
            "struct s { int a; struct t *n; char c; int more; };",
            "'struct s' has different layouts at b.c:2 and at a.c:2 ('use' uses it): "
            "member 'more' ('int' at offset 12) is only in b.c",
        ),
        (
            "struct s { int a; struct t *n; char c; };",
            "struct __attribute__((aligned(16))) s { int a; struct t *n; char c; };",
            "'struct s' has different layouts at b.c:2 and at a.c:2 ('use' uses it): "
            "size/alignment 16/16 in b.c, 12/4 in a.c",
        ),
        # through a pointer member, reported once although two structs reach it
        # and 'struct node' moves only because of it
        (
            "struct t { int x; };",
            "struct t { unsigned x:3; };",
            "'struct t' has different layouts at b.c:1 and at a.c:1 ('use' uses it): "
            "member 'x' is 'unsigned int:3' at offset 0 bit 0 in b.c, 'int' at offset 0 in a.c",
        ),
    ],
)
def test_layout_differs(tmp_path, old, new, msg):
    proc = _layout(tmp_path, LAYOUT_A, LAYOUT_B.replace(old, new))
    assert proc.returncode == 0, proc.stderr
    assert _warnings(proc) == ["tcc: warning: " + msg]


def test_layout_in_header_and_werror(tmp_path):
    header = "struct s { int a; int b; };\nint use(struct s *);\n"
    (tmp_path / "api.h").write_text(header)
    use = '#include "api.h"\nint u(void) { return use(0); }\n'
    defs = "\nstruct s { int a; short b; };\nint use(struct s *p) { return p->b; }\n"
    proc = _layout(tmp_path, use, defs, "-Werror=cross-tu")
    assert proc.returncode == 1
    assert _warnings(proc) == [
        "tcc: error: 'struct s' has different layouts at b.c:2 and at api.h:1 in a.c ('use' uses it): "
        "member 'b' is 'short' at offset 4 in b.c, 'int' at offset 4 in a.c"
    ]


def test_anonymous_struct_written_out(tmp_path):
    a = "typedef struct { int p; int q; } pair;\nextern pair ga;\nint u(void) { return ga.p; }\n"
    b = "typedef struct { int p; long q; } pair;\npair ga;\n"
    proc = _layout(tmp_path, a, b)
    assert _warnings(proc) == [
        "tcc: warning: 'ga' declared as 'struct {int p; int q}' at a.c:2, defined as 'struct {int p; long q}' at b.c:2"
    ]


def test_layout_one_command_archive_and_r(tmp_path):
    b = LAYOUT_B.replace("struct t { int x; };", "struct t { short x; };")
    msg = (
        "'struct t' has different layouts at b.c:1 and at a.c:1 ('use' uses it): "
        "member 'x' is 'short' at offset 0 in b.c, 'int' at offset 0 in a.c"
    )
    proc = _layout(tmp_path, LAYOUT_A, b)
    assert _warnings(proc) == ["tcc: warning: " + msg]
    proc = _tcc(tmp_path, "-ccompat", "-Wcross-tu", "a.c", "b.c", "st.c", "-nostdlib", "-o", "out")
    assert _warnings(proc) == ["tcc: warning: " + msg]
    assert _tcc(tmp_path, "-ar", "rcs", "libb.a", "b.o").returncode == 0
    proc = _link(tmp_path, "-Wcross-tu", inputs=("a.o", "st.o", "libb.a"))
    assert _warnings(proc) == ["tcc: warning: " + msg]
    assert _tcc(tmp_path, "-ccompat", "-Wcross-tu", "-r", "a.o", "b.o", "-o", "r.o").returncode == 0
    proc = _link(tmp_path, "-Wcross-tu", inputs=("r.o", "st.o"))
    assert _warnings(proc) == ["tcc: warning: " + msg]
