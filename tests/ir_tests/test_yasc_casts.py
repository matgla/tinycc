"""yasc casts: cast<...>(x) and unsafe_cast<...>(x) (frontend/gen/type/yasc_cast.c,
docs/yasc_casts.md).

yasc_cast_cases.c takes the case to run as argv[1]: it reaches its "ok" line
with the value, or traps on a cast's check (`udf #254`, a HardFault on the
QEMU board).  The checks are on at every -O.
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
    "u8_ok": 255,
    "i8_ok": -127,
    "u32_ok": 70000,
    "ll_i32_ok": -5,
    "i_u64_ok": 70000,
    "d_i32_ok": 1234,
    "d_i32_neg": -2,
    "f_u16_ok": 1,
    "enum_ok": 2,
    "enum_sparse_ok": 9,
    "enum_wide_ok": 0,
    "trunc": 9,
    "align_ok": 9,
    "align_n_ok": 7,
    "bits": 0x3FC00000,
    "bits_back": 6,
    "float": 3000000,
}
TRAPS = ["u8_over", "u8_neg", "i8_over", "u32_neg", "u_i32_over", "ll_i32_over", "ull_u32_over", "i_u64_neg",
         "d_i32_over", "d_nan", "d_u16_neg", "f_u16_over", "enum_over", "enum_neg", "enum_sparse_gap",
         "enum_wide_gap", "align_bad", "align_n_bad"]


def _build(tmp_path, opt, extra=""):
    cfg = CompileConfig(extra_cflags=f"{opt} {extra}".strip(), output_dir=tmp_path / "build",
                        output_suffix=opt.strip("-"))
    res = compile_testcase([CURRENT_DIR / "yasc_cast_cases.c"], MACHINE, config=cfg)
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
def test_cast_cases(tmp_path, opt):
    elf = _build(tmp_path, opt)
    got = {c: _run(elf, c) for c in list(RUNS) + TRAPS}
    assert {c: got[c] for c in RUNS} == RUNS
    assert sorted(c for c in TRAPS if got[c] != "trap") == []


def _compile(tmp_path, body, *flags, name="t", pragma=True):
    src = tmp_path / f"{name}.c"
    src.write_text(("#pragma yasc\n" if pragma else "") + "#include <stdint.h>\n" + body)
    obj = tmp_path / f"{name}.o"
    proc = subprocess.run([str(TCC), "-c", "-Wno-missing-prototypes", *flags, str(src), "-o", str(obj)],
                          capture_output=True, text=True)
    return proc, obj


PRELUDE = """
struct dirent { uint32_t d_ino; uint16_t d_reclen; };
struct pk { char c; uint32_t u; } __attribute__((packed));
enum op { OP_A, OP_B };
static const char g_msg[] = "hi";
static volatile uint32_t g_reg;
int use(const volatile void *p);
"""

ERRORS = {
    # value permissions
    "int_const": ("int f(void) { return cast<int(uint8_t)>(300); }", "300 is not a 'unsigned char'"),
    "int_const_neg": ("unsigned f(void) { return cast<int(uint32_t)>(-1); }", "-1 is not a 'unsigned int'"),
    "int_float_const": ("int f(void) { return cast<int(int8_t)>(200.0); }", "the value is not a 'signed char'"),
    "enum_const": ("enum op f(void) { return cast<enum(enum op)>(7); }", "7 is not one of its enumerators"),
    "as_lossy": ("uint8_t f(int x) { return cast<as(uint8_t)>(x); }", "needs cast<int(unsigned char)>"),
    "as_float_lossy": ("float f(int x) { return cast<as(float)>(x); }", "needs cast<float(float)>"),
    "trunc_float": ("int f(double d) { return cast<trunc(int)>(d); }", "int(T) for a float"),
    "bits_size": ("uint16_t f(float x) { return cast<bits(uint16_t)>(x); }", "is not the same size"),
    "bits_pointer": ("uintptr_t f(int * view p) { return cast<bits(uintptr_t)>(p); }", "is a pointer conversion"),
    "int_to_enum": ("enum op f(int x) { return cast<int(enum op)>(x); }", "int makes an integer type"),
    "to_bool": ("_Bool f(int x) { return cast<int(_Bool)>(x); }", "compare it (x != 0)"),
    # syntax
    "no_type": ("int f(long long x) { return cast<int>(x); }", "'int' needs its type: int(T)"),
    "unknown": ("int f(int x) { return cast<wide(int)>(x); }", "'wide' is not a cast permission"),
    "two_types": ("int f(int x) { return cast<int(int), trunc(int)>(x); }", "one permission carries the type"),
    "mix": ("int f(int * view p) { return cast<int(int), align(4)>(p); }", "do not mix"),
    "align_two": ("char * view f(char * view p) { return cast<align(3)>(p); }", "not a power of two"),
    # pointer permissions
    "ptr_missing": ("uint32_t f(uint8_t * view p) { return *cast<as(uint32_t *)>(p); }", "pointer conversion"),
    "align_missing": ("uint32_t f(uint8_t * view p) { return *cast<ptr(uint32_t *)>(p); }",
                      "the alignment rises from 1 to 4: add align"),
    "packed_align": ("uint32_t f(struct pk * view s) { return *cast<ptr(uint32_t *)>(&s->u); }",
                     "the alignment rises from 1 to 4"),
    "align_unneeded": ("uint32_t f(uint32_t * view p) { return *cast<ptr(uint32_t *), align>(p); }",
                       "align is not needed"),
    "align_unneeded_member": ("struct d { int pos; _Alignas(4) char buf[8]; };\n"
                              "uint32_t f(struct d * view s) { return *cast<ptr(uint32_t *), align>(s->buf); }",
                              "align is not needed"),
    "ptr_drops_const": ("char * view f(const char * view s) { return cast<ptr(char *)>(s); }",
                        "drops 'const': this needs qual"),
    "qual_safe_cast": ("char * view f(const char * view s) { return cast<qual(char *)>(s); }",
                       "qual is unsafe: write unsafe_cast"),
    "unsafe_needless": ("int f(long x) { return unsafe_cast<int(int)>(x); }", "nothing unsafe here: write cast"),
    "qual_unneeded": ("char * view f(inout char * view s) { return unsafe_cast<qual(char *)>(s); }",
                      "qual is not needed"),
    "qual_const_object": ("void f(void) { char * view p = unsafe_cast<qual(char *)>(g_msg); use(p); }",
                          "points at a const object"),
    "qual_literal": ("void f(void) { char * view p = unsafe_cast<qual(char *)>(\"lit\"); use(p); }",
                     "points at a const object"),
    "qual_volatile_object": ("void f(void) { uint32_t * view p = unsafe_cast<qual(uint32_t *)>(&g_reg); use(p); }",
                             "points at a volatile object"),
    "qual_nested": ("void f(inout char * view * view p) { const char * view * view q = cast<ptr(const char * view *)>(p); use(q); }",
                    "adds const below a level that is not const"),
    "addr_int_to_ptr_safe": ("void f(void) { uint32_t * view p = cast<addr(uint32_t *)>(0x40000000u); use(p); }",
                             "is unsafe: write unsafe_cast"),
    "addr_narrow": ("uint16_t f(int * view p) { return cast<addr(uint16_t)>(p); }", "does not fit that integer"),
    "fn_safe": ("typedef int (*cmp_t)(const void *, const void *);\n"
                "int c2(const int *a, const int *b);\n"
                "cmp_t f(void) { return cast<fn(cmp_t)>(c2); }", "write unsafe_cast"),
    "inner_plain": ("const char * view * f(char * view * view p) { return cast<ptr(const char **)>(p); }",
                    "only the outermost '*'"),
    # the C cast
    "c_int": ("uint16_t f(int x) { return (uint16_t)x; }",
              "C cast: write cast<int(unsigned short)>(...), or cast<trunc(...)>(...) to keep the low bits"),
    "c_ptr": ("const struct dirent * view f(const char * view b) { return (const struct dirent * view)b; }",
              "C cast: write cast<ptr(const struct dirent * view), align>(...)"),
    "c_qual": ("char * view f(const char * view s) { return (char * view)s; }",
               "C cast: write unsafe_cast<qual(char * view)>(...)"),
    "c_null": ("void f(void) { int * view maybe p = (int * view maybe)0; use(p); }", "C cast: write nullptr"),
    "c_as": ("long long f(int x) { return (long long)x; }", "C cast: write cast<as(long long)>(...)"),
    "c_float": ("float f(int x) { return (float)x; }", "C cast: write cast<float(float)>(...)"),
    "c_addr": ("uintptr_t f(int * view p) { return (uintptr_t)p; }", "C cast: write cast<addr(unsigned int)>(...)"),
}


@pytest.mark.parametrize("case", sorted(ERRORS))
def test_misuse_is_an_error(tmp_path, case):
    body, msg = ERRORS[case]
    proc, _ = _compile(tmp_path, PRELUDE + body)
    assert proc.returncode != 0, f"{case} compiled"
    assert msg in proc.stderr, proc.stderr


def test_casts_that_compile(tmp_path):
    proc, _ = _compile(tmp_path, PRELUDE + """
