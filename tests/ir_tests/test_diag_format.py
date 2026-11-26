"""-fdiagnostics-format=json|sarif, fix-it hints, -fdiagnostics-parseable-fixits,
-fmax-errors (driver/diag_out.c).

json: gcc's array of {kind, message, option, locations, children, fixits};
sarif: SARIF 2.1.0, one run, ruleId "-Wname" (or "error"/"warning"), notes as
relatedLocations, fix-its as fixes.  One document per process on stderr,
closed at exit -- also after an error that stops the compile and across
several -c units.  Fix-its: deprecated calls with a known replacement (a note,
a fix-it for a plain rename), yasc "write move(p)", a plain pointer parameter.
"""

import json
import subprocess
from pathlib import Path

import pytest

from cross_includes import INCLUDE_FLAGS

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

WARN_SRC = """\
int f(void) { g(); return; }
int h(void) {
  int x = 1;
  if (x = 2) return 1; return 0;
}
"""

DEP_SRC = """\
int sprintf(char *, const char *, ...) __attribute__((deprecated));
char *index(const char *, int) __attribute__((deprecated("old BSD")));
char buf[16];
char *f(const char *s, char *d)
{
  sprintf(d, "%d", 1);
  return index(s, 'a');
}
"""

MOVE_SRC = """\
#include <stdlib.h>
void take(char * owned p);
void g(void)
{
  char * owned p = malloc(3);
  take(p);
}
"""


def _write(tmp_path, name, src):
    p = tmp_path / name
    p.write_text(src)
    return p


def _tcc(tmp_path, *args, cwd=None):
    return subprocess.run([str(TCC), "-Wno-missing-prototypes", *INCLUDE_FLAGS, *map(str, args)], capture_output=True, text=True, cwd=cwd or tmp_path)


def _json(tmp_path, src, *flags, name="t.c"):
    _write(tmp_path, name, src)
    proc = _tcc(tmp_path, "-c", *flags, "-fdiagnostics-format=json", name, "-o", "t.o")
    return proc, json.loads(proc.stderr)


def _sarif(tmp_path, src, *flags, name="t.c"):
    _write(tmp_path, name, src)
    proc = _tcc(tmp_path, "-c", *flags, "-fdiagnostics-format=sarif", name, "-o", "t.o")
    doc = json.loads(proc.stderr)
    assert doc["version"] == "2.1.0"
    assert len(doc["runs"]) == 1
    assert doc["runs"][0]["tool"]["driver"]["name"] == "tcc"
    return proc, doc["runs"][0]["results"]


def test_json_warnings(tmp_path):
    proc, diags = _json(tmp_path, WARN_SRC, "-ccompat", "-Wall")
    assert proc.returncode == 0
    got = [(d["kind"], d.get("option"), d["locations"][0]["caret"]["line"]) for d in diags]
    assert ("warning", "-Wimplicit-function-declaration", 1) in got
    assert ("warning", "-Wreturn-type", 1) in got
    assert ("warning", "-Wparentheses", 4) in got
    for d in diags:
        assert d["locations"][0]["caret"]["file"] == "t.c"
        assert d["children"] == [] and d["fixits"] == []


