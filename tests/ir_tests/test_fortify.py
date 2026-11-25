"""_FORTIFY_SOURCE on top of __builtin_object_size and the __builtin___*_chk
builtins (frontend/gen/builtin/chk.c, objsize.c).

yasos libc's <string.h> / <stdio.h> turn memcpy(d, s, n) into
__builtin___memcpy_chk(d, s, n, __builtin_object_size(d, 0)) and kin when
_FORTIFY_SOURCE > 0 (sys/_fortify.h; level 2 the closest member for the string
and printf functions, level 3 __builtin_dynamic_object_size).  tcc calls the
plain function when the call provably fits (or nothing is known of the
destination), and the checking function otherwise: libc's __memcpy_chk,
__sprintf_chk, ..., libtcc1's __tcc_strcpy_chk and kin, all ending in
__chk_fail ("*** buffer overflow detected ***: terminated", abort).  sprintf
folds as gcc's does: a format without conversions, or "%s" of a known
string.  The object sizes are decided by the parser, so the same at every
-O.  A fortified call
gets the plain function's -Wstringop-overflow / -Wformat-overflow /
-Wformat-truncation warnings, and __has_builtin knows the builtins.

The QEMU runs link newlib: fortify_libc.h is a copy of libc's wrappers and
fortify_rt.c of its runtime (keep them in step with libs/libc).  With a yasos
tree around (libs/libc next to libs/tinycc, or YASOS_LIBC_DIR), the real libc
headers are compiled too.
"""

import os
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
MSG = "*** buffer overflow detected ***: terminated"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

# the target's headers as system headers (cross_includes.py): newlib's
# static helpers are no -Wunused-function of the test's
NEWLIB_INC = ["-nostdinc", f"-isystem{CURRENT_DIR / 'libc_includes'}", f"-isystem{CURRENT_DIR / 'libc_imports'}",
              f"-isystem{CURRENT_DIR / 'libc_includes' / 'newlib'}", f"-isystem{ROOT / 'include'}", f"-I{CURRENT_DIR}"]


def _compile(tmp_path, body, *flags, name="t"):
    src = tmp_path / f"{name}.c"
    src.write_text(body)
    obj = tmp_path / f"{name}.o"
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-c", *flags, str(src), "-o", str(obj)], capture_output=True,
                          text=True)
    return proc, obj


def _calls(obj):
    """{function: the functions it calls (the relocations of its BLs)}"""
    nm = subprocess.run(["arm-none-eabi-nm", "-S", "--defined-only", str(obj)], capture_output=True, text=True).stdout
    funcs = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in "tT":
            funcs[parts[3]] = [int(parts[0], 16) & ~1, int(parts[1], 16), set()]
    rel = subprocess.run(["arm-none-eabi-readelf", "-rW", str(obj)], capture_output=True, text=True).stdout
    section = None
    for line in rel.splitlines():
        m = re.match(r"Relocation section '\.rel(\S+)'", line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"\s*([0-9a-f]{8})\s+\S+\s+(\S+)\s+\S+\s+(\S+)", line)
        if m and section and section.startswith(".text") and ("JUMP24" in m.group(2) or "CALL" in m.group(2)):
            off = int(m.group(1), 16)
            for start, size, names in funcs.values():
                if start <= off < start + size:
                    names.add(m.group(3))
    return {f: names for f, (_, _, names) in funcs.items()}


def _checked(names):
    return any(n.endswith("_chk") for n in names)


# --------------------------------------------------------------------------
# Compile time: which calls fold to the plain function

