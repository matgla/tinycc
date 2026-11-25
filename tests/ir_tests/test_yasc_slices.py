"""yasc slices (frontend/gen/expr/yasc_slices.c, docs/plans/yasc_slices.md).

yasc_slice_cases.c takes the case to run as argv[1]: it reaches its "ok" line
with the value, or traps on a slice's check (`udf #254`, a HardFault on the
QEMU board).  The checks are on at every -O; the optimizer drops the ones it
proves, which the codegen tests below pin.
"""

import re
import subprocess
from pathlib import Path

import pytest

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

RUNS = {
    "sum": 15,
    "sum_down": 15,
    "at": 5,
    "part": 93,             # sum(2, 3, 4) * 10 + len 3
    "part_empty": 0,
    "tail": 9,              # 4 + 5
    "array_slice": 90305,   # 2 + 3, (1 + 2) * 100, (4 + 5) * 10000
    "pointer_slice": 9,     # 2 + 3 + 4
    "fill": 10112021,      # 10, 11, then 20, 21 from [2:]
    "string": 31,           # len 3, then its zero
    "string_tail": 31,      # "llo": len 3, zero-terminated still
    "nested": 235,          # 2 rows, the second of len 3, its [2] is 5
    "vla": 94,              # 2 + 3 + 4, len 4
    "sentinel_read": 32,    # s[len] == 0; walk to the zero: 3, from [1:]: 2
    "sentinel_cut": 371,    # strlen 3 after the cut, len still 7, buf[1] 'x'
    "terminated_pointer": 22,
    "terminated_array": 20,
}
TRAPS = ["at_len", "at_neg", "part_past_end", "part_backwards", "tail_past_end", "store_past_end",
         "sentinel_past", "sentinel_overwrite", "sentinel_overwrite_const", "sentinel_bump",
         "terminated_no_zero", "terminated_past_array", "terminated_past_slice", "strnlen_then_write",
         "strlen_then_write"]


def _build(tmp_path, opt):
    cfg = CompileConfig(extra_cflags=opt, output_dir=tmp_path / "build", output_suffix=opt.strip("-"))
    res = compile_testcase([CURRENT_DIR / "yasc_slice_cases.c"], MACHINE, config=cfg)
    assert res.success, res.error
    return res.elf_file


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    if "HardFault" in out:
        assert re.search(r"CFSR=0x00010000", out), out  # UNDEFINSTR: the check's udf
        return "trap"
    m = re.search(rf"(?m)^ok {case} (-?\d+)", out)
    assert m, out
    return int(m.group(1))


@pytest.mark.parametrize("opt", LEVELS)
def test_slice_cases(tmp_path, opt):
    elf = _build(tmp_path, opt)
    got = {c: _run(elf, c) for c in list(RUNS) + TRAPS}
    assert {c: got[c] for c in RUNS} == RUNS
    assert {c for c in TRAPS if got[c] != "trap"} == set()


def _compile(tmp_path, body, opt="-O1", name="t"):
    src = tmp_path / f"{name}.c"
    src.write_text("#pragma yasc\n" + body)
    obj = tmp_path / f"{name}.o"
    proc = subprocess.run([str(TCC), "-c", opt, str(src), "-o", str(obj)], capture_output=True, text=True)
    return proc, obj


ERRORS = {
    "arith": ("int f([]int s) { return s + 1; }", "a slice is used by"),
    "compare": ("int f([]int s) { return s == s; }", "a slice is used by"),
    "from_pointer": ("void f(int * view p) { []int s = p; (void)s; }", "slice it, p[0:n]"),
    "to_pointer": ("void f([]int s) { int * view p = s; (void)p; }", "use .ptr and .len"),
    "member": ("int f([]int s) { return s.size; }", "a slice has .ptr and .len, not .size"),
    "len_store": ("void f([]int s) { s.len = 3; }", ""),
    "drop_const": ("void f([]const int s) { []int t = s; (void)t; }", "does not convert"),
    "sentinel_array": ("void f(void) { char b[4]; [:0]char s = b; (void)s; }", "only a string literal"),
    "pointer_no_end": ("void f(int * view p) { []int s = p[1:]; (void)s; }", "a pointer has no length"),
    "vla": ("void f(int n) { int v[n]; []int s = v; (void)s; }", "slice the VLA with its length"),
    "cast": ("void f(unsigned long long x) { []int s = ([]int)x; (void)s; }", "a cast to a slice"),
    "terminated_not_zero": ("void f(char * view p) { [:0]char s = p[0:2:1]; (void)s; }", "x[a:b:0]"),
    "terminated_pointers": ("void f(int * view * view p) { (void)p[0:2:0]; }", "holds integers"),
    "terminated_drops": ("void f([]char s) { [:0]char t = s; (void)t; }", "does not convert"),
}


