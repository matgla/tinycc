"""@src() and yasc default arguments (docs/yasc_default_args.md).

@src() is a struct __yasc_src value of two words: the function's header (a
const struct __yasc_src_fn in .rodata, one per function, whose names are
self-relative words the linker fills in with R_ARM_REL32) and the line.  A
call passing it costs an immediate and the header's address; no RAM.  A
default argument is a constant fixed where it is written, or @src(): the
caller's location.  The run-time tests are test_qemu.py's yasc_src.c and
test_yasc_src_gcc_interop.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"
READELF = shutil.which("arm-none-eabi-readelf")
OBJDUMP = shutil.which("arm-none-eabi-objdump")

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

COPY = (
    "of type 'struct __yasc_src_fn': a header's names are stored relative to it, so a copy would point nowhere; "
    "use yasc_src"
)
NOT_HERE = "default arguments (line 2) go on a function declaration's own parameters, not in a typedef, a pointer or a parameter"
STATIC = (
    "@src() is made in a function: a static yasc_src would need a relocation at load time (read @src().line, "
    ".file, .func or .module here)"
)
DEFAULT = "a default argument is a constant expression or @src()"


def _compile(tmp_path, body, *flags, name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    obj = tmp_path / (Path(name).stem + ".o")
    cmd = [str(TCC), *INCLUDE_FLAGS, f"-I{ROOT / 'include'}", "-ccompat", "-c", *flags, str(src), "-o", str(obj)]
    return subprocess.run(cmd, capture_output=True, text=True), obj


def _errors(proc):
    return [line.split(": error: ", 1)[1] for line in proc.stderr.splitlines() if ": error: " in line]


@pytest.mark.parametrize(
    "body,err",
    [
        ("int f(int a = 1);", "a default argument is yasc ('#pragma yasc' or -std=yasc)"),
        ("#pragma yasc\nint g; int f(int a = g);", DEFAULT),
        ("#pragma yasc\nint f(int a, int b = a);", DEFAULT),
        # @src() itself, not its address or a member (that would be the declaration's line)
        ("#pragma yasc\nvoid f(const struct __yasc_src *p = &@src());", DEFAULT),
        ("#pragma yasc\nvoid f(unsigned line = @src().line);", DEFAULT),
        (
            "#pragma yasc\nint f(int a = 1, int b);",
            "parameter 2 of 'f' has no default argument but parameter 1 before it has one: defaults go at the end",
        ),
        ("#pragma yasc\nint f(int a = 1); int f(int a = 2);", "parameter 1 of 'f' has another default argument already (line 2)"),
        ("#pragma yasc\ntypedef int fn(int a = 1);", NOT_HERE),
        ("#pragma yasc\nint (*fp)(int a = 1);", NOT_HERE),
        ("#pragma yasc\nvoid f(void (*cb)(int x = 1));", NOT_HERE),
        ("#pragma yasc\nint h(void) { int f(int a = 1); return 0; }", "a default argument goes in a file-scope declaration only"),
        # only a direct call fills them in, as in C++
        ("#pragma yasc\nint f(int a = 1); int h(void) { int (*p)(int) = f; return p(); }", "too few arguments to function"),
        ("#pragma yasc\nint f(int a, int b = 2); int h(void) { return f(); }", "too few arguments to function"),
        # no static yasc_src: its header address would be a load-time relocation
        ("static struct __yasc_src g = @src();", STATIC),
        ("void h(void) { static struct __yasc_src s = @src(); }", STATIC),
        # the header is never copied
        ("struct __yasc_src_fn h;", "an object " + COPY),
        ("void f(struct __yasc_src_fn h);", "a parameter " + COPY),
        ("struct S { struct __yasc_src_fn h[2]; };", "a member " + COPY),
        ("int h(struct __yasc_src at) { return &at.file != 0; }",
         "cannot take the address of a yasc_src name: its word is a distance, not a pointer"),
        ("struct __yasc_src { int x; };", "struct/union/enum already defined"),
        ("int x = @foo();", "unknown builtin '@foo' (there are @min, @max and @src)"),
    ],
)
def test_rejected(tmp_path, body, err):
    proc, _ = _compile(tmp_path, body)
    assert proc.returncode != 0
    assert _errors(proc)[:1] == [err], proc.stderr


@pytest.mark.parametrize(
    "body",
    [
        # said again the same, or added by a later declaration
        "#pragma yasc\nint f(int a = 1); int f(int a = 1); int h(void) { return f(); }",
        "#pragma yasc\nint f(int a, int b); int f(int a, int b = 2); int f(int a = 1, int b); int h(void) { return f(); }",
        # constants: sizeof, an enumerator, nullptr, a string, a function's address, a cast
        "#pragma yasc\nenum { K = 4 };\nstatic int one(void) { return 1; }\n"
        "int f(int a = sizeof(long) * K, char *s = \"x\", int *?p = nullptr, int (*g)(void) = one,"
        " unsigned char c = (unsigned char)300);\nint h(void) { return f(); }",
        # a pointer parameter with a default is a view as any other
        "#pragma yasc\nstruct S;\nvoid f(const struct S *p = 0) {}\nvoid h(void) { f(); }",
        # yasc_src.h names the type before any @src(); a value, copied and kept
        "#pragma yasc\n#include <yasc_src.h>\nstatic yasc_src last;\n"
        "unsigned line(int a, yasc_src at = @src()) { last = at; return at.line + a; }\n"
        "unsigned h(void) { yasc_src c = @src(); yasc_src *p = &c; return line(1) + line(2, c) + p->line; }",
        # written out anywhere, at file scope too; sizeof / typeof make nothing
        "static const char *f = @src().file;\nstatic unsigned l = @src().line;\n"
        "int h(void) { return sizeof(@src()) + l + f[0]; }",
        "__typeof__(@src().file) q;",
        # C code calls a function the yasc header gave defaults
        "#pragma yasc\nint f(int a = 5);\n#pragma tcc c_begin\nint h(void) { return f(); }\n#pragma tcc c_end\n",
        # the types made in the middle of a parameter list outlive it
        "#pragma yasc\nvoid f(int a, struct __yasc_src at = @src()); void f(int a, struct __yasc_src at) {}",
    ],
)
def test_accepted(tmp_path, body):
    proc, _ = _compile(tmp_path, body)
    assert proc.returncode == 0, proc.stderr
    assert proc.stderr == ""


@pytest.mark.skipif(READELF is None, reason="needs arm-none-eabi-readelf")
@pytest.mark.parametrize("opt", ["-O0", "-O1", "-Os"])
def test_one_header_per_function_in_rodata(tmp_path, opt):
    """Three @src() in two functions: two headers, in .rodata only (no .data:
    their words are no load-time relocations), three PC-relative words each."""
    body = (
        "#pragma yasc\nextern int printf(const char *, ...);\n"
        "int f(struct __yasc_src at = @src()) { return printf(\"%s %s %u\", at.file, at.func, at.line); }\n"
        "int g(void) { return f() + f(); }\nint k(void) { return f(); }\n"
    )
    proc, obj = _compile(tmp_path, body, opt)
    assert proc.returncode == 0, proc.stderr
    sections = subprocess.run([READELF, "-SW", str(obj)], capture_output=True, text=True, check=True).stdout
    data = [line for line in sections.splitlines() if " .data " in line]
    assert not data or " 000000 " in data[0], sections
    relocs = subprocess.run([READELF, "-rW", str(obj)], capture_output=True, text=True, check=True).stdout
    rodata = relocs.split("'.rel.rodata'", 1)[1].split("Relocation section", 1)[0]
    kinds = [line.split()[2] for line in rodata.splitlines() if "R_ARM_" in line]
    assert kinds == ["R_ARM_REL32"] * 6, rodata


@pytest.mark.skipif(OBJDUMP is None, reason="needs arm-none-eabi-objdump")
@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_call_passes_two_words(tmp_path, opt):
    """A call that leaves the location out: the header's address and the line
    straight into r1/r2 -- no frame, no store (ra:struct_arg_split)."""
    body = (
        "#pragma yasc\nvoid log_warn(const char *msg, struct __yasc_src at = @src());\n"
        "void two(void) { log_warn(\"a\"); log_warn(\"b\"); }\n"
    )
    proc, obj = _compile(tmp_path, body, opt)
    assert proc.returncode == 0, proc.stderr
    dis = subprocess.run([OBJDUMP, "-d", str(obj)], capture_output=True, text=True, check=True).stdout
    insns = [line.split("\t")[2].split()[0] for line in dis.split("<two>:", 1)[1].splitlines()
             if line.count("\t") >= 2][:12]
    assert "str" not in insns and "strd" not in insns and "sub" not in insns, insns
    assert insns.count("bl") == 2, insns


@pytest.mark.skipif(READELF is None, reason="needs arm-none-eabi-readelf")
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_member_read_makes_no_object(tmp_path, opt):
    """`@src().file` / `.line` written out cost what __FILE__ / __LINE__ do:
    a string's address and a constant, no @src() object."""
    head = "extern int printf(const char *, ...);\nvoid f(void) { printf(\"%s:%u %s\", "
    sizes = []
    for args in ("@src().file, @src().line, @src().func", "__FILE__, __LINE__, __func__"):
        proc, obj = _compile(tmp_path, head + args + "); }\n", opt, name="m.c")
        assert proc.returncode == 0, proc.stderr
        out = subprocess.run([READELF, "-SW", str(obj)], capture_output=True, text=True, check=True).stdout
        sizes.append(sorted(line.split()[1:2] + line.split()[5:6] for line in out.splitlines()
                            if " .text" in line or " .rodata" in line))
        relocs = subprocess.run([READELF, "-rW", str(obj)], capture_output=True, text=True, check=True).stdout
        assert "R_ARM_REL32" not in relocs, relocs
    assert sizes[0] == sizes[1], sizes


