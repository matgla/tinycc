#!/usr/bin/env python3
"""Line-granularity reducer using runseed.sh (ground-truth QEMU oracle).

Interesting = both O-levels compile+run and print different checksum lines.
Greedy: try deleting each line (also matching-brace blocks), keep if still
interesting. Repeats until a fixed point.
"""
import subprocess, sys, os, tempfile

RUNSEED = "/home/mateusz/repos/tinycc/tests/fuzz/runseed.sh"

def sig(path, olevel):
    try:
        out = subprocess.run(["bash", RUNSEED, path, olevel], capture_output=True,
                             text=True, timeout=60).stdout.strip().splitlines()
        return out[-1] if out else "NO_OUTPUT"
    except subprocess.TimeoutExpired:
        return "TIMEOUT"

def interesting(lines, lo, hi, tmpdir):
    src = "\n".join(lines) + "\n"
    p = os.path.join(tmpdir, "cand.c")
    with open(p, "w") as f:
        f.write(src)
    a = sig(p, lo)
    if not a.startswith("checksum="):
        return False
    b = sig(p, hi)
    if not b.startswith("checksum="):
        return False
    return a != b

def block_end(lines, i):
    """If line i opens a block ({ at end), return index of matching close."""
    depth = 0
    opened = False
    for j in range(i, len(lines)):
        depth += lines[j].count("{") - lines[j].count("}")
        if lines[j].count("{"):
            opened = True
        if opened and depth <= 0:
            return j
        if j > i + 200:
            break
    return None

def main():
    src, lo, hi, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    lines = open(src).read().splitlines()
    tmpdir = tempfile.mkdtemp(prefix="rreduce")
    assert interesting(lines, lo, hi, tmpdir), "original not interesting!"
    changed = True
    rounds = 0
    while changed and rounds < 6:
        changed = False
        rounds += 1
        i = 0
        while i < len(lines):
            line = lines[i].strip()
            if not line or line.startswith("#include") or line.startswith("return"):
                # deleting a return from a helper whose value is used
                # introduces UB (uninitialized r0) — the divergence then
                # tracks garbage, not the original bug
                i += 1
                continue
            # try deleting a whole block first if the line opens one
            cand = None
            if line.endswith("{") or ("{" in line and "}" not in line):
                j = block_end(lines, i)
                if j is not None and j > i:
                    cand = lines[:i] + lines[j+1:]
                    if interesting(cand, lo, hi, tmpdir):
                        lines = cand
                        changed = True
                        print(f"[rreduce] deleted block {i}..{j} ({len(lines)} lines left)", flush=True)
                        continue
            # then the single line
            cand = lines[:i] + lines[i+1:]
            if interesting(cand, lo, hi, tmpdir):
                lines = cand
                changed = True
                print(f"[rreduce] deleted line {i} ({len(lines)} lines left)", flush=True)
                continue
            i += 1
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[rreduce] done: {len(lines)} lines -> {out}")

main()
