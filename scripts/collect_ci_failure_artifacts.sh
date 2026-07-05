#!/usr/bin/env bash
# Gather a compact debug bundle after a failing `make test`, for CI to upload as
# an artifact (see .github/workflows/ci.yml). It captures:
#
#   * make-test.log  — the full (untruncated) build+test console output
#   * junit.xml      — the structured pass/fail report
#   * armv8m-tcc, armv8m-libtcc1.a, config.mak — the exact cross compiler +
#     runtime that produced the failure, so it can be reproduced locally
#   * failed-test-dirs/ — ONLY the per-test work dirs (.elf/.o/...) of the tests
#     that actually failed. pytest keeps every test's tmp dir, which for the
#     ~13k-case torture suite is far too large to upload wholesale, so we map
#     each failed JUnit testcase to its tmp-dir prefix and copy just those.
#
# Best-effort throughout: a missing piece is skipped, never fatal, so the
# bundle is produced even when the build failed before any test ran.
set -uo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$TOP/ci-failure-artifacts}"
LOG="${MAKE_TEST_LOG:-/tmp/make-test.log}"
JUNIT="${PYTEST_JUNIT_XML:-/tmp/ci-junit.xml}"
BASETEMP_ROOT="${PYTEST_BASETEMP_ROOT:-/tmp/pytest-of-root}"
MAX_TESTDIR_BYTES="${MAX_TESTDIR_BYTES:-209715200}"  # 200 MB cap on collected tmp dirs

rm -rf "$OUT"
mkdir -p "$OUT"

# 1) Logs / reports.
[ -f "$LOG" ]   && cp "$LOG"   "$OUT/make-test.log" || true
[ -f "$JUNIT" ] && cp "$JUNIT" "$OUT/junit.xml"     || true

# 2) The cross compiler + runtime + build config.
for f in armv8m-tcc armv8m-tcc.exe armv8m-libtcc1.a config.mak; do
    [ -f "$TOP/$f" ] && cp "$TOP/$f" "$OUT/" || true
done

# 3) Work dirs of the failed tests only.
if [ -f "$OUT/junit.xml" ] && [ -d "$BASETEMP_ROOT" ]; then
    python3 - "$OUT/junit.xml" "$BASETEMP_ROOT" "$OUT/failed-test-dirs" "$MAX_TESTDIR_BYTES" <<'PY' || true
import os, re, shutil, sys, xml.etree.ElementTree as ET

junit, basetemp_root, dest, max_bytes = sys.argv[1:5]
max_bytes = int(max_bytes)

try:
    root = ET.parse(junit).getroot()
except Exception as e:
    print(f"collect: could not parse junit ({e})", file=sys.stderr)
    sys.exit(0)

# pytest names a test's tmp dir from re.sub(r"\W","_", node_name)[:30] + a number.
prefixes = {
    re.sub(r"\W", "_", tc.get("name", ""))[:30]
    for tc in root.iter("testcase")
    if tc.find("failure") is not None or tc.find("error") is not None
}
if not prefixes:
    print("collect: no failed testcases in junit")
    sys.exit(0)

def dir_size(p):
    total = 0
    for r, _, files in os.walk(p):
        for f in files:
            fp = os.path.join(r, f)
            if not os.path.islink(fp) and os.path.exists(fp):
                total += os.path.getsize(fp)
    return total

os.makedirs(dest, exist_ok=True)
total = copied = 0
for run in sorted(os.listdir(basetemp_root)):
    run_dir = os.path.join(basetemp_root, run)
    if not os.path.isdir(run_dir):
        continue
    for d in sorted(os.listdir(run_dir)):
        src = os.path.join(run_dir, d)
        if not os.path.isdir(src) or not any(d.startswith(p) for p in prefixes):
            continue
        sz = dir_size(src)
        if total + sz > max_bytes:
            print(f"collect: 200MB cap reached at {total} bytes; skipping remaining dirs",
                  file=sys.stderr)
            print(f"collect: copied {copied} failed-test dir(s), {total} bytes")
            sys.exit(0)
        shutil.copytree(src, os.path.join(dest, f"{run}__{d}"), dirs_exist_ok=True)
        total += sz
        copied += 1
print(f"collect: copied {copied} failed-test dir(s), {total} bytes")
PY
fi

# 4) One archive for upload.
( cd "$(dirname "$OUT")" && tar czf "$(basename "$OUT").tar.gz" "$(basename "$OUT")" ) || true
echo "collect: bundle at $OUT.tar.gz"
ls -la "$OUT" 2>/dev/null || true