def test_c_header_drops_defaults(tmp_path):
    """tcc -yasc-c-header: the C declaration lists every parameter."""
    inc = tmp_path / "inc"
    (inc / "yasc").mkdir(parents=True)
    (inc / "yasc" / "log.h").write_text(
        "#pragma yasc\n#pragma once\n#include <yasc_src.h>\n"
        "void log_at(const char *msg, yasc_src at = @src());\n"
        "int parse(const char *s, int base = 10, int (*conv)(int) = nullptr);\n"
    )
    proc = subprocess.run([str(TCC), "-yasc-c-header", "yasc/log.h"], cwd=inc, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    assert "void log_at(const char *msg, yasc_src at);" in proc.stdout, proc.stdout
    assert "int parse(const char *s, int base, int (*conv)(int));" in proc.stdout, proc.stdout
    assert "=" not in proc.stdout.split("#include <yasc_src.h>", 1)[1], proc.stdout


def test_module_defaults(tmp_path):
    """An exported function's defaults reach importers (.ymi) and stay out of
    its -fmodule-header C header."""
    ifc, hdr = tmp_path / "ifc", tmp_path / "hdr"
    proc, _ = _compile(
        tmp_path,
        "#pragma yasc\nmodule logm;\n#include <yasc_src.h>\n"
        "export int log_at(int level = 2, yasc_src at = @src()) { return level * 1000 + (int)at.line; }\n",
        f"-fmodule-dir={ifc}",
        f"-fmodule-header={hdr}",
        name="logm.c",
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    header = (hdr / "logm.h").read_text()
    assert "int log_at(int level, yasc_src at);" in header, header
    proc, _ = _compile(
        tmp_path,
        "#pragma yasc\nimport logm;\nint h(void) { return log_at() + log_at(1) + log_at(1, @src()); }\n",
        f"-fmodule-dir={ifc}",
        name="user.c",
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
