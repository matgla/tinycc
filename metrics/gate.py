#!/usr/bin/env python3
"""Track-first-then-block regression gate over metrics.db.

Compares a recorded run against its parent commit's run (same host) and
reports:
  - correctness regressions: a divergent seed that appears now but did not
    appear for the parent AND is not in the `accepted_divergence` allowlist.
  - codesize regressions: the `<total>` tcc/gcc instruction ratio grew by more
    than --codesize-tolerance-pct.

compile_time and perf are reported for visibility only -- they are noisy by
nature (hardware, scheduling) and are not part of the automated pass/fail
signal; judge them by eye on the dashboard during a migration step (see
docs/metrics_dashboard.md).

Modes:
  default   -- print the report, always exit 0 (safe to run before the
               baseline is green; this is the "track" half of track-first).
  --strict  -- exit 1 if any correctness or codesize regression survives the
               allowlist (the "block" half; flip on via METRICS_GATE_ENABLED
               in .github/workflows/metrics.yml once the baseline is clean).

Managing the allowlist (no raw SQL needed for the common case):
  python3 metrics/gate.py --db metrics.db \
      --accept ptr:olevels:12345 --reason "pre-existing, see docs/bugs.md"
"""

import argparse
import sqlite3
import sys
import time


def warn(msg: str) -> None:
    print(f"[gate] WARN: {msg}", file=sys.stderr, flush=True)


def resolve_run(conn: sqlite3.Connection, rev: str, host: str) -> sqlite3.Row:
    """rev may be a full/short sha or 'HEAD'-resolved sha the caller already
    turned into a real sha; we match by prefix so either works."""
    row = conn.execute(
        "SELECT * FROM runs WHERE host=? AND commit_sha LIKE ? ORDER BY run_ts DESC LIMIT 1",
        (host, rev + "%")).fetchone()
    if row is None:
        sys.exit(f"[gate] no recorded run for rev={rev!r} host={host!r} -- "
                 f"run metrics/record.py first")
    return row


def accepted_seeds(conn, profile, oracle) -> set:
    return {r[0] for r in conn.execute(
        "SELECT seed FROM accepted_divergence WHERE profile=? AND oracle=? AND seed IS NOT NULL",
        (profile, oracle))}


def accepted_baseline(conn, profile, oracle):
    row = conn.execute(
        "SELECT baseline FROM accepted_divergence WHERE profile=? AND oracle=? AND seed IS NULL",
        (profile, oracle)).fetchone()
    return row[0] if row else None


def check_correctness(conn, run_id, parent_id) -> list:
    """Return a list of (profile, oracle, new_seeds) regressions."""
    regressions = []
    for profile, oracle, count in conn.execute(
            "SELECT profile, oracle, divergent_count FROM correctness WHERE run_id=?",
            (run_id,)):
        cur_seeds = {r[0] for r in conn.execute(
            "SELECT seed FROM correctness_seed WHERE run_id=? AND profile=? AND oracle=?",
            (run_id, profile, oracle))}
        if parent_id is not None:
            parent_seeds = {r[0] for r in conn.execute(
                "SELECT seed FROM correctness_seed WHERE run_id=? AND profile=? AND oracle=?",
                (parent_id, profile, oracle))}
        else:
            warn(f"{profile}/{oracle}: no parent run recorded -- can't diff, "
                 f"treating all {count} seed(s) as pre-existing this time")
            parent_seeds = cur_seeds   # first-ever run: nothing "new"

        baseline = accepted_baseline(conn, profile, oracle)
        allow = accepted_seeds(conn, profile, oracle) | parent_seeds
        new_seeds = cur_seeds - allow
        if new_seeds and baseline is not None and len(cur_seeds) <= baseline:
            new_seeds = set()   # covered by a count-based allowlist entry
        if new_seeds:
            regressions.append((profile, oracle, sorted(new_seeds)))
    return regressions


def check_codesize(conn, run_id, parent_id, tolerance_pct: float):
    """Return (cur_ratio, parent_ratio, pct_delta) if the total ratio grew by
    more than tolerance_pct, else None."""
    cur = conn.execute(
        "SELECT ratio FROM codesize_rollup WHERE run_id=? AND suite='<total>'",
        (run_id,)).fetchone()
    if not cur or parent_id is None:
        return None
    parent = conn.execute(
        "SELECT ratio FROM codesize_rollup WHERE run_id=? AND suite='<total>'",
        (parent_id,)).fetchone()
    if not parent or parent[0] <= 0:
        return None
    pct = (cur[0] - parent[0]) / parent[0] * 100.0
    if pct > tolerance_pct:
        return cur[0], parent[0], pct
    return None