DECIDE = """\
#include "fortify_libc.h"
void use(void *);
struct S { char a[4]; int x; };
/* fits: the plain function */
void fit_memcpy(const char *s) { char b[8]; memcpy(b, s, 8); use(b); }
void fit_memset(void) { char b[8]; memset(b, 0, sizeof b); use(b); }
void fit_strcpy(void) { char b[8]; strcpy(b, "1234567"); use(b); }
void fit_strcat(void) { char b[8] = "abc"; strcat(b, ""); use(b); }
void fit_strncpy(const char *s) { char b[8]; strncpy(b, s, 8); use(b); }
void fit_sprintf_lit(void) { char b[8]; sprintf(b, "1234567"); use(b); }
void fit_sprintf_s(void) { char b[8]; sprintf(b, "%s", "abc"); use(b); }
void fit_snprintf(const char *s) { char b[8]; snprintf(b, sizeof b, "%s", s); use(b); }
void fit_heap(const char *s) { char *p = malloc(16); memcpy(p, s, 16); use(p); }
/* nothing known of the destination: the plain function */
void any_memcpy(char *d, const char *s, size_t n) { memcpy(d, s, n); }
void any_strcpy(char *d, const char *s) { strcpy(d, s); }
void any_sprintf(char *d, const char *s) { sprintf(d, "%s", s); }
/* a length or string known at run time only: the check */
void chk_memcpy(const char *s, size_t n) { char b[8]; memcpy(b, s, n); use(b); }
void chk_memmove(const char *s, size_t n) { char b[8]; memmove(b, s, n); use(b); }
void chk_memset(size_t n) { char b[8]; memset(b, 0, n); use(b); }
void chk_strcpy(const char *s) { char b[8]; strcpy(b, s); use(b); }
void chk_strcat(const char *s) { char b[8] = "a"; strcat(b, s); use(b); }
void chk_strncpy(const char *s, size_t n) { char b[8]; strncpy(b, s, n); use(b); }
void chk_strncat(const char *s, size_t n) { char b[8] = "a"; strncat(b, s, n); use(b); }
void chk_sprintf_s(const char *s) { char b[8]; sprintf(b, "%s", s); use(b); }
/* gcc folds only a format without conversions and "%s" (its builtins/sprintf-chk.c
   counts the checks), so does tcc: an integer conversion is checked even
   where it would fit */
void chk_sprintf_d(int x) { char b[32]; sprintf(b, "%d", x); use(b); }
void chk_sprintf_f(double d) { char b[64]; sprintf(b, "%f", d); use(b); }
void chk_snprintf(const char *s, size_t n) { char b[8]; snprintf(b, n, "%s", s); use(b); }
void chk_heap(const char *s, size_t n) { char *p = malloc(16); memcpy(p, s, n); use(p); }
void chk_vsprintf(const char *f, va_list ap) { char b[8]; vsprintf(b, f, ap); use(b); }
"""


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("level", [1, 2, 3])
def test_what_is_checked(tmp_path, opt, level):
    proc, obj = _compile(tmp_path, DECIDE, opt, f"-D_FORTIFY_SOURCE={level}", "-Wall", "-Werror", *NEWLIB_INC)
    assert proc.returncode == 0, proc.stderr
    calls = _calls(obj)
    for f, names in calls.items():
        if f.startswith("chk_"):
            assert _checked(names), (f, names)
        else:
            assert not _checked(names), (f, names)


def test_same_at_every_level(tmp_path):
    seen = {}
    for opt in LEVELS:
        proc, obj = _compile(tmp_path, DECIDE, opt, "-D_FORTIFY_SOURCE=2", *NEWLIB_INC, name=opt[1:])
        assert proc.returncode == 0, proc.stderr
        # the test's functions: an ARM Linux cross's -O0 also keeps newlib's
        # static helpers (no __GNUC__: plain statics, not static inline)
        seen[opt] = {f: _checked(n) for f, n in _calls(obj).items() if f.startswith(("fit_", "any_", "chk_"))}
    assert all(v == seen["-O0"] for v in seen.values()), seen


def test_off_changes_nothing(tmp_path):
    # no _FORTIFY_SOURCE, =0 and the header with nothing to do are the same
    for opt in ["-O0", "-O2"]:
        outs = []
        for flags in [[], ["-D_FORTIFY_SOURCE=0"], ["-D_FORTIFY_SOURCE=2", "-U_FORTIFY_SOURCE"]]:
            # (one file name: the object holds it)
            proc, obj = _compile(tmp_path, DECIDE, opt, *flags, *NEWLIB_INC)
            assert proc.returncode == 0, proc.stderr
            outs.append(obj.read_bytes())
            assert not any(_checked(n) for n in _calls(obj).values())
        assert outs[0] == outs[1] == outs[2], opt


