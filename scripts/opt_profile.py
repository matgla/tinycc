#!/usr/bin/env python3
"""Profile the optimizer over a corpus of translation units.

Reproduces the measurements in docs/plans/opt_pass_dedup_and_perf.md:

  * per-O-level wall clock, so the optimizer's share of compile time is visible
  * the aggregated TCC_PASS_TIMING table (self µs, inclusive µs, calls,
    productive calls) summed across every TU that compiled

Every pass is timed by tcc_pass_timing_begin/end (source/opt/engine/pass_timing.c),
which reports *self* time -- exclusive of nested timed passes -- and suppresses a
nested re-entry of the same pass name, so a pass that both times itself and is
timed by the pipeline driver is counted once.  The `prod` column is the number
of invocations that reported changes > 0; a `?` means the call site does not
report a change count.

Usage:
  scripts/opt_profile.py --cc ./armv8m-tcc --corpus corpus.txt -O2
  scripts/opt_profile.py --cc ./armv8m-tcc --glob 'tests/.../compile/*.c' \\
      --limit 300 --levels 0,1,2
"""

import argparse
import glob as globlib
import os
import re
import subprocess
import sys
import time
from collections import OrderedDict

PASS_LINE = re.compile(
    r"^PASS_TIME\s+(\S+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+|\?)")


def collect_sources(args):
    files = []
    if args.corpus:
        with open(args.corpus) as fh:
            files += [ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")]
    for pattern in args.glob:
        files += sorted(globlib.glob(pattern))
    files += args.files
    if args.limit:
        files = files[: args.limit]
    return files


def wall_clock(cc, files, level, extra):
    """Total wall time to compile every file at -O<level>, ignoring failures."""
    cmd_base = [cc, "-O%s" % level, "-c"] + extra
    ok = 0
    start = time.perf_counter()
    for src in files:
        r = subprocess.run(cmd_base + [src, "-o", os.devnull],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ok += r.returncode == 0
    return time.perf_counter() - start, ok


def pass_table(cc, files, level, extra):
    """Sum the TCC_PASS_TIMING dump over the corpus.

    One process per TU: the table is per-process, and a shared process would
    also hide which TU a pass spent its time on if we ever want that breakdown.
    """
    env = dict(os.environ, TCC_PASS_TIMING="1")
    totals = OrderedDict()
    cmd_base = [cc, "-O%s" % level, "-c"] + extra
    for src in files:
        r = subprocess.run(cmd_base + [src, "-o", os.devnull],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           env=env, text=True)
        for line in r.stdout.splitlines():
            m = PASS_LINE.match(line)
            if not m:
                continue
            name, self_us, incl_us, calls, prod = m.groups()
            e = totals.setdefault(name, [0, 0, 0, 0, False])
            e[0] += int(self_us)
            e[1] += int(incl_us)
            e[2] += int(calls)
            if prod == "?":
                e[4] = True
            else:
                e[3] += int(prod)
    return totals


def print_table(totals):
    total_self = sum(e[0] for e in totals.values())
    total_calls = sum(e[2] for e in totals.values())
    total_prod = sum(e[3] for e in totals.values())
    print("%-28s %10s %10s %9s %8s %7s %6s"
          % ("pass", "self_us", "incl_us", "calls", "prod", "prod%", "share"))
    dead_us = 0
    for name, (self_us, incl_us, calls, prod, unknown) in sorted(
            totals.items(), key=lambda kv: -kv[1][0]):
        rate = "?" if unknown else "%.2f%%" % (100.0 * prod / calls if calls else 0)
        if not unknown and prod == 0:
            dead_us += self_us
        print("%-28s %10d %10d %9d %8s %7s %5.1f%%"
              % (name, self_us, incl_us, calls,
                 "?" if unknown else prod, rate,
                 100.0 * self_us / total_self if total_self else 0))
    print()
    print("instrumented total: %d us over %d calls, %d productive (%.2f%%)"
          % (total_self, total_calls, total_prod,
             100.0 * total_prod / total_calls if total_calls else 0))
    print("spent by passes that never fired: %d us (%.1f%%)"
          % (dead_us, 100.0 * dead_us / total_self if total_self else 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cc", default="./armv8m-tcc", help="compiler under test")
    ap.add_argument("--corpus", help="file listing one source path per line")
    ap.add_argument("--glob", action="append", default=[], help="glob of sources (repeatable)")
    ap.add_argument("files", nargs="*", help="explicit source files")
    ap.add_argument("--limit", type=int, default=0, help="use only the first N sources")
    ap.add_argument("--levels", default="0,2", help="comma-separated -O levels for the wall-clock table")
    ap.add_argument("--table-level", default="2", help="-O level to profile per-pass")
    ap.add_argument("--no-table", action="store_true", help="wall clock only")
    ap.add_argument("--cflags", default="", help="extra flags passed to every compile")
    args = ap.parse_args()

    files = collect_sources(args)
    if not files:
        ap.error("no sources: pass --corpus, --glob or explicit files")
    extra = args.cflags.split()

    print("corpus: %d files, cc=%s %s" % (len(files), args.cc, " ".join(extra)))
    print()
    print("%-8s %10s %8s" % ("level", "wall_s", "compiled"))
    for level in args.levels.split(","):
        secs, ok = wall_clock(args.cc, files, level.strip(), extra)
        print("%-8s %10.2f %8d" % ("-O" + level.strip(), secs, ok))
    if args.no_table:
        return 0
    print()
    print("=== TCC_PASS_TIMING aggregate at -O%s ===" % args.table_level)
    print_table(pass_table(args.cc, files, args.table_level, extra))
    return 0


if __name__ == "__main__":
    sys.exit(main())
