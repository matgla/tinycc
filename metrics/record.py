#!/usr/bin/env python3
"""Record per-revision optimizer-regression metrics into the SQLite store.

For one git revision this collects four metric families and upserts them into
metrics.db (schema: metrics/schema.sql), keyed by (commit_sha, host):

  1. correctness  -- O1/O2 divergence per fuzz profile (reuses tests/fuzz/sweep_all.py)
  2. code size    -- instructions/function vs GCC   (reuses scripts/regression_disasm.py)
  3. compile time -- wall time of the code-size corpus compile (coarse, deterministic)
  4. perf         -- RP2350 hardware cycles          (reuses tests/benchmarks/run_benchmark.py)

Correctness + perf are measured against the tcc binary built IN PLACE at the repo
root (tests/fuzz/batch_sweep.py hardcodes armv8m-tcc and cannot be redirected), so
--rev must match the checked-out tree for those.  Code size + compile time CAN be
measured against any revision via --backfill (build_tcc_at_rev + TCC_OVERRIDE).

Idempotent: re-recording the same commit replaces its rows (no duplicates).

Examples
--------
  # record HEAD (built in place), fast prescan band, no hardware perf
  python3 metrics/record.py --db /var/lib/tcc-metrics/metrics.db \
      --rev HEAD --seed-lo 0 --seed-hi 2000 --mode prescan

  # nightly: full-recall triage band + per-function detail + RP2350 perf
  python3 metrics/record.py --db "$METRICS_DB" --rev HEAD \
      --seed-lo 0 --seed-hi 20000 --mode triage --codesize-detail \
      --perf-host 127.0.0.1 --perf-identity ~/.ssh/id_rp

  # seed the code-size / compile-time graphs from history (slow, run once)
  python3 metrics/record.py --db /var/lib/tcc-metrics/metrics.db --backfill 100
"""

import argparse
import csv
import io
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
SCHEMA_SQL = SCRIPT_DIR / "schema.sql"

# Make the reused modules importable.
sys.path.insert(0, str(REPO_ROOT / "tests" / "fuzz"))
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(REPO_ROOT / "tests" / "benchmarks"))


def warn(msg: str) -> None:
    print(f"[metrics] WARN: {msg}", file=sys.stderr, flush=True)


def info(msg: str) -> None:
    print(f"[metrics] {msg}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- git

def git_meta(rev: str) -> dict:
    """Resolve `rev` to full commit metadata via one `git show -s`."""
    fmt = "%H%n%P%n%an%n%ae%n%ct%n%s"
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "show", "-s", f"--format={fmt}", rev],
        capture_output=True, text=True, check=True).stdout.splitlines()
    sha, parents, author, email, cts, subject = (out + [""] * 6)[:6]
    return {
        "commit_sha": sha,
        "parent_sha": (parents.split() or [None])[0],
        "author": author,
        "author_email": email,
        "commit_ts": int(cts) if cts else 0,
        "subject": subject,
    }


def rev_list(n: int) -> list[str]:
    """First-parent commit shas, newest first, capped at n."""
    out = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "rev-list", "--first-parent",
         f"--max-count={n}", "mob"],
        capture_output=True, text=True, check=True).stdout.split()
    return out


# ------------------------------------------------------------------------- db

def connect(db_path: str) -> sqlite3.Connection:
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path, timeout=60)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.executescript(SCHEMA_SQL.read_text())   # self-initializing / idempotent
    return conn


