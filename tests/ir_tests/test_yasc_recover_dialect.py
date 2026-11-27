"""Error recovery keeps the file's dialect.

The file-scope recovery point is armed before the file's `#pragma yasc` is
read; restoring its saved state after an error made the rest of the file
regular C (`maybe`, `defer`, `unused(...)` became names).  After an error the
file is still yasc, and still regular C without the pragma.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")

ERRORS = {
    "syntax": "int x = ;\n",
    "contract": "struct S { int a; };\nint e(struct S *p);\nint e(out struct S *p) { p->a = 1; return 0; }\n",
}

AFTER = """\
int g(int u)
{
  int *?p = nullptr;
  defer u = 2;
  unused(u);
  return p != nullptr;
}
"""


def _run(tmp_path, text, *flags):
    src = tmp_path / "t.c"
    src.write_text(text)
    return subprocess.run([str(TCC), "-Wall", "-Wno-missing-prototypes", "-c", *flags, str(src), "-o",
                           str(tmp_path / "t.o")], capture_output=True, text=True, cwd=tmp_path)


@pytest.mark.parametrize("name", sorted(ERRORS))
def test_yasc_after_an_error(tmp_path, name):
    proc = _run(tmp_path, "#pragma yasc\n" + ERRORS[name] + AFTER)
    assert proc.returncode != 0
    lines = ERRORS[name].count("\n") + 1
    later = [l for l in proc.stderr.splitlines() if ": " in l and l.split(":")[1].isdigit()
             and int(l.split(":")[1]) > lines]
    assert later == [], proc.stderr


def test_regular_c_after_an_error(tmp_path):
    proc = _run(tmp_path, "int x = ;\nint g(int maybe) { return maybe; }\n", "-ccompat")
    assert [l for l in proc.stderr.splitlines() if "t.c:2:" in l] == [], proc.stderr
