"""-fsanitize=undefined and -ftrapv: frontend/gen/op/ubsan.c.

Trap mode (the default): every check is a test in the generated code and a
`udf #254` (HardFault on the QEMU board, a UDF stop under ptrace on YasOS).
Handler mode (-fno-sanitize-trap, -fsanitize-recover, -fno-sanitize-recover):
the failing side calls __ubsan_handle_* with clang's data record; the runtime
is yasos libc's ubsan.c, here its copy ubsan_runtime.c (compiled without the
sanitizer and linked in).  sanitize_cases.c takes the case to run as argv[1];
a case either reaches its "ok" line or traps (or reports).  Off by default,
and objects without the options are the compiler's ordinary ones.
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

TRAPS = """add add_const sub sub_const mul mul_min neg inc compound inline ladd lsub lmul lmul_min
div0 mod0 udiv0 divmin ldiv0 shl_big shl_neg shr_big shl_sign shl_negbase null null_store align
vla vla_neg bounds bounds_neg bounds_store""".split()
TRAPS += ["add_loop", "null_compound", "null_inc", "null_postinc", "null_member", "null_member_store", "null_dot",
          "align_store", "align_member", "null_inline", "null_after_inline"]
TRAPS += ["param_bounds", "param_bounds_neg", "param_bounds_const", "param_bounds_walk", "param_bounds_store",
          "param_bounds_loop", "param_bounds_inline", "param_bounds_down"]
RUNS = """add_ok add_const_ok sub_ok mul_ok neg_ok lmul_ok lmul_shift_ok negret_ok uadd_ok short_ok
div_ok shl_ok ushl_ok align_ok vla_ok bounds_ok bounds_addr_ok bounds_flex_ok null_inline_ok""".split()
RUNS += ["param_bounds_down_ok", "param_bounds_inline_ok", "param_bounds_ok", "param_bounds_const_ok", "param_bounds_min_ok", "param_bounds_walk_ok",
         "param_bounds_addr_ok", "param_bounds_loop_ok"]


def _runtime(tmp_path):
    """the handlers (ubsan_runtime.c), built bare-metal without -fsanitize"""
    out = tmp_path / "ubsan_runtime.o"
    if not out.exists():
        proc = subprocess.run([str(TCC), "-ccompat", "-Wall", "-Werror", "-nostdlib", "-fno-pic", "-mno-sb-relative-got",
                               "-O1", "-c", *INCLUDE_FLAGS, str(CURRENT_DIR / "ubsan_runtime.c"), "-o", str(out)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
    return out


def _build(tmp_path, flags, tag, handlers=False):
    # the build cleans its own directory: not the runtime's
    cfg = CompileConfig(extra_cflags=flags, output_dir=tmp_path / "build", output_suffix=tag,
                        extra_objs=[_runtime(tmp_path)] if handlers else None)
    res = compile_testcase([CURRENT_DIR / "sanitize_cases.c"], MACHINE, config=cfg)
    assert res.success, res.error
    return res.elf_file


def _run(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    if "HardFault" in out:
        # UNDEFINSTR: the check's UDF, not some other fault
        assert re.search(r"CFSR=0x00010000", out), out
        return "trap"
    assert re.search(rf"(?m)^ok {case} ", out), out
    return "ok"


@pytest.mark.parametrize("opt", LEVELS)
def test_undefined_all_cases(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -fsanitize=undefined", opt.strip("-"))
    got = {c: _run(elf, c) for c in TRAPS + RUNS}
    assert {c for c in TRAPS if got[c] != "trap"} == set()
    assert {c for c in RUNS if got[c] != "ok"} == set()


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_trapv_is_the_arithmetic_subset(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -ftrapv", "trapv" + opt.strip("-"))
    for c in ["add", "sub", "mul", "neg", "inc", "ladd", "lmul"]:
        assert _run(elf, c) == "trap", c
    # not -ftrapv's: division, shifts, pointers, bounds (SDIV by 0 gives 0)
    for c in ["add_ok", "mul_ok", "div0", "shl_big", "bounds", "vla_ok"]:
        assert _run(elf, c) == "ok", c


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_sub_options(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -fsanitize=shift,integer-divide-by-zero -fsanitize-trap=undefined", "sub" + opt.strip("-"))
    for c in ["shl_big", "shl_sign", "div0", "udiv0"]:
        assert _run(elf, c) == "trap", c
    for c in ["add", "mul", "bounds", "vla_ok"]:
        assert _run(elf, c) == "ok", c
    elf = _build(tmp_path, f"{opt} -fsanitize=undefined -fno-sanitize=bounds,signed-integer-overflow", "nob" + opt.strip("-"))
    for c in ["bounds", "add", "divmin"]:
        assert _run(elf, c) == "ok", c
    for c in ["null", "vla", "div0"]:
        assert _run(elf, c) == "trap", c


def test_ra_return_share_regression(tmp_path):
    """bug_struct_tag_paren_defers_body.c at -O1: the negation's check read a
    value the return-tail register share had overwritten (a false trap)."""
    cfg = CompileConfig(extra_cflags="-O1 -fsanitize=signed-integer-overflow", output_dir=tmp_path)
    res = compile_testcase([CURRENT_DIR / "bug_struct_tag_paren_defers_body.c"], MACHINE, config=cfg)
    assert res.success, res.error
    proc = subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)
    assert proc.stdout.split() == ["42", "-7", "-1", "-41"], proc.stdout + proc.stderr


def _run_expect(tmp_path, src, flags):
    """src built with flags prints its .expect file"""
    cfg = CompileConfig(extra_cflags=flags, output_dir=tmp_path)
    res = compile_testcase([CURRENT_DIR / src], MACHINE, config=cfg)
    assert res.success, res.error
    proc = subprocess.run(build_qemu_command(MACHINE, res.elf_file), shell=True, capture_output=True, text=True,
                          timeout=60)
    want = (CURRENT_DIR / src).with_suffix(".expect").read_text().split()
    assert proc.stdout.split()[:len(want)] == want, proc.stdout + proc.stderr


@pytest.mark.parametrize("opt", LEVELS)
def test_predec_reload_var_slot(tmp_path, opt):
    """`t[--i]` at -O0: inc()'s reload of i took the sanitized add's c.i as
    i's frame offset and read a word of t."""
    _run_expect(tmp_path, "bug_predec_reload_var_slot.c", f"{opt} -fsanitize=signed-integer-overflow")


