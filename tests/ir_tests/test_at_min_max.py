"""@min / @max: the short builtins (source/frontend/gen/builtin/misc.c).

What they promise over `a < b ? a : b` and a MIN() macro:
  - each operand is evaluated once, left to right;
  - a signed operand that may be negative never meets an unsigned one (the
    ternary turns -1 into UINT_MAX): an error, unless it is a non-negative
    constant -- @min(buflen, PATH_MAX) with a size_t buflen is fine;
  - code as good as the ternary: cmp/it/mov for integers, one
    vminnm.f32/vmaxnm.f32 on an FPv5 FPU, a call to fmin/fmax for doubles.
The runtime results are 910_at_min_max.c (FP matrix in test_qemu.py).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
OBJDUMP = "arm-none-eabi-objdump"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


def _compile(tmp_path, body, *flags, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    obj = tmp_path / "t.o"
    cmd = [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(obj)]
    return subprocess.run(cmd, capture_output=True, text=True), obj


def _errors(proc):
    return [line.split(": error: ", 1)[1] for line in proc.stderr.splitlines() if ": error: " in line]


def _insns(obj, func):
    out = subprocess.run([OBJDUMP, "-d", str(obj)], capture_output=True, text=True).stdout
    insns, inside = [], False
    for line in out.splitlines():
        if line.endswith(f"<{func}>:"):
            inside = True
            continue
        if inside:
            if not line.strip():
                break
            parts = line.split("\t")
            if len(parts) >= 3:
                insns.append(parts[2].split()[0])
    return insns


SIGNED_UNSIGNED = "@{op} of signed '{s}' and unsigned '{u}': a negative value would compare as a huge one; cast one of them"


@pytest.mark.parametrize(
    "body,err",
    [
        ("int f(int i, unsigned u) { return @min(i, u); }", SIGNED_UNSIGNED.format(op="min", s="int", u="unsigned int")),
        ("unsigned f(unsigned u, int i) { return @max(u, i); }", SIGNED_UNSIGNED.format(op="max", s="int", u="unsigned int")),
        ("unsigned f(unsigned u) { return @min(u, -1); }", SIGNED_UNSIGNED.format(op="min", s="int", u="unsigned int")),
        (
            "unsigned long long f(int a, unsigned long long b) { return @min(a, b); }",
            SIGNED_UNSIGNED.format(op="min", s="int", u="unsigned long long"),
        ),
        ("int f(int *p) { return @min(p, 1); }", "@min wants numbers, not 'int *'"),
        ("struct S { int x; }; int f(struct S s) { return @max(s, 1); }", "@max wants numbers, not 'struct S'"),
        ("int f(int a) { return @foo(a); }", "unknown builtin '@foo' (there are @min, @max and @src)"),
        # a static initializer parses with no code wanted: not constant, not the constant operand
        ("int g; static int x = @min(g, 3);", "initializer element is not constant"),
    ],
)
def test_rejected(tmp_path, body, err):
    proc, _ = _compile(tmp_path, body)
    assert proc.returncode != 0
    assert _errors(proc)[:1] == [err], proc.stderr


@pytest.mark.parametrize(
    "body",
    [
        # a non-negative constant meets an unsigned operand
        "typedef unsigned size_t; size_t f(size_t n) { return @min(n, 4096); }",
        "unsigned f(unsigned u) { return @max(u, 0); }",
        # unsigned char / short promote to int: the comparison is signed and exact
        "int f(unsigned short s, int i) { return @min(s, i); }",
        "int f(unsigned char c, int i) { return @max(c, i); }",
        # unsigned int fits in long long: signed and exact
        "long long f(unsigned a, long long b) { return @min(a, b); }",
        # mixed int and float: the usual arithmetic conversion
        "float f(int i, float x) { return @max(i, x); }",
        # constants: static initializers and sizeof
        "static const int k = @max(3, -7); int f(void) { return k + (int)sizeof(@min(1, 2LL)); }",
        # through a macro, as code that a regular compiler also builds spells it
        "#define MIN_OF(a, b) @min(a, b)\nint f(int a, int b) { return MIN_OF(a, b); }",
        # in yasc
        "#pragma yasc\nint f(int a, int b) { return @min(a, b) + @max(a, b); }",
    ],
)
def test_accepted(tmp_path, body):
    proc, _ = _compile(tmp_path, body)
    assert proc.returncode == 0, proc.stderr
    assert proc.stderr == ""


def test_constant_fold(tmp_path):
    proc, obj = _compile(tmp_path, "int f(void) { return @max(3, -7) * 100 + @min(10u, 20u); }", "-O0")
    assert proc.returncode == 0, proc.stderr
    assert _insns(obj, "f") == ["movw", "bx"] or _insns(obj, "f")[-1] == "bx"
    dis = subprocess.run([OBJDUMP, "-d", str(obj)], capture_output=True, text=True).stdout
    assert "#310" in dis


@pytest.mark.parametrize("level", ["-O1", "-O2", "-Os"])
@pytest.mark.parametrize(
    "body,func",
    [
        ("typedef unsigned size_t; size_t f(size_t n) { return @min(n, 4096); }", "f"),
        ("int f(int a, int b) { return @max(a, b); }", "f"),
        ("int f(int a, int b) { return @min(a, b); }", "f"),
        ("unsigned f(unsigned a, unsigned b) { return @max(a, b); }", "f"),
    ],
)
def test_integer_is_cmp_it_mov(tmp_path, level, body, func):
    proc, obj = _compile(tmp_path, body, level)
    assert proc.returncode == 0, proc.stderr
    insns = _insns(obj, func)
    assert insns[0].startswith("cmp") and insns[1] == "it" and insns[2].startswith("mov") and insns[3] == "bx", insns


@pytest.mark.parametrize("level", LEVELS)
@pytest.mark.parametrize("fpu", ["-mfpu=fpv5-sp-d16", "-mfpu=rp2350"])
def test_float_is_vminnm(tmp_path, level, fpu):
    body = "float mn(float a, float b) { return @min(a, b); }\nfloat mx(float a, float b) { return @max(a, b); }\n"
    proc, obj = _compile(tmp_path, body, level, fpu)
    assert proc.returncode == 0, proc.stderr
    assert "vminnm.f32" in _insns(obj, "mn") and "bl" not in _insns(obj, "mn")
    assert "vmaxnm.f32" in _insns(obj, "mx") and "bl" not in _insns(obj, "mx")
    proc, obj = _compile(tmp_path, body, level, fpu, "-mfloat-abi=hard")
    assert proc.returncode == 0, proc.stderr
    assert "vminnm.f32" in _insns(obj, "mn")


def _calls(obj):
    # readelf: objdump -r stops at tcc's own relocation type 0x8b
    out = subprocess.run(["readelf", "-rW", str(obj)], capture_output=True, text=True).stdout
    return [line.split()[-1] for line in out.splitlines() if "R_ARM_THM_CALL" in line or "R_ARM_THM_JUMP24" in line]


@pytest.mark.parametrize(
    "flags,body,callee",
    [
        # no FPv5 (and -mfloat-abi=soft: no FPU code at all): the libm call
        (["-mfpu=fpv4-sp-d16"], "float f(float a, float b) { return @min(a, b); }", "fminf"),
        (["-mfpu=fpv5-sp-d16", "-mfloat-abi=soft"], "float f(float a, float b) { return @max(a, b); }", "fmaxf"),
        # doubles: the call on every FPU here (on the RP2350 they are the DCP's)
        (["-mfpu=rp2350"], "double f(double a, double b) { return @min(a, b); }", "fmin"),
        (["-mfpu=fpv5-sp-d16"], "double f(double a, double b) { return @max(a, b); }", "fmax"),
    ],
)
def test_float_falls_back_to_call(tmp_path, flags, body, callee):
    proc, obj = _compile(tmp_path, body, "-O1", *flags)
    assert proc.returncode == 0, proc.stderr
    assert callee in _calls(obj)


def test_preprocessor_keeps_at(tmp_path):
    src = tmp_path / "p.c"
    src.write_text("#define MIN_OF(a, b) @min(a, b)\nint x = MIN_OF(1, 2);\n")
    out = subprocess.run([str(TCC), "-E", str(src)], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    assert "@min(1, 2)" in out.stdout.replace("@ min", "@min")