#include <stddef.h>
struct d { int pos; _Alignas(struct dirent) char buf[64]; };
typedef int (*cmp_t)(const void *, const void *);
int c2(const int *a, const int *b);
static _Alignas(4) uint8_t g_buf[16];
void f(struct d * view s, const char * view str, char * view * view argv, uint8_t * view raw, double dd)
{
  const struct dirent * view a = cast<ptr(const struct dirent *)>(s->buf);       /* aligned member: free */
  const struct dirent * view b = cast<ptr(const struct dirent *), align>(s->buf + s->pos);
  const uint32_t * view w = cast<ptr(const uint32_t *)>(g_buf);                  /* _Alignas global */
  const uint32_t * view w2 = cast<ptr(const uint32_t *), align(4)>(raw);
  char * view hit = unsafe_cast<qual(char *)>(str);
  const char * view * view cc = unsafe_cast<qual(const char * view *)>(argv);
  uint32_t * view dst = unsafe_cast<ptr(uint32_t *), qual, align>(str);
  volatile uint32_t * view reg = unsafe_cast<addr(volatile uint32_t *)>(0x40070000u);
  uintptr_t ad = cast<addr(uintptr_t)>(str);
  cmp_t cmp = unsafe_cast<fn(cmp_t)>(c2);
  const void * view vp = cast<as(const void *)>(raw);
  const uint8_t * view back = cast<ptr(const uint8_t *)>(vp);
  uint8_t lo = cast<trunc(uint8_t)>(s->pos);
  int8_t n = cast<int(int8_t)>(s->pos);
  uint32_t fb = cast<bits(uint32_t)>(1.5f);
  uint64_t db = cast<bits(uint64_t)>(dd);
  float fl = cast<float(float)>(dd);
  enum op o = cast<enum(enum op)>(s->pos);
  size_t len = cast<int(size_t)>(s->pos);
  long long wide = cast<as(long long)>(s->pos);
  uint16_t same = cast<int(uint16_t)>(lo);                                      /* lossless: no check */
  use(a); use(b); use(w); use(w2); use(hit); use(cc); use(dst); use(reg); use(cmp); use(back);
  (void)ad; (void)lo; (void)n; (void)fb; (void)db; (void)fl; (void)o; (void)len; (void)wide; (void)same;
}
""")
    assert proc.returncode == 0, proc.stderr


def test_yasc_qualifiers_come_from_the_operand(tmp_path):
    """an unspelled target pointer takes owned/view/maybe/borrows from the operand"""
    proc, _ = _compile(tmp_path, """
