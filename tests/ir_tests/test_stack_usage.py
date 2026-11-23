"""Stack use: -Wframe-larger-than=N, -Wstack-usage=N, -fstack-usage,
-fcallgraph-info and scripts/stack_usage.py (backend/generators/stack_usage.c).

The frame is what the function's prologue takes below the caller's SP (pushed
registers, locals, spills, the outgoing argument area) plus the most the body
drops SP further (scratch pushes, a large call's own argument window).  It is
the frame of the code generated, so it may differ between -O levels; the
reports must appear at every level, and the options must never change the
object.  None of them is in -Wall or the safe default (gcc agrees).
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
SCRIPT = ROOT / "scripts" / "stack_usage.py"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]

BIG = """extern int use(char *p);
int big(int n)
{
  char buf[5000];
  buf[n] = 1;
  return use(buf);
}
int small(int a, int b) { return a + b; }
"""

VLA = """extern int use(char *p);
int vla(int n) { char b[n]; b[0] = 0; return use(b); }
"""


def _cc(tmp_path, body, *flags, name="t.c", obj="t.o", ccompat=True):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / obj)]
    return subprocess.run(cmd, capture_output=True, text=True, cwd=tmp_path)


def _frame_warnings(proc):
    return re.findall(r"the frame size of '(\w+)' is (\d+) bytes, larger than (\d+) bytes", proc.stderr)


# ---- -Wframe-larger-than= ----------------------------------------------------


@pytest.mark.parametrize("opt", LEVELS)
def test_frame_larger_than_hit(tmp_path, opt):
    proc = _cc(tmp_path, BIG, opt, "-Wframe-larger-than=4096")
    assert proc.returncode == 0, proc.stderr
    w = _frame_warnings(proc)
    assert [x[0] for x in w] == ["big"], proc.stderr
    assert 5000 <= int(w[0][1]) < 5100 and w[0][2] == "4096"
    assert "t.c:7: warning:" in proc.stderr  # the closing brace, as gcc


def test_frame_larger_than_clean_and_off(tmp_path):
    assert _frame_warnings(_cc(tmp_path, BIG, "-Wframe-larger-than=6000")) == []
    assert _frame_warnings(_cc(tmp_path, BIG)) == []
    assert _frame_warnings(_cc(tmp_path, BIG, "-Wall", "-Wextra")) == []
    assert _frame_warnings(_cc(tmp_path, BIG, "-Wframe-larger-than=100", "-Wno-frame-larger-than")) == []
    assert _frame_warnings(_cc(tmp_path, BIG, "-Wframe-larger-than=100", "-w")) == []


def test_frame_larger_than_not_in_safe_default(tmp_path):
    body = BIG.replace("char *p", "char * view p")
    proc = _cc(tmp_path, body, ccompat=False)
    assert proc.returncode == 0 and _frame_warnings(proc) == [], proc.stderr


def test_frame_larger_than_errors(tmp_path):
    proc = _cc(tmp_path, BIG, "-Werror=frame-larger-than=4096")
    assert proc.returncode != 0 and "error: the frame size of 'big'" in proc.stderr
    proc = _cc(tmp_path, BIG, "-Wframe-larger-than=4096", "-Werror")
    assert proc.returncode != 0 and "error: the frame size of 'big'" in proc.stderr
    proc = _cc(tmp_path, BIG, "-Wframe-larger-than=4096", "-Werror", "-Wno-error=frame-larger-than")
    assert proc.returncode == 0 and "warning: the frame size of 'big'" in proc.stderr
    # an error does not stop the functions after it from being checked
    proc = _cc(tmp_path, BIG + "int big2(int n) { char b[5000]; b[n] = 1; return use(b); }\n",
               "-Werror=frame-larger-than=4096")
    assert [x[0] for x in _frame_warnings(proc)] == ["big", "big2"], proc.stderr


def test_frame_larger_than_needs_a_size(tmp_path):
    for bad in ("-Wframe-larger-than", "-Wframe-larger-than=", "-Wframe-larger-than=4k", "-Wstack-usage"):
        proc = _cc(tmp_path, BIG, bad)
        assert proc.returncode != 0 and "-W" in proc.stderr, (bad, proc.stderr)


def test_frame_larger_than_pragma(tmp_path):
    body = (
        "extern int use(char *p);\n"
        "#pragma GCC diagnostic push\n"
        '#pragma GCC diagnostic ignored "-Wframe-larger-than="\n'
        "int a(int n) { char b[600]; b[n] = 1; return use(b); }\n"
        "#pragma GCC diagnostic pop\n"
        "int c(int n) { char b[600]; b[n] = 1; return use(b); }\n"
        '#pragma GCC diagnostic error "-Wframe-larger-than"\n'
        "int d(int n) { char b[600]; b[n] = 1; return use(b); }\n"
    )
    for opt in ("-O0", "-O2"):
        proc = _cc(tmp_path, body, opt, "-Wframe-larger-than=512")
        assert [x[0] for x in _frame_warnings(proc)] == ["c", "d"], proc.stderr
        assert "t.c:8: error:" in proc.stderr and proc.returncode != 0  # one-line bodies: their line


def test_frame_includes_pushes_and_outgoing_area(tmp_path):
    """The .su number is the prologue's whole drop: PUSH + SUB SP."""
    body = ("extern int f8(int, int, int, int, int, int, int, int);\n"
            "int caller(int x) { int y = f8(x, 1, 2, 3, 4, 5, 6, 7); return f8(y, x, 2, 3, 4, 5, 6, 7) + y; }\n")
    for opt in LEVELS:
        proc = _cc(tmp_path, body, opt, "-fstack-usage")
        assert proc.returncode == 0, proc.stderr
        size = int((tmp_path / "t.su").read_text().split("\t")[1])
        dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(tmp_path / "t.o")], capture_output=True, text=True)
        if dis.returncode != 0 and not dis.stdout:
            pytest.skip("no arm-none-eabi-objdump")
        drop = 0
        for line in dis.stdout.splitlines()[:12]:
            m = re.search(r"\bpush(?:\.w)?\s+\{([^}]*)\}", line)
            if m:
                drop += 4 * len(m.group(1).split(","))
            m = re.search(r"\bsub(?:\.w|w)?\s+sp, #(\d+)", line)
            if m:
                drop += int(m.group(1))
        assert size == drop and size >= 16 + 8, (opt, size, drop, dis.stdout[:800])