@pytest.mark.parametrize("opt", LEVELS)
def test_cfg_label_addr_edges(tmp_path, opt):
    """a sanitized `s += 100` reached again through `goto *tab[0]`: with no
    CFG edge into the label, load_cse forwarded `s = 0` into its read."""
    _run_expect(tmp_path, "bug_cfg_label_addr_edges.c", f"{opt} -fsanitize=signed-integer-overflow")


def _obj(tmp_path, src, *flags):
    out = tmp_path / (src.stem + "".join(f.strip("-=,") for f in flags) + ".o")
    proc = subprocess.run([str(TCC), *INCLUDE_FLAGS, "-ccompat", "-c", *flags, str(src), "-o", str(out)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return out.read_bytes()


OBJ_SAMPLES = ["20_op_add.c", "bug_cmp0_fusion_deref_operand.c", "test_llong_bitwise.c", "sanitize_cases.c",
               "364_diamond_store_fwd.c", "708_computed_goto_labels.c"]


@pytest.mark.parametrize("opt", LEVELS)
def test_off_means_no_change(tmp_path, opt):
    for name in OBJ_SAMPLES:
        src = CURRENT_DIR / name
        plain = _obj(tmp_path, src, opt)
        assert _obj(tmp_path, src, opt, "-fsanitize=undefined", "-fno-sanitize=undefined") == plain, name
        assert _obj(tmp_path, src, opt, "-ftrapv", "-fno-trapv") == plain, name
        assert _obj(tmp_path, src, opt, "-fsanitize-trap") == plain, name
        assert _obj(tmp_path, src, opt, "-fsanitize-recover", "-fno-sanitize-recover=all") == plain, name


def test_checks_change_code(tmp_path):
    src = CURRENT_DIR / "sanitize_cases.c"
    assert _obj(tmp_path, src, "-O2", "-fsanitize=undefined") != _obj(tmp_path, src, "-O2")


def test_unsupported_sanitizer_is_an_error(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int f(int a) { return a + 1; }\n")
    for bad in ["-fsanitize=address", "-fsanitize=undefined,thread"]:
        proc = subprocess.run([str(TCC), "-ccompat", "-c", bad, str(src), "-o", str(tmp_path / "t.o")],
                              capture_output=True, text=True)
        assert proc.returncode != 0 and "not supported" in proc.stderr, proc.stderr


def test_constant_contexts_untouched(tmp_path):
    """Static initializers, case labels and array sizes stay constant
    expressions; sizeof does not evaluate."""
    src = tmp_path / "t.c"
    src.write_text("int g = 1 << 4; int a[2 + 3];\n"
                   "int f(int x, int *p) { switch (x) { case 3 * 7: return sizeof(*p / x); } return sizeof a; }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-c", "-O1", "-fsanitize=undefined", str(src), "-o",
                           str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    nm = subprocess.run(["arm-none-eabi-objdump", "-d", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert "udf" not in nm.stdout, nm.stdout


PROVEN = """
int sum(int n, const int a[n]) { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
unsigned usum(unsigned n, const unsigned a[n]) { unsigned s = 0; for (unsigned i = 0; i < n; i++) s += a[i]; return s; }
int loc(int k) { int buf[16]; for (int i = 0; i < 16; i++) buf[i] = i * k; return buf[k & 15]; }
int g[32];
int sumg(void) { int s = 0; for (int i = 0; i < 32; i++) s += g[i]; return s; }
void fill(int n, char d[n], char c) { for (int i = 0; i < n; i++) d[i] = c; }
unsigned last_slash(unsigned len, const char s[len])
{
  unsigned i;
  if (len == 0)
    return 0;
  for (i = len - 1; i > 0 && s[i] != '/'; i--)
    ;
  while (i > 0 && s[i - 1] == '/')
    i--;
  return s[i] == '/' ? i : len;
}
"""


def _udf_count(tmp_path, src, opt):
    c = tmp_path / "t.c"
    c.write_text(src)
    proc = subprocess.run([str(TCC), "-ccompat", "-c", opt, "-fsanitize=bounds", str(c), "-o", str(tmp_path / "t.o")],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(tmp_path / "t.o")], capture_output=True, text=True)
    return dis.stdout.count("udf"), dis.stdout


def _text(tmp_path, src, flags, tag):
    c = tmp_path / f"{tag}.c"
    c.write_text(src)
    o = tmp_path / f"{tag}.o"
    proc = subprocess.run([str(TCC), "-ccompat", "-c", *flags, str(c), "-o", str(o)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", str(o)], capture_output=True, text=True).stdout
    # instructions only: no addresses, encodings, file names or pc-relative comments
    return [re.sub(r"\s+@.*$|[0-9a-f]+ <([^>+]*)(\+0x[0-9a-f]+)?>", r"<\1>", l.split("\t", 2)[-1])
            for l in dis.splitlines() if re.match(r"\s+[0-9a-f]+:\t", l)]


PARAM_LOOPS = """
int s4(const int p[4]) { int s = 0; for (int i = 0; i < 4; i++) s += p[i]; return s; }
int sn(int n, const int p[n]) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }
void zero(int n, char d[n]) { for (int i = 0; i < n; i++) d[i] = 0; }
"""


@pytest.mark.parametrize("opt", ["-O2"])
def test_proven_checks_leave_the_loop_passes_the_same_loop(tmp_path, opt):
    """ssa:bounds_iv folds them before unrolling and IV strength reduction:
    buf[16] is unrolled, g[32] walks a post-incremented pointer, p[4] unrolls,
    exactly as without -fsanitize"""
    for name, src in (("proven", PROVEN), ("params", PARAM_LOOPS)):
        plain = _text(tmp_path, src, [opt], name + "0")
        checked = _text(tmp_path, src, [opt, "-fsanitize=bounds"], name + "1")
        assert plain == checked, "\n".join(f"{a:40} | {b}" for a, b in zip(plain, checked))


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_bounds_proven_by_the_loop_cost_nothing(tmp_path, opt):
    """i < n bounds i for a[n] (ssa:vrp sv_try_fold_iv_bound), i < 16 for buf[16],
    k & 15 too: no check is left"""
    n, dis = _udf_count(tmp_path, PROVEN, opt)
    assert n == 0, dis


def test_count_down_without_its_precondition_keeps_the_check(tmp_path):
    """len - 1 wraps at len == 0: without `if (len == 0)` the first s[i] is a real check"""
    src = ("unsigned f(unsigned len, const char s[len])\n"
           "{ unsigned i = len - 1; while (i > 0 && s[i] == '/') i--; return i; }\n")
    n, dis = _udf_count(tmp_path, src, "-O2")
    assert n >= 1, dis


def test_unsigned_loop_bound_does_not_decide_a_signed_compare(tmp_path):
    """i <u n says nothing of (int)i < (int)n: n = 0x80000001 makes it false at
    i = 1, so the signed compare (and its branch) must stay"""
    src = "int f(unsigned n) { int bad = 0; for (unsigned i = 0; i < n; i++) if ((int)i >= (int)n) bad++; return bad; }\n"
    _, dis = _udf_count(tmp_path, src, "-O2")
    assert re.search(r"\tb(lt|ge|gt|le)(\.n|\.w)?\t", dis), dis


@pytest.mark.parametrize("opt", ["-O1", "-O2"])
def test_param_bounds_check_is_one_compare(tmp_path, opt):
    """`int p[n]`: (p - start) + i folds to i while p keeps its value (ssa:fold's
    barrel-shifted constant, ssa:cprop's never-written PARAM): cmp, branch, udf"""
    n, dis = _udf_count(tmp_path, "int get(int n, const int p[n], int i) { return p[i]; }\n", opt)
    assert n == 1, dis
    body = [l for l in dis.splitlines() if re.match(r"\s+[0-9a-f]+:\t", l)]
    assert len(body) == 5, dis  # cmp, bcc, udf, ldr, bx


# ------------------------------------------------------------- handler mode

CASES_SRC = (CURRENT_DIR / "sanitize_cases.c").read_text().splitlines()


def _case_line(case):
    """the statement after `if (!strcmp(c, "case"))`"""
    return [i + 2 for i, l in enumerate(CASES_SRC) if f'strcmp(c, "{case}"))' in l][0]


INT = "cannot be represented in type 'int'"
LL = "cannot be represented in type 'long long'"
REPORTS = {
    "add": rf"signed integer overflow: 2147483647 \+ 1 {INT}",
    "add_const": rf"signed integer overflow: 2147483647 \+ 1 {INT}",
    "sub": rf"signed integer overflow: -2147483648 - 1 {INT}",
    "mul": rf"signed integer overflow: 65536 \* 65536 {INT}",
    "mul_min": rf"signed integer overflow: -2147483648 \* -1 {INT}",
    "neg": rf"negation of -2147483648 {INT}; cast to an unsigned type to negate this value to itself",
    "ladd": rf"signed integer overflow: 9223372036854775807 \+ 1 {LL}",
    "lsub": rf"signed integer overflow: -9223372036854775808 - 1 {LL}",
    "lmul": rf"signed integer overflow: 4294967296 \* 4294967296 {LL}",
    "lmul_min": rf"signed integer overflow: -9223372036854775808 \* -1 {LL}",
    "div0": "division by zero",
    "mod0": "division by zero",
    "udiv0": "division by zero",
    "ldiv0": "division by zero",
    "divmin": rf"division of -2147483648 by -1 {INT}",
    "shl_big": "shift exponent 32 is too large for 32-bit type 'int'",
    "shl_neg": "shift exponent -3 is negative",
    "shr_big": "shift exponent 32 is too large for 32-bit type 'int'",
    "shl_sign": rf"left shift of 1 by 31 places {INT}",
    "shl_negbase": "left shift of negative value -1",
    "null": "load of null pointer of type 'int'",
    # a store (also compound assignment and ++/--), a member access: clang's
    # type_mismatch kinds 1 and 3
    "null_store": "store to null pointer of type 'int'",
    "null_compound": "store to null pointer of type 'int'",
    "null_inc": "store to null pointer of type 'int'",
    "null_postinc": "store to null pointer of type 'int'",
    "null_member": "member access within null pointer of type 'struct P'",
    "null_member_store": "member access within null pointer of type 'struct P'",
    "null_dot": "member access within null pointer of type 'struct P'",
    "align": "load of misaligned address 0x[0-9a-f]+ for type 'int', which requires 4 byte alignment",
    "align_store": "store to misaligned address 0x[0-9a-f]+ for type 'int', which requires 4 byte alignment",
    "align_member": "member access within misaligned address 0x[0-9a-f]+ for type 'struct P', which requires 4 byte "
                    "alignment",
    "bounds": r"index 4 out of bounds for type 'int \[4\]'",
    "bounds_neg": r"index -1 out of bounds for type 'int \[4\]'",
    "bounds_store": r"index 4 out of bounds for type 'int \[4\]'",
}
# reported elsewhere than the case's own line
OTHER_LINE = {
    "vla": "variable length array bound evaluates to non-positive value 0",
    "vla_neg": "variable length array bound evaluates to non-positive value -3",
    "inc": rf"signed integer overflow: 2147483647 \+ 1 {INT}",
    "compound": rf"signed integer overflow: 2147483647 \+ 2 {INT}",
}


def _run_out(elf, case):
    proc = subprocess.run(build_qemu_command(MACHINE, elf, [case]), shell=True, capture_output=True, text=True,
                          timeout=60)
    out = proc.stdout + proc.stderr
    assert f"go {case}" in out, out
    return proc.returncode, out


@pytest.mark.parametrize("opt", LEVELS)
def test_handler_recover_reports_and_goes_on(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -fsanitize=undefined -fsanitize-recover", "rec" + opt.strip("-"), handlers=True)
    for case, msg in list(REPORTS.items()) + list(OTHER_LINE.items()):
        rc, out = _run_out(elf, case)
        assert rc == 0 and re.search(rf"(?m)^ok {case} ", out), (case, out)
        line = rf"\d+" if case in OTHER_LINE else str(_case_line(case))
        reports = re.findall(r"(?m)^.*runtime error: .*$", out)
        assert len(reports) == 1, (case, out)
        assert re.fullmatch(rf".*sanitize_cases\.c:{line}: runtime error: {msg}", reports[0]), (case, reports)
    for case in RUNS:
        rc, out = _run_out(elf, case)
        assert rc == 0 and re.search(rf"(?m)^ok {case} ", out) and "runtime error" not in out, (case, out)
    # a place is reported once, however often it fails
    rc, out = _run_out(elf, "add_loop")
    assert rc == 0 and out.count("runtime error") == 1, out


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_handler_abort(tmp_path, opt):
    elf = _build(tmp_path, f"{opt} -fsanitize=undefined -fno-sanitize-recover", "abt" + opt.strip("-"), handlers=True)
    for case in ["add", "lmul", "divmin", "shl_sign", "bounds", "vla", "null"]:
        rc, out = _run_out(elf, case)
        assert rc != 0 and "runtime error" in out and f"ok {case}" not in out and "HardFault" not in out, (case, out)
    rc, out = _run_out(elf, "add_ok")
    assert rc == 0 and "runtime error" not in out, out
    # -fno-sanitize-trap: handler mode, recovering
    elf = _build(tmp_path, f"{opt} -fsanitize=undefined -fno-sanitize-trap", "ntr" + opt.strip("-"), handlers=True)
    rc, out = _run_out(elf, "add")
    assert rc == 0 and "runtime error: signed integer overflow" in out and "ok add " in out, out


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_trap_wins_over_handlers(tmp_path, opt):
    # -fsanitize-trap names win in any order; -ftrapv's checks always trap
    elf = _build(tmp_path, f"{opt} -fsanitize-trap=signed-integer-overflow -fsanitize=undefined -fsanitize-recover",
                 "mix" + opt.strip("-"), handlers=True)
    assert _run(elf, "add") == "trap"
    rc, out = _run_out(elf, "div0")
    assert rc == 0 and "runtime error: division by zero" in out, out
    elf = _build(tmp_path, f"{opt} -ftrapv -fsanitize=shift -fno-sanitize-recover", "tv" + opt.strip("-"),
                 handlers=True)
    assert _run(elf, "add") == "trap"
    rc, out = _run_out(elf, "shl_big")
    assert rc != 0 and "runtime error: shift exponent 32" in out, out


def _readelf(obj, *args):
    return subprocess.run(["arm-none-eabi-readelf", *args, str(obj)], capture_output=True, text=True).stdout


def test_handler_record_layout(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("int f(int a, int b)\n{\n  return a + b;\n}\nint g(int *p) { return *p; }\n")
    obj = tmp_path / "t.o"
    proc = subprocess.run([str(TCC), "-ccompat", "-c", "-O1", "-fsanitize=signed-integer-overflow,null",
                           "-fno-sanitize-trap", str(src), "-o", str(obj)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    relocs = _readelf(obj, "-rW")
    assert "__ubsan_handle_add_overflow" in relocs and "__ubsan_handle_type_mismatch_v1" in relocs, relocs
    assert "_abort" not in relocs
    data = subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".data", str(obj), "/dev/stdout"],
                          capture_output=True).stdout
    rodata = subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".rodata", str(obj), "/dev/stdout"],
                            capture_output=True).stdout
    # add_overflow {file, line 3, column 0, type}; type_mismatch_v1 {file,
    # line 5, column 0, type, log2(4), 0 = load}
    assert data[4:12] == bytes([3, 0, 0, 0, 0, 0, 0, 0]), data.hex()
    assert data[16 + 4:16 + 12] == bytes([5, 0, 0, 0, 0, 0, 0, 0]), data.hex()
    assert data[16 + 16:16 + 18] == bytes([2, 0]), data.hex()
    # one "int" descriptor {kind 0, info log2(32) << 1 | signed} and the file
    # name, shared by both records (relocations: file, type, file, type)
    assert rodata.count(bytes([0, 0, 0x0B, 0]) + b"int\0") == 1, rodata
    assert rodata.count(b"t.c\0") == 1, rodata
    assert len(re.findall(r"R_ARM_ABS32", relocs.split(".rel.text")[0])) == 4, relocs
    proc = subprocess.run([str(TCC), "-ccompat", "-c", "-O1", "-fsanitize=signed-integer-overflow",
                           "-fno-sanitize-recover=signed-integer-overflow", str(src), "-o", str(obj)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert "__ubsan_handle_add_overflow_abort" in _readelf(obj, "-rW")
