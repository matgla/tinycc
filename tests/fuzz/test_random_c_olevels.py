"""Track 2a -- pytest wrapper for the O-level self-consistency differential.

Generates N seeded UB-free random C programs (``gen_c.py``) and asserts that
each one's observable output (stdout + exit code) is identical when compiled by
``armv8m-tcc`` at ``-O0``, ``-O1`` and ``-O2`` and run under QEMU
``mps2-an505``.  A divergence means an optimization changed behaviour -> a
candidate miscompile, with the O-level pinned.

The actual build/run plumbing lives in ``fuzz_harness.py`` (which reuses the
``tests/ir_tests`` QEMU infrastructure), and the diff logic in
``scripts/diff_olevels.py``; this module is a thin pytest front-end.

Clean skip: if QEMU / newlib is not prepared in this environment, every test is
skipped with a clear reason (no failures, no false negatives).

Seed count / range can be overridden:
    pytest tests/fuzz/test_random_c_olevels.py            # default seeds
    FUZZ_OLEVEL_SEEDS=0-199 pytest tests/fuzz/test_random_c_olevels.py
    FUZZ_OLEVEL_SEEDS=7,42,100 pytest tests/fuzz/test_random_c_olevels.py
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
import diff_olevels                 # noqa: E402

OPT_LEVELS = ["-O0", "-O1", "-O2"]

# Seeds temporarily pinned while Finding #15 was open.  Keep this map empty:
# any future entry is a fresh optimizer regression to root-cause.
KNOWN_DIVERGENCES = {}


def _default_seeds():
    spec = os.environ.get("FUZZ_OLEVEL_SEEDS")
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
    # Small default so the suite stays fast under QEMU; bump via the env var.
    return list(range(0, 12))


SEEDS = _default_seeds()

# Generator feature profile (Axis 2 of docs/plan_fuzz_reach_expansion.md).
# FUZZ_PROFILE=float sweeps the FP profile; "int" (default) is the historical stream.
PROFILE = os.environ.get("FUZZ_PROFILE", "int")


def _qemu_or_skip():
    usable, reason = H.qemu_available()
    if not usable:
        pytest.skip(f"QEMU/newlib not prepared: {reason}")


@pytest.mark.parametrize("seed", SEEDS, ids=[f"seed{s}" for s in SEEDS])
def test_olevel_self_consistency(seed, tmp_path):
    _qemu_or_skip()
    if seed in KNOWN_DIVERGENCES:
        pytest.xfail(KNOWN_DIVERGENCES[seed])

    src = tmp_path / f"fuzz_{seed}.c"
    src.write_text(generate_program(seed, PROFILE))

    consistent, results = diff_olevels.check_one(src, OPT_LEVELS, tmp_path)

    if not consistent:
        # Persist a repro alongside the per-level outputs for triage.
        results_dir = THIS_DIR / "results" / "olevels"
        diff_olevels._save_divergence(results_dir, f"seed_{seed}", src, results)
        detail = " | ".join(
            f"{r.label}={r.stdout.strip()!r}/exit{r.exit_code}"
            f"{'' if r.ok else ' ERR:' + (r.error.strip().splitlines()[0] if r.error.strip() else '?')}"
            for r in results
        )
        pytest.fail(
            f"O-level divergence for seed {seed} (repro saved to {results_dir}):\n"
            f"  {detail}"
        )