def upsert_run(conn: sqlite3.Connection, meta: dict, host: str, branch: str,
               trigger: str, seed_lo, seed_hi, mode, wall_seconds=None,
               tcc_build_ok=1) -> int:
    """Insert-or-replace the run row; return its run_id and wipe its child rows
    so the caller can re-insert fresh metrics (idempotent replace)."""
    conn.execute(
        """INSERT INTO runs(commit_sha, parent_sha, branch, author, author_email,
                            subject, commit_ts, run_ts, host, tcc_build_ok,
                            wall_seconds, seed_lo, seed_hi, mode, trigger)
           VALUES(:commit_sha,:parent_sha,:branch,:author,:author_email,:subject,
                  :commit_ts,:run_ts,:host,:tcc_build_ok,:wall_seconds,
                  :seed_lo,:seed_hi,:mode,:trigger)
           ON CONFLICT(commit_sha, host) DO UPDATE SET
                run_ts=excluded.run_ts, parent_sha=excluded.parent_sha,
                subject=excluded.subject, commit_ts=excluded.commit_ts,
                tcc_build_ok=excluded.tcc_build_ok, wall_seconds=excluded.wall_seconds,
                seed_lo=excluded.seed_lo, seed_hi=excluded.seed_hi,
                mode=excluded.mode, trigger=excluded.trigger""",
        {**meta, "branch": branch, "run_ts": int(time.time()), "host": host,
         "tcc_build_ok": tcc_build_ok, "wall_seconds": wall_seconds,
         "seed_lo": seed_lo, "seed_hi": seed_hi, "mode": mode, "trigger": trigger})
    run_id = conn.execute(
        "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
        (meta["commit_sha"], host)).fetchone()[0]
    for tbl in ("correctness", "correctness_seed", "codesize_rollup",
                "codesize_func", "compile_time", "perf"):
        conn.execute(f"DELETE FROM {tbl} WHERE run_id=?", (run_id,))
    return run_id


# ---------------------------------------------------------------- correctness

def record_correctness(conn, run_id, lo, hi, mode, jobs) -> None:
    """Sweep every profile and store per-(profile,oracle) divergence counts.
    Mirrors sweep_all.run_profile's oracle selection so the numbers match a
    manual `sweep_all.py` run exactly."""
    import sweep_all as SW

    def emit(line: str) -> None:
        print("    " + line, file=sys.stderr, flush=True)

    for name, oracle, _blurb in SW.PROFILES:
        merge_gcc = (mode != "triage" and oracle in ("vsgcc", "both"))
        ol, vg, gccbad, err = [], [], [], ""
        if merge_gcc:
            ol, vg, gccbad, err = SW.run_olevels_prescan_with_gcc(name, lo, hi, jobs, emit)
        else:
            if mode == "triage":
                ol, err = SW.run_olevels_triage_sweep(name, lo, hi, jobs, emit)
            else:
                ol, err = SW.run_olevels_prescan(name, lo, hi, jobs, emit)
            if not err and oracle in ("vsgcc", "both"):   # triage vs-gcc pass
                vg, verr = SW.run_vsgcc(name, lo, hi, jobs, emit)
                if verr:
                    warn(f"{name} vs-gcc: {verr}")

        if err:
            # A sweep error (e.g. QEMU/newlib not prepared) means "not measured":
            # skip the row so the graph shows a gap rather than a false 0.
            warn(f"{name} olevels: {err} -- skipping row")
            continue

        low = 1 if (mode != "triage" and name in SW.LOW_RECALL_ON_PRESCAN) else 0
        conn.execute(
            """INSERT INTO correctness(run_id,profile,oracle,divergent_count,
                    gccbad_count,seed_lo,seed_hi,mode,low_recall)
               VALUES(?,?,'olevels',?,0,?,?,?,?)""",
            (run_id, name, len(ol), lo, hi, mode, low))
        conn.executemany(
            "INSERT OR IGNORE INTO correctness_seed VALUES(?,?,'olevels',?)",
            [(run_id, name, s) for s in ol])
        if oracle in ("vsgcc", "both"):
            conn.execute(
                """INSERT INTO correctness(run_id,profile,oracle,divergent_count,
                        gccbad_count,seed_lo,seed_hi,mode,low_recall)
                   VALUES(?,?,'vsgcc',?,?,?,?,?,?)""",
                (run_id, name, len(vg), len(gccbad), lo, hi, mode, low))
            conn.executemany(
                "INSERT OR IGNORE INTO correctness_seed VALUES(?,?,'vsgcc',?)",
                [(run_id, name, s) for s in vg])
        info(f"{name}: olevels={len(ol)} vsgcc={len(vg)} gccbad={len(gccbad)}")


# ------------------------------------------------------------------- code size