def test_text_unchanged_without_format(tmp_path):
    _write(tmp_path, "t.c", WARN_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-c", "t.c", "-o", "t.o")
    assert "t.c:1: warning: implicit declaration of function 'g'" in proc.stderr.splitlines()
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-fdiagnostics-format=json", "-fdiagnostics-format=text", "-c", "t.c")
    assert proc.stderr.startswith("t.c:1: warning:")


def test_sarif_warnings(tmp_path):
    proc, res = _sarif(tmp_path, WARN_SRC, "-ccompat", "-Wall")
    assert proc.returncode == 0
    rules = {r["ruleId"] for r in res}
    assert {"-Wimplicit-function-declaration", "-Wreturn-type", "-Wparentheses"} <= rules
    for r in res:
        assert r["level"] == "warning"
        loc = r["locations"][0]["physicalLocation"]
        assert loc["artifactLocation"]["uri"] == "t.c"
        assert loc["region"]["startLine"] >= 1
        assert r["message"]["text"]


def test_no_diagnostics_is_an_empty_document(tmp_path):
    proc, diags = _json(tmp_path, "int x;\n")
    assert proc.returncode == 0 and diags == []
    proc, res = _sarif(tmp_path, "int x;\n")
    assert res == []


def test_werror_keeps_rule(tmp_path):
    proc, res = _sarif(tmp_path, WARN_SRC, "-ccompat", "-Wall", "-Werror=parentheses")
    assert proc.returncode == 1
    par = [r for r in res if r["ruleId"] == "-Wparentheses"]
    assert par and par[0]["level"] == "error"


def test_wno_and_pragma(tmp_path):
    _, diags = _json(tmp_path, WARN_SRC, "-ccompat", "-Wall", "-Wno-parentheses")
    assert not [d for d in diags if d.get("option") == "-Wparentheses"]
    src = '#pragma GCC diagnostic ignored "-Wparentheses"\n' + WARN_SRC
    _, diags = _json(tmp_path, src, "-ccompat", "-Wall")
    assert not [d for d in diags if d.get("option") == "-Wparentheses"]


def test_error_closes_document(tmp_path):
    src = "int f(void) { g(); return 0; }\nint x = ;\nint y = ;\n"
    proc, diags = _json(tmp_path, src, "-ccompat", "-Wall")
    assert proc.returncode == 1
    assert diags[-1]["kind"] == "error"
    assert "option" not in diags[-1]
    proc, res = _sarif(tmp_path, src, "-ccompat", "-Wall")
    assert res[-1]["ruleId"] == "error" and res[-1]["level"] == "error"


def test_one_document_for_several_units(tmp_path):
    _write(tmp_path, "a.c", "int f(void) { g(); return 0; }\n")
    _write(tmp_path, "b.c", "int k(void) { h(); return 0; }\n")
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-fdiagnostics-format=json", "-c", "a.c", "b.c")
    diags = json.loads(proc.stderr)
    assert [d["locations"][0]["caret"]["file"] for d in diags] == ["a.c", "b.c"]
    proc = _tcc(tmp_path, "-ccompat", "-Wall", "-fdiagnostics-format=sarif", "-c", "a.c", "b.c")
    doc = json.loads(proc.stderr)
    assert len(doc["runs"]) == 1 and len(doc["runs"][0]["results"]) == 2


def test_notes_are_children(tmp_path):
    src = "int v;\nvoid f(void) { int v = 1; (void)v; }\n"
    proc, diags = _json(tmp_path, src, "-ccompat", "-Wshadow")
    sh = [d for d in diags if d.get("option") == "-Wshadow"]
    assert sh and sh[0]["children"]
    assert sh[0]["children"][0]["kind"] == "note"
    assert sh[0]["children"][0]["locations"][0]["caret"]["line"] == 1
    proc, res = _sarif(tmp_path, src, "-ccompat", "-Wshadow")
    rel = res[0]["relatedLocations"]
    assert rel[0]["physicalLocation"]["region"]["startLine"] == 1
    assert "shadowed" in rel[0]["message"]["text"]


def test_inline_reported_once(tmp_path):
    src = "static inline int inl(int x) { if (x = 2) return 1; return 0; }\nint a(void) { return inl(1) + inl(2); }\n"
    for opt in ("-O0", "-O2"):
        _, diags = _json(tmp_path, src, "-ccompat", "-Wall", opt)
        assert len([d for d in diags if d.get("option") == "-Wparentheses"]) == 1, opt


def test_objects_unchanged(tmp_path):
    _write(tmp_path, "t.c", WARN_SRC)
    for opt in ("-O0", "-O2"):
        _tcc(tmp_path, "-ccompat", "-Wall", opt, "-c", "t.c", "-o", "a.o")
        _tcc(tmp_path, "-ccompat", "-Wall", opt, "-fdiagnostics-format=sarif", "-c", "t.c", "-o", "b.o")
        assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes()


@pytest.mark.parametrize("opt", ["-O0", "-O2", "-fsyntax-only"])
def test_max_errors(tmp_path, opt):
    src = "#include <stdlib.h>\nvoid g(void)\n{\n  char * owned p = unsafe(malloc(3));\n  char * owned q = unsafe(malloc(3));\n}\n"
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, opt, "-Werror=lifetime", "-Wno-unused", "-c", "t.c", "-o", "t.o")
    assert proc.stderr.count("error:") == 2
    # -O1+: the diagnostics are held (driver/diag_out.c), counted in -O0's
    # order when written
    proc = _tcc(tmp_path, opt, "-Werror=lifetime", "-Wno-unused", "-fmax-errors=1", "-c", "t.c", "-o", "t.o")
    assert proc.returncode == 1
    assert proc.stderr.count("error:") == 1
    assert "compilation terminated due to -fmax-errors=1." in proc.stderr
    # the stop comes from inside the lifetime check: an ASan build reports what
    # the check had allocated if the compile ends there (tcc_ir_flow_checks)
    assert "Sanitizer" not in proc.stderr, proc.stderr
    proc, diags = _json(tmp_path, src, opt, "-Werror=lifetime", "-Wno-unused", "-fmax-errors=1")
    assert len(diags) == 1 and diags[0]["kind"] == "error"


# ---------------------------------------------------------------- fix-its


def _col(src, line, word):
    return src.splitlines()[line - 1].index(word) + 1


