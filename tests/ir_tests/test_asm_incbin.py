"""`.incbin "file"[, skip[, count]]` in assembly sources.

The YasOS an524 kernel embeds its romfs image this way (rootfs.S).  The file
is looked up like GAS does -- as given, beside the including file, then on
the -I path -- and, like an #include, it is a -MD dependency.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

CURRENT_DIR = Path(__file__).parent
TCC = CURRENT_DIR.parent.parent / "armv8m-tcc"
OBJCOPY = shutil.which("arm-none-eabi-objcopy")

pytestmark = pytest.mark.skipif(
    OBJCOPY is None or not TCC.exists(),
    reason="needs armv8m-tcc and arm-none-eabi-objcopy",
)

BLOB = bytes(range(256)) * 2 + b"tail"


def _section_bytes(obj, name):
    out = obj.with_suffix(".bin")
    subprocess.run([OBJCOPY, "-O", "binary", "-j", name, str(obj), str(out)], check=True)
    return out.read_bytes()


def _assemble(tmp_path, body, *flags):
    sub = tmp_path / "src"
    sub.mkdir(exist_ok=True)
    src = sub / "blob.S"
    src.write_text('.section .blob, "a"\n' + body + "\n")
    obj = tmp_path / "blob.o"
    dep = tmp_path / "blob.d"
    proc = subprocess.run(
        [str(TCC), "-c", *flags, "-MD", "-MF", str(dep), "-o", str(obj), "src/blob.S"],
        capture_output=True, text=True, cwd=tmp_path,
    )
    return proc, obj, dep


@pytest.mark.parametrize(
    "where, flags",
    [("beside", ()), ("incdir", ("-Iinc",)), ("cwd", ())],
)
def test_incbin_lookup_and_dependency(tmp_path, where, flags):
    target = {"beside": tmp_path / "src", "incdir": tmp_path / "inc", "cwd": tmp_path}[where]
    target.mkdir(exist_ok=True)
    (target / "data.bin").write_bytes(BLOB)
    proc, obj, dep = _assemble(tmp_path, '.incbin "data.bin"', *flags)
    assert proc.returncode == 0, proc.stderr
    assert _section_bytes(obj, ".blob") == BLOB
    assert "data.bin" in dep.read_text(), dep.read_text()


def test_incbin_skip_and_count(tmp_path):
    (tmp_path / "data.bin").write_bytes(BLOB)
    proc, obj, _ = _assemble(tmp_path, '.incbin "data.bin", 10\n.incbin "data.bin", 3, 5')
    assert proc.returncode == 0, proc.stderr
    assert _section_bytes(obj, ".blob") == BLOB[10:] + BLOB[3:8]


@pytest.mark.parametrize(
    "body, message",
    [
        ('.incbin "missing.bin"', "can't find .incbin file"),
        ('.incbin "data.bin", 0, 100000', "shorter than the requested count"),
    ],
)
def test_incbin_errors(tmp_path, body, message):
    (tmp_path / "data.bin").write_bytes(BLOB)
    proc, _, _ = _assemble(tmp_path, body)
    assert proc.returncode != 0 and message in proc.stderr, proc.stderr
