"""Track 3a -- pytest wrapper for the tcc-vs-gcc differential.

Generates N seeded UB-free random C programs (``gen_c.py``), compiles each with
``arm-none-eabi-gcc -O2`` (the trusted oracle) and with ``armv8m-tcc`` at
``-O0``/``-O1``/``-O2``, runs all under the SAME QEMU ``mps2-an505`` harness, and
asserts every tcc level's (stdout, exit) signature matches gcc's.  This catches
bugs where all tcc levels agree but are wrong -- which Track 2a cannot.

Build/run plumbing lives in ``fuzz_harness.py`` (reuses the ``tests/ir_tests``
QEMU infra; gcc is linked against the same board ``boot.S`` + ``linker_script``
+ newlib as tcc, so only the generated code differs).

Clean skip: if QEMU / newlib / the gcc semihosting runtime is not prepared,
every test is skipped with a clear reason.

Override seeds via FUZZ_VSGCC_SEEDS (same syntax as the olevel wrapper).
The torture mode is exercised by the scripts/diff_vs_gcc.py CLI (--mode torture),
not duplicated here, to keep the pytest run fast and deterministic.
"""

import os
import sys
from pathlib import Path

import pytest

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent
SCRIPTS_DIR = REPO_ROOT / "scripts"
for p in (str(THIS_DIR), str(SCRIPTS_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

import fuzz_harness as H            # noqa: E402
from gen_c import generate_program  # noqa: E402

TCC_OPT_LEVELS = ["-O0", "-O1", "-O2"]
GCC_OPT = "-O2"

# Seeds known to diverge from gcc today (Phase BH findings, not yet fixed).
# Marked xfail so the suite stays green-by-default while still exercising the
# gcc-differential harness on the known-bad input.
#
# Findings (2026-06): armv8m-tcc disagrees with arm-none-eabi-gcc -O2 on UB-free
# Seeds temporarily pinned while Finding #15 was open.  Keep this map empty:
# any future entry is a fresh optimizer-vs-gcc regression to root-cause.
KNOWN_DIVERGENCES = {}


def _default_seeds():
    spec = os.environ.get("FUZZ_VSGCC_SEEDS")
    if spec:
        seeds = []
        for token in spec.split(","):
            token = token.strip()
            if "-" in token:
                lo, hi = token.split("-", 1)
                seeds.extend(range(int(lo), int(hi) + 1))
            elif token:
                seeds.append(int(token))
        return seeds
    return list(range(0, 12))


SEEDS = _default_seeds()

# Generator feature profile (Axis 2 of docs/plan_fuzz_reach_expansion.md).
# FUZZ_PROFILE=float sweeps the FP profile.  This (ARM-gcc) oracle is the gold
# standard for floats: soft-float -> IEEE correctly-rounded, no excess precision.
PROFILE = os.environ.get("FUZZ_PROFILE", "int")


def _gcc_ref_or_skip():
    usable, reason = H.gcc_reference_available()
    if not usable:
        pytest.skip(f"QEMU/newlib/gcc-runtime not prepared: {reason}")


def _save(results_dir: Path, tag: str, source: Path, ref, tcc_results):
    results_dir.mkdir(parents=True, exist_ok=True)
    case_dir = results_dir / tag
    case_dir.mkdir(parents=True, exist_ok=True)
    (case_dir / source.name).write_text(Path(source).read_text())
    lines = [f"# tcc-vs-gcc divergence: {tag}", "",
             f"[{ref.label} REFERENCE] exit={ref.exit_code} stdout={ref.stdout.strip()!r}"]
    for r in tcc_results:
        agree = "MATCH" if (r.ok and r.signature == ref.signature) else "DIFF"
        lines.append(f"[{r.label}] {agree} ok={r.ok} exit={r.exit_code} "
                     f"stdout={r.stdout.strip()!r} err={r.error.strip()!r}")
    (case_dir / "outputs.txt").write_text("\n".join(lines) + "\n")
    return case_dir


@pytest.mark.parametrize("seed", SEEDS, ids=[f"seed{s}" for s in SEEDS])
def test_tcc_matches_gcc(seed, tmp_path):
    _gcc_ref_or_skip()
    if seed in KNOWN_DIVERGENCES:
        pytest.xfail(KNOWN_DIVERGENCES[seed])

    src = tmp_path / f"fuzz_{seed}.c"
    src.write_text(generate_program(seed, PROFILE))

    ref = H.run_with_gcc(src, GCC_OPT, tmp_path)
    if not ref.ok:
        # A broken gcc reference build is an environment problem, not a tcc bug.
        pytest.skip(f"gcc reference build/run failed: "
                    f"{ref.error.strip().splitlines()[0] if ref.error.strip() else '?'}")

    tcc_results = [H.run_with_tcc(src, o, tmp_path) for o in TCC_OPT_LEVELS]
    mismatched = [r for r in tcc_results if not (r.ok and r.signature == ref.signature)]

    if mismatched:
        results_dir = THIS_DIR / "results" / "vs_gcc"
        _save(results_dir, f"seed_{seed}", src, ref, tcc_results)
        detail = [f"gcc{GCC_OPT}={ref.stdout.strip()!r}/exit{ref.exit_code}"]
        for r in tcc_results:
            mark = "" if (r.ok and r.signature == ref.signature) else "  <-- DIFF"
            detail.append(f"{r.label}={r.stdout.strip()!r}/exit{r.exit_code}{mark}")
        pytest.fail(
            f"tcc disagrees with gcc for seed {seed} (repro saved to {results_dir}):\n  "
            + "\n  ".join(detail)
        )
