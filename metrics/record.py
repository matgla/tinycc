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
      --detail-db /var/lib/tcc-metrics/codesize-detail.db \
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


def die(msg: str) -> None:
    print(f"[metrics] FATAL: {msg}", file=sys.stderr, flush=True)
    sys.exit(1)


# --------------------------------------------------------------------------- git

def _run_git(args: list[str]) -> str:
    """Run a git command in REPO_ROOT, surfacing stderr on failure.

    subprocess.CalledProcessError's default str() only includes the exit
    code, not stderr -- that swallowed the actual git error the last time
    this failed in CI (dubious-ownership in a container job), leaving just
    an unhelpful "returned non-zero exit status 128" traceback.
    """
    proc = subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        die(f"git {' '.join(args)} failed (exit {proc.returncode}): "
            f"{proc.stderr.strip()}")
    return proc.stdout


def git_meta(rev: str) -> dict:
    """Resolve `rev` to full commit metadata via one `git show -s`."""
    fmt = "%H%n%P%n%an%n%ae%n%ct%n%s"
    out = _run_git(["show", "-s", f"--format={fmt}", rev]).splitlines()
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
    return _run_git(
        ["rev-list", "--first-parent", f"--max-count={n}", "mob"]).split()


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


def ensure_codesize_corpus() -> None:
    """Fetch the two submodule-backed inputs the code-size corpus compiles need,
    before measuring -- both idempotent and fast when already satisfied.

    The CI metrics job checks out with `submodules: recursive`, but that is not
    enough for a full corpus:

      * newlib -- `tests/ir_tests/libc_includes/newlib` is a committed symlink
        into the newlib submodule's header dir. If that submodule is missing the
        symlink dangles and EVERY corpus compile fails "stdio.h not found",
        which (before run_csv_mode learned to check its child's exit code) was
        silently recorded as "0 funcs" with exit 0 -- the DB and Grafana showed
        zeros for every run. Recursive checkout normally provides it, but a
        partial/ manual checkout may not, so init it on demand.

      * GCC c-torture -- the gcctestsuite submodule is pinned `update = none`
        (see .gitmodules), so recursive checkout SKIPS it and the corpus would
        otherwise be just the small in-tree ir/float/bug suites. Sparse-fetch
        the ~16 MB of torture tests so code size is measured over them too.
    """
    newlib_hdr = REPO_ROOT / "tests/ir_tests/libc_includes/newlib/stdio.h"
    if not newlib_hdr.exists():
        info("codesize prep: initializing newlib submodule (libc headers) ...")
        rc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "submodule", "update", "--init",
             "--depth", "1",
             "tests/ir_tests/qemu/mps2-an505/libs/newlib"]).returncode
        if rc != 0 or not newlib_hdr.exists():
            die(f"codesize prep: newlib submodule init failed (exit {rc}); "
                "corpus compiles would all fail 'stdio.h not found'")

    info("codesize prep: fetching GCC c-torture tests (sparse; idempotent) ...")
    rc = subprocess.run(
        ["bash", str(REPO_ROOT / "tests/gcctestsuite/download_gcc_tests.sh")]).returncode
    if rc != 0:
        die(f"codesize prep: download_gcc_tests.sh failed (exit {rc})")


# TCC (and GCC) optimization levels the code-size corpus is measured at. Both
# compilers are run at the SAME level for each series, so the ratio compares
# like-for-like. Keep 'o2' last so the info() summary / gate baseline reflect it.
CODESIZE_OPTS = [("o0", "-O0"), ("o1", "-O1"), ("o2", "-O2")]


