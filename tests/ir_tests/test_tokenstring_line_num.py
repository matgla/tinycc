"""TokenString keeps line numbers as int: past line 65535 a macro expansion
must not rewind file->line_num modulo 65536."""

import subprocess
from pathlib import Path

import pytest

TCC = Path(__file__).parent.parent.parent / "armv8m-tcc"
pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

PAD = 70000
SOURCE = ("#define M(x) x\n" + "\n" * PAD +
          "int a = __LINE__;\nint b = M(1);\nint c = __LINE__;\n"
          "void f(void){ undeclared_thing = 1; }\n")


def _tcc(args, src):
    root = TCC.parent
    return subprocess.run([str(TCC), f"-B{root}", "-I", str(root / "include"), *args],
                          input=src, capture_output=True, text=True)


def test_line_after_macro_use_past_65535():
    r = _tcc(["-E", "-"], SOURCE)
    assert r.returncode == 0, r.stderr
    assert f"int a = {PAD + 2};" in r.stdout
    assert f"int c = {PAD + 4};" in r.stdout


def test_diagnostic_line_past_65535():
    r = _tcc(["-c", "-o", "/dev/null", "-"], SOURCE)
    assert r.returncode != 0
    assert f":{PAD + 5}: error" in r.stderr, r.stderr