#include <stdlib.h>
struct node { int v; };
void use(struct node * view n);
void f(void)
{
  void * owned maybe raw = malloc(sizeof(struct node));
  struct node * owned maybe n = cast<ptr(struct node *)>(move(raw));
  if (n)
    use(n);
  free(n);
}
""")
    assert proc.returncode == 0, proc.stderr


def test_void_cast_stays(tmp_path):
    proc, _ = _compile(tmp_path, "void f(int x) { (void)x; }\n")
    assert proc.returncode == 0, proc.stderr


def test_c_cast_in_regular_c_is_fine(tmp_path):
    proc, _ = _compile(tmp_path, "unsigned char f(int x) { return (unsigned char)x; }\n", "-ccompat", pragma=False)
    assert proc.returncode == 0, proc.stderr


def test_old_style_cast_can_be_lowered(tmp_path):
    body = "unsigned char f(int x) { return (unsigned char)x; }\n"
    proc, _ = _compile(tmp_path, body, "-Wno-error=old-style-cast")
    assert proc.returncode == 0 and "warning: C cast: write cast<int(unsigned char)>" in proc.stderr, proc.stderr
    proc, _ = _compile(tmp_path, body, "-Wno-old-style-cast")
    assert proc.returncode == 0 and "C cast" not in proc.stderr, proc.stderr


def test_c_macro_cast_is_regular_c(tmp_path):
    """a cast written in a regular-C header's macro reads as C: not flagged"""
    inc = tmp_path / "inc"
    inc.mkdir()
    (inc / "m.h").write_text("#define LOW(x) ((unsigned char)(x))\n#define AS_P(x) ((unsigned *)(x))\n")
    proc, _ = _compile(tmp_path, '#include "m.h"\nunsigned f(int x) { return LOW(x); }\n', "-I", str(inc))
    assert proc.returncode == 0, proc.stderr
    proc, _ = _compile(tmp_path, "#define MINE(x) ((unsigned char)(x))\nunsigned f(int x) { return MINE(x); }\n")
    assert proc.returncode != 0 and "C cast" in proc.stderr, proc.stderr


