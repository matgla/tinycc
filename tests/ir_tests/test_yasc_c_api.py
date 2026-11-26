"""yasc `c_api(T)`: the type C callers see in place of a yasc one
(docs/yasc_c_api.md).

    const char * borrows(static) maybe c_api(char *) dlerror(void);

The body and yasc callers keep the yasc type; a regular-C declaration of the
same function is compared with the C type (either order), and the function
converts to a pointer of the C type.  T may differ from the yasc type in
const and volatile only: a pointer turned into an int, int into long or char
into int is an error, as is a C type that promises C callers const where the
yasc declaration may write.  `tcc -yasc-c-header` writes T in the C header
with __yl_c_api(n, yasc type), and -Wyasc-c-api reports C code that writes
through what the function handed it read-only.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

Y = "#pragma yasc\n"
DL = "const char * borrows(static) maybe c_api(char *) dlerror(void);\n"


def _cc(tmp_path, src, *flags, name="t.c"):
    (tmp_path / name).write_text(src)
    return subprocess.run(
        [str(TCC), "-Wno-missing-prototypes", "-Wno-unused-parameter", *flags, "-c", str(tmp_path / name), "-o",
         str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )


ACCEPTED = {
    "c_redecl_after": Y + DL + "#pragma tcc c_begin\nchar *dlerror(void);\n#pragma tcc c_end\n"
    "int u(void) { const char * maybe e = dlerror(); return e ? *e : 0; }\n",
    "c_redecl_before": Y + "#pragma tcc c_begin\nchar *dlerror(void);\n#pragma tcc c_end\n" + DL +
    "int u(void) { const char * maybe e = dlerror(); return e ? *e : 0; }\n",
    # the definition need not say it again; a C declaration after it still fits
    "definition": Y + DL + "const char * borrows(static) maybe dlerror(void) { return \"\"; }\n"
    "#pragma tcc c_begin\nchar *dlerror(void);\n#pragma tcc c_end\n",
    "function_pointer": Y + DL + "#pragma tcc c_begin\nstruct ops { char *(*err)(void); };\n"
    "struct ops o = { dlerror };\n#pragma tcc c_end\n",
    "param_array": Y + "int execv(const char *path, const char * view const c_api(char * const) argv[]);\n"
    "#pragma tcc c_begin\nint execv(const char *path, char * const argv[]);\n#pragma tcc c_end\n",
    "out_inner_level": Y + "long sl(const char *s, out const char * borrows(s) c_api(char *) * maybe endptr, int b);\n"
    "#pragma tcc c_begin\nlong sl(const char *s, char **endptr, int b);\n#pragma tcc c_end\n",
    "member": Y + "struct pw { const char * view c_api(char *) name; int uid; };\n",
    # C may say more const of a result than yasc: it only reads it
    "result_more_const_in_c": Y + "char * borrows(static) c_api(const char *) k(void);\n",
    "same_c_api_twice": Y + DL + DL,
}


@pytest.mark.parametrize("name", sorted(ACCEPTED))
def test_accepted(tmp_path, name):
    proc = _cc(tmp_path, ACCEPTED[name])
    assert proc.returncode == 0, proc.stderr


REJECTED = {
    "pointer_to_int": (Y + "void * borrows(static) c_api(int) f(void);\n",
                       "c_api(int) cannot stand for 'void * borrows(static)': a pointer and something that is not one"),
    "int_to_long": (Y + "int * borrows(static) c_api(long *) g(void);\n",
                    "different types (c_api(...) may differ in const and volatile only)"),
    "char_to_int": (Y + "const char * borrows(static) c_api(int *) h(void);\n",
                    "c_api(int *) cannot stand for 'const char * borrows(static)'"),
    "c_const_yasc_writes": (Y + "int k(inout char * c_api(const char *) p);\n",
                            "it is const for C callers, and the yasc declaration may write it"),
    "member_c_const": (Y + "struct pw { char * view c_api(const char *) name; };\n",
                       "it is const for C callers, and the yasc declaration may write it"),
    "twice": (Y + "char * view c_api(char *) c_api(char *) m(void);\n", "c_api(...) twice in one declarator"),
    "redecl_other_c_api": (Y + "const char * borrows(static) c_api(char *) d(void);\n"
                           "const char * borrows(static) c_api(const char *) d(void);\n",
                           "'d' is declared again with another c_api(...)"),
    "c_redecl_not_the_c_type": (Y + DL + "#pragma tcc c_begin\nint *dlerror(void);\n#pragma tcc c_end\n",
                                "incompatible types for redefinition of 'dlerror'"),
    "function_pointer_other_type": (Y + DL + "#pragma tcc c_begin\nstruct ops { int *(*err)(void); };\n"
                                    "struct ops o = { dlerror };\n#pragma tcc c_end\n",
                                    "assignment from incompatible pointer type"),
    "nested_declarator": (Y + "char * borrows(static) c_api(char *) (*fp)(void);\n",
                          "c_api(...) in a declarator in parentheses is not supported"),
}


@pytest.mark.parametrize("name", sorted(REJECTED))
def test_rejected(tmp_path, name):
    src, msg = REJECTED[name]
    proc = _cc(tmp_path, src)
    assert proc.returncode != 0
    assert msg in proc.stderr, proc.stderr


YASC_HEADER = """#pragma yasc
#include <sys/_attrs.h>
/* null when no error happened since the last call */
const char * borrows(static) maybe c_api(char *) dlerror(void);
int execv(const char *path, const char * view const c_api(char * const) argv[]);
long sl(const char *s, out const char * borrows(s) c_api(char *) * maybe endptr, int base);
extern const char * view c_api(char *) maybe gname;
struct pw {
  const char * view c_api(char *) pw_name;
  int uid;
};
"""


def _header(tmp_path):
    (tmp_path / "yasc").mkdir(exist_ok=True)
    (tmp_path / "yasc" / "x.h").write_text(YASC_HEADER)
    proc = subprocess.run([str(TCC), "-yasc-c-header", str(tmp_path / "yasc" / "x.h"), "-o", str(tmp_path / "x.h")],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return (tmp_path / "x.h").read_text()


def test_c_header(tmp_path):
    out = _header(tmp_path)
    assert "char *dlerror(void) __yl_c_api(0, const char *);" in out
    assert "int execv(const char *path, char * const argv[]) __yl_c_api(2, const char * const []);" in out
    assert "long sl(const char *s, char **endptr, int base) __yl_c_api(2, const char * *);" in out
    assert "extern char *gname;" in out
    assert "char *pw_name __yl_view;" in out
    assert "c_api" not in out.replace("__yl_c_api", "")


def test_c_header_declarator_list(tmp_path):
    (tmp_path / "yasc").mkdir()
    (tmp_path / "yasc" / "x.h").write_text("#pragma yasc\nextern char * view a, * view c_api(char *) b;\n")
    proc = subprocess.run([str(TCC), "-yasc-c-header", str(tmp_path / "yasc" / "x.h")], capture_output=True,
                          text=True)
    assert proc.returncode != 0
    assert "c_api(...) in a declarator list: declare it on its own" in proc.stderr


# regular C against what the yasc declarations say (the attribute as the written header has it)
C_DECLS = """#define A(n, ...) __attribute__((yasc_c_api(n, __VA_ARGS__)))
char *dlerror(void) A(0, const char *);
long sl(const char *s, char **endptr, int base) A(2, const char **);
char *strcpy(char *d, const char *s);
unsigned long strlen(const char *s);
int printf(const char *, ...);
void wr(char *p);
"""

BAD = {
    "direct": "void f(void) { dlerror()[0] = 'x'; }\n",
    "local_index": "void f(int i) { char *e = dlerror(); e[i] = 0; }\n",
    "local_deref": "void f(void) { char *e = dlerror(); *e = 0; }\n",
    "arithmetic": "void f(void) { char *e = dlerror(); char *p = e + 2; p[0] = 0; }\n",
    "out_argument": "void f(const char *s) { char *end; sl(s, &end, 10); *end = 0; }\n",
    "to_writing_parameter": "void f(void) { wr(dlerror()); }\n",
    "to_builtin_strcpy": "void f(void) { char *e = dlerror(); strcpy(e, \"x\"); }\n",
}

MESSAGES = {
    "out_argument": "writing through what 'sl' stored through its argument 2: its yasc declaration says it is read-only",
    "to_writing_parameter": "the result of 'dlerror' goes to parameter 1 of 'wr', which may write through it",
    "to_builtin_strcpy": "the result of 'dlerror' goes to parameter 1 of 'strcpy', which may write through it",
}


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
@pytest.mark.parametrize("name", sorted(BAD))
def test_c_caller_writes(tmp_path, name, opt):
    proc = _cc(tmp_path, C_DECLS + BAD[name], "-ccompat", opt)
    assert proc.returncode == 0, proc.stderr
    msg = MESSAGES.get(name, "writing through the result of 'dlerror': its yasc declaration says it is read-only")
    assert msg in proc.stderr, proc.stderr
    assert "-Wyasc-c-api" in proc.stderr or "warning:" in proc.stderr


GOOD = {
    "reads": "int f(void) { char *e = dlerror(); printf(\"%s\\n\", e); return (int)strlen(e) + e[0]; }\n",
    "reassigned": "void f(char *buf) { char *e = dlerror(); e = buf; e[0] = 0; }\n",
    "own_buffer": "void f(char *buf) { char *e = buf; e[0] = 0; wr(e); }\n",
}


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
@pytest.mark.parametrize("name", sorted(GOOD))
def test_c_caller_quiet(tmp_path, name, opt):
    proc = _cc(tmp_path, C_DECLS + GOOD[name], "-ccompat", opt)
    assert proc.returncode == 0, proc.stderr
    assert "yasc declaration" not in proc.stderr, proc.stderr


def test_c_caller_warning_off(tmp_path):
    proc = _cc(tmp_path, C_DECLS + BAD["direct"], "-ccompat", "-Wno-yasc-c-api")
    assert proc.returncode == 0, proc.stderr
    assert "yasc declaration" not in proc.stderr, proc.stderr


def test_written_header_and_c_caller(tmp_path):
    """The header -yasc-c-header writes, read by a C unit: the warning comes from it."""
    _header(tmp_path)
    (tmp_path / "sys").mkdir()
    (tmp_path / "sys" / "_attrs.h").write_text(
        "#if defined(__TINYC__) && __has_attribute(__yasc_c_api__)\n"
        "#define __yl_c_api(n, ...) __attribute__((__yasc_c_api__(n, __VA_ARGS__)))\n"
        "#else\n#define __yl_c_api(n, ...)\n#endif\n#define __yl_view\n")
    src = '#include "x.h"\nvoid f(void) { char *e = dlerror(); if (e) e[0] = 0; }\n'
    proc = _cc(tmp_path, src, "-ccompat", "-I", str(tmp_path))
    assert proc.returncode == 0, proc.stderr
    assert "writing through the result of 'dlerror'" in proc.stderr, proc.stderr
    # a yasc unit reads the yasc header itself: const, and no warning
    ysrc = '#pragma yasc\n#include "x.h"\nint g(void) { const char * maybe e = dlerror(); return e ? *e : 0; }\n'
    proc = _cc(tmp_path, ysrc, "-I", str(tmp_path), name="y.c")
    assert proc.returncode == 0, proc.stderr
