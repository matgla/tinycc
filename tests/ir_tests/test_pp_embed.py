"""C23 `#embed` diagnostics, -E output and -MD dependencies.

The run-time behaviour is bug_pp_embed.c; this covers what a program cannot:
a missing resource or an unknown parameter is an error (an unknown directive
used to be a warning, so `{ #embed "f" }` compiled to an empty array), and the
resource is a -MD dependency like an #include.
"""

import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"

pytestmark = pytest.mark.skipif(not TCC.exists(), reason="needs armv8m-tcc")


def _compile(tmp_path, body, *flags):
    sub = tmp_path / "src"
    sub.mkdir(exist_ok=True)
    (sub / "data.bin").write_bytes(b"\x01\x02\xff")
    (sub / "t.c").write_text(body)
    return subprocess.run(
        [str(TCC), "-ccompat", *flags, "src/t.c"], capture_output=True, text=True, cwd=tmp_path
    )


def test_embed_preprocessed_output(tmp_path):
    proc = _compile(tmp_path, 'int a[] = {\n#embed "data.bin" suffix(, 4)\n};\n', "-E", "-P")
    assert proc.returncode == 0, proc.stderr
    assert "".join(proc.stdout.split()) == "inta[]={1,2,255,4};", proc.stdout


def test_embed_include_path_and_dependency(tmp_path):
    (tmp_path / "inc").mkdir()
    (tmp_path / "inc" / "other.bin").write_bytes(b"\x05")
    dep = tmp_path / "t.d"
    proc = _compile(
        tmp_path,
        'const unsigned char a[] = {\n#embed "data.bin"\n};\n'
        'const unsigned char b[] = {\n#embed <other.bin>\n};\n',
        "-Iinc", "-c", "-MD", "-MF", str(dep), "-o", str(tmp_path / "t.o"),
    )
    assert proc.returncode == 0, proc.stderr
    deps = dep.read_text()
    assert "data.bin" in deps and "other.bin" in deps, deps


@pytest.mark.parametrize(
    "line, message",
    [
        ('#embed "missing.bin"', "embed file 'missing.bin' not found"),
        ('#embed "data.bin" no_such_param(1)', "unsupported #embed parameter"),
        ('#embed "data.bin" limit(', "#embed"),
        ('#embed "data.bin" limit(-1)', "negative"),
        ("#embed", "#embed"),
    ],
)
def test_embed_errors(tmp_path, line, message):
    proc = _compile(tmp_path, "int a[] = {\n" + line + "\n};\n", "-c", "-o", str(tmp_path / "t.o"))
    assert proc.returncode != 0, proc.stderr
    assert message in proc.stderr, proc.stderr


def test_unknown_directive_still_warns(tmp_path):
    # Not every unknown directive becomes an error: #ident-style vendor
    # directives in old headers keep compiling, as before.
    proc = _compile(tmp_path, "#frobnicate\nint x;\n", "-c", "-o", str(tmp_path / "t.o"))
    assert proc.returncode == 0, proc.stderr
