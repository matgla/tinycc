"""yasc modules, step 1: `module NAME;`, `export`, private-by-default linkage.

docs/yasc_modules.md has the design.  A module unit's file-scope declarations
get internal linkage unless they say `export`; exported names keep their plain
C symbol names.  `module`, `import` and `export` are keywords only where a
file-scope declaration starts, so ordinary C uses of the words still compile.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
NM = shutil.which("arm-none-eabi-nm")

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _compile(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), *INCLUDE_FLAGS, "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


def _yasc(tmp_path, body, *flags):
    return _compile(tmp_path, "#pragma yasc\n" + body, *flags)


def _error(proc):
    assert proc.returncode != 0, "expected a compile error"
    lines = [line.split("error: ", 1)[1] for line in proc.stderr.splitlines() if "error: " in line]
    assert lines, proc.stderr
    return lines[0]


def _symbols(tmp_path):
    out = subprocess.run([NM, str(tmp_path / "t.o")], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        syms[parts[-1]] = parts[-2]
    return syms


MODULE = """\
module net.socket;

export typedef struct sock sock_t;
export typedef struct point { int x, y; } point_t;
export enum color { RED, GREEN };
struct sock { int fd; };

export sock_t * owned maybe sock_open(int port);
export int counter = 3;
int helper(int x) { return x + counter; }
static int helper2(void) { return 1; }
int priv_global;
int priv_init = 7;
extern int outside;
export point_t origin(void) { point_t p = {helper(1), helper2() + outside + priv_global + priv_init}; return p; }
sock_t * owned maybe sock_open(int port) { (void)port; return 0; }
export int use_color(enum color c) { return c; }
"""


@pytest.mark.skipif(NM is None, reason="needs arm-none-eabi-nm")
@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_module_linkage(tmp_path, opt):
    proc = _yasc(tmp_path, MODULE, opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    syms = _symbols(tmp_path)
    # exported: global, plain C names
    assert syms["origin"] == "T"
    assert syms["sock_open"] == "T"  # defined without `export` after an exported prototype
    assert syms["use_color"] == "T"
    assert syms["counter"] == "D"
    # private: local (or gone once inlined / unused)
    for name in ("helper", "helper2", "priv_global", "priv_init"):
        assert syms.get(name, "t").islower(), (name, syms.get(name))
    # extern still names something outside the module
    assert syms["outside"] == "U"


def test_not_a_module_keeps_c_linkage(tmp_path):
    if NM is None:
        pytest.skip("needs arm-none-eabi-nm")
    proc = _yasc(tmp_path, "int f(void) { return 1; }\nint g;\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    syms = _symbols(tmp_path)
    assert syms["f"] == "T"


def test_words_stay_identifiers_elsewhere(tmp_path):
    body = "int module = 1;\nint export(int import) { return import + module; }\n"
    proc = _yasc(tmp_path, body)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    proc = _compile(tmp_path, body, "-std=c23")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_typedef_named_module(tmp_path):
    proc = _yasc(tmp_path, "typedef int module;\nmodule x;\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_opaque_handle(tmp_path):
    proc = _yasc(tmp_path, "module a;\nstruct p { int x; };\nexport typedef struct p p_t;\nexport struct p * borrows(static) get(void);\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


def test_exported_union_by_value(tmp_path):
    proc = _yasc(tmp_path, "module a;\nexport union u { int a; float b; };\nexport union u get(void);\n")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


@pytest.mark.parametrize(
    "body, message",
    [
        (
            "module a;\nstruct p { int x; };\nexport struct p get(void);\n",
            "exported 'get' uses struct p by value, but its body is private: export the definition or use a pointer",
        ),
        (
            "module a;\nstruct p { int x; };\nexport struct q { struct p in; };\n",
            "exported 'q' uses struct p by value, but its body is private: export the definition or use a pointer",
        ),
        (
            "module a;\nstruct p { int x; };\nexport typedef struct p p3[3];\n",
            "exported 'p3' uses struct p by value, but its body is private: export the definition or use a pointer",
        ),
        (
            "module a;\nunion u { int x; };\nexport void put(union u);\n",
            "exported 'put' uses union u by value, but its body is private: export the definition or use a pointer",
        ),
        ("module a;\nenum e { A };\nexport int f(enum e);\n", "exported 'f' uses enum e, which is private: export the enum"),
        ("module a;\nexport static int f(void);\n", "'f' cannot be both export and static"),
        (
            "module a;\nint f(void);\nexport int f(void) { return 0; }\n",
            "'f' was declared private before: put 'export' on its first declaration",
        ),
        ("export int f(void);\n", "'export' outside a module: start the file with 'module NAME;'"),
        ("int x;\nmodule a;\n", "'module' must come before the file's first declaration"),
        ("module a;\nmodule b;\n", "a second module declaration: the file is already module 'a'"),
        ("module a;\nexport import b;\n", "'export import' is not supported yet"),
    ],
)
def test_module_errors(tmp_path, body, message):
    assert _error(_yasc(tmp_path, body)) == message


# ---------------------------------------------------------------------------
# Step 2: the interface file (.ymi)

TOK_IDENT = 256
_VAL1 = {0xC0, 0xC1, 0xC2, 0xC3, 0xC6, 0xC7, 0xCA, 0xCD, 0xD0, 0xD3, 0xD4}
_VAL2 = {0xC4, 0xC5, 0xCB, 0xCC, 0xCE, 0xCF}
_STRS = {0xC8, 0xC9, 0xD1, 0xD2}


def _load_ymi(path):
    """Decode a .ymi: header fields and the interface as a list of token spellings
    (punctuators of one character as themselves, int constants as numbers)."""
    import struct

    b = path.read_bytes()
    o = 4

    def u32():
        nonlocal o
        v = struct.unpack_from("<I", b, o)[0]
        o += 4
        return v

    def s():
        nonlocal o
        n = u32()
        v = b[o : o + n].decode()
        o += n
        return v

    assert b[:4] == b"YMI1"
    d = {"version": u32(), "id": s(), "module": s(), "source": s()}
    d["includes"] = [s() for _ in range(u32())]
    d["imports"] = [s() for _ in range(u32())]
    strings = [s() for _ in range(u32())]
    n = u32()
    words = struct.unpack_from("<%dI" % n, b, o)
    assert o + 4 * n == len(b)
    toks, i = [], 0
    while i < n:
        t = words[i]
        i += 1
        if t >= TOK_IDENT:
            toks.append(strings[t - TOK_IDENT])
        elif t == 0xD3:  # line number
            i += 1
        elif t in _VAL1:
            toks.append(str(words[i]) if t in (0xC2, 0xC3) else "<v%x>" % t)
            i += 1
        elif t in _VAL2:
            toks.append("<v%x>" % t)
            i += 2
        elif t in _STRS:
            size = words[i]
            nw = (size + 3) // 4
            raw = struct.pack("<%dI" % nw, *words[i + 1 : i + 1 + nw])[:size].rstrip(b"\0").decode()
            toks.append('"%s"' % raw if t == 0xC8 else raw)
            i += 1 + nw
        elif t < 128:
            toks.append(chr(t))
        else:
            toks.append("<%x>" % t)
    d["text"] = " ".join(toks)
    return d


IFACE_MODULE = """\
module net.socket;
#include <stddef.h>
#include "local.h"

