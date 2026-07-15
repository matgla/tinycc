#!/usr/bin/env python3
"""Compare the current (possibly dirty) working tree against the server baseline.

The CI metrics job records per-commit code size / compile time / perf into
`/var/lib/tcc-metrics/metrics.db` on the Pi (see docs/metrics_dashboard.md).
This script closes the local-dev loop while you are iterating on an
uncommitted change (e.g. an SSA loop-pass migration on this branch):

  1. build `armv8m-tcc` from the working tree (`make cross`, unless --no-build);
  2. measure it locally into a throwaway scratch metrics.db under a synthetic
     `worktree` identity, reusing metrics/record.py verbatim;
  3. fetch the server's baseline metrics.db over HTTP
     (the /metrics.db route added to metrics/codesize_detail_server.py), or use
     a local copy via --baseline-db;
  4. diff worktree-vs-baseline and print BOTH improvements and regressions.

Default scope is code size + compile time (fast, deterministic, no hardware).
--correctness adds the fuzz O1/O2 divergence sweep (minutes) and --perf adds the
RP2350 cycle benchmark (needs the board over SSH) -- both measured on the
worktree side and diffed against the baseline where it has matching rows.

Exit status is 0 unless --strict is given, in which case a code-size regression
beyond --codesize-tolerance-pct (on the -O2 <total> ratio) or a new correctness
divergence returns 1 -- mirroring metrics/gate.py's block half.

Examples
--------
  # code size + compile time vs the recorded HEAD commit on the server
  python3 metrics/compare_worktree.py --url http://metrics-box:8008

  # skip the rebuild (already ran `make cross`), also sweep correctness
  python3 metrics/compare_worktree.py --url http://metrics-box:8008 \
      --no-build --correctness --seed-hi 2000

  # offline: compare against a metrics.db you copied over yourself
  python3 metrics/compare_worktree.py --baseline-db ./base.db --strict

  # build+measure mob locally as the baseline; the measurement is cached in the
  # scratch db by sha, so subsequent runs skip the rebuild (--refresh-baseline
  # forces a re-measure)
  python3 metrics/compare_worktree.py --baseline-commit mob

  # fast iteration: production level only (skips the o0/o1 corpus compiles)
  python3 metrics/compare_worktree.py --baseline-commit mob --opt o2
"""

import argparse
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from argparse import Namespace
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
sys.path.insert(0, str(SCRIPT_DIR))

import record as REC   # noqa: E402  (metrics/record.py -- reused verbatim)

WORKTREE_SHA = "worktree"      # synthetic commit key for the dirty tree
WORKTREE_HOST = "worktree"     # scratch-db host key (never collides with CI rows)
CODESIZE_OPTS = ["o0", "o1", "o2"]


def info(msg: str) -> None:
    print(f"[compare] {msg}", file=sys.stderr, flush=True)


def warn(msg: str) -> None:
    print(f"[compare] WARN: {msg}", file=sys.stderr, flush=True)