def test_deprecated_notes(tmp_path):
    _write(tmp_path, "t.c", DEP_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-c", "t.c", "-o", "t.o")
    lines = proc.stderr.splitlines()
    assert "t.c:6: warning: 'sprintf' is deprecated" in lines
    assert "t.c:6: note: use snprintf(buf, size, fmt, ...), which bounds the output" in lines
    assert "t.c:7: note: use 'strchr' instead" in lines
    assert not [l for l in lines if l.startswith("fix-it:")]
    # the note goes with its warning
    proc = _tcc(tmp_path, "-ccompat", "-Wno-deprecated-declarations", "-c", "t.c", "-o", "t.o")
    assert proc.stderr == ""


def test_parseable_fixits(tmp_path):
    _write(tmp_path, "t.c", DEP_SRC)
    proc = _tcc(tmp_path, "-ccompat", "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    c = _col(DEP_SRC, 7, "index")
    assert f'fix-it:"t.c":{{7:{c}-7:{c + 5}}}:"strchr"' in proc.stderr.splitlines()
    # sprintf into a buffer of unknown size: a note, no fix-it (test_fixits.py)
    assert proc.stderr.count("fix-it:") == 1


def test_fixits_json_sarif(tmp_path):
    _, diags = _json(tmp_path, DEP_SRC, "-ccompat")
    idx = [d for d in diags if "'index'" in d["message"]][0]
    c = _col(DEP_SRC, 7, "index")
    assert idx["children"][0]["message"] == "use 'strchr' instead"
    assert idx["fixits"] == [
        {
            "start": {"file": "t.c", "line": 7, "column": c},
            "next": {"file": "t.c", "line": 7, "column": c + 5},
            "string": "strchr",
        }
    ]
    _, res = _sarif(tmp_path, DEP_SRC, "-ccompat")
    idx = [r for r in res if "'index'" in r["message"]["text"]][0]
    fix = idx["fixes"][0]
    assert fix["description"]["text"] == "use 'strchr' instead"
    rep = fix["artifactChanges"][0]["replacements"][0]
    assert rep["deletedRegion"] == {"startLine": 7, "startColumn": c, "endColumn": c + 5}
    assert rep["insertedContent"]["text"] == "strchr"
    spr = [r for r in res if "'sprintf'" in r["message"]["text"]][0]
    assert "fixes" not in spr and spr["relatedLocations"]


def _apply(src, fixits):
    """apply gcc-style JSON fix-its (one line each), right to left"""
    lines = src.splitlines(keepends=True)
    for f in sorted(fixits, key=lambda f: (f["start"]["line"], f["start"]["column"]), reverse=True):
        ln = f["start"]["line"] - 1
        a, b = f["start"]["column"] - 1, f["next"]["column"] - 1
        lines[ln] = lines[ln][:a] + f["string"] + lines[ln][b:]
    return "".join(lines)


def test_move_fixit(tmp_path):
    _, diags = _json(tmp_path, MOVE_SRC)
    mv = [d for d in diags if d.get("option") == "-Wimplicit-move"][0]
    assert len(mv["fixits"]) == 2
    fixed = _apply(MOVE_SRC, mv["fixits"])
    assert "  take(move(p));\n" in fixed
    _write(tmp_path, "fixed.c", fixed)
    proc = _tcc(tmp_path, "-c", "fixed.c", "-o", "f.o")
    assert "implicit-move" not in proc.stderr and "move(p)" not in proc.stderr


def test_move_fixit_needs_one_occurrence(tmp_path):
    src = MOVE_SRC.replace("  char * owned p = malloc(3);\n  take(p);", "  char * owned p = malloc(3); take(p);")
    _write(tmp_path, "t.c", src)
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    assert "write move(p)" in proc.stderr
    assert "fix-it:" not in proc.stderr


def test_plain_pointer_param_fixit(tmp_path):
    # a parameter's own pointer is a view: nothing to say
    _write(tmp_path, "t.c", "int f(char *s) { return s[0]; }\n")
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "t.c", "-o", "t.o")
    assert proc.returncode == 0 and proc.stderr == "", proc.stderr
    # its inner level: said at once, no guess
    _write(tmp_path, "i.c", "int f(char **s) { return s[0][0]; }\n")
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "i.c", "-o", "i.o")
    assert proc.stderr.splitlines() == ["i.c:1: error: plain pointer in yasc: say owned or view"], proc.stderr
    # a local: decided by the body too (test_fixits.py); a cast: no guess
    _write(tmp_path, "l.c", "void f(void) { char *s = 0; (void)s; }\n")
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "l.c", "-o", "l.o")
    assert 'fix-it:"l.c":{1:22-1:22}:" view maybe "' in proc.stderr, proc.stderr
    _write(tmp_path, "c.c", "int f(void) { return (char *)0 == 0; }\n")
    proc = _tcc(tmp_path, "-fdiagnostics-parseable-fixits", "-c", "c.c", "-o", "c.o")
    assert "note:" not in proc.stderr and "fix-it:" not in proc.stderr
