"""Shared checks for the frontend statement warnings (test_implicit_fallthrough,
test_parentheses, test_switch_warning, test_shadow): each option's hit, clean
case, -Wno-/-Werror=/-Wno-error=, -ccompat, the safe default, #pragma GCC
diagnostic, the same report once at -O0/-O1/-O2/-Os (a static inline called
twice: the inliner replays its body at every call), objects byte-identical with
and without the option, and system headers quiet.
"""

import subprocess
from pathlib import Path

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
LEVELS = ("-O0", "-O1", "-O2", "-Os")


def run(tmp_path, body, *flags, ccompat=True, name="t.c", out="t.o"):
    src = tmp_path / name
    src.write_text(body)
    cmd = [str(TCC)] + (["-ccompat"] if ccompat else []) + ["-c", *flags, str(src), "-o", str(tmp_path / out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def diags(proc, text):
    """(line, kind) of each diagnostic whose message contains TEXT"""
    out = []
    for line in proc.stderr.splitlines():
        if text in line and (": warning: " in line or ": error: " in line):
            parts = line.split(":")
            out.append((int(parts[1]), "error" if ": error: " in line else "warning"))
    return out


def check_option(tmp_path, option, text, hit, clean, hit_lines, in_wall, in_safe):
    """The standard set for OPTION (-W<option>) reporting TEXT on HIT_LINES of
    HIT, nothing on CLEAN."""
    w = "-W" + option
    # a hit, and a clean case
    proc = run(tmp_path, hit, w)
    assert proc.returncode == 0, proc.stderr
    assert [l for l, _ in diags(proc, text)] == hit_lines, proc.stderr
    proc = run(tmp_path, clean, w)
    assert diags(proc, text) == [], proc.stderr
    # -Wno-<name> after it, and -w
    assert diags(run(tmp_path, hit, w, "-Wno-" + option), text) == []
    assert diags(run(tmp_path, hit, w, "-w"), text) == []
    # -Werror=<name> makes it an error; -Werror with -Wno-error=<name> does not
    proc = run(tmp_path, hit, w, "-Werror=" + option)
    assert proc.returncode != 0 and diags(proc, text)[0][1] == "error", proc.stderr
    proc = run(tmp_path, hit, w, "-Werror", "-Wno-error=" + option)
    assert proc.returncode == 0 and {k for _, k in diags(proc, text)} == {"warning"}, proc.stderr
    # -ccompat: off unless asked for; -Wall has it when gcc's -Wall does
    assert diags(run(tmp_path, hit), text) == []
    assert bool(diags(run(tmp_path, hit, "-Wall"), text)) == in_wall
    # the safe default (yasc dialect, -Wall and the WD_SAFE options)
    proc = run(tmp_path, hit, ccompat=False)
    assert bool(diags(proc, text)) == (in_wall or in_safe), proc.stderr
    # #pragma GCC diagnostic ignored around the code
    quiet = '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "%s"\n%s#pragma GCC diagnostic pop\n' % (w, hit)
    assert diags(run(tmp_path, quiet, w), text) == []


def check_levels(tmp_path, option, text, body, lines):
    """BODY reports TEXT on LINES, once each, at every -O level, and its object
    is byte-identical with and without -W<option> (and with -Wall)."""
    w = "-W" + option
    for level in LEVELS:
        proc = run(tmp_path, body, level, w, out="with.o")
        assert proc.returncode == 0, proc.stderr
        assert sorted(l for l, _ in diags(proc, text)) == lines, (level, proc.stderr)
        run(tmp_path, body, level, "-w", out="without.o")
        run(tmp_path, body, level, "-Wall", w, out="wall.o")
        without = (tmp_path / "without.o").read_bytes()
        assert (tmp_path / "with.o").read_bytes() == without, level
        assert (tmp_path / "wall.o").read_bytes() == without, level


def check_system_header(tmp_path, option, text, header_body, use):
    """Quiet in a system header (-isystem), static inline functions included,
    at every level; the same header found by -I reports."""
    inc = tmp_path / "inc"
    inc.mkdir(exist_ok=True)
    (inc / "h.h").write_text(header_body)
    body = '#include <h.h>\n' + use
    for level in LEVELS:
        assert diags(run(tmp_path, body, level, "-W" + option, "-isystem", str(inc)), text) == [], level
    assert diags(run(tmp_path, body, "-O0", "-W" + option, "-I", str(inc)), text) != []