# ---- -Wstack-usage= -----------------------------------------------------------


@pytest.mark.parametrize("opt", LEVELS)
def test_stack_usage_warns_on_dynamic_frames(tmp_path, opt):
    proc = _cc(tmp_path, VLA + BIG.replace("extern int use(char *p);\n", ""), opt, "-Wstack-usage=4096")
    assert "stack usage of 'vla' might be unbounded" in proc.stderr, proc.stderr
    assert re.search(r"stack usage of 'big' is \d+ bytes, larger than 4096 bytes", proc.stderr), proc.stderr
    assert "'small'" not in proc.stderr
    # -Wframe-larger-than takes no notice of a frame being dynamic
    assert _frame_warnings(_cc(tmp_path, VLA, opt, "-Wframe-larger-than=4096")) == []


def test_stack_usage_switches(tmp_path):
    assert "stack usage" not in _cc(tmp_path, VLA).stderr
    assert "stack usage" not in _cc(tmp_path, VLA, "-Wall").stderr
    assert "stack usage" not in _cc(tmp_path, VLA, "-Wstack-usage=10", "-Wno-stack-usage").stderr
    proc = _cc(tmp_path, VLA, "-Werror=stack-usage=10")
    assert proc.returncode != 0 and "error: stack usage of 'vla'" in proc.stderr
    proc = _cc(tmp_path, VLA, "-Wstack-usage=10", "-Werror", "-Wno-error=stack-usage")
    assert proc.returncode == 0 and "warning: stack usage of 'vla'" in proc.stderr
    body = '#pragma GCC diagnostic ignored "-Wstack-usage="\n' + VLA
    assert "stack usage" not in _cc(tmp_path, body, "-Wstack-usage=10").stderr


# ---- -fstack-usage ------------------------------------------------------------


def _su(path, cols=False):
    rows = {}
    for line in path.read_text().splitlines():
        loc, size, qual = line.split("\t")
        file, lineno, col, name = loc.rsplit(":", 3)
        rows[name] = (Path(file).name, int(lineno), int(size), qual) + ((int(col),) if cols else ())
    return rows