def _parse_codesize_csv(csv_text: str):
    """Yield (suite, test, function, tcc_n, gcc_n) from run_csv_mode output.
    Column order is fixed (suite,test,function,tcc_O2,gcc_<opt>,ratio); we parse
    positionally so the dynamic gcc column name doesn't matter."""
    for row in csv.reader(io.StringIO(csv_text)):
        if len(row) < 6 or row[0] == "suite":
            continue
        try:
            yield row[0], row[1], row[2], int(row[3]), int(row[4])
        except ValueError:
            continue


def record_codesize(conn, run_id, jobs, detail: bool, tcc_override=None) -> float:
    """Record code size (rollup + optional per-function detail) and return the
    corpus compile wall-time (the coarse compile-time proxy)."""
    from regression_disasm import run_csv_mode
    t0 = time.monotonic()
    csv_text = run_csv_mode("-O2", None, "all", jobs, tcc_override=tcc_override)
    elapsed = time.monotonic() - t0

    rollup = {}   # suite -> [func_count, tcc, gcc]
    tot = [0, 0, 0]
    detail_rows = []
    for suite, test, func, tcc_n, gcc_n in _parse_codesize_csv(csv_text):
        r = rollup.setdefault(suite, [0, 0, 0])
        r[0] += 1; r[1] += tcc_n; r[2] += gcc_n
        tot[0] += 1; tot[1] += tcc_n; tot[2] += gcc_n
        if detail:
            ratio = (tcc_n / gcc_n) if gcc_n > 0 else 0.0
            detail_rows.append((run_id, suite, test, func, tcc_n, gcc_n, ratio))

    for suite, (fc, tcc_n, gcc_n) in list(rollup.items()) + [("<total>", tot)]:
        ratio = (tcc_n / gcc_n) if gcc_n > 0 else 0.0
        conn.execute(
            "INSERT OR REPLACE INTO codesize_rollup VALUES(?,?,?,?,?,?)",
            (run_id, suite, fc, tcc_n, gcc_n, ratio))
    if detail_rows:
        conn.executemany(
            "INSERT OR REPLACE INTO codesize_func VALUES(?,?,?,?,?,?,?)", detail_rows)
    info(f"codesize: {tot[0]} funcs, tcc={tot[1]} gcc={tot[2]} "
         f"ratio={tot[1]/tot[2]:.3f} in {elapsed:.0f}s"
         if tot[2] else f"codesize: {tot[0]} funcs")
    return elapsed


def record_compile_time(conn, run_id, corpus_secs, n_units) -> None:
    conn.execute(
        "INSERT OR REPLACE INTO compile_time VALUES(?,?,?,?)",
        (run_id, "codesize_corpus_o2", corpus_secs, n_units))


# ------------------------------------------------------------------------ perf

def record_perf(conn, run_id, perf_host, perf_identity, scratch: Path) -> None:
    """Run the RP2350 benchmark over SSH and store cycles/build-size.  Any
    failure (no host, SSH down, no board) is non-fatal: perf is simply absent
    for this commit and the dashboard shows a gap."""
    if not perf_host:
        return
    json_out = scratch / "perf.json"
    cmd = [sys.executable, str(REPO_ROOT / "tests" / "benchmarks" / "run_benchmark.py"),
           perf_host, "--opt-level", "all", "--save-data", str(json_out)]
    if perf_identity:
        cmd += ["--identity", perf_identity]
    rc = subprocess.run(cmd, cwd=str(REPO_ROOT)).returncode
    if rc != 0 or not json_out.exists():
        warn("perf skipped: RP2350 benchmark did not produce data")
        return
    from run_benchmark import load_results_json
    results = load_results_json(str(json_out))
    n = 0
    for key, res in results.items():
        # key like 'tcc_o2' / 'gcc_o0'; res.compiler is 'TCC'/'GCC'
        opt = key.split("_", 1)[1] if "_" in key else "o?"
        bs = res.build_size or {}
        for b in res.benchmarks:
            conn.execute(
                "INSERT OR REPLACE INTO perf VALUES(?,?,?,?,?,?,?,?,?)",
                (run_id, b.name, res.compiler, opt, b.cycles_per_iter,
                 bs.get("text"), bs.get("data"), bs.get("bss"), b.verify))
            n += 1
    info(f"perf: {n} benchmark rows from {len(results)} builds")


# ---------------------------------------------------------------------- record

