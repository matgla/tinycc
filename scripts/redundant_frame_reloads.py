#!/usr/bin/env python3
"""Count frame-slot loads a per-basic-block availability cache could remove.

Models exactly what a backend materialization cache can prove: within one
basic block, a `ldr rT,[sp,#N]` is redundant when rT already holds [sp,#N]
from an earlier ldr/str and nothing has written rT or stored to slot N since.
Reports the same-register case (deletable) apart from the other-register case
(convertible to a mov), because they are worth different amounts.
"""
import re, subprocess, sys, collections

OBJ = sys.argv[1]
FUNC_FILTER = sys.argv[2] if len(sys.argv) > 2 else None

out = subprocess.run(["arm-none-eabi-objdump", "-d", OBJ],
                     capture_output=True, text=True).stdout

func_re = re.compile(r"^([0-9a-f]+) <(.+)>:$")
insn_re = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f ]+)\t([a-z0-9.]+)\s*(.*)$")
# ldr/str with an sp/fp immediate offset
mem_re = re.compile(r"^(ldr|str)(?:\.w)?$")
memd_re = re.compile(r"^(ldrd|strd)(?:\.w)?$")
frame_re = re.compile(r"^(r\d+|ip|lr|fp|sl), \[(sp|r7|fp)(?:, #(-?\d+))?\]")
framed_re = re.compile(r"^(r\d+|ip|lr|fp|sl), (r\d+|ip|lr|fp|sl), \[(sp|r7|fp)(?:, #(-?\d+))?\]")
branch_re = re.compile(r"^(b|bl|blx|bx|bne|beq|bcc|bcs|bmi|bpl|bge|blt|bgt|ble|bhi|bls|bvs|bvc|cbz|cbnz)")
target_re = re.compile(r"\b([0-9a-f]+) <")

funcs = collections.OrderedDict()
cur = None
for line in out.splitlines():
    m = func_re.match(line)
    if m:
        cur = m.group(2)
        funcs[cur] = []
        continue
    if cur is None:
        continue
    m = insn_re.match(line)
    if m:
        funcs[cur].append((int(m.group(1), 16), m.group(3), m.group(4)))

def dest_regs(mnem, args):
    """Registers this instruction writes (approximate, conservative)."""
    base = mnem.split('.')[0]
    if base in ("cmp", "cmn", "tst", "teq", "str", "strb", "strh", "strd",
                "push", "b", "bne", "beq", "bcc", "bcs", "bmi", "bpl", "bge",
                "blt", "bgt", "ble", "bhi", "bls", "bvs", "bvc", "cbz", "cbnz",
                "it", "ite", "itt", "itte", "ittt", "iteee", "nop", "bx"):
        return set()
    if base in ("bl", "blx"):
        return {"ALL"}
    if base in ("pop", "ldm", "ldmia", "stmdb", "stm"):
        return {"ALL"}
    if base == "ldrd":
        m = framed_re.match(args)
        if m:
            return {m.group(1), m.group(2)}
        return {"ALL"}
    toks = [t.strip() for t in args.split(",")]
    if toks and re.match(r"^(r\d+|ip|lr|fp|sl)$", toks[0]):
        return {toks[0]}
    return {"ALL"}

total_same = total_other = total_loads = 0
per_func = []
for name, insns in funcs.items():
    if FUNC_FILTER and FUNC_FILTER not in name:
        continue
    targets = set()
    for _, mnem, args in insns:
        if branch_re.match(mnem.split('.')[0]):
            for t in target_re.findall(args):
                targets.add(int(t, 16))
    avail = {}          # slot offset -> set of regs holding it
    same = other = loads = 0
    for addr, mnem, args in insns:
        if addr in targets:
            avail = {}
        base = mnem.split('.')[0]
        m = mem_re.match(base)
        md = memd_re.match(base)
        slot = None
        if m:
            fm = frame_re.match(args)
            if fm:
                rt = fm.group(1); off = int(fm.group(3) or 0)
                slot = off
                if base == "ldr":
                    loads += 1
                    holders = avail.get(off, set())
                    if rt in holders:
                        same += 1
                    elif holders:
                        other += 1
        # apply writes
        w = dest_regs(mnem, args)
        if "ALL" in w:
            avail = {}
        else:
            for r in w:
                for k in list(avail):
                    avail[k].discard(r)
                    if not avail[k]:
                        del avail[k]
        # apply memory effects
        if md:
            fm = framed_re.match(args)
            if fm:
                off = int(fm.group(4) or 0)
                for k in (off, off + 4):
                    avail.pop(k, None)
                if base == "ldrd":
                    avail.setdefault(off, set()).add(fm.group(1))
                    avail.setdefault(off + 4, set()).add(fm.group(2))
            else:
                avail = {}
        elif m and slot is not None:
            fm = frame_re.match(args)
            rt = fm.group(1)
            if base == "str":
                avail.pop(slot, None)
            avail.setdefault(slot, set()).add(rt)
        elif base in ("strb", "strh", "strd", "stm", "stmdb", "push"):
            avail = {}
        elif base.startswith("str") or (base.startswith("ldr") and not m and not md):
            avail = {}
    if same or other:
        per_func.append((same + other, same, other, loads, name))
    total_same += same; total_other += other; total_loads += loads

per_func.sort(reverse=True)
print(f"{'redundant':>9} {'same-reg':>8} {'other-reg':>9} {'loads':>6}  function")
for tot, s, o, l, n in per_func[:20]:
    print(f"{tot:>9} {s:>8} {o:>9} {l:>6}  {n}")
print(f"\nTOTAL redundant frame loads: {total_same + total_other} "
      f"(same-reg {total_same}, other-reg {total_other}) out of {total_loads} frame loads")