def test_has_builtin():
    src = "".join(f"_Static_assert(__has_builtin(__builtin___{f}_chk), \"{f}\");\n"
                  for f in ["memcpy", "memmove", "memset", "mempcpy", "strcpy", "stpcpy", "strcat", "strncpy",
                            "stpncpy", "strncat", "sprintf", "snprintf", "vsprintf", "vsnprintf"])
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-fsyntax-only", "-x", "c", "-"], input=src, capture_output=True,
                          text=True)
    assert proc.returncode == 0, proc.stderr


# --------------------------------------------------------------------------
# The warnings: a fortified call warns as the plain one does

WARN = """\
#include "fortify_libc.h"
void use(void *);
void w_memcpy(void) { char b[4]; memcpy(b, "abcdefgh", 8); use(b); }
void w_strcpy(void) { char b[4]; strcpy(b, "abcdefgh"); use(b); }
void w_strncpy(const char *s) { char b[4]; strncpy(b, s, 6); use(b); }
void w_sprintf(void) { char b[4]; sprintf(b, "%s", "abcdefgh"); use(b); }
void w_snprintf(void) { char b[4]; snprintf(b, 8, "%s", "abcdefgh"); use(b); }
int w_sprintf_lit(void) { char b[4]; return sprintf(b, "abcdefgh"); }
void w_member(void) { struct { char a[4]; int x; } s; strcpy(s.a, "abcdef"); use(&s); }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_warnings_as_plain(tmp_path, opt):
    plain, _ = _compile(tmp_path, WARN, opt, "-Wall", *NEWLIB_INC, name="plain")
    assert plain.returncode == 0, plain.stderr
    assert plain.stderr.count("warning:") >= 7, plain.stderr
    for level in [1, 2, 3]:
        fort, obj = _compile(tmp_path, WARN, opt, "-Wall", f"-D_FORTIFY_SOURCE={level}", *NEWLIB_INC,
                             name=f"fort{level}")
        assert fort.returncode == 0, fort.stderr
        # level 1 looks at the whole struct for strcpy into a member: 8 bytes
        # the plain function's check, whatever the level (level 1 sizes the
        # member's whole struct for the run-time check, the warning does not)
        assert fort.stderr.replace("fort%d.c" % level, "plain.c") == plain.stderr, (level, fort.stderr)
        # each sure overflow still calls the checking function
        calls = _calls(obj)
        for f in ["w_memcpy", "w_strcpy", "w_strncpy", "w_sprintf", "w_snprintf", "w_sprintf_lit"]:
            assert _checked(calls[f]), (level, f, calls[f])
    off, _ = _compile(tmp_path, WARN, opt, "-Wall", "-Wno-stringop-overflow", "-Wno-format-overflow",
                      "-Wno-format-truncation", "-D_FORTIFY_SOURCE=2", *NEWLIB_INC, name="quiet")
    assert off.returncode == 0 and "warning" not in off.stderr, off.stderr
    err, _ = _compile(tmp_path, WARN, opt, "-Werror=stringop-overflow", "-D_FORTIFY_SOURCE=2", *NEWLIB_INC,
                      name="err")
    assert err.returncode != 0 and "error: 'memcpy' writing 8 bytes" in err.stderr, err.stderr


def test_direct_builtin_size_warns_once(tmp_path):
    # an explicit size under the object's: the size argument's own warning
    body = ("typedef unsigned int size_t;\nvoid use(void *);\n"
            "void f(void) { char b[16]; __builtin___memcpy_chk(b, \"abcdefgh\", 8, 4); use(b); }\n"
            "void g(void) { char b[4]; __builtin___memcpy_chk(b, \"abcdefgh\", 8, 4); use(b); }\n")
    proc, _ = _compile(tmp_path, body, "-Wall")
    assert proc.returncode == 0, proc.stderr
    assert proc.stderr.count("'memcpy' writing 8 bytes into a region of size 4") == 2, proc.stderr
    assert proc.stderr.count(":3:") == 1 and proc.stderr.count(":4:") == 1, proc.stderr


# --------------------------------------------------------------------------
# Run time (QEMU): an overflow aborts with the message, a fitting call runs

CASES = ["memcpy", "memmove", "memset", "strcpy", "stpcpy", "strncpy", "strcat", "strncat", "sprintf", "snprintf",
         "vsprintf", "vsnprintf", "heap", "member", "dynamic"]
OK_CASES = ["memcpy_ok", "strcpy_ok", "strcat_ok", "strncat_ok", "sprintf_ok", "vsprintf_ok", "heap_ok", "member_ok",
            "dynamic_ok"]


def _caught(case, level):
    if case == "member":
        return level >= 2  # level 1: the whole struct, which holds it
    if case == "dynamic":
        return level >= 3  # malloc(n): known at run time only
    return True


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    if MSG in out:
        assert not re.search(rf"(?m)^ok {case} ", out), out
        return "caught"
    assert re.search(rf"(?m)^ok {case} ", out), out
    return "ok"


RUNS = [(opt, 2) for opt in LEVELS] + [(opt, lvl) for opt in ["-O0", "-O2"] for lvl in [1, 3]]


@pytest.mark.parametrize("opt,level", RUNS)
def test_run(tmp_path, opt, level):
    cfg = CompileConfig(extra_cflags=f"{opt} -D_FORTIFY_SOURCE={level}", output_dir=tmp_path,
                        output_suffix=f"{opt[1:]}_{level}")
    res = compile_testcase([CURRENT_DIR / "fortify_cases.c", CURRENT_DIR / "fortify_rt.c"], MACHINE, config=cfg)
    assert res.success, res.error
    cases = CASES if level == 2 else ["memcpy", "strcpy", "sprintf", "heap", "member", "dynamic"]
    for c in cases:
        assert _run(res.elf_file, c) == ("caught" if _caught(c, level) else "ok"), (c, level)
    for c in OK_CASES:
        assert _run(res.elf_file, c) == "ok", c


# --------------------------------------------------------------------------
# yasos libc's own headers, when there is a yasos tree around

def _libc_dir():
    env = os.environ.get("YASOS_LIBC_DIR")
    d = Path(env) if env else ROOT.parent / "libc"
    return d if (d / "sys" / "_fortify.h").exists() else None


LIBC_SRC = """\
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
void use(void *);
void fit(const char *s, int x) { char b[16]; memcpy(b, s, 8); strcpy(b, "hello"); sprintf(b, "%s", "ab"); (void)x;
  snprintf(b, sizeof b, "%s", s); use(b); }
