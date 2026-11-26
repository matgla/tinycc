"""-ffortify (frontend/gen/builtin/fortify.c): calls checked against what the
callee's declaration says, with no macro and no _chk entry.

- null_terminated_string_arg(n): argument n's NUL has to be inside the buffer
  the caller names (__builtin_object_size, closest member):
  __tcc_fortify_str(p, size) before the call, __chk_fail when there is none.
  ssa:const_string_fold drops it for a string whose bytes it knows.
- access(mode, ref, size): `size` elements of argument ref's pointed-to type
  fit its buffer (whole object): `cmp` + `udf` before the call, nothing when
  constants prove it, -Wstringop-overflow when they disprove it.

A buffer the compiler cannot see (a pointer parameter) is not checked.  On by
default; under -ccompat only with _FORTIFY_SOURCE (not 0); -f[no-]fortify.
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

DECLS = """\
#include <stddef.h>
char *pdir(char *path) __attribute__((null_terminated_string_arg(1)));
void fill(char *d, size_t n) __attribute__((access(write_only, 1, 2)));
void fill_i(int *d, int n) __attribute__((access(write_only, 1, 2)));
void fill_rev(size_t n, char *d) __attribute__((access(write_only, 2, 1)));
void fill_none(char *d, size_t n) __attribute__((access(none, 1, 2)));
void use(void *);
"""

SRC = DECLS + """\
struct S { char a[4]; char c[12]; };
/* checked */
char *chk_glob(void) { static char g[16]; return pdir(g); }
char *chk_nonul(void) { char b[4] = "abcd"; return pdir(b); }
char *chk_member(struct S *s) { return pdir(s->a); }
void chk_rt(size_t n) { char b[8]; fill(b, n); use(b); }
void chk_int(int n) { int b[4]; fill_i(b, n); use(b); }
void chk_vla(size_t n, size_t m) { char b[n]; fill(b, m); use(b); }
/* nothing to check: a buffer not seen, sizes that fit, no size */
char *ok_param(char *p) { return pdir(p); }
void ok_param_fill(char *p, size_t n) { fill(p, n); }
void ok_const(void) { char b[8]; fill(b, 8); use(b); }
void ok_int(void) { int b[4]; fill_i(b, 4); use(b); }
void ok_rev(void) { char b[4]; fill_rev(4, b); use(b); }
void ok_none(size_t n) { char b[4]; fill_none(b, n); use(b); }
"""

# the string's bytes are known: checked at -O0, folded from -O1
FOLDED = DECLS + """\
char *lit_local(void) { char b[] = "/usr/lib"; return pdir(b); }
char *lit_direct(void) { return pdir((char *)"a/b"); }
"""


def _compile(tmp_path, body, *flags, name="t", ccompat=True):
    src = tmp_path / f"{name}.c"
    src.write_text(body)
    obj = tmp_path / f"{name}.o"
    # tcc's own <stddef.h>: a CI cross has no sysroot behind it
    cmd = [str(TCC), *(["-ccompat"] if ccompat else []), "-nostdinc", f"-isystem{ROOT / 'include'}", "-c", *flags,
           str(src), "-o", str(obj)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc, obj


def _funcs(obj):
    """{function: its disassembly}"""
    dis = subprocess.run(["arm-none-eabi-objdump", "-dr", str(obj)], capture_output=True, text=True).stdout
    out, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r"[0-9a-f]+ <(\w+)>:", line)
        if m:
            cur = m.group(1)
            out[cur] = ""
        elif cur:
            out[cur] += line + "\n"
    return out


def _checked(text):
    return "__tcc_fortify_str" in text or "udf" in text


@pytest.mark.parametrize("opt", LEVELS)
def test_what_is_checked(tmp_path, opt):
    proc, obj = _compile(tmp_path, SRC, opt, "-ffortify", "-Wall", "-Werror")
    assert proc.returncode == 0, proc.stderr
    funcs = _funcs(obj)
    for f, text in funcs.items():
        assert _checked(text) == f.startswith("chk"), (f, text)
    assert "__tcc_fortify_str" in funcs["chk_member"] and "movs\tr1, #4" in funcs["chk_member"]


@pytest.mark.parametrize("opt", LEVELS)
def test_known_string_is_folded(tmp_path, opt):
    proc, obj = _compile(tmp_path, FOLDED, opt, "-ffortify", "-Wall", "-Werror")
    assert proc.returncode == 0, proc.stderr
    for f, text in _funcs(obj).items():
        assert _checked(text) == (opt == "-O0" and f == "lit_local"), (f, text)


def test_constant_overflow_warns_and_traps(tmp_path):
    body = DECLS + "void over(void) { char b[8]; fill(b, 9); use(b); }\n" \
                   "void over_rev(void) { char b[4]; fill_rev(5, b); use(b); }\n"
    proc, obj = _compile(tmp_path, body, "-O2", "-ffortify", "-Wall")
    assert proc.returncode == 0, proc.stderr
    assert "'fill' accesses 9 bytes of an object of 8" in proc.stderr, proc.stderr
    assert "'fill_rev' accesses 5 bytes of an object of 4" in proc.stderr, proc.stderr
    for f, text in _funcs(obj).items():
        assert "udf" in text, (f, text)


@pytest.mark.parametrize("flags,on", [
    ([], False),                                   # -ccompat: off without _FORTIFY_SOURCE
    (["-D_FORTIFY_SOURCE=2"], True),
    (["-D_FORTIFY_SOURCE=1"], True),
    (["-D_FORTIFY_SOURCE=0"], False),
    (["-D_FORTIFY_SOURCE=2", "-fno-fortify"], False),
    (["-ffortify"], True),
])
def test_switches_under_ccompat(tmp_path, flags, on):
    proc, obj = _compile(tmp_path, SRC, "-O1", *flags)
    assert proc.returncode == 0, proc.stderr
    funcs = _funcs(obj)
    assert _checked(funcs["chk_rt"]) == on and _checked(funcs["chk_glob"]) == on


def test_off_is_the_unchecked_code(tmp_path):
    plain = re.sub(r" __attribute__\(\(\w+\([^)]*\)\)\)", "", SRC)
    _, a = _compile(tmp_path, SRC, "-O2", "-fno-fortify", name="a")
    _, b = _compile(tmp_path, plain, "-O2", "-ffortify", name="b")
    da = subprocess.run(["arm-none-eabi-objdump", "-d", str(a)], capture_output=True, text=True).stdout
    db = subprocess.run(["arm-none-eabi-objdump", "-d", str(b)], capture_output=True, text=True).stdout
    assert da.split("\n", 2)[2] == db.split("\n", 2)[2]


def test_on_by_default_in_the_safe_mode(tmp_path):
    body = ("#include <stddef.h>\n"
            "void fill(char *d, size_t n) __attribute__((access(write_only, 1, 2)));\n"
            "void chk_rt(size_t n) { char b[8] = undefined; fill(b, n); }\n")
    proc, obj = _compile(tmp_path, body, "-O1", ccompat=False)
    assert proc.returncode == 0, proc.stderr
    assert "udf" in _funcs(obj)["chk_rt"]
    proc, obj = _compile(tmp_path, body, "-O1", "-fno-fortify", name="off", ccompat=False)
    assert proc.returncode == 0, proc.stderr
    assert "udf" not in _funcs(obj)["chk_rt"]


def test_attributes_merge_over_declarations(tmp_path):
    body = ("#include <stddef.h>\n"
            "void fill(char *d, size_t n);\n"
            "void fill(char *d, size_t n) __attribute__((access(write_only, 1, 2)));\n"
            "char *pdir(char *p);\n"
            "[[gnu::null_terminated_string_arg(1)]] char *pdir(char *p);\n"
            "void use(void *);\n"
            "void chk_rt(size_t n) { char b[8]; fill(b, n); use(b); }\n"
            "char *chk_str(void) { static char g[4]; return pdir(g); }\n")
    proc, obj = _compile(tmp_path, body, "-O1", "-ffortify", "-Wall", "-Werror")
    assert proc.returncode == 0, proc.stderr
    funcs = _funcs(obj)
    assert "udf" in funcs["chk_rt"] and "__tcc_fortify_str" in funcs["chk_str"]


# --------------------------------------------------------------------------
# Run time (QEMU)

CAUGHT = {"str_nonul": "chk", "str_member": "chk", "str_heap": "chk", "fill": "trap", "ints": "trap",
          "ints_neg": "trap", "vla": "trap", "rev": "trap", "rev_rt": "trap"}
OK = ["str_ok", "str_null_ok", "fill_ok", "ints_ok", "vla_ok", "rev_ok"]


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    if "chk_fail" in out:
        return "chk"
    if "HardFault" in out:
        assert re.search(r"CFSR=0x00010000", out), out  # UNDEFINSTR: the check's UDF
        return "trap"
    assert re.search(rf"(?m)^ok {case} ", out), out
    return "ok"


@pytest.mark.parametrize("opt", LEVELS)
def test_run(tmp_path, opt):
    cfg = CompileConfig(extra_cflags=f"{opt} -ffortify -Wno-stringop-overflow", output_dir=tmp_path,
                        output_suffix=opt[1:])
    res = compile_testcase([CURRENT_DIR / "compiler_fortify_cases.c"], MACHINE, config=cfg)
    assert res.success, res.error
    for c, want in CAUGHT.items():
        assert _run(res.elf_file, c) == want, c
    for c in OK:
        assert _run(res.elf_file, c) == "ok", c


# --------------------------------------------------------------------------
# yasc's syntax for the same: `char s[:0]`, `T p[n]` with n a parameter
# (also one after p), `const void buf[n]` for bytes

SYNTAX = """\
#include <stddef.h>
char * borrows(path) pdir(char path[maybe :0]);
void fill(char d[n], size_t n);
void wr(const void buf[n], size_t n);
void rd(out void buf[n], size_t n);
void fill_first(size_t n, char d[n]);
void use(char *p);
char * borrows(static) chk_str(void) { static char g[16]; return pdir(g); }
void chk_rt(size_t n) { char b[8] = undefined; fill(b, n); use(b); }
void chk_wr(size_t n) { int b[4] = undefined; wr(b, n); }
void chk_rd(size_t n) { int b[4] = undefined; rd(b, n); }
void chk_first(size_t n) { char b[8] = undefined; fill_first(n, b); use(b); }
void ok_const(void) { char b[8] = undefined; fill(b, 8); use(b); }
void ok_bytes(void) { int b[4] = undefined; wr(b, sizeof b); }
void ok_first(void) { char b[8] = undefined; fill_first(8, b); use(b); }
"""

ATTRS = """\
#include <stddef.h>
char * borrows(path) pdir(char * maybe path) __attribute__((null_terminated_string_arg(1)));
void fill(char *d, size_t n) __attribute__((access(read_write, 1, 2)));
void wr(const void *buf, size_t n) __attribute__((access(read_only, 1, 2)));
void rd(out void *buf, size_t n) __attribute__((access(write_only, 1, 2)));
void fill_first(size_t n, char *d) __attribute__((access(read_write, 2, 1)));
void use(char *p);
""" + SYNTAX.split("void use(char *p);\n", 1)[1]


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_syntax_is_the_attributes(tmp_path, opt):
    pa, a = _compile(tmp_path, SYNTAX, opt, "-Wno-missing-prototypes", name="syn", ccompat=False)
    pb, b = _compile(tmp_path, ATTRS, opt, "-Wno-missing-prototypes", name="att", ccompat=False)
    assert pa.returncode == 0 and pb.returncode == 0, pa.stderr + pb.stderr
    fa, fb = _funcs(a), _funcs(b)
    for f, text in fa.items():
        assert _checked(text) == f.startswith("chk"), (f, text)
        assert text == fb[f], (f, text, fb[f])


def test_bound_after_the_array_checks_the_body(tmp_path):
    """`d[n], size_t n`: the function's entry knows n, -fsanitize=bounds
    checks d[i] against it"""
    body = ("#include <stddef.h>\n"
            "int bad(const char d[n], size_t n) { int s = 0; for (size_t i = 0; i <= n; i++) s += d[i]; return s; }\n"
            "int good(const char d[n], size_t n) { int s = 0; for (size_t i = 0; i < n; i++) s += d[i]; return s; }\n")
    proc, obj = _compile(tmp_path, body, "-O2", "-fsanitize=bounds", "-Wno-missing-prototypes", ccompat=False)
    assert proc.returncode == 0, proc.stderr
    funcs = _funcs(obj)
    assert "udf" in funcs["bad"] and "udf" not in funcs["good"]


@pytest.mark.parametrize("decl,msg", [
    ("void f(char s[:1]);", "0: a parameter array's sentinel"),
    ("void f(char d[m], unsigned n);", "'m' undeclared"),
    ("void f(char d[p], char *p);", "a parameter's bound is an integer parameter"),
    ("void f(char d[d]);", "a parameter's bound is an integer parameter"),
])
def test_syntax_errors(tmp_path, decl, msg):
    proc, _ = _compile(tmp_path, decl + "\n", ccompat=False)
    assert proc.returncode != 0 and msg in proc.stderr, proc.stderr


def test_syntax_is_yasc_only(tmp_path):
    # regular C: `[:0]` is no syntax, a later parameter is not in scope
    proc, _ = _compile(tmp_path, "void f(char s[:0]);\n")
    assert proc.returncode != 0
    proc, _ = _compile(tmp_path, "void f(char d[n], unsigned n);\n")
    assert proc.returncode != 0 and "'n' undeclared" in proc.stderr
