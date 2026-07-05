#!/usr/bin/env python3
"""Run sweep_all.py over a large inclusive seed range in bounded chunks.

Large sweep_all.py bands can create enough temporary batch_sweep work to fill a
tmpfs before the process exits and its cleanup hooks run.  This wrapper keeps
the live scratch footprint bounded by running one sweep_all.py child per chunk.

Usage:
    python3 tests/fuzz/sweep_all_chunks.py 0 99999
    python3 tests/fuzz/sweep_all_chunks.py 0 99999 --chunk-size 5000 --jobs 24
    python3 tests/fuzz/sweep_all_chunks.py 0 99999 --profiles ptr,bitfield --olevels-only
    python3 tests/fuzz/sweep_all_chunks.py 0 99999 --dry-run --mode triage

All options not recognized by this wrapper are passed through to sweep_all.py.
Reports use sweep_all.py's default per-chunk names unless --out-template is set.
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent.parent
SWEEP_ALL = THIS_DIR / "sweep_all.py"


def _chunks(lo: int, hi: int, chunk_size: int) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    start = lo
    while start <= hi:
        end = min(start + chunk_size - 1, hi)
        out.append((start, end))
        start = end + 1
    return out


def _format_template(template: str, lo: int, hi: int, chunk: int) -> str:
    try:
        return template.format(lo=lo, hi=hi, chunk=chunk)
    except KeyError as e:
        raise SystemExit(f"unknown --out-template placeholder {{{e.args[0]}}}") from e


def _reject_plain_out(passthrough: list[str], multi_chunk: bool, out_template: bool) -> None:
    for i, arg in enumerate(passthrough):
        if arg == "--out":
            value = passthrough[i + 1] if i + 1 < len(passthrough) else ""
            if out_template:
                raise SystemExit("--out-template cannot be combined with pass-through --out")
            if not multi_chunk:
                return
            raise SystemExit(
                "--out would be reused for every chunk and overwrite reports; "
                f"use --out-template instead (saw --out {value!r})"
            )
        if arg.startswith("--out="):
            if out_template:
                raise SystemExit("--out-template cannot be combined with pass-through --out")
            if not multi_chunk:
                return
            raise SystemExit(
                "--out would be reused for every chunk and overwrite reports; "
                "use --out-template instead"
            )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lo", type=int, help="first seed, inclusive")
    ap.add_argument("hi", type=int, help="last seed, inclusive")
    ap.add_argument("--chunk-size", "--max-group", "--max-group-size",
                    dest="chunk_size", type=int, default=5000,
                    help="maximum seeds per sweep_all.py child (default 5000)")
    ap.add_argument("--sweep-all", default=str(SWEEP_ALL),
                    help="path to sweep_all.py (default tests/fuzz/sweep_all.py)")
    ap.add_argument("--out-template", default="",
                    help="per-chunk report path template; supports {lo}, {hi}, {chunk}")
    ap.add_argument("--stop-on-nonzero", action="store_true",
                    help="stop after the first sweep_all.py child with non-zero rc")
    ap.add_argument("--dry-run", action="store_true",
                    help="print child commands without running them")
    args, passthrough = ap.parse_known_args(argv)

    if passthrough and passthrough[0] == "--":
        passthrough = passthrough[1:]
    if args.lo > args.hi:
        raise SystemExit(f"lo must be <= hi (got {args.lo} > {args.hi})")
    if args.chunk_size <= 0:
        raise SystemExit(f"--chunk-size must be positive (got {args.chunk_size})")

    ranges = _chunks(args.lo, args.hi, args.chunk_size)
    _reject_plain_out(passthrough, multi_chunk=len(ranges) > 1,
                      out_template=bool(args.out_template))

    sweep_all = Path(args.sweep_all)
    if not sweep_all.exists():
        raise SystemExit(f"sweep_all.py not found: {sweep_all}")

    print(
        f"sweep_all_chunks: {args.lo}..{args.hi} split into {len(ranges)} "
        f"chunk(s), max {args.chunk_size} seeds each",
        flush=True,
    )

    nonzero = 0
    for idx, (lo, hi) in enumerate(ranges, 1):
        cmd = [sys.executable, "-u", str(sweep_all), str(lo), str(hi), *passthrough]
        if args.out_template:
            cmd += ["--out", _format_template(args.out_template, lo, hi, idx)]

        print(f"\n[{idx}/{len(ranges)}] sweep_all.py {lo}..{hi}", flush=True)
        print("  " + shlex.join(cmd), flush=True)
        if args.dry_run:
            continue

        try:
            rc = subprocess.run(cmd, cwd=str(REPO_ROOT)).returncode
        except KeyboardInterrupt:
            print("\ninterrupted", file=sys.stderr)
            return 130

        if rc != 0:
            nonzero += 1
            print(f"[{idx}/{len(ranges)}] sweep_all.py exited rc={rc}", flush=True)
            if args.stop_on_nonzero:
                return min(rc, 125)

    if args.dry_run:
        return 0
    return min(nonzero, 125)


if __name__ == "__main__":
    raise SystemExit(main())
