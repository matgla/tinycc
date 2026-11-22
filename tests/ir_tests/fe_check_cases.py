"""Shared driver of the frontend constant / type check tests
(test_const_expr_checks.py, test_sign_compare_conversion.py,
test_array_bounds.py, test_sizeof_checks.py).

A case is one -W option: a source with hits (each `/* W: text */` comment
marks a line that must warn, with text in the message), a clean source, and
whether the option is in -Wall (and so in the safe default).  Every case is
run through the same switches: -W<name>, -Wno-<name>, -Werror=<name>,
-Wno-error=<name> under -Werror, -ccompat (off unless -Wall / -W<name>), the
safe default, #pragma GCC diagnostic ignored, the same warnings at
-O0/-O1/-O2/-Os -- once each, also in a static inline function called twice
-- and the objects identical with and without the option.
"""

import re
import subprocess
from pathlib import Path

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

LEVELS = ["-O0", "-O1", "-O2", "-Os"]


class Case:
    def __init__(self, option, wall, hit, clean, extra=()):
        self.option = option
        self.wall = wall
        self.hit = hit
        self.clean = clean
        self.extra = tuple(extra)  # flags every compile of the case takes

    def expected(self):
        """[(line, text)] from the /* W: text */ marks of the hit source"""
        out = []
        for n, line in enumerate(self.hit.splitlines(), 1):
            for m in re.finditer(r"/\* W: (.*?) \*/", line):
                out.append((n, m.group(1)))
        return out


def run(tmp_path, body, *flags, ccompat=True, obj="t.o", name="t.c"):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / obj)]
    return subprocess.run(cmd, capture_output=True, text=True)


def diags(proc, kind=None):
    """[(line, kind, message)] of t.c"""
    out = []
    for line in proc.stderr.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("warning", "error") and parts[0].rsplit(":", 1)[0].endswith("t.c"):
            try:
                n = int(parts[0].rsplit(":", 1)[1])
            except ValueError:
                continue
            if kind is None or parts[1] == kind:
                out.append((n, parts[1], parts[2]))
    return out


def matches(proc, expected, kind="warning"):
    """every expected (line, text) warned once, nothing else of KIND"""
    got = diags(proc, kind)
    left = list(got)
    for n, text in expected:
        hits = [d for d in left if d[0] == n and text in d[2]]
        assert len(hits) == 1, f"line {n} '{text}': {got}\n{proc.stderr}"
        left.remove(hits[0])
    assert not left, f"unexpected: {left}\n{proc.stderr}"


def check_hit(tmp_path, c):
    proc = run(tmp_path, c.hit, "-W" + c.option, *c.extra)
    assert proc.returncode == 0, proc.stderr
    matches(proc, c.expected())


def check_clean(tmp_path, c):
    proc = run(tmp_path, c.clean, "-Wall", "-W" + c.option, *c.extra)
    assert proc.returncode == 0 and not diags(proc), proc.stderr


def check_wno(tmp_path, c):
    proc = run(tmp_path, c.hit, "-Wall", "-W" + c.option, "-Wno-" + c.option, *c.extra)
    matches(proc, [])


def check_werror(tmp_path, c):
    proc = run(tmp_path, c.hit, "-W" + c.option, "-Werror=" + c.option, *c.extra)
    assert proc.returncode != 0
    assert diags(proc, "error"), proc.stderr
    proc = run(tmp_path, c.hit, "-W" + c.option, "-Werror", "-Wno-error=" + c.option, *c.extra)
    assert proc.returncode == 0, proc.stderr
    matches(proc, c.expected())


def check_ccompat(tmp_path, c):
    # -ccompat: off by default, -Wall turns the -Wall ones on
    matches(run(tmp_path, c.hit, *c.extra), [])
    matches(run(tmp_path, c.hit, "-Wall", *c.extra), c.expected() if c.wall else [])


def check_safe_default(tmp_path, c):
    proc = run(tmp_path, c.hit, *c.extra, ccompat=False)
    assert proc.returncode == 0, proc.stderr
    matches(proc, c.expected() if c.wall else [])


def check_pragma(tmp_path, c):
    body = '#pragma GCC diagnostic ignored "-W%s"\n%s' % (c.option, c.hit)
    matches(run(tmp_path, body, "-W" + c.option, *c.extra), [])
    body = '#pragma GCC diagnostic error "-W%s"\n%s' % (c.option, c.hit)
    proc = run(tmp_path, body, "-W" + c.option, *c.extra)
    assert proc.returncode != 0 and diags(proc, "error"), proc.stderr


def check_levels(tmp_path, c):
    """the same warnings at every -O, also through a static inline called twice"""
    exp = c.expected()
    for lvl in LEVELS:
        matches(run(tmp_path, c.hit, "-W" + c.option, lvl, *c.extra), exp)
    # the hit source's functions made static inline, each called twice
    body = re.sub(r"^(int|void|char|unsigned) (\w+)\((?=[^;\n]*\)\n\{)", r"static inline \1 \2(", c.hit, flags=re.M)
    calls = re.findall(r"^static inline (?:int|void|char|unsigned) (\w+)\(([^)]*)\)", body, flags=re.M)
    # (an argument that is not a constant: a call whose arguments all are may
    # be folded by evaluating the body at compile time, which reports nothing)
    lines = ["int use_%d(void)" % i for i in range(2)]
    tail = "int fe_arg;\n"
    for i in range(2):
        stmts = []
        for fn, params in calls:
            ps = [] if params.strip() in ("", "void") else params.split(",")
            args = ["0" if "*" in p or "[" in p else "fe_arg" for p in ps]
            stmts.append("  (void)%s(%s);" % (fn, ", ".join(args)))
        tail += "%s\n{\n%s\n  return 0;\n}\n" % (lines[i], "\n".join(stmts))
    for lvl in LEVELS:
        matches(run(tmp_path, body + tail, "-W" + c.option, lvl, *c.extra), exp)


def check_objects(tmp_path, c):
    """a check never changes code"""
    for lvl in LEVELS:
        p1 = run(tmp_path, c.hit, "-Wno-all", "-Wno-" + c.option, lvl, *c.extra, obj="a.o")
        p2 = run(tmp_path, c.hit, "-Wall", "-W" + c.option, lvl, *c.extra, obj="b.o")
        assert p1.returncode == 0 and p2.returncode == 0, p1.stderr + p2.stderr
        assert (tmp_path / "a.o").read_bytes() == (tmp_path / "b.o").read_bytes(), lvl


ALL_CHECKS = [
    check_hit,
    check_clean,
    check_wno,
    check_werror,
    check_ccompat,
    check_safe_default,
    check_pragma,
    check_levels,
    check_objects,
]
