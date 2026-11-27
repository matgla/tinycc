"""yasc objects owned as a whole: `regex_t owned(regfree) re`
(docs/static_analysis_todo.md).

The object (a struct, not a pointer) holds what a function gave it and a
deallocator takes back: `int regcomp(out regex_t owned(regfree) *view
preg, ...)` writes it (a status result's test deciding), `void
regfree(drop regex_t owned(regfree) *preg)` releases it, and a read
through a pointer that cannot write (`const regex_t *view`) uses it.  The
slot check follows it like an owned item: leaks, use after release,
release twice, the wrong deallocator.  Its address written into by such a
function while the local is not declared owned(...) is reported too.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PRELUDE = """\
#pragma yasc
typedef struct { int re_nsub; void *?impl; } regex_t;
int regcomp(out regex_t owned(regfree) *preg, const char *pat, int fl);
int regexec(const regex_t *preg, const char *s);
void regfree(drop regex_t owned(regfree) *preg);
void other_free(drop regex_t owned(other_free) *p);
"""
PRELUDE_LINES = PRELUDE.count("\n")
START = "void f(void)\n{\n  regex_t owned(regfree) re = undefined;\n  if (regcomp(&re, \"a\", 0))\n    return;\n"


def _diags(tmp_path, body, *flags):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + body)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wlifetime", "-c", *flags, str(src), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 0, proc.stderr
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] == "warning":
            out.append((int(parts[0].rsplit(":", 1)[1]) - PRELUDE_LINES, parts[2]))
    return out


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2", "-Os"])
def test_clean(tmp_path, opt):
    assert _diags(tmp_path, START + "  regexec(&re, \"x\");\n  regfree(&re);\n}\n", opt) == []


BAD = {
    "leak": (START + "  regexec(&re, \"x\");\n}\n", 7, "'re' leaks: it still owns what it holds at the end of its scope"),
    "used_after_release": (START + "  regfree(&re);\n  regexec(&re, \"x\");\n}\n", 7, "'re' is used after it was released"),
    "released_twice": (START + "  regfree(&re);\n  regfree(&re);\n}\n", 7, "'re' is released twice"),
    "wrong_deallocator": (START + "  other_free(&re);\n}\n", 6, "'re' is released by regfree, not other_free"),
    "not_declared_owned": (
        "void f(void)\n{\n  regex_t re = undefined;\n  regcomp(&re, \"a\", 0);\n}\n",
        4,
        "what 'regcomp' writes is owned, but the local it is written into is not declared owned(...): "
        "it is never released",
    ),
}


@pytest.mark.parametrize("name", sorted(BAD))
@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_reported(tmp_path, name, opt):
    body, line, msg = BAD[name]
    assert (line, msg) in _diags(tmp_path, body, opt)


def test_needs_a_deallocator(tmp_path):
    src = tmp_path / "t.c"
    src.write_text(PRELUDE + "void f(void) { regex_t owned re = undefined; (void)re; }\n")
    proc = subprocess.run([str(TCC), "-ccompat", "-c", str(src), "-o", str(tmp_path / "t.o")], capture_output=True, text=True)
    assert proc.returncode != 0 and "an owned object names its deallocator: owned(release)" in proc.stderr