export typedef struct sock sock_t;
export typedef struct point { int x, y; } point_t;
export enum color { RED, GREEN = 5 };
struct sock { int fd; };

export sock_t * owned maybe sock_open(int port);
export int counter = 3, other, arr[2] = {1, 2};
int helper(int x) { return x + counter; }
export point_t origin(void) { point_t p = {helper(1), 2}; return p; }
sock_t * owned maybe sock_open(int port) { (void)port; return 0; }
export inline int twice(int v) { return v * 2 + RED; }
export static inline int thrice(int v) { return v * 3; }
export struct __attribute__((packed)) packed_s { char c; int i; };
export size_t len_of(const char * view s);
"""

IFACE_TEXT = (
    "typedef struct sock sock_t ; "
    "typedef struct point { int x , y ; } point_t ; "
    "enum color { RED , GREEN = 5 } ; "
    "sock_t * owned maybe sock_open ( int port ) ; "
    "extern int counter , other , arr [ 2 ] ; "
    "point_t origin ( void ) ; "
    "static inline int twice ( int v ) { return v * 2 + RED ; } "
    "static inline int thrice ( int v ) { return v * 3 ; } "
    "struct __attribute__ ( ( packed ) ) packed_s { char c ; int i ; } ; "
    "size_t len_of ( const char * view s ) ;"
)


def _module(tmp_path, body, *flags, out="m.o"):
    (tmp_path / "local.h").write_text("#define LOCAL 1\n")
    src = tmp_path / "m.c"
    src.write_text("#pragma yasc\n" + body)
    return subprocess.run(
        [str(TCC), *INCLUDE_FLAGS, "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / out)],
        capture_output=True,
        text=True,
    )


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_interface_contents(tmp_path, opt):
    proc = _module(tmp_path, IFACE_MODULE, opt)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    d = _load_ymi(tmp_path / "net" / "socket.ymi")
    assert d["version"] == 1 and d["module"] == "net.socket"
    assert d["id"].startswith("tcc ")
    assert d["source"].endswith("m.c")
    assert d["includes"] == ["<stddef.h>", str((tmp_path / "local.h").resolve())]
    assert d["imports"] == []
    assert d["text"] == IFACE_TEXT
    assert "helper" not in d["text"] and "fd" not in d["text"]


def test_module_dir(tmp_path):
    proc = _module(tmp_path, "module a.b;\nexport int f(void);\n", "-fmodule-dir=" + str(tmp_path / "ifc"))
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    assert _load_ymi(tmp_path / "ifc" / "a" / "b.ymi")["text"] == "int f ( void ) ;"
    assert not (tmp_path / "a").exists()


def test_interface_unchanged_keeps_its_time(tmp_path):
    import os

    body = "module a;\nexport int f(int x) { return x + 1; }\n"
    assert _module(tmp_path, body).returncode == 0
    ymi = tmp_path / "a.ymi"
    os.utime(ymi, ns=(1_000_000_000, 1_000_000_000))
    assert _module(tmp_path, body.replace("x + 1", "x * 2")).returncode == 0
    assert ymi.stat().st_mtime_ns == 1_000_000_000  # a body edit leaves it alone
    assert _module(tmp_path, body.replace("int x", "long x")).returncode == 0
    assert ymi.stat().st_mtime_ns != 1_000_000_000  # a signature edit rewrites it
    assert _load_ymi(ymi)["text"] == "int f ( long x ) ;"


def test_no_interface_without_module_or_on_error(tmp_path):
    assert _module(tmp_path, "int f(void) { return 1; }\n").returncode == 0
    assert list(tmp_path.glob("*.ymi")) == []
    assert _module(tmp_path, "module a;\nexport int f(void) { return x; }\n").returncode != 0
    assert list(tmp_path.glob("*.ymi")) == []


@pytest.mark.parametrize(
    "body, message",
    [
        (
            "module a;\nexport struct p get(void);\nstruct p { int x; };\n",
            "exported 'get' uses struct p by value, but its body is private: export the definition or use a pointer",
        ),
        (
            "module a;\ntypedef int priv_t;\nexport priv_t get(void);\n",
            "an exported declaration uses the private typedef 'priv_t': export it",
        ),
        (
            "module a;\nenum { N = 4 };\nexport int tbl[N];\n",
            "an exported declaration uses 'N' of a private enum: export the enum",
        ),
        (
            "module a;\nstatic int h(void) { return 1; }\nexport inline int f(void) { return h(); }\n",
            "an exported inline function uses the private 'h': export it",
        ),
        (
            "module a;\nstruct s { int fd; };\nexport inline int fd_of(struct s * view p) { return p->fd; }\n",
            "an exported inline function uses struct s, whose body is private: export the definition",
        ),
        (
            "module a;\n#pragma pack(1)\nexport struct x { char c; int i; };\n",
            "'export' under #pragma pack is not supported yet: use __attribute__((packed))",
        ),
    ],
)
def test_interface_errors(tmp_path, body, message):
    assert _error(_yasc(tmp_path, body)) == message


def test_interface_names_that_are_fine(tmp_path):
    # members are not names; exported enums and typedefs may be used; so may
    # what `extern` declares (it is outside the module, not private)
    body = (
        "module a;\nextern int outside(void);\nexport inline int h(void) { return outside(); }\n"
        "int fd;\nexport struct q { int fd; };\n"
        "export inline int g(struct q * view p) { return p->fd; }\n"
        "export enum { M = 2 };\nexport int tbl[M];\n"
        "export typedef int pub_t;\nexport pub_t get(void);\n"
    )
    proc = _yasc(tmp_path, body)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    body = (
        "module a;\nint fd;\nexport struct q { int fd; };\n"
        "export inline int g(struct q * view p) { return p->fd; }\n"
        "export enum { M = 2 };\nexport int tbl[M];\n"
        "export typedef int pub_t;\nexport pub_t get(void);\n"
    )
    proc = _yasc(tmp_path, body)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


# ---------------------------------------------------------------------------
# Step 3: import (the run-time test is test_qemu.py::test_yasc_modules_run)


def _cc(tmp_path, name, body, *flags):
    src = tmp_path / f"{name}.c"
    src.write_text("#pragma yasc\n" + body)
    return subprocess.run(
        [str(TCC), *INCLUDE_FLAGS, "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / f"{name}.o")],
        capture_output=True,
        text=True,
    )


def _ok(proc):
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    return proc


def test_import_records_imports(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport typedef struct p { int x; } p_t;\n"))
    _ok(_cc(tmp_path, "b", "module b;\nimport a;\nexport p_t twice(p_t v);\n"))
    d = _load_ymi(tmp_path / "b.ymi")
    assert d["imports"] == ["a"]
    assert d["text"] == "p_t twice ( p_t v ) ;"
    # b's importer gets a too, and a second import is a no-op
    _ok(_cc(tmp_path, "m", "import b;\nimport a;\nint f(void) { p_t v = {1}; return twice(v).x; }\n"))


def test_import_ignores_importer_macros(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport struct p { int x; };\nexport int get(struct p * view q);\n"))
    _ok(_cc(tmp_path, "m", "#define x 999\n#define get broken(\nimport a;\n#undef x\n#undef get\n"
            "int f(struct p * view q) { return q->x + get(q); }\n"))


def test_import_includes_module_headers(tmp_path):
    (tmp_path / "h.h").write_text("typedef int handle_t;\n#define HANDLE_MAX 7\n")
    _ok(_cc(tmp_path, "a", "module a;\n#include \"h.h\"\nexport handle_t first(void);\n"))
    # the importer gets h.h's declarations, and its macros (legacy C is legacy)
    _ok(_cc(tmp_path, "m", "import a;\nint f(void) { handle_t h = first(); return h + HANDLE_MAX; }\n"))


def test_import_dependency_file(tmp_path):
    ifc = tmp_path / "ifc"
    _ok(_cc(tmp_path, "a", "module a;\nexport int f(void);\n", f"-fmodule-dir={ifc}"))
    _ok(_cc(tmp_path, "m", "import a;\nint g(void) { return f(); }\n", f"-fmodule-dir={ifc}",
            "-MD", "-MF", str(tmp_path / "m.d")))
    assert str(ifc / "a.ymi") in (tmp_path / "m.d").read_text()


def _import_error(tmp_path, body, *flags):
    return _error(_cc(tmp_path, "m", body, *flags))


def test_import_not_found(tmp_path):
    assert _import_error(tmp_path, "import nosuch;\n") == (
        "module 'nosuch' not found: compile it first (its interface is looked for in -fmodule-dir=, "
        "the object's directory and -I)"
    )


def test_import_itself(tmp_path):
    assert _import_error(tmp_path, "module m;\nimport m;\n") == "module 'm' imports itself"


def test_import_cycle(tmp_path):
    _ok(_cc(tmp_path, "cb", "module cb;\nexport int fb(void);\n"))
    _ok(_cc(tmp_path, "ca", "module ca;\nimport cb;\nexport int fa(void);\n"))
    assert _error(_cc(tmp_path, "cb", "module cb;\nimport ca;\nexport int fb(void);\n")) == (
        "modules import each other: 'ca' imports 'cb', the module being compiled"
    )


def test_import_missing_semicolon(tmp_path):
    assert _import_error(tmp_path, "import a\nint x;\n") == "';' after the module name expected"


def test_import_damaged(tmp_path):
    (tmp_path / "bad.ymi").write_bytes(b"garbage")
    assert _import_error(tmp_path, "import bad;\n").endswith("bad.ymi' is damaged or not a module interface")
    _ok(_cc(tmp_path, "a", "module a;\nexport int f(void);\n"))
    data = (tmp_path / "a.ymi").read_bytes()
    (tmp_path / "a.ymi").write_bytes(data[:-3])  # cut short
    assert _import_error(tmp_path, "import a;\n").endswith("a.ymi' is damaged or not a module interface")


def test_import_wrong_module_name(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport int f(void);\n"))
    (tmp_path / "a.ymi").rename(tmp_path / "other.ymi")
    assert _import_error(tmp_path, "import other;\n").endswith("other.ymi' is the interface of module 'a', not 'other'")


def test_import_other_compiler(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport int f(void);\n"))
    ymi = tmp_path / "a.ymi"
    ymi.write_bytes(ymi.read_bytes().replace(b"tcc ", b"xcc ", 1))
    msg = _import_error(tmp_path, "import a;\n")
    assert "was built by another compiler or for another ABI (xcc " in msg and msg.endswith("rebuild module 'a'")


def test_import_other_float_abi(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport int f(void);\n", "-mfloat-abi=soft"))
    msg = _import_error(tmp_path, "import a;\n", "-mfloat-abi=hard", "-mfpu=fpv5-sp-d16")
    assert "for another ABI" in msg


def test_import_conflict_points_at_the_module(tmp_path):
    _ok(_cc(tmp_path, "a", "module a;\nexport struct p { int x; };\n"))
    proc = _cc(tmp_path, "m", "struct p { int y; };\nimport a;\n")
    assert proc.returncode != 0
    assert "m.c:3:\n" in proc.stderr, proc.stderr  # the import's line
    assert "a.c:3: error: struct/union/enum already defined" in proc.stderr, proc.stderr


# ---------------------------------------------------------------------------
# Step 4: -fmodule-deps


def _deps(tmp_path, *names, flags=()):
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-fmodule-deps", *flags, *[str(tmp_path / f"{n}.c") for n in names]],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    return proc.stdout


def test_module_deps(tmp_path):
    (tmp_path / "a.c").write_text("#pragma yasc\nmodule app.main;\nimport net.sock;\nimport geom;\nint x;\n")
    assert _deps(tmp_path, "a") == "module app.main\nimport net.sock\nimport geom\n"
    assert list(tmp_path.glob("**/*.ymi")) == []  # nothing compiled, nothing imported


def test_module_deps_after_preprocessing(tmp_path):
    (tmp_path / "a.c").write_text(
        "#pragma yasc\n#define USE_NET 1\n#define MOD geom\nmodule app;\n"
        "#if USE_NET\nimport net.sock;\n#else\nimport net.fake;\n#endif\n"
        "import MOD;\nstruct s { int import; };\nint f(void) { int module = 1; return module; }\n"
        "import late.one;\n"
    )
    assert _deps(tmp_path, "a") == "module app\nimport net.sock\nimport geom\nimport late.one\n"


def test_module_deps_plain_c_and_headers(tmp_path):
    (tmp_path / "h.h").write_text("int import;\n")
    (tmp_path / "c.c").write_text('#include "h.h"\nint module(void) { return import; }\n')
    assert _deps(tmp_path, "c") == ""  # not yasc: the words are names
    (tmp_path / "y.c").write_text('#pragma yasc\n#include "h.h"\nimport a;\n')
    assert _deps(tmp_path, "y") == "import a\n"


def test_module_deps_several_files_and_output(tmp_path):
    (tmp_path / "a.c").write_text("#pragma yasc\nmodule a;\n")
    (tmp_path / "b.c").write_text("#pragma yasc\nmodule b;\nimport a;\n")
    out = _deps(tmp_path, "a", "b")
    assert out == f"file {tmp_path / 'a.c'}\nmodule a\nfile {tmp_path / 'b.c'}\nmodule b\nimport a\n"
    _deps(tmp_path, "b", flags=("-o", str(tmp_path / "b.deps")))
    assert (tmp_path / "b.deps").read_text() == "module b\nimport a\n"


MAKEFILE = """\
TCC = {tcc} -ccompat
SRCS = main.c shapes.c geom.c
all: $(SRCS:.c=.o)
%.o: %.c
\t$(TCC) -c $< -o $@ -MD -MF $*.d
%.mdep: %.c
\t$(TCC) -fmodule-deps $< | awk -v o=$*.o '{{ i = $$2; gsub(/\\./, "/", i); i = i ".ymi" }} \\
\t  $$1 == "module" {{ print i ": " o " ;" }} $$1 == "import" {{ print o ": " i }}' > $@
include $(SRCS:.c=.mdep)
-include $(SRCS:.c=.d)
"""


@pytest.mark.skipif(shutil.which("make") is None or shutil.which("awk") is None, reason="needs make and awk")
def test_module_deps_orders_a_parallel_make(tmp_path):
    """The recipe in docs/yasc_modules.md: modules build before their importers,
    a body edit rebuilds one object, a signature edit its importers too."""
    import os
    import time

    (tmp_path / "geom.c").write_text(
        "#pragma yasc\nmodule lib.geom;\nexport typedef struct point { int x, y; } point_t;\n"
        "export int moves = 0;\nexport int norm(point_t p) { return p.x + p.y; }\n"
    )
    (tmp_path / "shapes.c").write_text(
        "#pragma yasc\nmodule shapes;\nimport lib.geom;\n"
        "export int twice(point_t p) { moves++; return 2 * norm(p); }\n"
    )
    (tmp_path / "main.c").write_text("#pragma yasc\nimport shapes;\nint f(void) { point_t p = {1, 2}; return twice(p); }\n")
    (tmp_path / "Makefile").write_text(MAKEFILE.format(tcc=TCC))

    def make():
        proc = subprocess.run(["make", "-j8", "-C", str(tmp_path)], capture_output=True, text=True)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        return [line.split(" -o ")[1].split()[0] for line in proc.stdout.splitlines() if " -c " in line]

    assert make() == ["geom.o", "shapes.o", "main.o"]
    assert make() == []
    time.sleep(1.1)
    src = tmp_path / "geom.c"
    src.write_text(src.read_text().replace("p.x + p.y", "p.y + p.x"))
    assert make() == ["geom.o"]
    time.sleep(1.1)
    src.write_text(src.read_text().replace("export int moves", "export long moves"))
    assert sorted(make()) == ["geom.o", "main.o", "shapes.o"]


# ---------------------------------------------------------------------------
# Step 5: -fmodule-header= (the run-time test is test_qemu.py::test_yasc_modules_c_caller)

HEADER_MODULE = """\
module net.socket;
#include <stddef.h>
import geom;