@pytest.mark.parametrize("case", sorted(ERRORS))
def test_misuse_is_an_error(tmp_path, case):
    body, msg = ERRORS[case]
    proc, _ = _compile(tmp_path, body)
    assert proc.returncode != 0, f"{case} compiled"
    assert msg in proc.stderr, proc.stderr


def test_conversions_that_compile(tmp_path):
    proc, _ = _compile(tmp_path, """
static int nums[4];
void use([]const int s);
void f([]int s, [:0]const char z)
{
  []const int t = s;          /* adds const */
  []const char w = z;         /* drops the sentinel */
  [:0]const char lit = "x";   /* a literal ends in a zero */
  [][]const int rows = (([]const int[2]){t, nums[1:3]});
  use(nums);                  /* an array argument */
  use(t[1:]);
  (void)w; (void)lit; (void)rows;
}
""")
    assert proc.returncode == 0, proc.stderr


def test_slice_parameter_is_a_view_source(tmp_path):
    """a view result may point into a slice parameter (the elision, borrows(s))"""
    proc, _ = _compile(tmp_path, """
const int * view first([]const int s) { return s.ptr; }
const int * borrows(s) at([]const int s, const int * view q, unsigned i) { (void)q; return &s[i]; }
""")
    assert proc.returncode == 0, proc.stderr


def _disasm(obj):
    return subprocess.run(["arm-none-eabi-objdump", "-d", str(obj)], capture_output=True, text=True).stdout


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_checks_the_loop_proves_cost_nothing(tmp_path, opt):
    """s[i] for i < s.len, up or down, and constant indexes into a slice of an
    array: no check left; a slice is r0:r1"""
    proc, obj = _compile(tmp_path, """
int sum([]const int s) { int t = 0; for (unsigned i = 0; i < s.len; i++) t += s[i]; return t; }
int down([]const int s) { int t = 0; for (unsigned i = s.len; i > 0; i--) t += s[i - 1]; return t; }
unsigned len([]const char s) { return s.len; }
""", opt)
    assert proc.returncode == 0, proc.stderr
    dis = _disasm(obj)
    assert "udf" not in dis, dis
    body = dis.split("<len>:")[1]
    assert re.search(r"\tmov\s+r0, r1\b|\tmovs?\s+r0, r1\b", body) or "r1" in body.split("bx")[0], body


def test_unproven_index_keeps_one_compare(tmp_path):
    proc, obj = _compile(tmp_path, "int at([]const int s, unsigned i) { return s[i]; }\n", "-O2")
    assert proc.returncode == 0, proc.stderr
    dis = _disasm(obj)
    assert dis.count("udf") == 1, dis


def test_sentinel_costs(tmp_path):
    """[:0]: a zero stored at an index up to len is one compare (the index's);
    a value not known costs a second only at len; p[0:n:0] is one load"""
    proc, obj = _compile(tmp_path, """
void cut([:0]char s, unsigned i) { s[i] = '\\0'; }
void put([:0]char s, unsigned i, char c) { s[i] = c; }
[:0]char mk(inout char * view p, unsigned n) { return p[0:n:0]; }
""", "-O2")
    assert proc.returncode == 0, proc.stderr
    dis = _disasm(obj)
    fn = lambda name: dis.split(f"<{name}>:")[1].split(">:")[0]
    assert fn("cut").count("udf") == 1, dis
    assert fn("put").count("udf") == 2, dis
    assert fn("mk").count("udf") == 1 and fn("mk").count("ldrb") == 1, dis


STRNLEN_DECL = "typedef unsigned size_t;\nsize_t strnlen(const char * view s, size_t max);\n"
STRNLEN_PROVEN = STRNLEN_DECL + """
unsigned len(char * view p, unsigned m)
{
  unsigned n = strnlen(p, m);
  if (n == m)
    return 0;
  return p[0:n:0].len;           /* p[n] is strnlen's zero */
}
static inline __attribute__((always_inline)) unsigned plen(const char * view p, unsigned m)
{
  unsigned n = strnlen(p, m);
  if (n == m)
    return 0;                    /* the inlined return slot: written twice */
  return n;
}
unsigned via_slot(char * view p)
{
  unsigned n = plen(p, 64);
  if (n == 0)
    return 0;
  return p[0:n:0].len;
}
/* dirname's end: an index counted down from len - 1, plus one, is <= len */
unsigned cut([:0]char s)
{
  unsigned i;
  if (s.len == 0)
    return 0;
  i = s.len - 1;
  while (i > 0 && s[i] == '/')
    i--;
  while (i > 0 && s[i] != '/')
    i--;
  s[i + 1] = 0;
  return i;
}
"""


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_vrp_proves_strnlen_zero_and_end(tmp_path, opt):
    """ssa:vrp: strnlen(p, m) < m reads a zero at p[n] (through an inlined
    function's return slot too), and `i + 1 <= len` for i below len"""
    proc, obj = _compile(tmp_path, STRNLEN_PROVEN, opt)
    assert proc.returncode == 0, proc.stderr
    dis = _disasm(obj)
    assert "udf" not in dis, dis