@pytest.mark.parametrize("opt", LEVELS)
def test_su_file(tmp_path, opt):
    body = BIG + VLA.replace("extern int use(char *p);\n", "")
    proc = _cc(tmp_path, body, opt, "-fstack-usage", obj="x.o")
    assert proc.returncode == 0, proc.stderr
    rows = _su(tmp_path / "x.su")
    assert set(rows) == {"big", "small", "vla"}
    assert rows["big"][0] == "t.c" and rows["big"][1] == 2 and rows["big"][3] == "static"  # the name's line
    assert rows["big"][2] >= 5000
    assert rows["small"][2] < 16
    assert rows["vla"][3] == "dynamic"


# gcc 15 (arm-none-eabi, -O0) gives these .su locations for LOC: the name's
# line and 1-based byte column; a name a macro made, the expansion's line
# and column 1.
LOC = """extern int use(char *p);
static int  sq (int x) { char b[40]; b[x] = 1; return use(b) * x; }
int
big(int n)
{
  char buf[5000];
  buf[n] = 1;
  return use(buf);
}
\tint tabbed(int x) { return use((char*)0) + x; }
#define MK(n) int n(int x) { return x + 1; }
MK(made)
int (wrapped)(int a) { return a; }
int (*getf(void))(int) { return wrapped; }
int q(void) { return sq(1); }
static inline int  si (int x)
{
  char b[600]; b[x] = 1;
  return use(b) * 3;
}
int (*keep)(int) = si;
"""
LOC_GCC = {"sq": (2, 13), "big": (4, 1), "tabbed": (10, 6), "made": (12, 1), "wrapped": (13, 6), "getf": (14, 7),
           "q": (15, 5), "si": (16, 20)}


@pytest.mark.parametrize("opt", LEVELS)
def test_su_location_as_gcc(tmp_path, opt):
    """The name's line and column, also for a body replayed at the end of the
    unit (si: a static inline whose address is taken)."""
    proc = _cc(tmp_path, LOC, opt, "-fstack-usage")
    assert proc.returncode == 0, proc.stderr
    rows = _su(tmp_path / "t.su", cols=True)
    assert set(rows) >= set(LOC_GCC) - {"sq"}, rows  # sq may be inlined away
    for name, (line, col) in LOC_GCC.items():
        if name in rows:
            assert (rows[name][1], rows[name][4]) == (line, col), (opt, name, rows[name])


@pytest.mark.parametrize("opt", LEVELS)
def test_warnings_at_closing_brace(tmp_path, opt):
    proc = _cc(tmp_path, LOC, opt, "-Wframe-larger-than=512", "-Wstack-usage=512")
    assert proc.returncode == 0, proc.stderr
    lines = {(m.group(2), int(m.group(1))) for m in
             re.finditer(r"t\.c:(\d+): warning: (?:the frame size|stack usage) of '(\w+)'", proc.stderr)}
    assert lines == {("big", 9), ("si", 20)}, proc.stderr
    assert proc.stderr.count("'big'") == 2 and proc.stderr.count("'si'") == 2, proc.stderr


def test_su_bounded_window(tmp_path):
    """A call passing more than 256 bytes on the stack drops SP around itself
    alone: dynamic,bounded, and the window is in the number."""
    body = ("struct S { int a[100]; };\nextern int take(struct S s);\nextern int few(int, int, int, int, int, int);\n"
            "int dyn(struct S *p, int x) { return few(x, 1, 2, 3, 4, 5) + take(*p); }\n")
    for opt in LEVELS:
        assert _cc(tmp_path, body, opt, "-fstack-usage").returncode == 0
        name, line, size, qual = _su(tmp_path / "t.su")["dyn"]
        assert qual == "dynamic,bounded" and size >= 400, (opt, size, qual)


def test_su_default_name_and_no_file_without_option(tmp_path):
    (tmp_path / "u.c").write_text(BIG)
    proc = subprocess.run([str(TCC), "-ccompat", "-c", "-fstack-usage", "u.c"], capture_output=True, text=True,
                          cwd=tmp_path)
    assert proc.returncode == 0 and (tmp_path / "u.su").exists(), proc.stderr
    assert _cc(tmp_path, BIG, obj="v.o").returncode == 0
    assert not (tmp_path / "v.su").exists() and not (tmp_path / "v.ci").exists()