export typedef struct sock sock_t;
export enum color { RED, GREEN = 5 };
struct sock { int fd; };
export sock_t * owned maybe sock_open(int port);
export void sock_close(drop sock_t * owned s);
export size_t name_len(const char * view s);
export int counter = 3, arr[2] = {1, 2};
export const double scale = 2.5;
export float half(float f) { return f * 0.5f; }
export unsigned long big(void) { return 4000000000ul; }
export inline int twice(int v) { return v * 2; }
export static inline long long mask(void) { return 0xffffffffffull + 'a' + sizeof("q\\n"); }
"""

HEADER_TEXT = """\
/* C header of yasc module net.socket (m.c), written by tcc -fmodule-header.
   Do not edit: it is written again when the module's interface changes. */
#ifndef YASC_MODULE_NET_SOCKET_H
#define YASC_MODULE_NET_SOCKET_H
#include <stddef.h>
#include <geom.h>

typedef struct sock sock_t;
enum color {
  RED, GREEN = 5
};
sock_t * sock_open(int port);
void sock_close(sock_t * s);
size_t name_len(const char * s);
extern int counter, arr[2];
extern const double scale;
float half(float f);
unsigned long big(void);
int twice(int v);
static inline long long mask(void) {
  return 1099511627775ull + 'a' + sizeof("q\\n");
}