void any(char *d, const char *s, size_t n) { memcpy(d, s, n); strcpy(d, s); sprintf(d, "%s", s); }
void chk(const char *s, size_t n) { char b[16]; memcpy(b, s, n); strcpy(b, s); strcat(b, s); use(b); }
void chk_heap(const char *s, size_t n) { char *p = malloc(10); memcpy(p, s, n); use(p); }
void *(*fp)(void *, const void *, size_t) = memcpy;
"""


@pytest.mark.skipif(_libc_dir() is None, reason="no yasos libc (libs/libc or YASOS_LIBC_DIR)")
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_yasos_libc_headers(tmp_path, opt):
    libc = _libc_dir()
    sysinc = libc.parent.parent / "source" / "sys" / "include"
    inc = ["-nostdinc", f"-I{libc}", f"-I{sysinc}", f"-I{ROOT / 'include'}"]
    outs = {}
    for level in [None, 0, 1, 2, 3]:
        flags = [] if level is None else [f"-D_FORTIFY_SOURCE={level}"]
        proc, obj = _compile(tmp_path, LIBC_SRC, opt, "-Wall", "-Werror", *flags, *inc)
        assert proc.returncode == 0, proc.stderr
        outs[level] = obj.read_bytes()
        calls = _calls(obj)
        for f, names in calls.items():
            assert _checked(names) == (bool(level) and f.startswith("chk")), (level, f, names)
    assert outs[None] == outs[0]