def record_codesize(conn, run_id, jobs, detail: bool, tcc_override=None) -> dict:
    """Record code size (rollup + optional per-function detail) at each opt level
    and return {opt: corpus_compile_wall_seconds} (the coarse compile-time proxy,
    one per level)."""
    from regression_disasm import run_csv_mode
    ensure_codesize_corpus()
    elapsed = {}
    for opt, flag in CODESIZE_OPTS:
        t0 = time.monotonic()
        # Compile BOTH tcc and gcc at this level (tcc_opt=flag, gcc_opt=flag).
        csv_text = run_csv_mode(flag, None, "all", jobs,
                                tcc_override=tcc_override, tcc_opt=flag)
        elapsed[opt] = time.monotonic() - t0

        rollup = {}   # suite -> [func_count, tcc, gcc]
        tot = [0, 0, 0]
        detail_rows = []
        for suite, test, func, tcc_n, gcc_n in _parse_codesize_csv(csv_text):
            r = rollup.setdefault(suite, [0, 0, 0])
            r[0] += 1; r[1] += tcc_n; r[2] += gcc_n
            tot[0] += 1; tot[1] += tcc_n; tot[2] += gcc_n
            if detail:
                ratio = (tcc_n / gcc_n) if gcc_n > 0 else 0.0
                detail_rows.append((run_id, suite, test, func, opt, tcc_n, gcc_n, ratio))

        for suite, (fc, tcc_n, gcc_n) in list(rollup.items()) + [("<total>", tot)]:
            ratio = (tcc_n / gcc_n) if gcc_n > 0 else 0.0
            conn.execute(
                "INSERT OR REPLACE INTO codesize_rollup VALUES(?,?,?,?,?,?,?)",
                (run_id, suite, opt, fc, tcc_n, gcc_n, ratio))
        if detail_rows:
            conn.executemany(
                "INSERT OR REPLACE INTO codesize_func VALUES(?,?,?,?,?,?,?,?)", detail_rows)
        info(f"codesize[{opt}]: {tot[0]} funcs, tcc={tot[1]} gcc={tot[2]} "
             f"ratio={tot[1]/tot[2]:.3f} in {elapsed[opt]:.0f}s"
             if tot[2] else f"codesize[{opt}]: {tot[0]} funcs in {elapsed[opt]:.0f}s")
    return elapsed


def record_compile_time(conn, run_id, scope, corpus_secs, n_units) -> None:
    conn.execute(
        "INSERT OR REPLACE INTO compile_time VALUES(?,?,?,?)",
        (run_id, scope, corpus_secs, n_units))


def import_codesize(conn, run_id, src_db_path, commit_sha) -> bool:
    """Copy summary codesize/compile_time rows recorded for `commit_sha` in
    another metrics db (e.g. a cloud-runner scratch db from a faster build host)
    into `run_id`, instead of recomputing them locally.

    Deliberately do not import codesize_func into the Grafana-facing metrics DB:
    that table is thousands of rows per run and makes dashboard variables/panels
    unusably noisy.  Use --detail-db to store those rows in a separate database
    for the standalone file-by-file viewer.
    """
    conn.execute("ATTACH DATABASE ? AS src", (src_db_path,))
    try:
        src_run = conn.execute(
            "SELECT run_id FROM src.runs WHERE commit_sha=? ORDER BY run_ts DESC LIMIT 1",
            (commit_sha,)).fetchone()
        found = src_run is not None
        if found:
            src_run_id = src_run[0]
            conn.execute(
                """INSERT OR REPLACE INTO codesize_rollup
                   SELECT ?, suite, opt, func_count, tcc_size, gcc_size, ratio
                   FROM src.codesize_rollup WHERE run_id=?""", (run_id, src_run_id))
            conn.execute(
                """INSERT OR REPLACE INTO compile_time
                   SELECT ?, scope, seconds, n_units
                   FROM src.compile_time WHERE run_id=?""", (run_id, src_run_id))
        else:
            warn(f"no codesize data for {commit_sha[:12]} in {src_db_path} -- skipping import")
        conn.commit()   # DETACH requires no pending transaction on `conn`, success or not
        if not found:
            return False
        n = conn.execute(
            "SELECT COUNT(*) FROM codesize_rollup WHERE run_id=?", (run_id,)).fetchone()[0]
        info(f"imported codesize rollup/compile_time from {src_db_path} "
             f"({n} codesize rows)")
        return n > 0
    finally:
        conn.execute("DETACH DATABASE src")