def test_su_static_inline_once(tmp_path):
    body = ("extern int use(char *p);\nstatic inline int iv(int n) { char b[64]; b[n] = 0; return use(b); }\n"
            "int a1(int n) { return iv(n) + iv(n + 1); }\nint (*keep)(int) = iv;\n")
    for opt in LEVELS:
        assert _cc(tmp_path, body, opt, "-fstack-usage").returncode == 0
        names = [line.split("\t")[0].rsplit(":", 1)[1] for line in (tmp_path / "t.su").read_text().splitlines()]
        assert sorted(names) == ["a1", "iv"], (opt, names)


# ---- -fcallgraph-info ---------------------------------------------------------


def _ci(path, taken=None):
    funcs, cur = {}, None
    for line in path.read_text().splitlines():
        w = line.split()
        if not w or w[0] == "#":
            continue
        if w[0] == "function":
            cur = funcs[w[1]] = {"bytes": int(w[3]), "qual": w[4], "local": "local" in w, "call": [], "tailcall": [],
                                 "indirect": 0, "sigs": [], "where": w[2]}
        elif w[0] in ("call", "tailcall"):
            cur[w[0]].append(w[1])
        elif w[0] == "type":
            cur["type"] = w[1]
        elif w[0] == "address-taken":
            if taken is not None:
                taken.append(" ".join(w[1:]))
        else:
            cur["indirect"] += int(w[1])
            cur["sigs"].append((w[0], int(w[1]), w[2]))
    return funcs


