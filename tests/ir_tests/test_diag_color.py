"""-fdiagnostics-color: gcc's colours on errors and warnings.

auto (the default) colours only when stderr is a terminal, NO_COLOR is unset
and TERM is not "dumb"; an unset TERM still colours (YasOS sets none).  The
text without colour is unchanged, so every other test reading stderr through
a pipe sees the old messages.
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
    first = proc.stderr.splitlines()[0]
    assert first == f"{LOCUS}{path}:1:{RESET} {WARNING}warning:{RESET} implicit declaration of function 'g'"


def test_error_is_red(tmp_path):
    proc, path = _compile(tmp_path, "-fdiagnostics-color", "-Werror=return-type")
    assert proc.returncode != 0
    assert f"{LOCUS}{path}:1:{RESET} {ERROR}error:{RESET} 'return' with no value" in proc.stderr


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
