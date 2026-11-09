"""Internal spill (-f[no-]internal-spill): saved function bodies and prototype
parameter lists go to a file beside the output while compiling.

The file must be the compile's own: two compiles writing the same output at
once (a build that names one object twice, or pytest-xdist running two tests
on one fixture) each truncated and deleted the other's, and the second failed
with "internal spill: cannot read back".
"""

import subprocess
import concurrent.futures
from pathlib import Path

from cross_includes import INCLUDE_FLAGS

ROOT = Path(__file__).parent.parent.parent  # libs/tinycc
TCC = ROOT / "armv8m-tcc"
BUILD_DIR = Path(__file__).parent / "build" / "internal_spill"

CFLAGS = ["-O2", "-nostdlib", "-mcpu=cortex-m33", "-mthumb", "-mfloat-abi=soft", "-fno-pic", "-c"]


def _source(path: Path) -> Path:
    """Enough deferred bodies and prototypes to spill past the 8 KB write buffer."""
    parts = []
    for i in range(160):
        parts.append(f"static int f{i}(int a, int b, int c, int d);")
    for i in range(160):
        parts.append(
            f"static int f{i}(int a, int b, int c, int d)\n{{\n"
            f"  int s = 0;\n"
            f"  for (int k = 0; k < a; k++) {{ s += (k * {i + 3}) ^ b; s -= c >> (k & 7); s += d * k; }}\n"
            f"  if (s > {i * 7}) s = s / (b | 1) + c; else s = s * {i + 1} - d;\n"
            f"  return s + a * {i} - b * c + d;\n}}\n"
        )
    parts.append("int entry(int x) {\n  int r = 0;")
    for i in range(160):
        parts.append(f"  r += f{i}(x, r, {i}, x + {i});")
    parts.append("  return r;\n}")
    path.write_text("\n".join(parts) + "\n")
    return path


def _compile(src: Path, out: Path, extra=()):
    cmd = [str(TCC), *INCLUDE_FLAGS, *CFLAGS, *extra, str(src), "-o", str(out)]
    return subprocess.run(cmd, capture_output=True, text=True)


def test_internal_spill_output_identical_and_cleaned_up():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    src = _source(BUILD_DIR / "spill_src.c")
    a, b = BUILD_DIR / "spill_on.o", BUILD_DIR / "spill_off.o"
    r = _compile(src, a)
    assert r.returncode == 0, r.stderr
    r = _compile(src, b, ["-fno-internal-spill"])
    assert r.returncode == 0, r.stderr
    assert a.read_bytes() == b.read_bytes()
    assert not list(BUILD_DIR.glob("spill_on.o*.spill")), "spill file left behind"


def test_internal_spill_leaves_other_spill_files_alone():
    """A file another compile of the same output would use stays as it is."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    src = _source(BUILD_DIR / "spill_src2.c")
    out = BUILD_DIR / "shared.o"
    foreign = BUILD_DIR / "shared.o.spill"
    foreign.write_bytes(b"another compile's records")
    r = _compile(src, out)
    assert r.returncode == 0, r.stderr
    assert foreign.exists(), "the compile deleted a spill file it did not make"
    assert foreign.read_bytes() == b"another compile's records"
    foreign.unlink()


def test_internal_spill_concurrent_compiles_of_one_output():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    src = _source(BUILD_DIR / "spill_src3.c")
    ref = BUILD_DIR / "concurrent_ref.o"
    r = _compile(src, ref, ["-fno-internal-spill"])
    assert r.returncode == 0, r.stderr
    out = BUILD_DIR / "concurrent.o"
    for _ in range(3):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: _compile(src, out), range(8)))
        for res in results:
            assert res.returncode == 0, res.stderr
        assert out.read_bytes() == ref.read_bytes()