def test_cast_is_a_name_where_one_is_declared(tmp_path):
    proc, _ = _compile(tmp_path, "int f(int cast) { return cast < 3; }\nint g(int unsafe_cast) { return unsafe_cast; }\n")
    assert proc.returncode == 0, proc.stderr


def _disasm(obj):
    return subprocess.run(["arm-none-eabi-objdump", "-d", str(obj)], capture_output=True, text=True).stdout


def _udfs(obj):
    return len(re.findall(r"\budf\b", _disasm(obj)))


@pytest.mark.parametrize("opt", LEVELS)
def test_proven_casts_cost_nothing(tmp_path, opt):
    proc, obj = _compile(tmp_path, """
struct d { int pos; _Alignas(4) char buf[16]; };
uint16_t widen(uint8_t x) { return cast<int(uint16_t)>(x); }
int trunc_(int x) { return cast<trunc(uint8_t)>(x); }
const uint32_t * view member(struct d * view s) { return cast<ptr(const uint32_t *)>(s->buf); }
long long as_(int x) { return cast<as(long long)>(x); }
uint8_t konst(void) { return cast<int(uint8_t)>(200); }
""", opt)
    assert proc.returncode == 0, proc.stderr
    assert _udfs(obj) == 0, _disasm(obj)


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_optimizer_drops_checks_it_proves(tmp_path, opt):
    proc, obj = _compile(tmp_path, "uint8_t f(unsigned x) { return cast<int(uint8_t)>(x & 0x7f); }\n", opt)
    assert proc.returncode == 0, proc.stderr
    assert _udfs(obj) == 0, _disasm(obj)


def test_unproven_checks_and_no_sanitize(tmp_path):
    body = ("uint8_t f(int x) { return cast<int(uint8_t)>(x); }\n"
            "const uint32_t * view g(const uint8_t * view p) { return cast<ptr(const uint32_t *), align>(p); }\n"
            "enum e { A, B, C };\nenum e h(int x) { return cast<enum(enum e)>(x); }\n")
    proc, obj = _compile(tmp_path, body, "-O1")
    assert proc.returncode == 0, proc.stderr
    assert _udfs(obj) == 3, _disasm(obj)
    proc, obj = _compile(tmp_path, body, "-O1", "-fno-sanitize=cast")
    assert proc.returncode == 0, proc.stderr
    assert _udfs(obj) == 0, _disasm(obj)


@pytest.mark.parametrize("opt", LEVELS)
def test_no_check_traps_in_the_cases_build(tmp_path, opt):
    """-fno-sanitize=cast: the conversions still happen (C's)"""
    elf = _build(tmp_path, opt, "-fno-sanitize=cast")
    assert _run(elf, "u8_over") == 0
    assert _run(elf, "trunc") == 9
