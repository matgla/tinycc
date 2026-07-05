#!/usr/bin/env python3
"""Fast O0-vs-O1 checker for a single C file via the QEMU harness."""
import os, subprocess, sys
from pathlib import Path
os.environ["ASAN_OPTIONS"] = "detect_leaks=0"
sys.path.insert(0, str(Path("tests/fuzz")))
import fuzz_harness as H
from pathlib import Path

def run(source):
    wd = Path("/tmp/opencode/_wd"); wd.mkdir(exist_ok=True)
    r0 = H.run_with_tcc(Path(source), "-O0", wd)
    r1 = H.run_with_tcc(Path(source), "-O1", wd)
    return (r0.ok and r1.ok and r0.signature == r1.signature), r0, r1

if __name__ == "__main__":
    src = sys.argv[1]
    ok, r0, r1 = run(src)
    print("OK" if ok else "DIVERGE", "|", repr(r0.stdout.strip()), "vs", repr(r1.stdout.strip()))
