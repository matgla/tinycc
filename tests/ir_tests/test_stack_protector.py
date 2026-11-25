"""-fstack-protector, -fstack-protector-strong, -fstack-protector-all,
-fstack-protector-explicit, and the attributes stack_protect /
no_stack_protector.

A canary right below the saved registers (backend/arch/arm/thumb/arm-thumb-frame.c),
copied from __stack_chk_guard by the prologue and compared by the epilogue,
which calls __stack_chk_fail on a mismatch.  Which functions get one is gcc's
rule, decided by the frontend (frontend/gen/op/ubsan.c).  The runtime half is
libc's: newlib has both symbols (and a constructor that sets the guard), so
the QEMU runs need nothing from tinycc; yasos libc has them too.
"""

import re
import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

from qemu_run import CompileConfig, build_qemu_command, compile_testcase

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
MACHINE = "mps2-an505"
LEVELS = ["-O0", "-O1", "-O2", "-Os"]

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

OK_CASES = ["char_ok", "int_ok", "addr_ok", "alloca_ok", "vla_ok", "ret64_ok", "setjmp_ok", "explicit_ok"]
# the overruns each level catches (the others would wreck the frame: not run);
# "explicit" is a stack_protect function, protected at every level
CAUGHT = {
    "-fstack-protector": ["char", "alloca", "vla", "explicit"],
    "-fstack-protector-strong": ["char", "int", "addr", "alloca", "vla", "explicit"],
    "-fstack-protector-all": ["char", "int", "addr", "alloca", "vla", "explicit"],
    "-fstack-protector-explicit": ["explicit"],
}


def _build(tmp_path, flags, tag):
    cfg = CompileConfig(extra_cflags=flags, output_dir=tmp_path, output_suffix=tag)
    res = compile_testcase([CURRENT_DIR / "ssp_cases.c"], MACHINE, config=cfg)
    assert res.success, res.error
    return res.elf_file


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    if "stack smashing detected" in out:
        return "caught"
    assert re.search(rf"(?m)^ok {case} ", out), out
    return "ok"


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("level", list(CAUGHT))
def test_overrun_caught(tmp_path, opt, level):
    elf = _build(tmp_path, f"{opt} {level}", (opt + level).replace("-", "_"))
    for c in CAUGHT[level]:
        assert _run(elf, c) == "caught", c
    for c in OK_CASES:
        assert _run(elf, c) == "ok", c


def _disasm(tmp_path, body, *flags):
    """{function: (its bytes, the symbols its relocations name)}, and the object."""
    src = tmp_path / "t.c"
    src.write_text(body)
    obj = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-c", *flags, str(src), "-o", str(obj)], capture_output=True,
                          text=True)
    assert proc.returncode == 0, proc.stderr
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
        if m and section and section.startswith(".text"):
            off = int(m.group(1), 16)
            for f, (start, size, names) in funcs.items():
                if start <= off < start + size:
                    names.add((m.group(3), m.group(2)))
    return funcs, obj.read_bytes()


SELECT = """extern void use(void *); extern int g(int);
int scalars(int a, int b) { return g(a) * b; }
int small_chars(void) { char b[4]; use(b); return b[0]; }
int big_chars(void) { char b[8]; use(b); return b[0]; }
int ints(void) { int a[4]; use(a); return a[0]; }
int addr(void) { int x = 0; use(&x); return x; }
int with_alloca(int n) { char *p = __builtin_alloca(n); use(p); return p[0]; }
int tail(int a) { return g(a); }
__attribute__((naked)) void bare(void) { __asm__("bx lr"); }
"""


def _protected(funcs):
    return {f for f, (_, _, names) in funcs.items() if any(n == "__stack_chk_fail" for n, _ in names)}


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_which_functions(tmp_path, opt):
    funcs, _ = _disasm(tmp_path, SELECT, opt, "-fstack-protector")
    assert _protected(funcs) == {"big_chars", "with_alloca"}
    funcs, _ = _disasm(tmp_path, SELECT, opt, "-fstack-protector-strong")
    assert _protected(funcs) == {"small_chars", "big_chars", "ints", "addr", "with_alloca"}
    funcs, _ = _disasm(tmp_path, SELECT, opt, "-fstack-protector-all")
    assert _protected(funcs) == set(funcs) - {"bare"}
    # (`tail` among them: no tail call, the check runs after the callee returns)


@pytest.mark.parametrize("opt", LEVELS)
def test_off_means_no_change(tmp_path, opt):
    for name in ["sanitize_cases.c", "20_op_add.c", "test_llong_bitwise.c", "708_computed_goto_labels.c"]:
        src = CURRENT_DIR / name
        outs = []
        for flags in [[], ["-fstack-protector-all", "-fno-stack-protector"]]:
            obj = tmp_path / f"{src.stem}{len(flags)}.o"
            proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-ccompat", "-c", opt, *flags, str(src), "-o", str(obj)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            outs.append(obj.read_bytes())
        assert outs[0] == outs[1], name
    # -strong leaves a function it does not protect as it was
    plain, _ = _disasm(tmp_path, SELECT, opt)
    strong, _ = _disasm(tmp_path, SELECT, opt, "-fstack-protector-strong")
    assert strong["scalars"][1] == plain["scalars"][1]
    assert not _protected(strong) & {"scalars", "tail"}