STRNLEN_KEPT = {
    "written": "void f(inout char * view p, unsigned m) { unsigned n = strnlen(p, m); if (n == m) return; p[n] = 'x'; use(p[0:n:0]); }",
    "call": "void f(inout char * view p, unsigned m) { unsigned n = strnlen(p, m); if (n == m) return; touch(p); use(p[0:n:0]); }",
    "no_test": "void f(inout char * view p, unsigned m) { unsigned n = strnlen(p, m); use(p[0:n:0]); }",
    "other_bound": "void f(inout char * view p, unsigned m, unsigned k) { unsigned n = strnlen(p, m); if (n == k) return; use(p[0:n:0]); }",
    "other_string": "void f(inout char * view p, char * view q, unsigned m) { unsigned n = strnlen(q, m); if (n == m) return; use(p[0:n:0]); }",
    "slot_written": """static inline __attribute__((always_inline)) unsigned plen(inout char * view p, unsigned m)
{ unsigned n = strnlen(p, m); if (n == m) return 0; p[n] = 'x'; return n; }
void f(inout char * view p) { unsigned n = plen(p, 64); if (n == 0) return; use(p[0:n:0]); }""",
}


@pytest.mark.parametrize("case", sorted(STRNLEN_KEPT))
def test_vrp_keeps_strnlen_checks_it_cannot_prove(tmp_path, case):
    """the zero is gone, never was proven, or is another string's: the check stays"""
    src = STRNLEN_DECL + "void use([:0]char s);\nvoid touch(inout char * view p);\n" + STRNLEN_KEPT[case]
    proc, obj = _compile(tmp_path, src, "-O2", name=case)
    assert proc.returncode == 0, proc.stderr
    assert "udf" in _disasm(obj)


STRLEN_DECL = "typedef unsigned size_t;\nsize_t strlen(const char * view s);\nvoid use([:0]char s);\nvoid touch(char * view p);\n"
STRLEN_PROVEN = STRLEN_DECL + """
unsigned len(char * view p)
{
  unsigned n = strlen(p);
  return p[0:n:0].len;           /* p[n] is strlen's zero: no test needed */
}
/* libgen's shape: "" answered before the slice */
unsigned nonempty(char * view maybe p)
{
  unsigned n;
  if (p == nullptr || (n = strlen(p)) == 0)
    return 0;
  return p[0:n:0].len;
}
"""


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_vrp_proves_strlen_zero(tmp_path, opt):
    """ssa:vrp: n = strlen(p) reads a zero at p[n], with nothing written since"""
    proc, obj = _compile(tmp_path, STRLEN_PROVEN, opt)
    assert proc.returncode == 0, proc.stderr
    dis = _disasm(obj)
    assert "udf" not in dis, dis


STRLEN_KEPT = {
    "written": "void f(inout char * view p) { unsigned n = strlen(p); p[n] = 'x'; use(p[0:n:0]); }",
    "call": "void f(inout char * view p) { unsigned n = strlen(p); touch(p); use(p[0:n:0]); }",
    "other_string": "void f(inout char * view p, inout char * view q) { unsigned n = strlen(q); use(p[0:n:0]); }",
    "other_index": "void f(inout char * view p) { unsigned n = strlen(p); if (n == 0) return; use(p[0:n - 1:0]); }",
}


@pytest.mark.parametrize("case", sorted(STRLEN_KEPT))
def test_vrp_keeps_strlen_checks_it_cannot_prove(tmp_path, case):
    """the zero is gone, is another string's, or is not at the index read: the check stays"""
    proc, obj = _compile(tmp_path, STRLEN_DECL + STRLEN_KEPT[case], "-O2", name=case)
    assert proc.returncode == 0, proc.stderr
    assert "udf" in _disasm(obj)
