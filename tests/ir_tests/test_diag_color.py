"""-fdiagnostics-color: gcc's colours on errors and warnings; the source line
under them (-fdiagnostics-show-caret) and [-Wname] after a warning
(-fdiagnostics-show-option).

auto (the default) colours only when stderr is a terminal, NO_COLOR is unset
and TERM is not "dumb"; an unset TERM still colours (YasOS sets none).  The
source line and [-Wname] are shown on a terminal (NO_COLOR or not) or with
-fdiagnostics-color=always.  Through a pipe the text is unchanged, so every
other test reading stderr sees the old one-line messages.
"""

import os
import pty
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

LOCUS = "\033[01m\033[K"
RESET = "\033[m\033[K"
ERROR = "\033[01;31m\033[K"
WARNING = "\033[01;35m\033[K"
NOTE = "\033[01;36m\033[K"
QUOTE = LOCUS

SRC = "int f(void) { g(); return; }\n"


def _env(**over):
    env = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "TERM")}
    env.update({k: v for k, v in over.items() if v is not None})
    return env


def _compile(tmp_path, *flags, src=SRC, env=None):
    path = tmp_path / "t.c"
    path.write_text(src)
    proc = subprocess.run(
        [str(TCC), "-ccompat", "-Wall", "-c", *flags, str(path), "-o", str(tmp_path / "t.o")],
        capture_output=True,
        text=True,
        env=env or _env(),
    )
    return proc, str(path)


def _compile_on_tty(tmp_path, *flags, env):
    """stderr on a pseudo-terminal, so auto sees a terminal."""
    path = tmp_path / "t.c"
    path.write_text(SRC)
    master, slave = pty.openpty()
    try:
        proc = subprocess.run(
            [str(TCC), "-ccompat", "-Wall", "-c", *flags, str(path), "-o", str(tmp_path / "t.o")],
            stdout=subprocess.DEVNULL,
            stderr=slave,
            env=env,
        )
        os.close(slave)
        slave = -1
        out = b""
        while True:
            try:
                chunk = os.read(master, 4096)
            except OSError:  # EIO: the slave side is closed
                break
            if not chunk:
                break
            out += chunk
    finally:
        if slave >= 0:
            os.close(slave)
        os.close(master)
    return proc.returncode, out.decode()


def test_always(tmp_path):
    proc, path = _compile(tmp_path, "-fdiagnostics-color=always")
    assert proc.returncode == 0
    assert proc.stderr.splitlines()[:3] == [
        f"{LOCUS}{path}:1:{RESET} {WARNING}warning:{RESET} implicit declaration of function {QUOTE}'g'{RESET}"
        f" {WARNING}[-Wimplicit-function-declaration]{RESET}",
        f"    1 | int f(void) {{ {WARNING}g{RESET}(); return; }}",
        f"      |               {WARNING}^{RESET}",
    ]


def test_error_is_red(tmp_path):
    proc, path = _compile(tmp_path, "-fdiagnostics-color", "-Werror=return-type")
    assert proc.returncode != 0
    assert (
        f"{LOCUS}{path}:1:{RESET} {ERROR}error:{RESET} {QUOTE}'return'{RESET} with no value"
        f" {ERROR}[-Werror=return-type]{RESET}\n"
        f"    1 | int f(void) {{ g(); {ERROR}return{RESET}; }}\n"
        f"      |                    {ERROR}^~~~~~{RESET}\n"
    ) in proc.stderr


def test_plain_text_unchanged(tmp_path):
    """Without colour the text is the old format, byte for byte."""
    proc, path = _compile(tmp_path)
    assert proc.stderr.splitlines() == [
        f"{path}:1: warning: implicit declaration of function 'g'",
        f"{path}:1: warning: 'return' with no value",
    ]


@pytest.mark.parametrize("flags", [(), ("-fdiagnostics-color=auto",)])
def test_auto_through_a_pipe_is_plain(tmp_path, flags):
    proc, _ = _compile(tmp_path, *flags)
    assert proc.stderr and "\033" not in proc.stderr


@pytest.mark.parametrize(
    "flags", [("-fdiagnostics-color=never",), ("-fno-diagnostics-color",), ("-fno-color-diagnostics",)]
)
def test_never(tmp_path, flags):
    proc, _ = _compile(tmp_path, "-fdiagnostics-color", *flags)
    assert proc.stderr and "\033" not in proc.stderr


def test_clang_spelling(tmp_path):
    proc, _ = _compile(tmp_path, "-fcolor-diagnostics")
    assert WARNING in proc.stderr


def test_last_option_wins(tmp_path):
    proc, _ = _compile(tmp_path, "-fno-diagnostics-color", "-fdiagnostics-color=always")
    assert WARNING in proc.stderr


@pytest.mark.parametrize("term", [None, "xterm-256color"])
def test_auto_on_a_terminal(tmp_path, term):
    rc, out = _compile_on_tty(tmp_path, env=_env(TERM=term))
    assert rc == 0 and WARNING in out, out


@pytest.mark.parametrize("env", [{"TERM": "dumb"}, {"TERM": "xterm", "NO_COLOR": "1"}])
def test_auto_on_a_terminal_switched_off(tmp_path, env):
    rc, out = _compile_on_tty(tmp_path, env=_env(**env))
    assert rc == 0 and "warning:" in out and "\033" not in out, out


def test_always_beats_no_color(tmp_path):
    rc, out = _compile_on_tty(tmp_path, "-fdiagnostics-color=always", env=_env(NO_COLOR="1"))
    assert WARNING in out, out