#endif
"""


def _header_module(tmp_path, body=HEADER_MODULE, *flags):
    _ok(_cc(tmp_path, "geom", "module geom;\nexport typedef struct point { int x, y; } point_t;\n",
            f"-fmodule-header={tmp_path / 'inc'}"))
    return _module(tmp_path, body, f"-fmodule-header={tmp_path / 'inc'}", *flags)


def test_header_contents(tmp_path):
    proc = _header_module(tmp_path)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    assert (tmp_path / "inc" / "net" / "socket.h").read_text() == HEADER_TEXT
    # the interface still carries the yasc qualifiers and the inline body
    text = _load_ymi(tmp_path / "net" / "socket.ymi")["text"]
    assert "sock_t * owned maybe sock_open" in text
    assert "static inline int twice ( int v ) { return v * 2 ; }" in text


@pytest.mark.skipif(NM is None, reason="needs arm-none-eabi-nm")
def test_export_inline_is_defined_for_c(tmp_path):
    proc = _header_module(tmp_path)
    assert proc.returncode == 0, proc.stderr
    syms = _symbols_of(tmp_path / "m.o")
    assert syms["twice"] == "T"  # a C caller calls this one
    assert "mask" not in syms or syms["mask"].islower()  # static inline: no symbol


def _symbols_of(obj):
    out = subprocess.run([NM, str(obj)], capture_output=True, text=True, check=True).stdout
    return {line.split()[-1]: line.split()[-2] for line in out.splitlines()}


@pytest.mark.skipif(shutil.which("arm-none-eabi-gcc") is None, reason="needs arm-none-eabi-gcc")
def test_header_is_c99_for_gcc(tmp_path):
    proc = _header_module(tmp_path)
    assert proc.returncode == 0, proc.stderr
    (tmp_path / "user.c").write_text(
        "#include <net/socket.h>\n"
        "int use(void) { point_t p = {1, 2}; sock_t *s = sock_open(80); sock_close(s);\n"
        "  return twice(3) + p.x + (int)mask() + RED + counter + arr[1] + (int)half(2.0f) + (int)scale\n"
        "    + (int)name_len(\"x\") + (int)big(); }\n"
    )
    r = subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-std=c99", "-Wall", "-Wextra",
                        "-pedantic", "-Werror", f"-I{tmp_path / 'inc'}", "-c", str(tmp_path / "user.c"),
                        "-o", str(tmp_path / "user.o")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


QUAL_ARGS_MODULE = """\
module q;
int close(int drop owned(close) maybe fd);
struct grid { int n; };
export struct tok borrows(text) { const char * borrows(text) pos; };
static struct grid the_grid;
export struct grid * borrows(static) grid_get(void) { return &the_grid; }
export const char * borrows(s) skip(const char * view s) { return s + 1; }
export char * owned takes(p) keep(char * owned p) { return p; }
export int owned(close) maybe open_fd(int f);
export struct tok borrows(src) tok_init(const char * view src);
export int takes(int v);
"""

QUAL_ARGS_TEXT = """\
struct grid * grid_get(void);
const char * skip(const char * s);
char * keep(char * p);
int open_fd(int f);
struct tok tok_init(const char * src);
int takes(int v);
"""


def test_header_leaves_out_qualifier_arguments(tmp_path):
    """borrows(...), takes(...) and owned(dealloc) are yasc: none of their
    tokens reach the C header (a C caller failed on `borrows(static)`), while
    a function called takes keeps its name"""
    proc = _module(tmp_path, QUAL_ARGS_MODULE, f"-fmodule-header={tmp_path / 'inc'}")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    text = (tmp_path / "inc" / "q.h").read_text()
    assert QUAL_ARGS_TEXT in text, text
    assert "borrows" not in text and "(close)" not in text and "(p)" not in text, text


IN_MODULE = """\
module ro;
export int first(int n, const char * view * view argv);
export int count(const char * view a[], char * view b[], int c[4], inout char * view e[], int g[owned]);
export int bytes(const int * view maybe p, const char * view s, char * view const * view v);
export int rows(const char (* view m)[4], const char (* view c)[4]);
"""

IN_TEXT = """\
int first(int n, const char * const * argv);
int count(const char * const a[], char * const b[], int const c[4], char * e[], int g[]);
int bytes(const int * p, const char * s, char * const * v);
int rows(const char(* m) [4], const char(* c) [4]);
"""


def test_header_spells_in_as_const(tmp_path):
    """an `in` parameter's target is const in the C header: before the '*'
    of its outermost pointer, and for an array parameter before its name --
    the elements (`char * view b[]` is `char * const b[]`, not `char const *
    b[]`); inout and an owned array say nothing"""
    proc = _module(tmp_path, IN_MODULE, f"-fmodule-header={tmp_path / 'inc'}")
    assert proc.returncode == 0 and not proc.stderr, proc.stderr
    text = (tmp_path / "inc" / "ro.h").read_text()
    assert IN_TEXT in text, text
    if shutil.which("arm-none-eabi-gcc"):
        (tmp_path / "user.c").write_text("#include <ro.h>\n")
        r = subprocess.run(["arm-none-eabi-gcc", "-std=c99", "-Wall", "-Wextra", "-pedantic", "-Werror",
                            f"-I{tmp_path / 'inc'}", "-fsyntax-only", str(tmp_path / "user.c")],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr


def test_header_unchanged_keeps_its_time(tmp_path):
    import os

    assert _header_module(tmp_path).returncode == 0
    h = tmp_path / "inc" / "net" / "socket.h"
    os.utime(h, ns=(1_000_000_000, 1_000_000_000))
    assert _module(tmp_path, HEADER_MODULE.replace("f * 0.5f", "f / 2.0f"),
                   f"-fmodule-header={tmp_path / 'inc'}").returncode == 0
    assert h.stat().st_mtime_ns == 1_000_000_000


def test_header_only_for_modules(tmp_path):
    assert _module(tmp_path, "int f(void) { return 1; }\n", f"-fmodule-header={tmp_path / 'inc'}").returncode == 0
    assert not (tmp_path / "inc").exists()


def test_header_rejects_defer_in_static_inline(tmp_path):
    body = "module a;\nextern void note(int);\nexport static inline void f(int v) { defer note(v); note(0); }\n"
    assert _error(_yasc(tmp_path, body, f"-fmodule-header={tmp_path / 'inc'}")) == (
        "-fmodule-header: an exported static inline function uses 'defer', which C does not have"
    )
    # without a C header it is fine: importers are yasc
    proc = _yasc(tmp_path, body)
    assert proc.returncode == 0 and not proc.stderr, proc.stderr


# ---------------------------------------------------------------------------
# examples/yasc_modules (the run-time test is test_qemu.py::test_yasc_modules_example)

EXAMPLE = CURRENT_DIR.parent.parent / "examples" / "yasc_modules"


@pytest.mark.skipif(shutil.which("make") is None or shutil.which("awk") is None, reason="needs make and awk")
def test_example_makefile(tmp_path):
    """The example's own Makefile: `make modules` builds both modules in order
    under -j, with their interfaces and C headers."""
    work = tmp_path / "ex"
    shutil.copytree(EXAMPLE, work)
    proc = subprocess.run(["make", "-j8", "-C", str(work), "modules", f"TCC={TCC}"], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    compiles = [line.split(" -o ")[1].split()[0] for line in proc.stdout.splitlines() if " -c " in line]
    assert compiles == ["vec.o", "shapes.o"]
    for f in ("lib/vec.ymi", "shapes.ymi", "include/lib/vec.h", "include/shapes.h"):
        assert (work / f).is_file(), f
    assert _load_ymi(work / "shapes.ymi")["imports"] == ["lib.vec"]
    assert "#include <lib/vec.h>" in (work / "include" / "shapes.h").read_text()


# ---------------------------------------------------------------------------
# Step 6: regular C behind a module is unchecked too


def test_import_brings_unchecked_c(tmp_path):
    (tmp_path / "legacy.h").write_text("void *c_alloc(unsigned n);\nchar *c_name(void);\n")
    _ok(_cc(tmp_path, "a", 'module a;\n#include "legacy.h"\nexport char * borrows(static) a_view(void);\n'
            "export void a_release(void * owned maybe p);\n"))
    proc = _cc(tmp_path, "m", "import a;\nvoid f(void) {\n  char * owned p = c_alloc(1);\n"
               "  char * owned q = a_view();\n  a_release(move(p)); a_release(move(q)); }\n")
    assert proc.returncode == 0, proc.stderr
    # c_alloc comes from the module's C header: unchecked; a_view is the
    # module's own (yasc) view: checked
    warnings = [l for l in proc.stderr.splitlines() if "warning: " in l]
    assert len(warnings) == 1 and "m.c:5:" in warnings[0] and "owns nothing" in warnings[0], proc.stderr