def record_one(conn, meta, host, branch, trigger, args, tcc_override=None,
               do_correctness=True, do_perf=True) -> None:
    t0 = time.monotonic()
    run_id = upsert_run(conn, meta, host, branch, trigger,
                        args.seed_lo, args.seed_hi, args.mode)
    if do_correctness:
        record_correctness(conn, run_id, args.seed_lo, args.seed_hi, args.mode, args.jobs)
    corpus_secs = record_codesize(conn, run_id, args.jobs, args.codesize_detail, tcc_override)
    n_units = conn.execute(
        "SELECT func_count FROM codesize_rollup WHERE run_id=? AND suite='<total>'",
        (run_id,)).fetchone()
    record_compile_time(conn, run_id, corpus_secs, n_units[0] if n_units else None)
    if do_perf and args.perf_host:
        record_perf(conn, run_id, args.perf_host, args.perf_identity,
                    Path(args.scratch or "."))
    conn.execute("UPDATE runs SET wall_seconds=? WHERE run_id=?",
                 (time.monotonic() - t0, run_id))
    conn.commit()
    info(f"recorded {meta['commit_sha'][:12]} ({meta['subject'][:50]}) "
         f"in {time.monotonic()-t0:.0f}s")


def do_backfill(conn, host, branch, args) -> None:
    """Seed code-size + compile-time history across past revisions.  Correctness
    and perf are NOT backfillable (batch_sweep is in-place-only; perf needs the
    board per rev), so those are skipped -- consistent with track-first."""
    revs = rev_list(args.backfill)
    info(f"backfill: {len(revs)} revisions (codesize + compile-time only)")
    for i, rev in enumerate(revs, 1):
        try:
            meta = git_meta(rev)
        except subprocess.CalledProcessError:
            warn(f"skip {rev}: bad rev"); continue
        if conn.execute("SELECT 1 FROM codesize_rollup r JOIN runs u USING(run_id) "
                        "WHERE u.commit_sha=? AND u.host=?",
                        (meta["commit_sha"], host)).fetchone():
            info(f"[{i}/{len(revs)}] {rev[:12]} already has codesize -- skip")
            continue
        try:
            from regression_disasm import build_tcc_at_rev
            tcc_path, build_dir = build_tcc_at_rev(rev, args.jobs)
        except SystemExit:
            warn(f"[{i}/{len(revs)}] {rev[:12]} build failed -- skip"); continue
        try:
            info(f"[{i}/{len(revs)}] recording {rev[:12]} ...")
            record_one(conn, meta, host, branch, "backfill", args,
                       tcc_override=tcc_path, do_correctness=False, do_perf=False)
        finally:
            shutil.rmtree(build_dir, ignore_errors=True)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", required=True, help="path to metrics.db")
    p.add_argument("--rev", default="HEAD", help="git revision to record (default HEAD)")
    p.add_argument("--seed-lo", type=int, default=0)
    p.add_argument("--seed-hi", type=int, default=2000)
    p.add_argument("--mode", choices=["prescan", "triage"], default="prescan")
    p.add_argument("--codesize-detail", action="store_true",
                   help="also store per-function code size (large; nightly)")
    p.add_argument("--perf-host", help="SSH host for the RP2350 benchmark (omit to skip perf)")
    p.add_argument("--perf-identity", help="SSH identity file for --perf-host")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    p.add_argument("--host", default=os.environ.get("METRICS_HOST") or socket.gethostname())
    p.add_argument("--branch", default="mob")
    p.add_argument("--trigger", default="manual")
    p.add_argument("--scratch", help="scratch dir for perf JSON (default cwd)")
    p.add_argument("--backfill", type=int, metavar="N",
                   help="record codesize+compile-time for the last N first-parent commits")
    p.add_argument("--no-correctness", action="store_true",
                   help="skip the fuzz sweep (codesize/compile-time only)")
    args = p.parse_args(argv)

    conn = connect(args.db)
    try:
        if args.backfill:
            do_backfill(conn, args.host, args.branch, args)
        else:
            meta = git_meta(args.rev)
            record_one(conn, meta, args.host, args.branch, args.trigger, args,
                       do_correctness=not args.no_correctness)
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