CG = """extern int g(int);
extern int (*fp)(int);
extern double h(double);
static int st(int x) { char b[200]; b[x] = 0; return g(b[1]); }
int t(int x) { return g(x); }
int u(int x) { return fp(x) + 1; }
int w(int x) { return st(x) + g(x + 1) + (int)h(x); }
long long dv(long long a, long long b) { return a / b; }
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_ci_file(tmp_path, opt):
    proc = _cc(tmp_path, CG, opt, "-fcallgraph-info=su", "-fno-inline-functions", "-fno-inline-small-functions",
               "-fno-inline-functions-called-once")
    assert proc.returncode == 0, proc.stderr
    funcs = _ci(tmp_path / "t.ci")
    assert not (tmp_path / "t.su").exists()  # -fstack-usage writes that
    assert funcs["u"]["indirect"] == 1 and funcs["u"]["call"] == []
    assert "g" in funcs["w"]["call"] + funcs["w"]["tailcall"] and "h" in funcs["w"]["call"]
    assert "__aeabi_ldivmod" in funcs["dv"]["call"] + funcs["dv"]["tailcall"]  # a helper the backend calls
    if "st" in funcs:
        assert funcs["st"]["local"] and not funcs["w"]["local"]
    assert "g" in funcs["t"]["call"] + funcs["t"]["tailcall"]
    if opt != "-O0":
        assert funcs["t"]["tailcall"] == ["g"] and funcs["t"]["bytes"] == 0


IND = """typedef int (*op_t)(int);
struct S { int v; };
extern int use(char *p);
static int sq(int x) { char b[40]; b[x] = 1; return use(b) * x; }
int neg(int x) { char b[100]; b[x] = 1; return -use(b); }
int twice(int a, int b) { return a + b; }
int by(const struct S *a) { return a->v; }
int apply(op_t f, int x) { return f(x) + 1; }
op_t table[] = { neg };
int (*cmp)(const void *) = (int (*)(const void *))by;
int run(int x) { op_t g = sq; return apply(g, x) + table[x & 1](x) + cmp(&x); }
double dv(double (*h)(double, ...), float (*k)(void), double d) { return h(d) + k(); }
"""


@pytest.mark.parametrize("opt", LEVELS)
def test_ci_indirect_types(tmp_path, opt):
    proc = _cc(tmp_path, IND, opt, "-fcallgraph-info", "-fno-inline-functions", "-fno-inline-small-functions",
               "-fno-inline-functions-called-once")
    assert proc.returncode == 0, proc.stderr
    taken = []
    funcs = _ci(tmp_path / "t.ci", taken)
    assert (tmp_path / "t.ci").read_text().startswith("# tcc callgraph-info 2\n")
    assert funcs["neg"]["type"] == "i(i)" and funcs["twice"]["type"] == "i(i,i)"
    assert funcs["by"]["type"] == "i(i)"  # every pointer is an i: the shape, not the C type
    assert {x[2] for x in funcs["apply"]["sigs"]} == {"i(i)"}
    assert {x[2] for x in funcs["dv"]["sigs"]} == {"d(d,...)", "f()"}
    # neg in a table, sq through a local, by through a cast: their addresses
    # are taken; twice is never referred to, apply only called
    assert sorted(taken) == ["by", "neg", "sq local"], taken


def test_ci_and_su_together(tmp_path):
    assert _cc(tmp_path, CG, "-O1", "-fstack-usage", "-fcallgraph-info").returncode == 0
    su, ci = _su(tmp_path / "t.su"), _ci(tmp_path / "t.ci")
    assert {k: v[2] for k, v in su.items()} == {k: v["bytes"] for k, v in ci.items()}


# ---- the options never change the object --------------------------------------

IDENTITY = sorted(CURRENT_DIR.glob("[0-9]*.c"))[::40]
ALL_STACK = ["-fstack-usage", "-fcallgraph-info=su", "-Wframe-larger-than=16", "-Wstack-usage=16", "-Wvla", "-Walloca"]


@pytest.mark.parametrize("opt", LEVELS)
def test_objects_byte_identical(tmp_path, opt):
    compared = 0
    for src in IDENTITY + [None]:
        if src is None:
            text, name = BIG + VLA.replace("extern int use(char *p);\n", "") + CG, "own.c"
        else:
            text, name = src.read_text(errors="replace"), src.name
        a = _cc(tmp_path, text, opt, name=name, obj="a.o")
        if a.returncode != 0:
            continue
        b = _cc(tmp_path, text, opt, *ALL_STACK, name=name, obj="b.o")
        assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes(), (name, opt)
        compared += 1
    assert compared >= 10


# ---- scripts/stack_usage.py ---------------------------------------------------

PROG_A = """int deep(int n);
int leafy(int x);
extern int lib(int);
int (*cb)(int);
static int helper(int x) { char b[100]; b[x] = 0; return leafy(b[1]); }
int main(void) { return helper(1); }
int rec(void) { return deep(3); }
int ind(void) { return cb(1); }
int unk(void) { char b[32]; b[0] = 0; return lib(b[0]); }
int dyn(int n) { char b[n]; b[0] = 0; return leafy(b[0]); }
"""
PROG_B = """static int helper(int x) { char b[300]; b[x] = 0; return b[2]; }
int leafy(int x) { char b[64]; b[x] = 0; return b[x] + helper(x); }
int deep(int n) { return n ? deep(n - 1) : 0; }
"""


def _script(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


@pytest.fixture
def prog(tmp_path):
    for name, body in (("a.c", PROG_A), ("b.c", PROG_B)):
        proc = _cc(tmp_path, body, "-O0", "-fstack-usage", "-fcallgraph-info=su", name=name, obj=name[0] + ".o")
        assert proc.returncode == 0, proc.stderr
    return tmp_path


def test_script_sums_the_worst_path(prog):
    ci = _ci(prog / "a.ci")
    cib = _ci(prog / "b.ci")
    proc = _script(prog)
    assert proc.returncode == 0, proc.stderr
    # main -> helper (a.c's, static) -> leafy -> helper (b.c's, static)
    want = ci["main"]["bytes"] + ci["helper"]["bytes"] + cib["leafy"]["bytes"] + cib["helper"]["bytes"]
    assert f"main: {want} bytes" in proc.stdout, proc.stdout
    assert proc.stdout.count("helper") == 2


def test_script_unbounded_and_unknown(prog):
    out = _script(prog, "-e", "rec", "-e", "ind", "-e", "dyn", "-e", "unk", "-q").stdout
    assert "rec: UNBOUNDED (recursion)" in out
    # nothing of cb's type has its address taken: a callee no file describes
    assert re.search(r"ind: >= \d+ bytes \(unknown: \(indirect i\(i\)\)\)", out), out
    assert "ind: UNBOUNDED (indirect)" in _script(prog, "-e", "ind", "--indirect-unbounded", "-q").stdout
    assert "dyn: UNBOUNDED (dynamic)" in out
    assert re.search(r"unk: >= \d+ bytes \(unknown: lib\)", out), out
    budget = _ci(prog / "a.ci")["unk"]["bytes"] + 100
    assert f"unk: {budget} bytes" in _script(prog, "-e", "unk", "--unknown-budget", "100", "-q").stdout
    assert f"unk: {budget} bytes" in _script(prog, "-e", "unk", "--budget", "lib=100", "-q").stdout
    assert "ind: UNBOUNDED" not in _script(prog, "-e", "ind", "--indirect-budget", "64", "-q").stdout


def test_script_limit(prog):
    assert _script(prog, "--limit", "100000").returncode == 0
    assert _script(prog, "--limit", "16").returncode == 1
    assert _script(prog, "-e", "rec", "--limit", "100000").returncode == 1
    assert _script(prog, "-e", "unk", "--limit", "100000").returncode == 1
    assert _script(prog, "-e", "nosuch").returncode == 2
    files = sorted(os.listdir(prog))
    assert "a.ci" in files and "b.su" in files
    assert _script(prog / "a.ci", prog / "b.ci", "--top", "2").returncode == 0


# ---- scripts/stack_usage.py: calls through a pointer --------------------------

DISPATCH = """typedef int (*cb_t)(int);
extern cb_t handlers[];
extern int ext_cb(int);
int dispatch(int i, int x) { char b[16]; b[x & 15] = 0; return handlers[i](x + b[1]); }
int (*other)(void *) ;
int via_void(void *p) { return other(p) + 1; } /* not a tail call: an ARM Linux cross makes one */
cb_t pick(void) { return ext_cb; }
int main(void) { return dispatch(0, 1); }
"""
HANDLERS = """struct big { int a[50]; };
extern int dispatch(int, int);
static int small(int x) { char b[8]; b[x & 7] = 1; return b[0]; }
int large(int x) { char b[600]; b[x] = 1; return b[3]; }
int wrong_shape(int a, int b) { char k[2000]; k[a] = 1; return k[b]; }
int typed(struct big *p) { char k[700]; k[p->a[0]] = 1; return k[1]; }
int (*handlers[])(int) = { small, large };
int (*keep2)(int, int) = wrong_shape;
int (*keep3)(struct big *) = typed;
"""


def _two(tmp_path, a, b, *flags):
    for name, body in (("a.c", a), ("b.c", b)):
        proc = _cc(tmp_path, body, "-O0", "-fcallgraph-info", *flags, name=name, obj=name[0] + ".o")
        assert proc.returncode == 0, proc.stderr
    return _ci(tmp_path / "a.ci"), _ci(tmp_path / "b.ci")


def test_script_resolves_indirect_by_shape(tmp_path):
    a, b = _two(tmp_path, DISPATCH, HANDLERS)
    out = _script(tmp_path, "-e", "dispatch").stdout
    # the worst address-taken i(i) is typed -- a struct pointer and an int
    # have one shape; ext_cb (address taken, described nowhere) is unknown;
    # wrong_shape is i(i,i) and not a candidate
    want = a["dispatch"]["bytes"] + b["typed"]["bytes"]
    assert f"dispatch: >= {want} bytes (unknown: ext_cb)" in out, out
    assert "typed  [static]" in out and "wrong_shape" not in out
    assert f"dispatch: {want} bytes" in _script(tmp_path, "-e", "dispatch", "--budget", "ext_cb=8", "-q").stdout
    # a big budget for the unknown one makes it the worst
    out = _script(tmp_path, "-e", "dispatch", "--budget", "ext_cb=4000", "-q").stdout
    assert f"dispatch: {a['dispatch']['bytes'] + 4000} bytes" in out
    # the old behaviour
    assert "dispatch: UNBOUNDED (indirect)" in _script(tmp_path, "-e", "dispatch", "--indirect-unbounded").stdout
    out = _script(tmp_path, "-e", "dispatch", "--indirect-unbounded", "--indirect-budget", "64", "-q").stdout
    assert f"dispatch: {a['dispatch']['bytes'] + 64} bytes" in out
    # a void pointer reaches the struct pointer's function (a cast in the source)
    out = _script(tmp_path, "-e", "via_void", "--budget", "ext_cb=8").stdout
    assert f"via_void: {a['via_void']['bytes'] + b['typed']['bytes']} bytes" in out and "typed" in out, out


def test_script_indirect_recursion_and_no_candidate(tmp_path):
    rec = HANDLERS + "int again(int x) { return dispatch(0, x); }\nint (*keep4)(int) = again;\n"
    _two(tmp_path, DISPATCH.replace("extern int ext_cb(int);", "int ext_cb(int x) { return x; }"), rec)
    assert "dispatch: UNBOUNDED (recursion)" in _script(tmp_path, "-e", "dispatch", "-q").stdout
    # nothing of the shape has its address taken: unknown, unless budgeted
    body = "int (*fp)(long long);\nint f(void) { return fp(1); }\n"
    proc = _cc(tmp_path, body, "-O0", "-fcallgraph-info", name="c.c", obj="c.o")
    assert proc.returncode == 0, proc.stderr
    out = _script(tmp_path / "c.ci", "-e", "f", "-q").stdout
    assert re.search(r"f: >= \d+ bytes \(unknown: \(indirect i\(l\)\)\)", out), out
    assert re.search(r"f: \d+ bytes$", _script(tmp_path / "c.ci", "-e", "f", "-q", "--indirect-budget", "32").stdout)


def test_script_version1_and_unknown_type(tmp_path):
    (tmp_path / "old.ci").write_text("# tcc callgraph-info 1\nfunction f x.c:1 16 static\nindirect 1\n"
                                     "function g x.c:2 8 static\n")
    proc = _script(tmp_path / "old.ci", "-e", "f", "-q")
    assert "f: UNBOUNDED (indirect)" in proc.stdout and "version 1" in proc.stderr
    (tmp_path / "new.ci").write_text("# tcc callgraph-info 2\nfunction f x.c:1 16 static\ntype i()\n"
                                     "indirect 1 ?\nfunction g x.c:2 8 static\ntype i()\naddress-taken g\n")
    assert "f: UNBOUNDED (indirect)" in _script(tmp_path / "new.ci", "-e", "f", "-q").stdout
    assert "f: 48 bytes" in _script(tmp_path / "new.ci", "-e", "f", "-q", "--indirect-budget", "32").stdout


def test_script_archive_ci_sections(tmp_path):
    """An archive's .ci (armv8m-libtcc1.ci, libc.ci): `object NAME` sections,
    whose local functions stay apart; given as the archive or the .ci."""
    (tmp_path / "libx.ci").write_text(
        "object a.o\n# tcc callgraph-info 2\nfunction pub x.c:1 8 static\ntype i()\ncall helper\n"
        "function helper x.c:2 100 static local\ntype i()\n"
        "object b.o\n# tcc callgraph-info 2\nfunction pub2 y.c:1 8 static\ntype i()\ncall helper\n"
        "function helper y.c:2 300 static local\ntype i()\n")
    (tmp_path / "libx.a").write_bytes(b"!<arch>\n")
    for arg in ("libx.ci", "libx.a"):
        out = _script(tmp_path / arg, "-e", "pub", "-e", "pub2", "-q").stdout
        assert "pub: 108 bytes" in out and "pub2: 308 bytes" in out, out
    assert _script(tmp_path / "nolib.a").returncode != 0


def test_script_libtcc1(tmp_path):
    lib = ROOT / "armv8m-libtcc1.ci"
    if not lib.exists():
        pytest.skip("armv8m-libtcc1.a not built")
    body = "float g(unsigned long long x) { return x; }\n"
    assert _cc(tmp_path, body, "-O1", "-fcallgraph-info").returncode == 0
    assert "unknown: __aeabi_ul2f" in _script(tmp_path / "t.ci", "-e", "g", "-q").stdout
    out = _script(tmp_path / "t.ci", "-e", "g", "--libtcc1").stdout
    assert "__aeabi_ul2f  [static]" in out, out  # libtcc1.c's, described by the archive's .ci
