"""The compiler itself under valgrind memcheck: it reads nothing it has not
written.

ir_codegen_scan_interleaved_word_copy (source/ir/codegen.c) tested the
alignment of the run's offsets before checking it had found a run at all: a
word load not followed by its store left them unset, and the -O1+ peephole
read them for every such load (2026-11-20).  The result was thrown away, so
no object changed -- only valgrind sees it, hence a valgrind run.  Likewise
the padding of a string token's last word (tok_str_add2), compared by
save_function_body.  And the token names' arena (toksym_bump), written to
the spill file whole by tok_names_spill: a name's padding byte and a retired
chunk's free tail were never written.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
ROOT = CURRENT_DIR.parent.parent
TCC = ROOT / "armv8m-tcc"

pytestmark = [
    pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc"),
    pytest.mark.skipif(shutil.which("valgrind") is None, reason="needs valgrind"),
]

# A word load whose next instruction is not its store: the scan stops at once.
SRC = """int g(int *p, int *q) { int a = p[1]; q[3] = a + 1; return a; }
struct W { int a, b, c, d; };
void cp(struct W *d, const struct W *s) { *d = *s; }
void sh(int *p) { for (int i = 0; i < 6; i++) p[i + 1] = p[i]; }
"""


@pytest.mark.parametrize("opt", ["-O1", "-O2", "-Os"])
def test_codegen_scans_read_no_uninitialised_memory(tmp_path, opt):
    src = tmp_path / "w.c"
    src.write_text(SRC)
    proc = subprocess.run(
        ["valgrind", "-q", "--error-exitcode=99", "--leak-check=no", str(TCC), "-ccompat", opt, "-c", str(src),
         "-o", str(tmp_path / "w.o")],
        capture_output=True, text=True, timeout=300)
    errors = [line for line in proc.stderr.splitlines() if "codegen" in line or "uninitialised" in line]
    assert proc.returncode == 0 and not errors, proc.stderr


# A string token's last word was only partly written by tok_str_add2: at -O1+
# a deferred body's two copies (save_function_body) compared its padding.
INLINE_STR = """extern int puts(const char *);
static inline int say(void) { return puts("abcde") + puts("xy"); }
int run(void) { return say() + say(); }
int main(void) { return puts("odd!!") + run(); }
"""


@pytest.mark.parametrize("opt", ["-O0", "-O1", "-O2"])
def test_token_strings_have_no_uninitialised_padding(tmp_path, opt):
    src = tmp_path / "s.c"
    src.write_text(INLINE_STR)
    proc = subprocess.run(
        ["valgrind", "-q", "--error-exitcode=99", "--leak-check=no", str(TCC), "-ccompat", opt, "-c", str(src),
         "-o", str(tmp_path / "s.o")],
        capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0 and "uninitialised" not in proc.stderr, proc.stderr


# More than the spill's 8 KB write buffer, so it reaches write(): prototypes'
# parameter lists (sym_params_release) and the token names (tok_names_spill),
# several name chunks, names of even length (a padding byte each).
def _many_prototypes():
    lines = [f"int fn_{i:04d}x(int alpha_{i}, char *beta_{i}, long gamma{i});" for i in range(1500)]
    lines.append("int main(void) { return fn_0001x(1, 0, 2); }")
    return "\n".join(lines) + "\n"


@pytest.mark.parametrize("opt", ["-O0", "-O2"])
def test_spill_file_gets_no_uninitialised_bytes(tmp_path, opt):
    src = tmp_path / "p.c"
    src.write_text(_many_prototypes())
    proc = subprocess.run(
        ["valgrind", "-q", "--error-exitcode=99", "--leak-check=no", str(TCC), "-ccompat", opt, "-c", str(src),
         "-o", str(tmp_path / "p.o")],
        capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0 and "uninitialised" not in proc.stderr, proc.stderr
