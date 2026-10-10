"""-fdrop-unused-statics: which statics reach the object file.

At -O1 and above, static functions and initialized static objects are
deferred to the end of the TU, and only those something live refers to are
emitted (prune_unused_statics, frontend/gen/inline/emit.c).  Generated C --
zig's C backend -- defines every helper and constant table it might need;
before this, all of them landed in the object.

Runtime correctness of the deferred shapes is 475_drop_unused_statics.c;
this checks the symbol table.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
READELF = shutil.which("arm-none-eabi-readelf")

pytestmark = pytest.mark.skipif(
    READELF is None or not TCC.exists(),
    reason="needs armv8m-tcc and arm-none-eabi-readelf",
)

SOURCE = r"""
typedef long long wide;

/* Unused: every one of these is dropped. */
__attribute__((noinline)) static int dead_func(int x) { return x + 1; }
static const int dead_table[4] = {1, 2, 3, 4};
static const char dead_string[] = "never printed";
static int dead_bss;
static int dead_data = 5;
/* Referenced only from dead code. */
static int only_from_dead_value = 7;
static int dead_caller(void) { return only_from_dead_value + dead_func(1); }
/* Referenced only from an unused static inline body. */
static int only_from_inline = 9;
static inline int dead_inline(void) { return only_from_inline; }
/* zig.h's shape: a block-scope prototype, not a call to an undeclared name. */
static wide dead_libcall_wrapper(wide a, wide b)
{
  extern wide __aeabi_ldivmod_like(wide, wide);
  return __aeabi_ldivmod_like(a, b);
}
/* Forward-declared const object, defined later, never used. */
struct pair { int a, b; };
static const struct pair dead_fwd_const;
static const struct pair dead_fwd_const = {1, 2};

/* Live: reached from the exported function, directly or through data. */
__attribute__((noinline)) static int live_callee(int x) { return x * 3; }
static int live_via_table(int x) { return x - 1; }
static int (*const live_table[])(int) = {live_via_table};
static int live_counter = 2;
__attribute__((used)) static int kept_by_used = 3;

int exported(int x) { return live_callee(x) + live_table[0](x) + live_counter++; }
"""

DEAD = [
    "dead_func",
    "dead_table",
    "dead_string",
    "dead_bss",
    "dead_data",
    "only_from_dead_value",
    "dead_caller",
    "only_from_inline",
    "dead_inline",
    "dead_libcall_wrapper",
    "dead_fwd_const",
]
LIVE = ["exported", "live_callee", "live_via_table", "live_table", "live_counter", "kept_by_used"]


def _defined_symbols(tmp_path, *flags):
    src = tmp_path / "statics.c"
    src.write_text(SOURCE)
    obj = tmp_path / "statics.o"
    proc = subprocess.run([str(TCC), "-c", *flags, "-o", str(obj), str(src)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    out = subprocess.run([READELF, "-sW", str(obj)], capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        cols = line.split()
        # Num: Value Size Type Bind Vis Ndx Name -- defined = a real section index.
        if len(cols) == 8 and cols[6].isdigit():
            names.add(cols[7])
    return names


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_unused_statics_are_dropped(tmp_path, opt):
    names = _defined_symbols(tmp_path, opt)
    assert not names & set(DEAD), f"emitted though unused: {sorted(names & set(DEAD))}"
    assert set(LIVE) <= names, f"live but missing: {sorted(set(LIVE) - names)}"


@pytest.mark.parametrize("flags", [("-O0",), ("-O1", "-fno-drop-unused-statics")])
def test_everything_kept_without_the_option(tmp_path, flags):
    names = _defined_symbols(tmp_path, *flags)
    # dead_inline is a static inline: never emitted unless referenced.
    expected = set(DEAD + LIVE) - {"dead_inline"}
    assert expected <= names, f"missing: {sorted(expected - names)}"


# A dropped static is still parsed for its diagnostics (check_dropped_statics),
# with nothing it produces reaching the object.

def _compile(tmp_path, source, *flags):
    src = tmp_path / "check.c"
    src.write_text(source)
    obj = tmp_path / "check.o"
    proc = subprocess.run([str(TCC), "-c", "-O1", *flags, "-o", str(obj), str(src)], capture_output=True, text=True)
    return proc, obj


@pytest.mark.parametrize(
    "dead",
    [
        "static int dead(int x) { return x + undeclared_name; }",
        "static short dead = (int)&main;",
        "static void dead(void) { static short s = (int)&main; (void)s; }",
    ],
)
def test_error_in_dropped_static_is_reported(tmp_path, dead):
    proc, _ = _compile(tmp_path, "int main(void);\n" + dead + "\nint main(void) { return 0; }\n")
    assert proc.returncode != 0 and "error" in proc.stderr, proc.stderr


def test_warning_in_dropped_function_is_reported(tmp_path):
    proc, _ = _compile(tmp_path, "static void dead(void) { int *p = 5; (void)p; }\nint main(void) { return 0; }\n")
    assert proc.returncode == 0, proc.stderr
    assert "warning" in proc.stderr


def test_dropped_statics_leave_nothing_behind(tmp_path):
    # Everything a body can put in a section or the symbol table, in dropped
    # functions: the object must hold main and nothing else.
    source = r"""