def sync_codesize_detail(src_db_path: str, detail_db_path: str, commit_sha: str,
                         host: str, branch: str, keep: int | None = None) -> bool:
    """Copy per-function codesize rows into a separate detail database.

    The detail DB uses the same schema as metrics.db but is not mounted by
    Grafana.  It is intended for metrics/codesize_detail_server.py, where the
    file/function cardinality is useful instead of toxic to dashboard UX.
    """
    conn = connect(detail_db_path)
    conn.execute("ATTACH DATABASE ? AS src", (src_db_path,))
    try:
        cols = [
            "commit_sha", "parent_sha", "author", "author_email", "subject",
            "commit_ts", "run_ts", "tcc_build_ok", "wall_seconds", "seed_lo",
            "seed_hi", "mode", "trigger", "notes",
        ]
        select_cols = ", ".join(cols)
        src_run = conn.execute(
            f"""SELECT run_id, {select_cols}, host, branch
                FROM src.runs
                WHERE commit_sha=? AND host=?
                ORDER BY run_ts DESC LIMIT 1""",
            (commit_sha, host)).fetchone()
        if src_run is None:
            src_run = conn.execute(
                f"""SELECT run_id, {select_cols}, host, branch
                    FROM src.runs
                    WHERE commit_sha=?
                    ORDER BY run_ts DESC LIMIT 1""",
                (commit_sha,)).fetchone()
        if src_run is None:
            warn(f"no run metadata for {commit_sha[:12]} in {src_db_path} "
                 f"-- skipping detail sync")
            conn.commit()
            return False

        src_run_id = src_run[0]
        values = dict(zip(cols, src_run[1:1 + len(cols)]))
        values["host"] = host or src_run[1 + len(cols)]
        values["branch"] = branch or src_run[2 + len(cols)]
        conn.execute(
            """INSERT INTO runs(commit_sha,parent_sha,branch,author,author_email,
                                subject,commit_ts,run_ts,host,tcc_build_ok,
                                wall_seconds,seed_lo,seed_hi,mode,trigger,notes)
               VALUES(:commit_sha,:parent_sha,:branch,:author,:author_email,
                      :subject,:commit_ts,:run_ts,:host,:tcc_build_ok,
                      :wall_seconds,:seed_lo,:seed_hi,:mode,:trigger,:notes)
               ON CONFLICT(commit_sha, host) DO UPDATE SET
                    parent_sha=excluded.parent_sha,
                    branch=excluded.branch,
                    author=excluded.author,
                    author_email=excluded.author_email,
                    subject=excluded.subject,
                    commit_ts=excluded.commit_ts,
                    run_ts=excluded.run_ts,
                    tcc_build_ok=excluded.tcc_build_ok,
                    wall_seconds=excluded.wall_seconds,
                    seed_lo=excluded.seed_lo,
                    seed_hi=excluded.seed_hi,
                    mode=excluded.mode,
                    trigger=excluded.trigger,
                    notes=excluded.notes""",
            values)
        detail_run_id = conn.execute(
            "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
            (values["commit_sha"], values["host"])).fetchone()[0]

        for tbl in ("codesize_rollup", "codesize_func", "compile_time"):
            conn.execute(f"DELETE FROM {tbl} WHERE run_id=?", (detail_run_id,))
        conn.execute(
            """INSERT OR REPLACE INTO codesize_rollup
               SELECT ?, suite, opt, func_count, tcc_size, gcc_size, ratio
               FROM src.codesize_rollup WHERE run_id=?""",
            (detail_run_id, src_run_id))
        conn.execute(
            """INSERT OR REPLACE INTO codesize_func
               SELECT ?, suite, test, function, opt, tcc_size, gcc_size, ratio
               FROM src.codesize_func WHERE run_id=?""",
            (detail_run_id, src_run_id))
        conn.execute(
            """INSERT OR REPLACE INTO compile_time
               SELECT ?, scope, seconds, n_units
               FROM src.compile_time WHERE run_id=?""",
            (detail_run_id, src_run_id))
        n = conn.execute(
            "SELECT COUNT(*) FROM codesize_func WHERE run_id=?",
            (detail_run_id,)).fetchone()[0]
        conn.commit()
        if keep is not None:
            prune_old_runs(conn, values["branch"], values["host"], keep)
        info(f"synced {n} per-function codesize rows to {detail_db_path}")
        return n > 0
    finally:
        conn.execute("DETACH DATABASE src")
        conn.close()


