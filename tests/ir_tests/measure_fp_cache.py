#!/usr/bin/env python3
"""
Measure frame pointer offset calculation redundancy in compiled code.

Usage:
    python measure_fp_cache.py <object_file>
    
Example:
    python measure_fp_cache.py /tmp/test_fp_cache.o
"""

import subprocess
import sys
import re
from collections import Counter


def analyze_obj_file(obj_file):
    """Analyze an object file for frame pointer offset calculations."""
    
    # Disassemble the object file
    result = subprocess.run(
        ["arm-none-eabi-objdump", "-d", obj_file],
        capture_output=True,
        text=True
    )
    
    if result.returncode != 0:
        print(f"Error: {result.stderr}")
        sys.exit(1)
    
    disasm = result.stdout
    
    # Pattern to match frame pointer offset calculations:
    # sub.w Rx, r7, #const  (where const is a multiple of 4, typically stack offset)
    fp_pattern = re.compile(
        r"sub\.w\s+(r\w+|ip|lr|sp),\s*r7,\s*#(\d+)",
        re.IGNORECASE
    )
    
    # Also match rsb (reverse subtract) patterns for negative offsets
    rsb_pattern = re.compile(
        r"rsb\s+(r\w+|ip|lr|sp),\s*(r\w+|ip|lr|sp),\s*#0",
        re.IGNORECASE
    )
    
    # Match mov.w Rx, #const followed by rsb (for large constants)
    mov_rsb_pattern = re.compile(
        r"mov\.w\s+(r\w+|ip|lr|sp),\s*#(\d+).*?\n.*rsb",
        re.IGNORECASE | re.DOTALL
    )
    
    fp_calcs = []
    for match in fp_pattern.finditer(disasm):
        reg = match.group(1)
        offset = match.group(2)
        fp_calcs.append((reg, int(offset)))
    
    # Count unique offset calculations
    offset_counts = Counter(offset for reg, offset in fp_calcs)
    
    print(f"\n=== Frame Pointer Offset Analysis for {obj_file} ===\n")
    
    print(f"Total FP offset calculations: {len(fp_calcs)}")
    print(f"\nBreakdown by offset:")
    print(f"{'Offset':>10} {'Count':>8} {'Status'}")
    print("-" * 40)
    
    total_redundant = 0
    for offset, count in sorted(offset_counts.items()):
        if count > 1:
            status = f"REDUNDANT ({count-1} extra)"
            total_redundant += (count - 1)
        else:
            status = "OK"
        print(f"#{offset:>8}: {count:>8}  {status}")
    
    print(f"\n{'='*40}")
    print(f"Total redundant calculations: {total_redundant}")
    if len(fp_calcs) > 0:
        savings_pct = (total_redundant / len(fp_calcs)) * 100
        print(f"Potential savings: {total_redundant} instructions ({savings_pct:.1f}%)")
    
    # List all functions and their FP calculations
    print(f"\n\nDetailed by function:")
    print("-" * 60)
    
    current_func = None
    func_calcs = {}
    
    for line in disasm.split('\n'):
        # Check for function header
        func_match = re.match(r"^[0-9a-f]+\s+<(.+)>:", line)
        if func_match:
            current_func = func_match.group(1)
            func_calcs[current_func] = []
        
        # Check for FP calculation in this line
        fp_match = fp_pattern.search(line)
        if fp_match and current_func:
            reg = fp_match.group(1)
            offset = int(fp_match.group(2))
            func_calcs[current_func].append((reg, offset))
    
    for func, calcs in sorted(func_calcs.items()):
        if calcs:
            unique_offsets = len(set(offset for reg, offset in calcs))
            total = len(calcs)
            redundant = total - unique_offsets
            print(f"  {func:40s}: {total:2d} calcs, {unique_offsets:2d} unique, {redundant:2d} redundant")
    
    return total_redundant


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <object_file>")
        sys.exit(1)
    
    obj_file = sys.argv[1]
    analyze_obj_file(obj_file)
