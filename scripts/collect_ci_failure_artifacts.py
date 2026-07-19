#!/usr/bin/env python3
"""
Gather a compact debug bundle after a failing `make test`, for CI to upload as
an artifact (see .github/workflows/ci.yml). It captures:

  * make-test.log  -- the full (untruncated) build+test console output
  * junit.xml      -- the structured pass/fail report
  * armv8m-tcc, armv8m-libtcc1.a, config.mak -- the exact cross compiler +
    runtime that produced the failure, so it can be reproduced locally
  * failed-test-dirs/ -- ONLY the per-test work dirs (.elf/.o/...) of the tests
    that actually failed. pytest keeps every test's tmp dir, which for the
    ~13k-case torture suite is far too large to upload wholesale, so we map
    each failed JUnit testcase to its tmp-dir prefix and copy just those.

Best-effort throughout: a missing piece is skipped, never fatal, so the bundle
is produced even when the build failed before any test ran.

Usage:  scripts/collect_ci_failure_artifacts.py [OUTDIR]
"""

import argparse
import os
import re
import shutil
import sys
import tarfile
import xml.etree.ElementTree as ET
from pathlib import Path

TOP = Path(__file__).resolve().parent.parent

LOG = Path(os.environ.get("MAKE_TEST_LOG", "/tmp/make-test.log"))
JUNIT = Path(os.environ.get("PYTEST_JUNIT_XML", "/tmp/ci-junit.xml"))
BASETEMP_ROOT = Path(os.environ.get("PYTEST_BASETEMP_ROOT", "/tmp/pytest-of-root"))
# 200 MB cap on collected tmp dirs
MAX_TESTDIR_BYTES = int(os.environ.get("MAX_TESTDIR_BYTES", 209715200))

BUILD_ARTIFACTS = ["armv8m-tcc", "armv8m-tcc.exe", "armv8m-libtcc1.a", "config.mak"]


def dir_size(path: Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            fp = Path(root) / f
            if not fp.is_symlink() and fp.exists():
                total += fp.stat().st_size
    return total


def failed_test_prefixes(junit: Path) -> set[str]:
    """pytest names a test's tmp dir from re.sub(r'\\W','_', node_name)[:30] + a number."""
    try:
        root = ET.parse(junit).getroot()
    except Exception as e:
        print(f"collect: could not parse junit ({e})", file=sys.stderr)
        return set()
    return {
        re.sub(r"\W", "_", tc.get("name", ""))[:30]
        for tc in root.iter("testcase")
        if tc.find("failure") is not None or tc.find("error") is not None
    }


def copy_failed_test_dirs(junit: Path, basetemp_root: Path, dest: Path) -> None:
    prefixes = failed_test_prefixes(junit)
    if not prefixes:
        print("collect: no failed testcases in junit")
        return

    dest.mkdir(parents=True, exist_ok=True)
    total = copied = 0
    for run in sorted(os.listdir(basetemp_root)):
        run_dir = basetemp_root / run
        if not run_dir.is_dir():
            continue
        for d in sorted(os.listdir(run_dir)):
            src = run_dir / d
            if not src.is_dir() or not any(d.startswith(p) for p in prefixes):
                continue
            sz = dir_size(src)
            if total + sz > MAX_TESTDIR_BYTES:
                print(f"collect: {MAX_TESTDIR_BYTES}-byte cap reached at {total} bytes; "
                      f"skipping remaining dirs", file=sys.stderr)
                print(f"collect: copied {copied} failed-test dir(s), {total} bytes")
                return
            shutil.copytree(src, dest / f"{run}__{d}", dirs_exist_ok=True)
            total += sz
            copied += 1
    print(f"collect: copied {copied} failed-test dir(s), {total} bytes")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir", nargs="?", default=str(TOP / "ci-failure-artifacts"),
                    help="directory to build the bundle in (it is removed first)")
    args = ap.parse_args()

    out = Path(args.outdir)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)

    # 1) Logs / reports.
    if LOG.is_file():
        shutil.copy(LOG, out / "make-test.log")
    if JUNIT.is_file():
        shutil.copy(JUNIT, out / "junit.xml")

    # 2) The cross compiler + runtime + build config.
    for name in BUILD_ARTIFACTS:
        src = TOP / name
        if src.is_file():
            shutil.copy(src, out / name)

    # 3) Work dirs of the failed tests only.
    if (out / "junit.xml").is_file() and BASETEMP_ROOT.is_dir():
        try:
            copy_failed_test_dirs(out / "junit.xml", BASETEMP_ROOT, out / "failed-test-dirs")
        except Exception as e:
            print(f"collect: failed-test-dir collection skipped ({e})", file=sys.stderr)

    # 4) One archive for upload.
    archive = out.parent / f"{out.name}.tar.gz"
    try:
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(out, arcname=out.name)
    except Exception as e:
        print(f"collect: could not create archive ({e})", file=sys.stderr)

    print(f"collect: bundle at {archive}")
    if out.is_dir():
        for entry in sorted(out.iterdir()):
            print(f"  {entry.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