extern int only_named_by_dead_code(void);
static const char *tbl[] = {"x", "y"};
static const char *d1(void) { return "string literal"; }
static void *d2(void) { lab: return &&lab; }
static double d3(double x) { return x * 1.2345; }
static const char *d4(void) { return __func__; }
static int d5(void) { return ((struct { int a; }){7}).a; }
static int d6(void) { static int local_static = 3; return local_static + tbl[0][0]; }
static int d7(void) { char buf[] = "a longer string literal"; return buf[3]; }
static int d8(void) { int big[64] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20}; return big[7]; }
static int (*d9(void))(void) { return only_named_by_dead_code; }
static int d10(int x) { int nested(int y) { return x + y; } int (*p)(int) = nested; return p(1); }
static int d11(int x) { int r; __asm__ volatile("adds %0, %1, #1" : "=r"(r) : "r"(x)); return r; }
int main(void) { return 0; }
"""
    proc, obj = _compile(tmp_path, source)
    assert proc.returncode == 0, proc.stderr
    out = subprocess.run([READELF, "-sW", str(obj)], capture_output=True, text=True, check=True).stdout
    names = [cols[7] for cols in (line.split() for line in out.splitlines())
             if len(cols) == 8 and cols[0][:-1].isdigit() and cols[3] not in ("FILE", "SECTION") and cols[7] != "$t"]
    assert names == ["main"], names
    sizes = subprocess.run(["arm-none-eabi-size", "-A", str(obj)], capture_output=True, text=True, check=True).stdout
    for line in sizes.splitlines():
        cols = line.split()
        if cols and cols[0] in (".data", ".rodata", ".bss"):
            assert cols[1] == "0", line


# What the optimizer leaves unreferenced is collected from the object
# (gc_unreferenced_statics): a write-only static, once dead_static_store has
# removed its stores -- including zig's `*(&((T *)&x)->f) = v` field stores
# and struct copies into it -- goes, with the functions only it pointed to.

def test_write_only_static_and_its_closure_are_collected(tmp_path):
    source = r"""
struct big { int v[12]; };
extern struct big get(void);
static void only_in_initializer(void) {}
struct holder { int a; struct big b; void (*f)(void); };
static struct holder write_only = {1, {{0}}, only_in_initializer};
int main(void)
{
  (*(&((struct holder *)&write_only)->a)) = 7;
  (*(&((struct holder *)&write_only)->b)) = get();
  return 0;
}
"""
    proc, obj = _compile(tmp_path, source, "-O2")
    assert proc.returncode == 0, proc.stderr
    out = subprocess.run([READELF, "-sW", str(obj)], capture_output=True, text=True, check=True).stdout
    defined = [cols[7] for cols in (line.split() for line in out.splitlines())
               if len(cols) == 8 and cols[0][:-1].isdigit() and cols[6].isdigit()]
    assert "write_only" not in defined and "only_in_initializer" not in defined, defined
    assert "main" in defined


# Replaying a deferred body restores the file name it was saved with.  That
# name is already resolved (directory included), so it must not be resolved
# again like a #line name: each replay prepended the directory once more, and
# the kernel's CBE file reported ./.zig-cache/o/H/./.zig-cache/o/H/.../kernel.c
# (and wrote it into the debug info) until the name overflowed its buffer.

@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_replayed_body_keeps_a_relative_file_name(tmp_path, opt):
    sub = tmp_path / "sub"
    sub.mkdir()
    # A body calling an unprototyped function is replayed on the spot.
    (sub / "a.c").write_text(
        "int old();\n"
        "static int s(int x) { return old(x); }\n"
        "int f(int x) { return s(x); }\n"
        "static int t(int x) { return old(x) + 1; }\n"
        "int g(int x) { return t(x); }\n"
        "int h(int x) { if (x) return 1; }\n"
    )
    proc = subprocess.run([str(TCC), "-c", opt, "-o", "a.o", "./sub/a.c"], capture_output=True, text=True, cwd=tmp_path)
    assert proc.returncode == 0, proc.stderr
    assert "./sub/a.c:6: warning: function might return no value: 'h'" in proc.stderr.splitlines(), proc.stderr
