#!/usr/bin/env python3
"""Portable timing wrapper to mimic GNU time `-v -a -o` output.

This exists because macOS `/usr/bin/time` is BSD time and does not support the
GNU flags used elsewhere (`-v -a -o`).

It writes a subset of GNU time -v lines that `qemu_run.parse_time_output()`
expects:
- User time (seconds)
- System time (seconds)
- Maximum resident set size (kbytes)

Usage (as a command prefix):
    python3 timewrap.py -a -o out.txt -- <command> <args...>
"""

from __future__ import annotations

import argparse
import os
import shlex
import sys
import time


def _kb_from_ru_maxrss(ru_maxrss: int) -> int:
    # ru_maxrss units:
    # - macOS/BSD: bytes
    # - Linux: kilobytes
    if sys.platform == "darwin":
        return int(ru_maxrss // 1024)
    return int(ru_maxrss)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("-a", action="store_true")
    parser.add_argument("-o", dest="output", type=str, default="")
    parser.add_argument("--", dest="_dashdash", action="store_true")
    args, rest = parser.parse_known_args(argv)

    if "--" in rest:
        dd = rest.index("--")
        cmd = rest[dd + 1 :]
    else:
        cmd = rest

    if not cmd:
        raise SystemExit("timewrap.py: missing command (use `-- <cmd> ...`) ")

    # Spawn the child and wait using wait4 so we can get rusage.
    start_wall = time.perf_counter()

    try:
        pid = os.fork()
    except AttributeError:
        raise SystemExit("timewrap.py requires fork() (Unix-only)")

    if pid == 0:
        # Child: exec command, inherit stdio.
        os.execvp(cmd[0], cmd)

    # Parent
    _, status, rusage = os.wait4(pid, 0)
    elapsed = time.perf_counter() - start_wall

    if os.WIFEXITED(status):
        rc = os.WEXITSTATUS(status)
    elif os.WIFSIGNALED(status):
        rc = 128 + os.WTERMSIG(status)
    else:
        rc = 1

    out_lines = []
    out_lines.append(f"Command being timed: {shlex.join(cmd)}")
    out_lines.append(f"User time (seconds): {rusage.ru_utime:.6f}")
    out_lines.append(f"System time (seconds): {rusage.ru_stime:.6f}")
    out_lines.append(f"Elapsed (wall clock) time (seconds): {elapsed:.6f}")
    out_lines.append(f"Maximum resident set size (kbytes): {_kb_from_ru_maxrss(rusage.ru_maxrss)}")

    text = "\n".join(out_lines) + "\n"

    if args.output:
        mode = "a" if args.a else "w"
        with open(args.output, mode, encoding="utf-8") as f:
            f.write(text)
    else:
        # Match time(1): write to stderr.
        sys.stderr.write(text)

    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