# ------------------------------------------------------------------------ perf

def record_perf(conn, run_id, perf_host, perf_identity, scratch: Path,
                strict: bool = False) -> None:
    """Run the RP2350 benchmark over SSH and store cycles/build-size.

    By default any failure (no host, SSH down, no board) is non-fatal: perf is
    simply absent for this commit and the dashboard shows a gap.

    With strict=True (the CI perf job, which runs on the dedicated board host)
    an operational benchmark failure -- run_benchmark.py exiting non-zero,
    producing no data, or an on-hardware verify FAIL -- is fatal, so the job
    goes red instead of silently green.  This does NOT gate on perf *numbers*
    (slower cycles); regression gating is metrics/gate.py's job."""
    if not perf_host:
        return
    json_out = scratch / "perf.json"
    # -u so the child's progress (Building.../cmake/make/flash/serial) streams to
    # the CI log in real time.  Without it the child's stdout is a pipe, CPython
    # block-buffers its print()s, and the whole perf phase looks hung for minutes.
    cmd = [sys.executable, "-u",
           str(REPO_ROOT / "tests" / "benchmarks" / "run_benchmark.py"),
           perf_host, "--opt-level", "all", "--save-data", str(json_out)]
    if perf_identity:
        cmd += ["--identity", perf_identity]
    info(f"perf: launching RP2350 benchmark on {perf_host} (6 builds; may take several minutes) ...")
    rc = subprocess.run(cmd, cwd=str(REPO_ROOT)).returncode
    if rc != 0 or not json_out.exists():
        msg = (f"RP2350 benchmark failed (exit {rc}, data "
               f"{'present' if json_out.exists() else 'missing'})")
        if strict:
            die(f"perf: {msg}")
        warn(f"perf skipped: {msg}")
        return
    from run_benchmark import load_results_json
    results = load_results_json(str(json_out))
    # Defense in depth: run_benchmark.py now exits non-zero on a verify FAIL, so
    # the rc check above normally catches it; re-check the loaded data anyway so
    # a future regression that swallows the exit code can't slip a bad row past
    # strict mode.  (Dies before commit -> no perf rows this run, dashboard gap.)
    if strict:
        bad = [f"{key}:{b.name}" for key, res in results.items()
               for b in res.benchmarks if b.verify == "FAIL"]
        if not results or bad:
            die("perf: on-hardware verify FAIL: "
                + (", ".join(bad) or "no benchmark results produced"))
    n = 0
    for key, res in results.items():
        # key like 'tcc_o2' / 'gcc_o0'; older benchmark JSON labels compiler
        # as 'TCC-O2'/'GCC-O2', but the metrics schema stores opt separately.
        opt = key.split("_", 1)[1] if "_" in key else "o?"
        compiler = (res.compiler or key).split("-", 1)[0].upper()
        bs = res.build_size or {}
        for b in res.benchmarks:
            conn.execute(
                "INSERT OR REPLACE INTO perf VALUES(?,?,?,?,?,?,?,?,?)",
                (run_id, b.name, compiler, opt, b.cycles_per_iter,
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
    if args.import_codesize_from:
        import_codesize(conn, run_id, args.import_codesize_from, meta["commit_sha"])
        if args.detail_db:
            sync_codesize_detail(args.import_codesize_from, args.detail_db,
                                 meta["commit_sha"], host, branch,
                                 args.detail_keep)
    else:
        corpus_secs = record_codesize(conn, run_id, args.jobs, args.codesize_detail, tcc_override)
        for opt, secs in corpus_secs.items():
            n_units = conn.execute(
                "SELECT func_count FROM codesize_rollup "
                "WHERE run_id=? AND suite='<total>' AND opt=?",
                (run_id, opt)).fetchone()
            record_compile_time(conn, run_id, f"codesize_corpus_{opt}", secs,
                                 n_units[0] if n_units else None)
        if args.detail_db:
            if args.codesize_detail:
                conn.commit()
                sync_codesize_detail(args.db, args.detail_db, meta["commit_sha"],
                                     host, branch, args.detail_keep)
                conn.execute("DELETE FROM codesize_func WHERE run_id=?", (run_id,))
                conn.commit()
            else:
                warn("--detail-db requested without --codesize-detail; no "
                     "per-function rows were recorded to sync")
    if do_perf and args.perf_host:
        record_perf(conn, run_id, args.perf_host, args.perf_identity,
                    Path(args.scratch or "."), strict=args.perf_strict)
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


# --------------------------------------------------------------------- retention

def prune_old_runs(conn, branch, host, keep) -> None:
    """Retention: keep only the newest `keep` runs for (branch, host), ranked by
    commit_ts, and delete the rest.  Child rows (correctness/codesize_*/perf/...)
    are removed by ON DELETE CASCADE, so one DELETE per run cleans everything.

    The DB file itself does not shrink -- SQLite reuses the freed pages for
    future inserts, so with a fixed `keep` the file size plateaus at roughly the
    high-water mark rather than growing unbounded.  No VACUUM in the hot path:
    it would rewrite the whole file on every CI run for no net win.
    """
    if keep < 1:
        die(f"--keep must be >= 1 (got {keep}); refusing to delete every run")
    victims = conn.execute(
        """SELECT run_id FROM runs WHERE branch=? AND host=?
           ORDER BY commit_ts DESC, run_ts DESC
           LIMIT -1 OFFSET ?""",
        (branch, host, keep)).fetchall()
    if not victims:
        info(f"retention: nothing to prune (<= {keep} runs for "
             f"branch={branch} host={host})")
        return
    conn.executemany("DELETE FROM runs WHERE run_id=?",
                     [(v[0],) for v in victims])
    conn.commit()
    info(f"retention: pruned {len(victims)} run(s) beyond newest {keep} "
         f"for branch={branch} host={host}")


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
    p.add_argument("--perf-strict", action="store_true",
                   help="treat an operational perf-run failure (run_benchmark.py "
                        "exiting non-zero / no data / on-hardware verify FAIL) as "
                        "fatal so CI goes red. Default: perf failure is non-fatal "
                        "(dashboard gap). Does not gate on perf numbers -- that is "
                        "metrics/gate.py's job.")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    p.add_argument("--host", default=os.environ.get("METRICS_HOST") or socket.gethostname())
    p.add_argument("--branch", default="mob")
    p.add_argument("--trigger", default="manual")
    p.add_argument("--scratch", help="scratch dir for perf JSON (default cwd)")
    p.add_argument("--backfill", type=int, metavar="N",
                   help="record codesize+compile-time for the last N first-parent commits")
    p.add_argument("--no-correctness", action="store_true",
                   help="skip the fuzz sweep (codesize/compile-time only)")
    p.add_argument("--import-codesize-from", metavar="DB_PATH",
                   help="skip local codesize/compile-time measurement; copy those rows "
                        "from another metrics.db recorded for the same commit (e.g. a "
                        "cloud-runner scratch db)")
    p.add_argument("--detail-db", metavar="DB_PATH",
                   help="copy per-function codesize rows into this separate SQLite db "
                        "for metrics/codesize_detail_server.py. Detail rows are not "
                        "imported into the Grafana-facing --db.")
    p.add_argument("--detail-keep", type=int, metavar="N",
                   help="retention for --detail-db: keep only the newest N runs for "
                        "this --branch/--host")
    p.add_argument("--keep", type=int, metavar="N",
                   help="retention: after recording, keep only the newest N runs for "
                        "this --branch/--host (by commit_ts) and delete older ones "
                        "(child rows cascade). Bounds the Grafana metrics DB.")
    args = p.parse_args(argv)

    conn = connect(args.db)
    try:
        if args.backfill:
            do_backfill(conn, args.host, args.branch, args)
        else:
            meta = git_meta(args.rev)
            record_one(conn, meta, args.host, args.branch, args.trigger, args,
                       do_correctness=not args.no_correctness)
        if args.keep is not None:
            prune_old_runs(conn, args.branch, args.host, args.keep)
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
