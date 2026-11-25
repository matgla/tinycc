"""-Wignored-attributes (the safe default; not in -Wall, where -Wall -Werror
builds of code using an unchecked attribute would stop): an attribute that
promises a check tcc does not make.

counted_by reads as checked to whoever reads the code; tcc ignores it, and says so -- once per attribute in a unit, in either syntax
(__attribute__((x)), __x__, [[gnu::x]]), never for a system header.
fallthrough and maybe_unused only silence warnings tcc does not give.  Other
unknown attributes stay under -Wunsupported.  The checked ones are not
reported, in either spelling: malloc, nonnull and returns_nonnull (-Wnonnull,
the yasc ownership of a malloc result), alloc_size (__builtin_object_size and
the object-size checks), access and null_terminated_string_arg (-ffortify),
format (-Wformat), warn_unused_result
and nodiscard (-Wunused-result), deprecated (-Wdeprecated-declarations).
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _run(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True
    )


def _named(proc):
    out = []
    for line in proc.stderr.splitlines():
        if "attribute is not checked by tcc" in line:
            out.append(line.split("'")[1])
    return out


CASES = {
    "counted_by": "struct cb { int n; char d[] __attribute__((counted_by(n))); };",
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_reported(tmp_path, name):
    assert _named(_run(tmp_path, CASES[name] + "\n", "-Wignored-attributes")) == [name]


APPLIED = {
    "nonnull": ("void cp(char *d) __attribute__((nonnull(1)));", "[[gnu::nonnull(1)]] void cp(char *d);"),
    "returns_nonnull": ("char *nn(void) __attribute__((returns_nonnull));", "[[gnu::returns_nonnull]] char *nn(void);"),
    "malloc": ("void *mk(unsigned n) __attribute__((malloc));", "[[gnu::malloc]] void *mk(unsigned n);"),
    "alloc_size": ("void *mk2(unsigned n) __attribute__((alloc_size(1)));",
                   "[[gnu::alloc_size(1, 2)]] void *mk3(unsigned n, unsigned m);"),
    "format": ("int lg(const char *f, ...) __attribute__((format(printf, 1, 2)));",
               "[[gnu::format(printf, 1, 2)]] int lg(const char *f, ...);"),
    "warn_unused_result": ("int r(void) __attribute__((warn_unused_result));",
                           "[[gnu::warn_unused_result]] int r(void);"),
    "nodiscard": ("[[nodiscard]] int nd(void);", "[[nodiscard(\"why\")]] int nd(void);"),
    "deprecated": ("int old(void) __attribute__((deprecated));", "[[deprecated(\"use new\")]] void old(void);"),
    "access": ("void w(char *d) __attribute__((access(write_only, 1)));", "[[gnu::access(read_write, 1, 2)]] void w(char *d, int n);"),
    "null_terminated_string_arg": ("void s(const char *p) __attribute__((null_terminated_string_arg(1)));",
                                   "[[gnu::null_terminated_string_arg(1)]] void s(const char *p);"),
}


@pytest.mark.parametrize("name", sorted(APPLIED))
@pytest.mark.parametrize("spelling", [0, 1])
def test_checked_ones_are_not_reported(tmp_path, name, spelling):
    proc = _run(tmp_path, APPLIED[name][spelling] + "\n", "-Wignored-attributes")
    assert _named(proc) == [] and not proc.stderr, proc.stderr


def test_gnu_spelling_of_nonnull_applies(tmp_path):
    body = "[[gnu::nonnull]] void cp(char *d, char *s);\nvoid t(char *p) { cp(p, 0); }\n"
    proc = _run(tmp_path, body, "-Wall")
    assert "null passed as argument 2, which is declared nonnull" in proc.stderr, proc.stderr


def test_gnu_spelling_of_malloc_applies_in_yasc(tmp_path):
    (tmp_path / "attr.h").write_text("[[gnu::malloc]] void *my_alloc(unsigned n);\n")
    src = tmp_path / "t.c"
    src.write_text('#include "attr.h"\nvoid f(void) { my_alloc(8); }\n')
    proc = subprocess.run([str(TCC), "-std=yasc", "-c", str(src), "-o", str(tmp_path / "t.o")],
                          capture_output=True, text=True)
    # an owned result, as with __attribute__((malloc))
    assert "an owned result is discarded" in proc.stderr, proc.stderr


def test_once_per_unit_in_any_spelling(tmp_path):
    body = (
        "struct a { int n; char d[] __attribute__((counted_by(n))); };\n"
        "struct b { int n; char d[] __attribute__((__counted_by__(n))); };\n"
        "struct c { int n; char d[] [[gnu::counted_by(n)]]; };\n"
    )
    proc = _run(tmp_path, body, "-Wignored-attributes")
    assert _named(proc) == ["counted_by"] and "t.c:1:" in proc.stderr, proc.stderr


def test_silent_ones(tmp_path):
    body = "[[maybe_unused]] static int mu;\nint sw(int a) { switch (a) { case 1: a++; [[fallthrough]]; default: return a; } }\n"
    proc = _run(tmp_path, body, "-Wignored-attributes")
    assert not proc.stderr, proc.stderr


def test_not_in_a_system_header(tmp_path):
    inc = tmp_path / "sys"
    inc.mkdir()
    (inc / "s.h").write_text(CASES["counted_by"].replace("cb ", "cb2 ") + "\n" + '#include "near.h"\n')
    (inc / "near.h").write_text(CASES["counted_by"].replace("cb ", "cb3 ") + "\n")
    proc = _run(tmp_path, '#include <s.h>\n' + CASES["counted_by"] + "\n", "-Wignored-attributes", "-isystem", str(inc))
    assert _named(proc) == ["counted_by"] and "t.c:2:" in proc.stderr, proc.stderr


def test_user_header_is_reported(tmp_path):
    (tmp_path / "u.h").write_text(CASES["counted_by"] + "\n")
    proc = _run(tmp_path, '#include "u.h"\n', "-Wignored-attributes")
    assert _named(proc) == ["counted_by"], proc.stderr


def test_switches(tmp_path):
    body = CASES["counted_by"] + "\n"
    assert _named(_run(tmp_path, body)) == []
    assert _named(_run(tmp_path, body, "-Wall")) == []
    assert _named(_run(tmp_path, body, "-Wignored-attributes", "-Wno-ignored-attributes")) == []
    assert _named(_run(tmp_path, body, "-Wignored-attributes")) == ["counted_by"]


def test_safe_default(tmp_path):
    src = tmp_path / "t.c"
    src.write_text("struct cb { int n; char d[] __attribute__((counted_by(n))); };\n")
    proc = subprocess.run([str(TCC), "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode == 0 and _named(proc) == ["counted_by"], proc.stderr


def test_other_unknown_attributes(tmp_path):
    body = "int f(void) __attribute__((hot));\n"
    assert not _run(tmp_path, body, "-Wignored-attributes").stderr
    assert "'hot' attribute ignored" in _run(tmp_path, body, "-Wunsupported").stderr