def test_predefined_macros(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int a = __SSP__; int b = __SSP_STRONG__; int c = __SSP_ALL__;\n")
    for flag, ok in [("-fstack-protector", "__SSP__"), ("-fstack-protector-strong", "__SSP_STRONG__"),
                     ("-fstack-protector-all", "__SSP_ALL__"), ("-fstack-protector-explicit", "__SSP_EXPLICIT__")]:
        proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-E", "-dM", flag, str(src)], capture_output=True, text=True)
        defined = set(re.findall(r"#define (__SSP\w*__) (\d)", proc.stdout))
        assert {d[0] for d in defined} == {ok}, proc.stdout[-300:]


# ---- -fstack-protector-explicit, stack_protect / no_stack_protector -------
# SP marks stack_protect, NSP no_stack_protector (both spellings); -DNOATTR
# turns them into nothing.
ATTRS = """#ifdef NOATTR
#define SP
#define NSP
#define SP23
#define NSP23
#else
#define SP __attribute__((stack_protect))
#define NSP __attribute__((__no_stack_protector__))
#define SP23 [[gnu::stack_protect]]
#define NSP23 [[gnu::no_stack_protector]]
#endif
extern void use(void *); extern int g(int);
int scalars(int a, int b) { return g(a) * b; }
int big_chars(void) { char b[8]; use(b); return b[0]; }
int ints(void) { int a[4]; use(a); return a[0]; }
SP int sp_scalars(int a, int b) { return g(a) * b; }
SP23 int sp23_scalars(int a) { return g(a) + 1; }
int sp_proto(int a) SP;
int sp_proto(int a) { return g(a) - 1; }
NSP int nsp_big_chars(void) { char b[16]; use(b); return b[0]; }
NSP23 int nsp23_ints(void) { int a[4]; use(a); return a[1]; }
int nsp_proto(void) NSP;
int nsp_proto(void) { char b[32]; use(b); return g(b[1]); }
SP static int sp_helper(int a) { char b[4]; b[0] = (char)a; use(b); return b[0]; }
int calls_helper(int a) { return sp_helper(a) + sp_helper(a + 1); }
"""
SP_FUNCS = {"sp_scalars", "sp23_scalars", "sp_proto", "sp_helper"}
EXPECT = {
    None: set(),
    "-fstack-protector": {"big_chars"} | SP_FUNCS,
    "-fstack-protector-strong": {"big_chars", "ints"} | SP_FUNCS,
    "-fstack-protector-all": {"scalars", "big_chars", "ints", "calls_helper"} | SP_FUNCS,
    "-fstack-protector-explicit": SP_FUNCS,
}


@pytest.mark.parametrize("opt", LEVELS)
@pytest.mark.parametrize("mode", list(EXPECT), ids=lambda m: m or "none")
def test_attributes_and_explicit(tmp_path, opt, mode):
    flags = [opt] + ([mode] if mode else [])
    funcs, _ = _disasm(tmp_path, ATTRS, *flags)
    assert _protected(funcs) == EXPECT[mode]
    if mode:
        # a stack_protect body is not inlined into an unprotected caller
        assert "sp_helper" in funcs


@pytest.mark.parametrize("opt", LEVELS)
def test_attributes_without_a_flag_change_nothing(tmp_path, opt):
    _, with_attrs = _disasm(tmp_path, ATTRS, opt)
    _, without = _disasm(tmp_path, ATTRS, opt, "-DNOATTR")
    assert with_attrs == without


@pytest.mark.parametrize("opt", LEVELS)
def test_explicit_leaves_others_unchanged(tmp_path, opt):
    """-explicit with no stack_protect anywhere: the objects of no flag."""
    for name in ["sanitize_cases.c", "20_op_add.c", "708_computed_goto_labels.c"]:
        src = CURRENT_DIR / name
        outs = []
        for flags in [[], ["-fstack-protector-explicit"]]:
            obj = tmp_path / f"{src.stem}{len(flags)}.o"
            proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, "-ccompat", "-c", opt, *flags, str(src), "-o", str(obj)],
                                  capture_output=True, text=True)
            assert proc.returncode == 0, proc.stderr
            outs.append(obj.read_bytes())
        assert outs[0] == outs[1], name


def test_attributes_known(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("""#if __has_attribute(stack_protect) && __has_attribute(__stack_protect__) && \\
    __has_attribute(no_stack_protector) && __has_attribute(__no_stack_protector__)
int yes;
#else
#error missing
#endif
__attribute__((stack_protect)) int f(void) { return 1; }
__attribute__((no_stack_protector)) int g(void) { return 2; }
[[gnu::stack_protect]] int h(void) { return 3; }
[[gnu::no_stack_protector]] int k(void) { return 4; }
""")
    for flags in [[], ["-fstack-protector-strong"]]:
        proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-Wall", "-Wignored-attributes", "-Wunsupported", "-c", *flags, str(src),
                               "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        assert "attribute" not in proc.stderr, proc.stderr


def test_conflicting_attributes(tmp_path):
    """Both on one function: no_stack_protector wins, as with gcc (a warning)."""
    body = ("extern void use(void *);\n"
            "__attribute__((stack_protect, no_stack_protector)) int both(void) { char b[16]; use(b); return b[0]; }\n"
            "int later(void) __attribute__((no_stack_protector));\n"
            "__attribute__((stack_protect)) int later(void) { char b[16]; use(b); return b[1]; }\n")
    src = tmp_path / "c.c"
    src.write_text(body)
    proc = subprocess.run([str(TCC), "-Wno-missing-prototypes", "-ccompat", "-Wignored-attributes", "-c", "-fstack-protector-all", str(src),
                           "-o", str(tmp_path / "c.o")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert proc.stderr.count("conflicts with 'no_stack_protector'") == 2, proc.stderr
    funcs, _ = _disasm(tmp_path, body, "-fstack-protector-all")
    assert _protected(funcs) == set()
