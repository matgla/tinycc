#!/usr/bin/env python3
"""Replace #ifndef/#define header guards with #pragma once in source/ and ir/.

Scans all .h files under source/ and ir/ for the pattern:
    #ifndef NAME_H
    #define NAME_H
    ...
    #endif /* NAME_H */

And replaces them with:
    #pragma once

Files that already contain #pragma once are skipped.
Conditional #ifndefs (not header guards) are left untouched.
"""

import os
import re
import sys
from pathlib import Path

# Directories to scan
SCAN_DIRS = ["source", "ir"]

# Regex for the header guard pattern (lines 11-12 in these files)
GUARD_START_RE = re.compile(r"^#ifndef\s+([A-Z_][A-Z0-9_]*_H)\s*$")
GUARD_DEFINE_RE = re.compile(r"^#define\s+([A-Z_][A-Z0-9_]*_H)\s*$")
# The #endif at end of file, optionally with comment
ENDFILE_RE = re.compile(r"^#endif\s*(/\*\s*[A-Z_][A-Z0-9_]*_H\s*\*/)?\s*$")


def find_header_guard_define(lines):
    """Find the #define line that matches a preceding #ifndef.
    Returns (guard_name, define_line_index) or (None, -1).
    """
    for i, line in enumerate(lines):
        m = GUARD_START_RE.match(line)
        if not m:
            continue
        guard_name = m.group(1)
        # Look for #define on the very next non-empty line
        for j in range(i + 1, min(i + 3, len(lines))):
            stripped = lines[j].strip()
            if not stripped:
                continue
            dm = GUARD_DEFINE_RE.match(stripped)
            if dm and dm.group(1) == guard_name:
                return guard_name, j
            break  # only check next non-empty line
    return None, -1


def find_endfile_comment(lines, guard_name):
    """Find the last #endif that closes the header guard.
    Returns the line index or -1 if not found.
    """
    last_idx = -1
    for i, line in enumerate(lines):
        if ENDFILE_RE.match(line):
            last_idx = i
    return last_idx


def process_file(filepath):
    """Process a single file. Returns True if modified."""
    with open(filepath, "r") as f:
        content = f.read()

    lines = content.split("\n")

    # Check if already has #pragma once
    for line in lines[:20]:
        if "#pragma once" in line:
            return False

    guard_name, define_idx = find_header_guard_define(lines)
    if guard_name is None:
        return False

    # Find the endfile
    endfile_idx = find_endfile_comment(lines, guard_name)
    if endfile_idx == -1:
        return False

    # Verify the guard name matches the #endif comment
    endfile_line = lines[endfile_idx]
    if guard_name not in endfile_line:
        return False

    # Build new content:
    # Remove the #ifndef line (index of #ifndef is one before define_idx)
    ifndef_idx = define_idx - 1
    new_lines = []
    for i, line in enumerate(lines):
        if i == ifndef_idx:
            # Replace with #pragma once
            new_lines.append("#pragma once")
        elif i == define_idx:
            # Remove the #define line
            continue
        elif i == endfile_idx:
            # Remove the #endif line
            continue
        else:
            new_lines.append(line)

    # Ensure file ends with a newline
    result = "\n".join(new_lines)
    if not result.endswith("\n"):
        result += "\n"

    with open(filepath, "w") as f:
        f.write(result)

    return True


def main():
    root = Path(__file__).parent.parent
    modified = []
    skipped = []
    errors = []

    for scan_dir in SCAN_DIRS:
        dirpath = root / scan_dir
        if not dirpath.is_dir():
            print(f"WARNING: {scan_dir}/ not found, skipping", file=sys.stderr)
            continue

        for filepath in sorted(dirpath.rglob("*.h")):
            try:
                if process_file(filepath):
                    modified.append(str(filepath))
            except Exception as e:
                errors.append((str(filepath), str(e)))

    print(f"\nModified {len(modified)} files:")
    for f in modified:
        print(f"  {f}")

    if errors:
        print(f"\nErrors ({len(errors)}):")
        for f, e in errors:
            print(f"  {f}: {e}")

    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