def print_visibility(conn, run_id, parent_id) -> None:
    """compile_time / perf: informational only, never gates."""
    ct = conn.execute(
        "SELECT seconds FROM compile_time WHERE run_id=? AND scope='codesize_corpus_o2'",
        (run_id,)).fetchone()
    if ct and parent_id is not None:
        pct_row = conn.execute(
            "SELECT seconds FROM compile_time WHERE run_id=? AND scope='codesize_corpus_o2'",
            (parent_id,)).fetchone()
        if pct_row and pct_row[0] > 0:
            pct = (ct[0] - pct_row[0]) / pct_row[0] * 100.0
            print(f"[gate] compile time: {ct[0]:.1f}s ({pct:+.1f}% vs parent) -- informational")
    for row in conn.execute(
            "SELECT benchmark, compiler, opt_level, cycles_per_iter FROM perf WHERE run_id=?",
            (run_id,)):
        print(f"[gate] perf {row[0]} {row[1]}/{row[2]}: {row[3]:.0f} cycles/iter -- informational")


def do_accept(conn, spec: str, reason: str) -> None:
    parts = spec.split(":")
    if len(parts) != 3:
        sys.exit("--accept expects PROFILE:ORACLE:SEED")
    profile, oracle, seed = parts
    conn.execute(
        "INSERT OR REPLACE INTO accepted_divergence(profile,oracle,seed,baseline,reason,added_by,added_ts) "
        "VALUES(?,?,?,NULL,?,?,?)",
        (profile, oracle, int(seed), reason or "unspecified", "metrics_gate.py", int(time.time())))
    conn.commit()
    print(f"[gate] accepted {profile}/{oracle} seed {seed}: {reason}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", required=True)
    p.add_argument("--rev", default="HEAD")
    p.add_argument("--host")
    p.add_argument("--strict", action="store_true",
                   help="exit 1 on any unaccepted regression (the 'block' switch)")
    p.add_argument("--codesize-tolerance-pct", type=float, default=1.0)
    p.add_argument("--accept", metavar="PROFILE:ORACLE:SEED",
                   help="add an allowlist entry and exit (no gate check)")
    p.add_argument("--reason", default="", help="reason text for --accept")
    args = p.parse_args(argv)

    conn = sqlite3.connect(args.db)
    if args.accept:
        do_accept(conn, args.accept, args.reason)
        return 0

    import socket
    import subprocess
    host = args.host or socket.gethostname()
    rev = args.rev
    if rev == "HEAD" or len(rev) < 40:
        try:
            rev = subprocess.run(["git", "rev-parse", rev], capture_output=True,
                                 text=True, check=True).stdout.strip()
        except subprocess.CalledProcessError:
            pass   # fall through to prefix match against whatever was passed

    conn.row_factory = sqlite3.Row
    run = resolve_run(conn, rev, host)
    parent = None
    if run["parent_sha"]:
        parent = conn.execute(
            "SELECT run_id FROM runs WHERE commit_sha=? AND host=?",
            (run["parent_sha"], host)).fetchone()
    parent_id = parent[0] if parent else None
    if parent_id is None:
        warn(f"no recorded run for parent {(run['parent_sha'] or '?')[:12]} -- "
             f"limited comparison this time")

    correctness_regressions = check_correctness(conn, run["run_id"], parent_id)
    codesize_regression = check_codesize(conn, run["run_id"], parent_id, args.codesize_tolerance_pct)
    print_visibility(conn, run["run_id"], parent_id)

    ok = True
    if correctness_regressions:
        ok = False
        print(f"[gate] CORRECTNESS REGRESSION on {run['commit_sha'][:12]}:")
        for profile, oracle, seeds in correctness_regressions:
            print(f"  {profile}/{oracle}: new divergent seed(s) {seeds}")
    if codesize_regression:
        ok = False
        cur, par, pct = codesize_regression
        print(f"[gate] CODESIZE REGRESSION: ratio {par:.3f} -> {cur:.3f} ({pct:+.1f}%, "
             f"tolerance {args.codesize_tolerance_pct}%)")
    if ok:
        print(f"[gate] {run['commit_sha'][:12]}: no regressions vs parent")

    if args.strict and not ok:
        return 1
    if not ok:
        print("[gate] (non-strict mode: not failing the build -- track-first policy)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