def die(msg: str) -> None:
    print(f"[compare] FATAL: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


# --------------------------------------------------------------------- measure

def build_cross(jobs: int) -> None:
    info(f"building working tree (make cross -j{jobs}) ...")
    rc = subprocess.run(["make", "cross", f"-j{jobs}"], cwd=str(REPO_ROOT)).returncode
    if rc != 0:
        die(f"`make cross` failed (exit {rc}); fix the build or pass --no-build "
            "if armv8m-tcc is already current")
    if not (REPO_ROOT / "armv8m-tcc").exists():
        die("make cross reported success but armv8m-tcc is missing")


def measure_worktree(scratch_db: str, args) -> tuple[sqlite3.Connection, int, str]:
    """Record the in-place armv8m-tcc into `scratch_db` under the worktree key.
    Returns (open connection, worktree run_id, base commit sha)."""
    base = REC.git_meta("HEAD")
    base_sha = base["commit_sha"]
    dirty = subprocess.run(["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
                           capture_output=True, text=True).stdout.strip()
    tag = "dirty" if dirty else "clean"
    meta = {**base, "commit_sha": WORKTREE_SHA, "parent_sha": base_sha,
            "subject": f"[worktree/{tag}] {base['subject']}"}

    rec_args = Namespace(
        db=scratch_db, seed_lo=args.seed_lo, seed_hi=args.seed_hi, mode=args.mode,
        jobs=args.jobs, codesize_detail=True, import_codesize_from=None,
        detail_db=None, detail_keep=None, perf_host=args.perf_host,
        perf_identity=args.perf_identity, perf_strict=False,
        scratch=args.scratch or tempfile.gettempdir())

    conn = REC.connect(scratch_db)
    info(f"measuring worktree (base {base_sha[:12]}, {tag}, "
         f"opts={'/'.join(args.opts)}"
         f"{', +correctness' if args.correctness else ''}"
         f"{', +perf' if args.perf else ''}) ...")
    REC.record_one(conn, meta, WORKTREE_HOST, "worktree", "worktree", rec_args,
                   do_correctness=args.correctness, do_perf=bool(args.perf and args.perf_host),
                   codesize_opts=args.opts)
    run_id = conn.execute(
        "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
        (WORKTREE_SHA, WORKTREE_HOST)).fetchone()[0]
    return conn, run_id, base_sha


def measure_commit_baseline(conn, ref, args) -> tuple[int, str, str, bool]:
    """Build tcc at `ref` and measure ITS code size against the current corpus
    with the local gcc, into the same scratch db keyed by the resolved sha.

    Keying by the real sha makes the scratch db a cache: a re-run against the
    same ref finds a run whose <total> rollup covers every requested opt level
    and skips the rebuild entirely. --refresh-baseline forces a re-measure,
    e.g. after the test corpus or the local gcc changed.

    This is the trustworthy alternative to the server baseline: the server rows
    are measured on the CI host (a different arm-none-eabi-gcc, so the ratio
    denominator moves) and against that host's test corpus (so func_count can
    differ from the worktree). Building the baseline commit here means the diff
    uses the SAME gcc and the SAME test files on both sides -- only the tcc
    binary differs, which is what we actually want to compare.

    Correctness/perf are NOT measured here: record_one runs those against the
    in-place armv8m-tcc (batch_sweep hardcodes it), which would silently score
    the worktree binary, not this commit's.
    Returns (run_id, commit_sha, subject, was_cached).
    """
    from regression_disasm import build_tcc_at_rev
    base = REC.git_meta(ref)
    base_sha = base["commit_sha"]
    if not args.refresh_baseline:
        ph = ",".join("?" * len(args.opts))
        row = conn.execute(
            "SELECT r.run_id, r.subject FROM runs r WHERE r.commit_sha=? AND r.host=? "
            "AND (SELECT COUNT(DISTINCT c.opt) FROM codesize_rollup c "
            f"WHERE c.run_id=r.run_id AND c.suite='<total>' AND c.opt IN ({ph}))=? "
            "LIMIT 1",
            (base_sha, WORKTREE_HOST, *args.opts, len(args.opts))).fetchone()
        if row:
            info(f"baseline {ref} ({base_sha[:12]}) already measured in scratch db; "
                 "reusing cached run (--refresh-baseline to re-measure)")
            return row[0], base_sha, row[1], True
    meta = {**base, "subject": f"[baseline/{ref}] {base['subject']}"}
    info(f"building baseline commit {ref} ({base_sha[:12]}) for local measurement ...")
    tcc_path, build_dir = build_tcc_at_rev(ref, args.jobs)
    rec_args = Namespace(
        db=None, seed_lo=args.seed_lo, seed_hi=args.seed_hi, mode=args.mode,
        jobs=args.jobs, codesize_detail=True, import_codesize_from=None,
        detail_db=None, detail_keep=None, perf_host=None, perf_identity=None,
        perf_strict=False, scratch=args.scratch or tempfile.gettempdir())
    try:
        info(f"measuring baseline commit code size (tcc={tcc_path}, "
             f"opts={'/'.join(args.opts)}) ...")
        REC.record_one(conn, meta, WORKTREE_HOST, "baseline", "baseline", rec_args,
                       tcc_override=tcc_path, do_correctness=False, do_perf=False,
                       codesize_opts=args.opts)
    finally:
        shutil.rmtree(build_dir, ignore_errors=True)
    run_id = conn.execute(
        "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
        (base_sha, WORKTREE_HOST)).fetchone()[0]
    return run_id, base_sha, meta["subject"], False


# --------------------------------------------------------------------- baseline

def fetch_baseline(url: str, dest: str) -> str:
    """Download the server's metrics.db to `dest`. `url` may be a base URL
    (the /metrics.db route is appended) or a full URL to the db."""
    full = url if url.rstrip("/").endswith(".db") else url.rstrip("/") + "/metrics.db"
    info(f"fetching baseline metrics.db from {full} ...")
    try:
        with urllib.request.urlopen(full, timeout=60) as r, open(dest, "wb") as f:
            shutil.copyfileobj(r, f)
    except (urllib.error.URLError, OSError) as e:
        die(f"fetch failed ({full}): {e}")
    # A misrouted request (wrong path/port) often returns an HTML 404 with a
    # 200-ish body; reject anything that is not a real SQLite file up front so
    # the error points at the fetch, not a confusing downstream query failure.
    with open(dest, "rb") as f:
        if f.read(16) != b"SQLite format 3\x00":
            die(f"fetched {full} but it is not a SQLite database (wrong URL, or "
                "the server has no --metrics-db configured)")
    return full


def resolve_baseline_run(conn, base_sha: str, host: str):
    """Pick the baseline run to diff against, preferring the exact base commit,
    then the merge-base with mob, then the newest run for `host`.
    Returns (run_id, commit_sha, subject, how) or None."""
    def by_sha(sha):
        return conn.execute(
            "SELECT run_id, commit_sha, subject FROM runs "
            "WHERE host=? AND commit_sha LIKE ? ORDER BY run_ts DESC LIMIT 1",
            (host, sha + "%")).fetchone()

    row = by_sha(base_sha)
    if row:
        return (*row, f"base commit {base_sha[:12]}")

    mb = subprocess.run(["git", "-C", str(REPO_ROOT), "merge-base", "HEAD", "mob"],
                        capture_output=True, text=True)
    if mb.returncode == 0 and mb.stdout.strip():
        merge_base = mb.stdout.strip()
        row = by_sha(merge_base)
        if row:
            warn(f"base commit {base_sha[:12]} not on server; "
                 f"using merge-base with mob ({merge_base[:12]})")
            return (*row, f"merge-base w/ mob {merge_base[:12]}")

    row = conn.execute(
        "SELECT run_id, commit_sha, subject FROM runs WHERE host=? "
        "ORDER BY commit_ts DESC, run_ts DESC LIMIT 1", (host,)).fetchone()
    if row:
        warn(f"neither base commit nor merge-base recorded for host={host}; "
             f"falling back to newest recorded run {row[1][:12]}")
        return (*row, f"newest run {row[1][:12]} (host {host})")
    return None


# ----------------------------------------------------------------------- load

def load_codesize(conn, run_id) -> dict:
    return {(suite, opt): dict(func_count=fc, tcc=tcc, gcc=gcc, ratio=ratio)
            for suite, opt, fc, tcc, gcc, ratio in conn.execute(
                "SELECT suite,opt,func_count,tcc_size,gcc_size,ratio "
                "FROM codesize_rollup WHERE run_id=?", (run_id,))}


def load_codesize_func(conn, run_id, opt="o2") -> dict:
    return {(suite, test, func): dict(tcc=tcc, gcc=gcc)
            for suite, test, func, tcc, gcc in conn.execute(
                "SELECT suite,test,function,tcc_size,gcc_size FROM codesize_func "
                "WHERE run_id=? AND opt=?", (run_id, opt))}


def load_compile_time(conn, run_id) -> dict:
    return {scope: sec for scope, sec in conn.execute(
        "SELECT scope, seconds FROM compile_time WHERE run_id=?", (run_id,))}


def load_perf(conn, run_id) -> dict:
    return {(b, c, o): cyc for b, c, o, cyc in conn.execute(
        "SELECT benchmark,compiler,opt_level,cycles_per_iter FROM perf WHERE run_id=?",
        (run_id,))}


def load_correctness(conn, run_id) -> dict:
    out = {}
    for p, o in conn.execute(
            "SELECT profile, oracle FROM correctness WHERE run_id=?", (run_id,)):
        out.setdefault((p, o), set())
    for p, o, s in conn.execute(
            "SELECT profile, oracle, seed FROM correctness_seed WHERE run_id=?", (run_id,)):
        out.setdefault((p, o), set()).add(s)
    return out


# --------------------------------------------------------------------- report

def pct(cur, base) -> float:
    return (cur - base) / base * 100.0 if base else 0.0


def split_movers(rows):
    """Partition delta-first rows into (better, worse): improvements sorted
    biggest-win first, regressions sorted biggest-loss first. Zero-delta rows
    (present on one side only) sort with the regressions' tail."""
    better = sorted((r for r in rows if r[0] < 0), key=lambda r: (r[0], r[1]))
    worse = sorted((r for r in rows if r[0] >= 0), key=lambda r: (-r[0], r[1]))
    return better, worse


def tag_lower_is_better(delta: float) -> str:
    return "better" if delta < 0 else "worse" if delta > 0 else "same"


class Report:
    """Accumulates lines + a regression/improvement tally for the verdict."""
    def __init__(self):
        self.lines = []
        self.regressions = 0
        self.improvements = 0
        # gross code-size movement (tcc instructions) at the selected opt level,
        # decomposed so the verdict can show both sides instead of just the net.
        # None when per-function baseline data is unavailable (server db keeps
        # rollups only) -- then the verdict omits the byte breakdown.
        self.improved_bytes = None
        self.regressed_bytes = None
        # Column labels for the two sides being diffed.  Default to the
        # worktree-vs-server framing; --range overrides them with the two refs.
        self.base_label = "baseline"
        self.new_label = "worktree"

    def add(self, line=""):
        self.lines.append(line)

    def render(self):
        return "\n".join(self.lines)


def report_codesize(rep, wt, base, tol, opts):
    rep.add("CODE SIZE  (tcc instructions vs GCC; lower tcc / lower ratio is better)")
    if not wt:
        rep.add("  worktree recorded no code size -- nothing to compare")
        rep.add()
        return
    hdr = "  {:3} {:<12} {:>10} {:>10} {:>10} {:>9} {:>18} {:>9}".format(
        "opt", "suite", "gcc", f"{rep.base_label} tcc"[:10], f"{rep.new_label} tcc"[:10],
        "delta", "ratio base->wt", "verdict")
    rep.add(hdr)
    rep.add("  " + "-" * (len(hdr) - 2))
    mismatch = next((opt for opt in opts
                     if (w := wt.get(("<total>", opt))) and (b := base.get(("<total>", opt)))
                     and w["func_count"] != b["func_count"]), None)
    if mismatch:
        w, b = wt[("<total>", mismatch)], base[("<total>", mismatch)]
        rep.add("  NOTE: func_count differs (worktree {} vs baseline {}); suite "
                "totals and ratios are NOT comparable across a different corpus/gcc."
                .format(w["func_count"], b["func_count"]))
        rep.add("        Use --baseline-commit <ref> for a same-corpus, same-gcc local diff.")
        rep.add()
    for opt in opts:
        w = wt.get(("<total>", opt))
        b = base.get(("<total>", opt))
        if not w or not b:
            rep.add(f"  {opt} {'<total>':<12} {'(missing on ' + ('baseline' if w else 'worktree') + ')':>59}")
            continue
        d = w["tcc"] - b["tcc"]
        rp = pct(w["ratio"], b["ratio"])
        gcc = w["gcc"] if w["gcc"] else b["gcc"]
        rep.add("  {:3} {:<12} {:>10} {:>10} {:>10} {:>+9} {:>8.3f}->{:<7.3f}{:>+6.1f}% {}".format(
            opt, "<total>", gcc, b["tcc"], w["tcc"], d, b["ratio"], w["ratio"], rp,
            tag_lower_is_better(d)))
        if opt == "o2":     # the gated series
            if rp > tol:
                rep.regressions += 1
            elif rp < -tol:
                rep.improvements += 1

    # Per-suite breakdown at the highest selected level, largest movers first.
    det = opts[-1]
    suites = sorted({s for (s, o) in wt if o == det and s != "<total>"}
                    | {s for (s, o) in base if o == det and s != "<total>"})
    rows = []
    new_test_rows = []
    for s in suites:
        w = wt.get((s, det))
        b = base.get((s, det))
        wt_n = w["tcc"] if w else None
        b_n = b["tcc"] if b else None
        d = (wt_n or 0) - (b_n or 0)
        if d != 0 or (w is None) != (b is None):
            gcc = (w["gcc"] if w and w["gcc"] else (b["gcc"] if b else None))
            if b is None:
                new_test_rows.append((d, s, b_n, wt_n, gcc))
            else:
                rows.append((d, s, b_n, wt_n, gcc))
    if rows or new_test_rows:
        rep.add()
        if new_test_rows:
            rep.add(f"  per-suite at -{det.upper()} (NEW tests, not counted in statistics):")
            for d, s, b_n, wt_n, gcc in new_test_rows:
                rep.add("    {:<14} {:>10} {:>10} {:>10} {:>+9}  {}".format(
                    s, "-" if gcc is None else gcc, "-",
                    "-" if wt_n is None else wt_n, d, tag_lower_is_better(d)))
            rep.add()
        if rows:
            rep.add(f"  per-suite at -{det.upper()} (nonzero movers; better first, worse below):")
            hdr = "    {:<14} {:>10} {:>10} {:>10} {:>9}  {}".format(
                "suite", "gcc", rep.base_label, rep.new_label, "delta", "verdict")
            rep.add(hdr)
            rep.add("    " + "-" * 59)
            better, worse = split_movers(rows)
            for group_i, group in enumerate((better, worse)):
                if group_i and better and worse:
                    rep.add("    " + "-" * 59)
                for d, s, b_n, wt_n, gcc in group:
                    rep.add("    {:<14} {:>10} {:>10} {:>10} {:>+9}  {}".format(
                        s, "-" if gcc is None else gcc, "-" if b_n is None else b_n,
                        "-" if wt_n is None else wt_n, d, tag_lower_is_better(d)))
    rep.add()


def report_func_deltas(rep, wt, base, top_n, det):
    if not wt:
        return
    if not base:
        rep.add("  per-function: baseline has no codesize_func rows (the server "
                "metrics.db keeps rollups only); use --baseline-commit or a "
                "detail-bearing --baseline-db for a per-function diff")
        rep.add()
        return
    rows = []
    new_test_rows = []
    for key in set(wt) | set(base):
        w, b = wt.get(key), base.get(key)
        wt_n = w["tcc"] if w else None
        b_n = b["tcc"] if b else None
        d = (wt_n or 0) - (b_n or 0)
        if d or (w is None) != (b is None):
            gcc = (w["gcc"] if w and w["gcc"] else (b["gcc"] if b else None))
            if b is None:
                new_test_rows.append((d, key, b_n, wt_n, gcc))
            else:
                rows.append((d, key, b_n, wt_n, gcc))
    if not rows and not new_test_rows:
        rep.add(f"  per-function at -{det.upper()}: no movers")
        rep.add()
        return
    if new_test_rows:
        rep.add(f"  per-function at -{det.upper()} (NEW tests, not counted in statistics):")
        for d, (suite, test, func), b, w, gcc in new_test_rows:
            name = f"{suite}/{test}:{func}"
            rep.add("    {:<52} {:>8} {:>8} {:>8} {:>+8}  {}".format(
                name if len(name) <= 52 else "..." + name[-49:],
                "-" if gcc is None else gcc, "-",
                "-" if w is None else w, d, tag_lower_is_better(d)))
        rep.add()
    if rows:
        all_better, all_worse = split_movers(rows)
        better = all_better[:top_n]
        worse = all_worse[:top_n]
        rep.add(f"  per-function at -{det.upper()} (top {len(better)} better of "
                f"{len(all_better)}, top {len(worse)} worse of {len(all_worse)}):")
        hdr = "    {:<52} {:>8} {:>8} {:>8} {:>8}  {}".format(
            "name", "gcc", rep.base_label, rep.new_label, "delta", "verdict")
        rep.add(hdr)
        rep.add("    " + "-" * 92)
        for group_i, group in enumerate((better, worse)):
            if group_i == 1:
                if not worse:
                    rep.add("    0 worse")
                    break
                if better:
                    rep.add("    " + "-" * 92)
            for d, (suite, test, func), b, w, gcc in group:
                name = f"{suite}/{test}:{func}"
                rep.add("    {:<52} {:>8} {:>8} {:>8} {:>+8}  {}".format(
                    name if len(name) <= 52 else "..." + name[-49:],
                    "-" if gcc is None else gcc, "-" if b is None else b,
                    "-" if w is None else w, d, tag_lower_is_better(d)))
    rep.add()


def report_compile_time(rep, wt, base, opts):
    lines = []
    for opt in opts:
        key = f"codesize_corpus_{opt}"
        if key in wt and key in base:
            w, b = wt[key], base[key]
            lines.append("  {}: {} {:.1f}s -> {} {:.1f}s  ({:+.1f}%)".format(
                opt, rep.base_label, b, rep.new_label, w, pct(w, b)))
    if not lines:
        return
    rep.add("COMPILE TIME  (corpus wall time; informational, hardware-noisy)")
    rep.lines.extend(lines)
    rep.add()


def report_perf(rep, wt, base):
    if not wt:
        return
    rep.add("PERF  (RP2350 cycles/iter; informational)")
    if not base:
        rep.add("  no perf rows on baseline -- worktree measured, nothing to diff")
        rep.add()
        return
    hdr = "  {:<16} {:<10} {:>12} {:>12} {:>10} {}".format(
        "benchmark", "cc/opt", "base", "worktree", "delta", "verdict")
    rep.add(hdr)
    rep.add("  " + "-" * (len(hdr) - 2))
    for key in sorted(set(wt) | set(base)):
        b, c, o = key
        wv, bv = wt.get(key), base.get(key)
        if wv is None or bv is None:
            continue
        d = wv - bv
        rep.add("  {:<16} {:<10} {:>12.0f} {:>12.0f} {:>+10.0f} {}".format(
            b, f"{c}/{o}", bv, wv, d, tag_lower_is_better(d)))
    rep.add()


def report_correctness(rep, wt, base):
    if not wt:
        return
    rep.add("CORRECTNESS  (fuzz O1/O2 divergence; new=regression, fixed=improvement)")
    any_line = False
    for key in sorted(set(wt) | set(base)):
        profile, oracle = key
        w = wt.get(key, set())
        b = base.get(key, set())
        if key not in base:
            rep.add(f"  {profile}/{oracle}: no baseline data "
                    f"(worktree divergent={len(w)})")
            any_line = True
            continue
        new = sorted(w - b)
        fixed = sorted(b - w)
        if new:
            rep.regressions += 1
        if fixed:
            rep.improvements += 1
        if new or fixed:
            any_line = True
            parts = []
            if new:
                parts.append(f"new(regressed)={new}")
            if fixed:
                parts.append(f"fixed(improved)={fixed}")
            rep.add(f"  {profile}/{oracle}: " + "  ".join(parts))
    if not any_line:
        rep.add("  no change vs baseline")
    rep.add()


# ----------------------------------------------------------------------- main

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = p.add_argument_group("baseline source (choose one)")
    src.add_argument("--url", help="base URL of the metrics server "
                     "(codesize_detail_server on :8008); /metrics.db is appended")
    src.add_argument("--baseline-db", help="use a local metrics.db instead of fetching")
    src.add_argument("--baseline-commit", metavar="REF",
                     help="build tcc at this git ref (e.g. 'mob' or a sha) and measure "
                          "it locally as the baseline -- same corpus + same gcc as the "
                          "worktree, so ratios/func_count are comparable (code size + "
                          "compile time only; correctness/perf are in-place-binary only). "
                          "Measurements are cached in the scratch db by sha and reused")
    src.add_argument("--range", metavar="A..B", dest="rev_range",
                     help="compare two committed revisions instead of the working "
                          "tree: build+measure both A (base) and B (new) locally "
                          "(same corpus + gcc) and diff B vs A. Skips `make cross` "
                          "and the worktree. Example: --range HEAD~1..HEAD")
    p.add_argument("--refresh-baseline", action="store_true",
                   help="re-measure --baseline-commit even if its sha is already "
                        "in the scratch db (use after the corpus or gcc changed)")
    p.add_argument("--baseline-host", default="armv8m-metrics",
                   help="host key of the recorded baseline rows (default: the CI "
                        "METRICS_HOST 'armv8m-metrics')")
    p.add_argument("--opt", action="append", choices=CODESIZE_OPTS, metavar="LEVEL",
                   help="opt level(s) to measure+compare (o0/o1/o2; repeatable, "
                        "e.g. --opt o2 for a fast production-level-only run; "
                        "default: all three)")
    p.add_argument("--no-build", action="store_true",
                   help="do not run `make cross` first (armv8m-tcc must be current)")
    p.add_argument("--correctness", action="store_true",
                   help="also run the fuzz O1/O2 divergence sweep (slow)")
    p.add_argument("--perf", action="store_true",
                   help="also run the RP2350 cycle benchmark (needs --perf-host)")
    p.add_argument("--seed-lo", type=int, default=0)
    p.add_argument("--seed-hi", type=int, default=2000)
    p.add_argument("--mode", choices=["prescan", "triage"], default="prescan")
    p.add_argument("--perf-host", help="SSH host for the RP2350 benchmark")
    p.add_argument("--perf-identity", help="SSH identity file for --perf-host")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    p.add_argument("--scratch", help="scratch dir for perf JSON (default: system temp)")
    p.add_argument("--scratch-db", help="path for the worktree scratch metrics.db "
                   "(default: <tmp>/tcc-worktree-metrics.db; reused idempotently)")
    p.add_argument("--top-funcs", type=int, default=50,
                   help="how many largest per-function -O2 deltas to print per "
                        "direction: up to N better and N worse (default 50; 0 disables)")
    p.add_argument("--codesize-tolerance-pct", type=float, default=1.0,
                   help="o2 <total> ratio growth beyond this %% counts as a "
                        "code-size regression (default 1.0)")
    p.add_argument("--strict", action="store_true",
                   help="exit 1 if any code-size or correctness regression is found")
    args = p.parse_args(argv)
    args.opts = [o for o in CODESIZE_OPTS if o in (args.opt or CODESIZE_OPTS)]

    base_ref = new_ref = None
    if args.rev_range:
        if any((args.url, args.baseline_db, args.baseline_commit)):
            p.error("--range is exclusive with --url/--baseline-db/--baseline-commit")
        if ".." not in args.rev_range:
            p.error("--range must look like A..B (e.g. HEAD~1..HEAD)")
        base_ref, _, new_ref = args.rev_range.partition("..")
        base_ref, new_ref = base_ref.strip(), new_ref.strip()
        if not base_ref or not new_ref:
            p.error("--range needs both endpoints: A..B")
    elif not args.url and not args.baseline_db and not args.baseline_commit:
        p.error("need --range (two commits), --url (fetch from server), "
                "--baseline-db (local copy), or --baseline-commit (build+measure "
                "a ref locally)")
    if args.perf and not args.perf_host:
        p.error("--perf requires --perf-host")
    if args.rev_range and (args.correctness or args.perf):
        warn("--correctness/--perf are ignored with --range: both endpoints are "
             "built and measured for code size + compile time only")
        args.correctness = args.perf = False
    if args.baseline_commit and args.correctness:
        warn("--correctness is ignored with --baseline-commit: correctness is "
             "measured against the in-place binary, not the baseline commit")

    if not args.rev_range:
        if not args.no_build:
            build_cross(args.jobs)
        elif not (REPO_ROOT / "armv8m-tcc").exists():
            die("--no-build given but armv8m-tcc does not exist; build it first")

    scratch_db = args.scratch_db or str(
        Path(tempfile.gettempdir()) / "tcc-worktree-metrics.db")
    if args.rev_range:
        wt_conn = REC.connect(scratch_db)
        wt_run = base_sha = None  # set below from the resolved refs
    else:
        wt_conn, wt_run, base_sha = measure_worktree(scratch_db, args)

    tmp_baseline = None
    try:
        if args.rev_range:
            base_conn = wt_conn      # same scratch db; do not double-close
            base_run, base_run_sha, base_subject, _bc = measure_commit_baseline(
                wt_conn, base_ref, args)
            new_run, new_run_sha, new_subject, _nc = measure_commit_baseline(
                wt_conn, new_ref, args)
            wt_run, base_sha = new_run, new_run_sha
            how = f"local build of {base_ref}..{new_ref}"
            host_line = f"  local range diff (same corpus + gcc)  scratch-db={scratch_db}"
        elif args.baseline_commit:
            base_conn = wt_conn      # same scratch db; do not double-close
            base_run, base_run_sha, base_subject, cached = measure_commit_baseline(
                wt_conn, args.baseline_commit, args)
            how = (f"local build of {args.baseline_commit}"
                   + (" [cached]" if cached else ""))
            host_line = f"  local baseline (same corpus + gcc)  scratch-db={scratch_db}"
        else:
            if args.baseline_db:
                baseline_path = args.baseline_db
                if not Path(baseline_path).is_file():
                    die(f"--baseline-db not found: {baseline_path}")
            else:
                fd, tmp_baseline = tempfile.mkstemp(prefix="tcc-baseline-", suffix=".db")
                os.close(fd)
                fetch_baseline(args.url, tmp_baseline)
                baseline_path = tmp_baseline

            base_conn = sqlite3.connect(f"file:{baseline_path}?mode=ro", uri=True)
            chosen = resolve_baseline_run(base_conn, base_sha, args.baseline_host)
            if chosen is None:
                die(f"baseline has no runs for host={args.baseline_host!r}; "
                    "check --baseline-host or that the server has recorded data")
            base_run, base_run_sha, base_subject, how = chosen
            host_line = f"  host={args.baseline_host}  scratch-db={scratch_db}"

        rep = Report()
        if args.rev_range:
            rep.base_label, rep.new_label = base_ref, new_ref
        rep.add("=" * 78)
        if args.rev_range:
            rep.add(f"new {new_ref} ({new_run_sha[:12]})  vs  base {base_ref} "
                    f"({base_run_sha[:12]})  [{how}]")
            rep.add(f"  new:  {new_subject}")
            rep.add(f"  base: {base_subject}")
        else:
            rep.add(f"worktree (base {base_sha[:12]})  vs  baseline {base_run_sha[:12]} "
                    f"[{how}]")
            rep.add(f"  baseline: {base_subject}")
        rep.add(host_line)
        rep.add("=" * 78)
        rep.add()

        report_codesize(rep, load_codesize(wt_conn, wt_run),
                        load_codesize(base_conn, base_run),
                        args.codesize_tolerance_pct, args.opts)
        det = args.opts[-1]
        wt_funcs = load_codesize_func(wt_conn, wt_run, det)
        base_funcs = load_codesize_func(base_conn, base_run, det)
        if base_funcs:
            improved = regressed = 0
            for key in set(wt_funcs) | set(base_funcs):
                if key not in base_funcs:
                    continue     # NEW test function; excluded like the report table
                d = ((wt_funcs.get(key) or {}).get("tcc", 0)) - base_funcs[key]["tcc"]
                if d < 0:
                    improved += -d
                elif d > 0:
                    regressed += d
            rep.improved_bytes = improved
            rep.regressed_bytes = regressed
        if args.top_funcs > 0:
            report_func_deltas(rep, wt_funcs, base_funcs, args.top_funcs, det)
        report_compile_time(rep, load_compile_time(wt_conn, wt_run),
                            load_compile_time(base_conn, base_run), args.opts)
        if args.perf:
            report_perf(rep, load_perf(wt_conn, wt_run), load_perf(base_conn, base_run))
        if args.correctness:
            report_correctness(rep, load_correctness(wt_conn, wt_run),
                               load_correctness(base_conn, base_run))

        verdict = (f"VERDICT: {rep.improvements} improvement(s), "
                   f"{rep.regressions} regression(s)")
        if rep.improved_bytes is not None:
            net = rep.regressed_bytes - rep.improved_bytes
            verdict += (f"\n        code size at -{det.upper()}: "
                        f"improved by {rep.improved_bytes:,}, "
                        f"regressed by {rep.regressed_bytes:,} "
                        f"(net {net:+,d}) instructions")
        rep.add("=" * 78)
        rep.add(verdict)
        rep.add("=" * 78)
        print(rep.render())
        if base_conn is not wt_conn:
            base_conn.close()
    finally:
        wt_conn.close()
        if tmp_baseline:
            try:
                os.unlink(tmp_baseline)
            except OSError:
                pass

    if args.strict and rep.regressions:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