def test_include_chain(tmp_path):
    (tmp_path / "h.h").write_text("int h(void) { g(); return 0; }\n")
    proc, path = _compile(tmp_path, "-fdiagnostics-color", src='#include "h.h"\n')
    lines = proc.stderr.splitlines()
    assert lines[0] == f"In file included from {LOCUS}{path}:1{RESET}:"
    assert lines[1].startswith(f"{LOCUS}{tmp_path / 'h.h'}:1:{RESET} {WARNING}warning:{RESET} ")


# ---- the source line and [-Wname] ---------------------------------------


def _plain(tmp_path, src, *flags):
    proc, path = _compile(tmp_path, "-fdiagnostics-show-caret", "-fdiagnostics-show-option", *flags, src=src)
    return proc.stderr.splitlines(), path


def test_caret_and_option(tmp_path):
    lines, path = _plain(tmp_path, SRC)
    assert lines == [
        f"{path}:1: warning: implicit declaration of function 'g' [-Wimplicit-function-declaration]",
        "    1 | int f(void) { g(); return; }",
        "      |               ^",
        f"{path}:1: warning: 'return' with no value [-Wreturn-type]",
        "    1 | int f(void) { g(); return; }",
        "      |                    ^~~~~~",
    ]


def test_each_alone(tmp_path):
    proc, path = _compile(tmp_path, "-fdiagnostics-show-option")
    assert proc.stderr.splitlines()[0] == (
        f"{path}:1: warning: implicit declaration of function 'g' [-Wimplicit-function-declaration]"
    )
    proc, path = _compile(tmp_path, "-fdiagnostics-show-caret")
    assert proc.stderr.splitlines()[:2] == [
        f"{path}:1: warning: implicit declaration of function 'g'",
        "    1 | int f(void) { g(); return; }",
    ]


def test_no_line_numbers(tmp_path):
    lines, _ = _plain(tmp_path, SRC, "-fno-diagnostics-show-line-numbers")
    assert lines[1:3] == [" int f(void) { g(); return; }", "               ^"]


def test_no_unique_name_shows_the_line_alone(tmp_path):
    """'g' twice on the line: no caret (tcc keeps no column), the line alone."""
    src = "int f(void) { return g() + g(); }\n"
    lines, path = _plain(tmp_path, src)
    assert lines == [
        f"{path}:1: warning: implicit declaration of function 'g' [-Wimplicit-function-declaration]",
        "    1 | int f(void) { return g() + g(); }",
    ]


def test_tabs_and_utf8_line_up(tmp_path):
    src = "int g(void)\n{\n\treturn zażółć_nope;\n}\n"
    proc, path = _compile(tmp_path, "-fdiagnostics-show-caret", src=src)
    assert proc.stderr.splitlines() == [
        f"{path}:3: error: 'zażółć_nope' undeclared",
        "    3 |         return zażółć_nope;",
        "      |                ^~~~~~~~~~~",
    ]


def test_quote_at_start_is_kept(tmp_path):
    """A message that starts with its quoted name keeps all of its text."""
    proc, path = _compile(tmp_path, "-fdiagnostics-color=always")
    assert proc.stderr.splitlines()[3] == (
        f"{LOCUS}{path}:1:{RESET} {WARNING}warning:{RESET} {QUOTE}'return'{RESET} with no value"
        f" {WARNING}[-Wreturn-type]{RESET}"
    )


def test_note_on_the_same_line_without_a_name(tmp_path):
    """A note about the line just shown, with no name of its own on it, does
    not show it again."""
    src = "#include <string.h>\nvoid f(void) { char b[4]; strcpy(b, \"hello world\"); (void)b; }\n"
    proc, path = _compile(tmp_path, "-fdiagnostics-show-caret", src=src)
    lines = proc.stderr.splitlines()
    i = next(k for k, l in enumerate(lines) if "note:" in l)
    assert lines[i - 2].endswith('strcpy(b, "hello world"); (void)b; }'), lines
    assert lines[i - 1].strip(" |") == "^~~~~~", lines
    assert i == len(lines) - 1 or not lines[i + 1].startswith("    2 |"), lines


def test_plain_output(tmp_path):
    proc, path = _compile(tmp_path, "-fdiagnostics-color=always", "-fdiagnostics-plain-output")
    assert proc.stderr.splitlines() == [
        f"{path}:1: warning: implicit declaration of function 'g'",
        f"{path}:1: warning: 'return' with no value",
    ]


def test_no_caret_no_option_with_colour(tmp_path):
    proc, path = _compile(
        tmp_path, "-fdiagnostics-color=always", "-fno-diagnostics-show-caret", "-fno-diagnostics-show-option"
    )
    assert proc.stderr.splitlines()[0] == (
        f"{LOCUS}{path}:1:{RESET} {WARNING}warning:{RESET} implicit declaration of function {QUOTE}'g'{RESET}"
    )
    assert len(proc.stderr.splitlines()) == 2


def test_json_unchanged(tmp_path):
    """the JSON output takes the message alone: no quotes coloured, no tag"""
    proc, _ = _compile(tmp_path, "-fdiagnostics-color=always", "-fdiagnostics-format=json")
    assert "\033" not in proc.stderr and "[-W" not in proc.stderr and " | " not in proc.stderr


def test_terminal_shows_caret_without_colour(tmp_path):
    rc, out = _compile_on_tty(tmp_path, env=_env(TERM="xterm", NO_COLOR="1"))
    lines = out.replace("\r\n", "\n").splitlines()
    assert rc == 0 and "\033" not in out, out
    assert lines[0].endswith("implicit declaration of function 'g' [-Wimplicit-function-declaration]"), out
    assert lines[1:3] == ["    1 | int f(void) { g(); return; }", "      |               ^"], out


def test_dumb_terminal_is_plain(tmp_path):
    rc, out = _compile_on_tty(tmp_path, env=_env(TERM="dumb"))
    assert rc == 0 and "[-W" not in out and " | " not in out, out
