#!/usr/bin/env python3
"""
Regression test: compile all pytest-registered tests with TCC -O2 and GCC -O2,
count instructions per function, and produce a summary report.

Usage:
  ./scripts/regression_disasm.py                   # run and print summary
  ./scripts/regression_disasm.py --save baseline   # save current results as baseline
  ./scripts/regression_disasm.py --diff baseline   # compare against saved baseline
  ./scripts/regression_disasm.py --csv             # output raw CSV
  ./scripts/regression_disasm.py --dump-dir /path  # save per-test disassembly dumps
  ./scripts/regression_disasm.py --graph HEAD~1    # graph HEAD~1 vs working tree
  ./scripts/regression_disasm.py --graph staged HEAD  # graph staged vs HEAD

Options:
  -O0 / -O1 / -O2 / -O3   GCC optimization level (default: -O2)
  -j N                     parallel jobs (default: nproc)
  --suite ir|tests2|float|gcc-compile|gcc-execute|bug|all
                           which test suites to include (default: all)
  --no-cache               disable cache tracking (skip comparison)
  --overwrite              force overwrite all cache entries in both main and pending cache
"""

import argparse
import csv
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from pathlib import Path

from disasm_common import (
    SCRIPT_DIR,
    TCC_DIR,
    DisasmCache,
    compile_gcc,
    compile_tcc,
    count_all_functions,
    disassemble,
    eprint,
    get_functions,
    get_tcc_path,
    run,
)

TCC = get_tcc_path()
IR_TESTS_DIR = TCC_DIR / "tests" / "ir_tests"

PRINT_LOCK = threading.Lock()


def collect_tests(suite_filter: str):
    import importlib.util

    if str(IR_TESTS_DIR) not in sys.path:
        sys.path.insert(0, str(IR_TESTS_DIR))

    spec = importlib.util.spec_from_file_location("test_qemu", IR_TESTS_DIR / "test_qemu.py")
    test_qemu = importlib.util.module_from_spec(spec)
    sys.modules["test_qemu"] = test_qemu
    spec.loader.exec_module(test_qemu)

    def primary(tf):
        if isinstance(tf, (list, tuple)) and isinstance(tf[0], str):
            return tf[0]
        return tf

    def resolve(path_str, base_dir):
        p = Path(path_str)
        if p.is_absolute():
            return str(p)
        return str((base_dir / p).resolve())

    tests = []

    if suite_filter in ("all", "ir", "tests2"):
        for entry in test_qemu.TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            path = resolve(primary(tf), IR_TESTS_DIR)
            is_tests2 = "/tests2/" in path
            if suite_filter == "tests2" and not is_tests2:
                continue
            if suite_filter == "ir" and is_tests2:
                continue
            tests.append(("ir" if not is_tests2 else "tests2", path, ""))

    if suite_filter in ("all", "float"):
        for entry in test_qemu.FLOAT_TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            tests.append(("float", resolve(primary(tf), IR_TESTS_DIR), ""))

    if suite_filter in ("all", "bug"):
        for entry in test_qemu.TCC_BUG_TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            tests.append(("bug", resolve(primary(tf), IR_TESTS_DIR), ""))

    if suite_filter in ("all", "ir"):
        for entry in test_qemu.FUNCTION_SECTIONS_TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            tests.append(("func-sections", resolve(primary(tf), IR_TESTS_DIR), ""))
        for entry in test_qemu.GNU89_INLINE_TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            tests.append(("gnu89-inline", resolve(primary(tf), IR_TESTS_DIR), ""))
        for entry in test_qemu.PIC_TEXT_DATA_SEP_TEST_FILES:
            tf = entry[0] if isinstance(entry, tuple) else entry
            tests.append(("pic-tds", resolve(primary(tf), IR_TESTS_DIR), ""))

    if suite_filter in ("all", "gcc-compile", "gcc-execute"):
        sys.path.insert(0, str(TCC_DIR / "tests" / "gcctestsuite"))
        try:
            from conftest import discover_gcc_compile_tests, discover_gcc_execute_tests, should_skip_gcc_test

            if suite_filter in ("all", "gcc-compile"):
                for tc in discover_gcc_compile_tests():
                    if should_skip_gcc_test(tc.source):
                        continue
                    tests.append(("gcc-compile", str(tc.source), tc.dg_options))
            if suite_filter in ("all", "gcc-execute"):
                for tc in discover_gcc_execute_tests():
                    if should_skip_gcc_test(tc.source):
                        continue
                    tests.append(("gcc-execute", str(tc.source), tc.dg_options))
        except Exception as exc:
            eprint(f"# WARNING: GCC torture discovery failed: {exc}")

    tests = [(s, src, flags) for s, src, flags in tests
             if f"{s}/{Path(src).stem}" not in DISASM_SKIP_TESTS]
    return tests


DISASM_SKIP_TESTS = {
    # float test requiring sys/mman.h (not available on bare-metal)
    "float/119_random_stuff",
    # compile tests requiring -std=gnu89 or -fpermissive (invalid in -std=gnu11)
    "gcc-compile/20020418-1",
    "gcc-compile/20020927-1",
    "gcc-compile/920415-1",
    "gcc-compile/920817-1",
    "gcc-compile/20180605-1",
    "gcc-compile/pr72802",
    # compile tests requiring GCC-specific builtins
    "gcc-compile/pr37669",
    # compile tests requiring specific flags incompatible with ARM defaults
    "gcc-compile/pr39845",
    "gcc-compile/pr123365",
    # compile tests expecting compilation errors (dg-error)
    "gcc-compile/20030305-1",
    "gcc-compile/pr28865",
    "gcc-compile/pr48767",
    "gcc-compile/pr83547",
    # stress test exceeding TCC internal limits
    "gcc-compile/20001226-1",
    # execute tests requiring -std=gnu89
    "gcc-execute/920415-1",
    "gcc-execute/920728-1",
    # execute tests requiring -std=c2y (not supported by all arm-none-eabi-gcc versions)
    "gcc-execute/uabs-1",
    "gcc-execute/uabs-2",
    "gcc-execute/uabs-3",
}

TRACE_TESTS = {"memcpy-a1", "memcpy-a2", "memcpy-a4", "memcpy-a8", "memclr"}


def process_one(idx: int, total: int, suite: str, src: str, tmpdir: Path, dump_dir: str, gcc_opt: str, extra_flags: str = "", tcc_opt: str = "-O2"):
    src_path = Path(src)
    basename = src_path.stem
    key = f"{suite}/{basename}"
    trace = basename in TRACE_TESTS
    tcc_obj = tmpdir / f"{suite}_{basename}_tcc.o"
    gcc_obj = tmpdir / f"{suite}_{basename}_gcc.o"
    tcc_dump_path = tmpdir / f"{suite}_{basename}_tcc.dump"
    gcc_dump_path = tmpdir / f"{suite}_{basename}_gcc.dump"
    status = "OK"

    if trace:
        eprint(f"  TRACE {key}: starting tcc compile")
    try:
        tcc_result = compile_tcc(src, tcc_obj, opt=tcc_opt, extra_flags=extra_flags or None)
    except subprocess.TimeoutExpired:
        with PRINT_LOCK:
            eprint(f"[{idx}/{total}] {key} ... SKIP (tcc compile timed out)")
        return {"type": "skip", "key": key, "reason": "tcc compile timed out"}
    if trace:
        eprint(f"  TRACE {key}: tcc done (rc={tcc_result.returncode})")
    if tcc_result.returncode != 0:
        with PRINT_LOCK:
            eprint(f"[{idx}/{total}] {key} ... SKIP (tcc compile failed)")
            if tcc_result.stderr:
                for line in tcc_result.stderr.strip().splitlines():
                    eprint(f"    {line}")
        return {"type": "skip", "key": key, "reason": "tcc compile failed"}

    if trace:
        eprint(f"  TRACE {key}: starting gcc compile")
    try:
        gcc_ef = ["-ffreestanding"] + (extra_flags.split() if extra_flags else [])
        gcc_result = compile_gcc(src, gcc_obj, opt=gcc_opt, extra_flags=gcc_ef)
    except subprocess.TimeoutExpired:
        with PRINT_LOCK:
            eprint(f"[{idx}/{total}] {key} ... SKIP (gcc compile timed out)")
        return {"type": "skip", "key": key, "reason": "gcc compile timed out"}
    if trace:
        eprint(f"  TRACE {key}: gcc done (rc={gcc_result.returncode})")
    if gcc_result.returncode != 0:
        with PRINT_LOCK:
            eprint(f"[{idx}/{total}] {key} ... SKIP (gcc compile failed)")
            if gcc_result.stderr:
                for line in gcc_result.stderr.strip().splitlines():
                    eprint(f"    {line}")
        return {"type": "skip", "key": key, "reason": "gcc compile failed"}

    if trace:
        eprint(f"  TRACE {key}: disassembling")
    tcc_text = disassemble(tcc_obj)
    gcc_text = disassemble(gcc_obj)
    if trace:
        eprint(f"  TRACE {key}: disasm done (tcc={len(tcc_text)} gcc={len(gcc_text)} chars)")
    tcc_dump_path.write_text(tcc_text)
    gcc_dump_path.write_text(gcc_text)

    if dump_dir:
        shutil.copy(tcc_dump_path, Path(dump_dir) / f"{suite}_{basename}_tcc.dump")
        shutil.copy(gcc_dump_path, Path(dump_dir) / f"{suite}_{basename}_gcc.dump")

    if trace:
        eprint(f"  TRACE {key}: getting functions")
    tcc_funcs = get_functions(tcc_obj, include_local=True)
    gcc_funcs = get_functions(gcc_obj, include_local=True)
    common = sorted(tcc_funcs & gcc_funcs)
    if trace:
        eprint(f"  TRACE {key}: {len(common)} common functions")

    if not common:
        if not tcc_funcs or not gcc_funcs:
            with PRINT_LOCK:
                eprint(f"[{idx}/{total}] {key} ... SKIP (no functions)")
            return {"type": "skip", "key": key, "reason": "no functions"}
        tcc_counts = count_all_functions(tcc_text, sorted(tcc_funcs), with_clones=False)
        gcc_counts = count_all_functions(gcc_text, sorted(gcc_funcs), with_clones=True)
        tcc_total = sum(tcc_counts.values())
        gcc_total = sum(gcc_counts.values())
        if tcc_total == 0 and gcc_total == 0:
            with PRINT_LOCK:
                eprint(f"[{idx}/{total}] {key} ... SKIP (no instructions)")
            return {"type": "skip", "key": key, "reason": "no instructions"}
        funcs = [("*", tcc_total, gcc_total)]
        status = "OK (whole-file)"
    else:
        if trace:
            eprint(f"  TRACE {key}: counting instructions")
        tcc_counts = count_all_functions(tcc_text, common, with_clones=False)
        gcc_counts = count_all_functions(gcc_text, common, with_clones=True)
        funcs = [(func, tcc_counts[func], gcc_counts[func]) for func in common]

    tcc_obj.unlink(missing_ok=True)
    gcc_obj.unlink(missing_ok=True)

    with PRINT_LOCK:
        eprint(f"[{idx}/{total}] {key} ... {status}")
    if trace:
        eprint(f"  TRACE {key}: DONE")
    return {"type": "ok", "key": key, "funcs": funcs}


def run_all(tests, jobs, gcc_opt, dump_dir, suite="all", tcc_opt="-O2"):
    total = len(tests)
    eprint(f"Compiling {total} tests (TCC {tcc_opt} vs GCC {gcc_opt}), jobs={jobs} ...")
    eprint(f"Suites: {suite}")
    sys.stderr.flush()
    errors = []
    with tempfile.TemporaryDirectory(prefix="regression_disasm.") as tmpdir:
        tmpdir = Path(tmpdir)
        results = []
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            future_to_test = {}
            for i, (suite, src, flags) in enumerate(tests):
                f = ex.submit(process_one, i + 1, total, suite, src, tmpdir, dump_dir, gcc_opt, flags, tcc_opt)
                future_to_test[f] = (suite, src)
            eprint(f"Submitted {len(future_to_test)} futures, waiting ...")
            sys.stderr.flush()

            pending = set(future_to_test.keys())
            while pending:
                done, pending = wait(pending, timeout=20, return_when=FIRST_COMPLETED)
                if not done:
                    eprint(f"STUCK: {len(pending)} test(s) still running after 20s:")
                    for f in list(pending)[:20]:
                        s, src = future_to_test[f]
                        eprint(f"  {s}/{Path(src).stem}")
                    sys.stderr.flush()
                for future in done:
                    try:
                        results.append(future.result())
                    except Exception as exc:
                        s, src = future_to_test[future]
                        key = f"{s}/{Path(src).stem}"
                        eprint(f"ERROR processing {key}: {exc}")
                        errors.append(key)
        eprint(f"All {total} tests done ({len(errors)} errors).")
        sys.stderr.flush()
        return results, errors


def collect_data(results):
    func_tcc = {}
    func_gcc = {}
    all_entries = []
    skipped = []
    total_tcc = 0
    total_gcc = 0
    seen_tests = set()

    for res in results:
        if res["type"] == "skip":
            skipped.append(f"{res['key']} # {res['reason']}")
        else:
            key = res["key"]
            seen_tests.add(key)
            for func, tc, gc in res["funcs"]:
                fkey = f"{key}::{func}"
                func_tcc[fkey] = tc
                func_gcc[fkey] = gc
                all_entries.append(fkey)
                total_tcc += tc
                total_gcc += gc

    return {
        "func_tcc": func_tcc,
        "func_gcc": func_gcc,
        "all_entries": all_entries,
        "skipped": skipped,
        "total_tcc": total_tcc,
        "total_gcc": total_gcc,
        "test_count": len(seen_tests),
        "func_count": len(all_entries),
    }


def output_csv(data, gcc_opt, tcc_opt="-O2"):
    print("suite,test,function,tcc_{},gcc_{},ratio".format(
        tcc_opt.lstrip("-"), gcc_opt.lstrip("-")))
    for key in sorted(data["all_entries"]):
        test_key, func_name = key.split("::", 1)
        suite = test_key.split("/", 1)[0]
        test_name = test_key.split("/", 1)[1]
        tcc_n = data["func_tcc"][key]
        gcc_n = data["func_gcc"][key]
        ratio = f"{tcc_n / gcc_n:.2f}" if gcc_n > 0 else "N/A"
        print(f"{suite},{test_name},{func_name},{tcc_n},{gcc_n},{ratio}")


def save_baseline(data, name, gcc_opt):
    path = SCRIPT_DIR / f"{name}.csv"
    head = subprocess.run(["git", "-C", str(TCC_DIR), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    rev = head.stdout.strip() if head.returncode == 0 else "unknown"
    with open(path, "w") as f:
        f.write(f"# Baseline: {subprocess.run(['date', '-Iseconds'], capture_output=True, text=True).stdout.strip()} GCC_OPT={gcc_opt} TCC={rev}\n")
        f.write(f"test,function,tcc_O2,gcc_{gcc_opt}\n")
        for key in sorted(data["all_entries"]):
            test_key, func_name = key.split("::", 1)
            f.write(f"{test_key},{func_name},{data['func_tcc'][key]},{data['func_gcc'][key]}\n")
    print(f"Baseline saved to {path} ({data['func_count']} functions from {data['test_count']} tests)")


def diff_baseline(data, name, gcc_opt):
    path = SCRIPT_DIR / f"{name}.csv"
    if not path.exists():
        eprint(f"ERROR: Baseline file not found: {path}")
        sys.exit(1)

    base_tcc = {}
    with open(path) as f:
        reader = csv.reader(f)
        for row in reader:
            if not row or row[0].startswith("#") or row[0] == "test":
                continue
            base_tcc[f"{row[0]}::{row[1]}"] = int(row[2])

    regressions = []
    improvements = []
    unchanged = 0
    new_funcs = []
    removed_funcs = []

    current_keys = set(data["all_entries"])
    for key in sorted(data["all_entries"]):
        tcc_now = data["func_tcc"][key]
        if key not in base_tcc:
            new_funcs.append(f"{key} ({tcc_now} instr)")
            continue
        delta = tcc_now - base_tcc[key]
        if delta > 0:
            regressions.append(f"{key:<50} {base_tcc[key]:4d} -> {tcc_now:4d}  (+{delta})")
        elif delta < 0:
            improvements.append(f"{key:<50} {base_tcc[key]:4d} -> {tcc_now:4d}  ({delta})")
        else:
            unchanged += 1
        del base_tcc[key]

    for key, val in base_tcc.items():
        removed_funcs.append(f"{key} (was {val} instr)")

    print("=== Regression Report: TCC -O2 instruction counts ===")
    print()
    if regressions:
        print(f"REGRESSIONS ({len(regressions)} functions got worse):")
        for line in regressions:
            print(f"  [!] {line}")
        print()
    if improvements:
        print(f"IMPROVEMENTS ({len(improvements)} functions got better):")
        for line in improvements:
            print(f"  [+] {line}")
        print()
    print(f"Unchanged: {unchanged} functions")
    if new_funcs:
        print()
        print(f"NEW ({len(new_funcs)} functions added):")
        for line in new_funcs:
            print(f"  [N] {line}")
    if removed_funcs:
        print()
        print(f"REMOVED ({len(removed_funcs)} functions no longer present):")
        for line in removed_funcs:
            print(f"  [R] {line}")
    print()
    print(f"Total TCC instructions: {data['total_tcc']}")


def print_summary(data, gcc_opt):
    print("=" * 60)
    print(f"  TCC -O2 vs GCC {gcc_opt} — Instruction Count Summary")
    print(f"  Tests compiled: {data['test_count']}  Functions compared: {data['func_count']}")
    print("=" * 60)
    print()

    buckets = {"better": 0, "close": 0, "ok": 0, "warn": 0, "bad": 0}
    worst = []
    for key in data["all_entries"]:
        tcc_n = data["func_tcc"][key]
        gcc_n = data["func_gcc"][key]
        if gcc_n == 0:
            continue
        ratio100 = tcc_n * 100 // gcc_n
        if ratio100 < 100:
            buckets["better"] += 1
        elif ratio100 < 120:
            buckets["close"] += 1
        elif ratio100 < 150:
            buckets["ok"] += 1
        elif ratio100 < 200:
            buckets["warn"] += 1
        else:
            buckets["bad"] += 1
        if ratio100 >= 150:
            worst.append((ratio100, key, tcc_n, gcc_n))

    print("Distribution of TCC/GCC ratios:")
    print()
    print(f"  {'TCC better    (< 1.0x):':<24} {buckets['better']:>5d} functions")
    print(f"  {'Close match   (1.0-1.2x):':<24} {buckets['close']:>5d} functions")
    print(f"  {'Acceptable    (1.2-1.5x):':<24} {buckets['ok']:>5d} functions")
    print(f"  {'Needs work    (1.5-2.0x):':<24} {buckets['warn']:>5d} functions")
    print(f"  {'Poor          (>= 2.0x):':<24} {buckets['bad']:>5d} functions")
    print()

    if data["total_gcc"] > 0:
        overall = data["total_tcc"] / data["total_gcc"]
        print(f"Overall: {data['total_tcc']} TCC instr / {data['total_gcc']} GCC instr = {overall:.2f}x")
    else:
        print(f"Overall: {data['total_tcc']} TCC instr / {data['total_gcc']} GCC instr")
    print()

    suite_tcc = defaultdict(int)
    suite_gcc = defaultdict(int)
    suite_funcs = defaultdict(int)
    for key in data["all_entries"]:
        suite = key.split("/", 1)[0]
        suite_tcc[suite] += data["func_tcc"][key]
        suite_gcc[suite] += data["func_gcc"][key]
        suite_funcs[suite] += 1

    print("--- Per-suite breakdown ---")
    print()
    print(f"  {'suite':<20}  {'funcs':>6}  {'TCC':>6}  {'GCC':>6}  {'ratio':>6}")
    print(f"  {'-'*20}  {'-'*6}  {'-'*6}  {'-'*6}  {'-'*6}")
    for s in sorted(suite_tcc.keys()):
        st, sg, sf = suite_tcc[s], suite_gcc[s], suite_funcs[s]
        ratio = f"{st / sg:.2f}" if sg > 0 else "N/A"
        print(f"  {s:<20}  {sf:>6}  {st:>6}  {sg:>6}  {ratio:>5s}x")
    print()

    if worst:
        print("--- Worst ratios (>= 1.5x TCC/GCC, top 30) ---")
        print()
        print(f"  {'test::function':<50}  {'TCC':>6}  {'GCC':>6}  {'ratio':>6}")
        print(f"  {'-'*50}  {'-'*6}  {'-'*6}  {'-'*6}")
        for ratio100, key, tcc_n, gcc_n in sorted(worst, key=lambda x: -x[0])[:30]:
            print(f"  {key:<50}  {tcc_n:>6}  {gcc_n:>6}  {ratio100/100:>5.2f}x")
        print()

    abs_worst = []
    for key in data["all_entries"]:
        tcc_n = data["func_tcc"][key]
        gcc_n = data["func_gcc"][key]
        diff = tcc_n - gcc_n
        if diff > 0:
            abs_worst.append((diff, key, tcc_n, gcc_n))
    if abs_worst:
        print("--- Largest absolute diffs (TCC - GCC instr, top 30) ---")
        print()
        print(f"  {'test::function':<50}  {'TCC':>6}  {'GCC':>6}  {'diff':>6}")
        print(f"  {'-'*50}  {'-'*6}  {'-'*6}  {'-'*6}")
        for diff, key, tcc_n, gcc_n in sorted(abs_worst, key=lambda x: -x[0])[:30]:
            print(f"  {key:<50}  {tcc_n:>6}  {gcc_n:>6}  {diff:>+6d}")
        print()

    test_tcc = defaultdict(int)
    test_gcc = defaultdict(int)
    for key in data["all_entries"]:
        test_key = key.split("::", 1)[0]
        test_tcc[test_key] += data["func_tcc"][key]
        test_gcc[test_key] += data["func_gcc"][key]

    test_sorted = []
    for tk, tv in test_tcc.items():
        gv = test_gcc[tk]
        r = tv * 100 // gv if gv > 0 else 0
        test_sorted.append((r, tk, tv, gv))

    print("--- Per-test totals (top 30 worst ratios) ---")
    print()
    print(f"  {'test':<50}  {'TCC':>6}  {'GCC':>6}  {'ratio':>6}")
    print(f"  {'-'*50}  {'-'*6}  {'-'*6}  {'-'*6}")
    for r, tk, tv, gv in sorted(test_sorted, key=lambda x: -x[0])[:30]:
        ratio = f"{r/100:.2f}" if gv > 0 else "N/A"
        print(f"  {tk:<50}  {tv:>6}  {gv:>6}  {ratio:>5s}x")
    print()

    test_diff_sorted = [(tv - test_gcc[tk], tk, tv, test_gcc[tk]) for tk, tv in test_tcc.items()]
    print("--- Per-test totals (top 30 largest absolute diffs) ---")
    print()
    print(f"  {'test':<50}  {'TCC':>6}  {'GCC':>6}  {'diff':>6}")
    print(f"  {'-'*50}  {'-'*6}  {'-'*6}  {'-'*6}")
    for d, tk, tv, gv in sorted(test_diff_sorted, key=lambda x: -x[0])[:30]:
        print(f"  {tk:<50}  {tv:>6}  {gv:>6}  {d:>+6d}")
    print()

    if data["skipped"]:
        print(f"Skipped: {len(data['skipped'])} tests")
        print()

    print("Tip: use --save <name> to save a baseline, --diff <name> to compare later")
    print("     use --dump-dir <path> to save per-test .dump files")
    print("     use --csv for machine-readable output")
    print("     use --suite ir|tests2|float|gcc-compile|gcc-execute|bug|all")


def graph_bar(left, right, width=30):
    maxv = max(left, right, 1)
    left_w = left * width // maxv
    right_w = right * width // maxv
    return "=" * left_w + "|" + "#" * right_w


def print_graph(left_csv_text: str, right_csv_text: str, left_label: str, right_label: str, gcc_opt: str):
    def agg(csv_text):
        reader = csv.DictReader(csv_text.splitlines())
        test_tcc = defaultdict(int)
        suite_tcc = defaultdict(int)
        suite_gcc = defaultdict(int)
        gcc_col = f"gcc_{gcc_opt}"
        for row in reader:
            test_key = row["suite"] + "/" + row["test"]
            test_tcc[test_key] += int(row["tcc_O2"])
            suite_tcc[row["suite"]] += int(row["tcc_O2"])
            suite_gcc[row["suite"]] += int(row[gcc_col])
        return test_tcc, suite_tcc, suite_gcc

    left_test, left_suite, _ = agg(left_csv_text)
    right_test, right_suite, _ = agg(right_csv_text)

    total_left = sum(left_test.values())
    total_right = sum(right_test.values())
    delta = total_right - total_left
    pct = f"{delta * 100 / total_left:.2f}" if total_left > 0 else "0"

    print("=" * 60)
    print(f"  Graph: {left_label} -> {right_label}")
    print("=" * 60)
    print()
    print(f"  {'Metric':<20}  {left_label:>10}  {right_label:>10}  {'delta':>10}  {'pct':>10}")
    print(f"  {'-'*20}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*10}")
    print(f"  {'Total TCC instr':<20}  {total_left:>10}  {total_right:>10}  {delta:>10}  {pct:>9}%")
    print()

    print("--- Per-suite TCC instructions ---")
    print()
    print(f"  {'suite':<20}  {left_label:>10}  {right_label:>10}  {'delta':>10}  {'pct':>10}  visual")
    print(f"  {'-'*20}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*30}")
    for s in sorted(set(left_suite.keys()) | set(right_suite.keys())):
        l = left_suite.get(s, 0)
        r = right_suite.get(s, 0)
        d = r - l
        p = f"{d * 100 / l:.1f}" if l > 0 else "0"
        print(f"  {s:<20}  {l:>10}  {r:>10}  {d:>10}  {p:>9}%  {graph_bar(l, r)}")
    print()

    print("--- Top test changes ---")
    print()
    print(f"  {'test':<40}  {left_label:>10}  {right_label:>10}  {'delta':>10}  {'pct':>10}  visual")
    print(f"  {'-'*40}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*30}")
    all_tests = set(left_test.keys()) | set(right_test.keys())
    rows = []
    for k in all_tests:
        l = left_test.get(k, 0)
        r = right_test.get(k, 0)
        d = r - l
        rows.append((abs(d), k, l, r, d))
    for _, k, l, r, d in sorted(rows, key=lambda x: -x[0])[:30]:
        p = f"{d * 100 / l:.1f}" if l > 0 else "N/A"
        print(f"  {k:<40}  {l:>10}  {r:>10}  {d:>10}  {p:>9}%  {graph_bar(l, r)}")
    print()


def build_tcc_at_rev(rev: str, jobs: int):
    build_dir = Path(tempfile.mkdtemp(prefix=f"tcc_graph_{rev.replace('~', '_')}."))
    eprint(f"Building TCC for '{rev}' in {build_dir} ...")
    if rev == "staged":
        run(["git", "-C", str(TCC_DIR), "checkout-index", "--all", "--prefix=" + str(build_dir) + "/"])
    else:
        archive = run(["git", "-C", str(TCC_DIR), "archive", rev], stdout=subprocess.PIPE)
        subprocess.run(["tar", "-x", "-C", str(build_dir)], input=archive.stdout, check=True)
    configure = run(["./configure"], cwd=str(build_dir), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if configure.returncode != 0:
        eprint(f"ERROR: configure failed for {rev}")
        shutil.rmtree(build_dir)
        sys.exit(1)
    make = run(["make", "cross", f"-j{jobs}"], cwd=str(build_dir), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if make.returncode != 0:
        eprint(f"ERROR: make cross failed for {rev}")
        shutil.rmtree(build_dir)
        sys.exit(1)
    return str(build_dir / "armv8m-tcc"), build_dir


def run_csv_mode(gcc_opt, dump_dir, suite, jobs, tcc_override=None, tcc_opt="-O2"):
    env = dict(os.environ)
    if tcc_override:
        env["TCC_OVERRIDE"] = tcc_override
    # --tcc-opt uses the "=" form so its "-O1" value isn't swallowed by the
    # bare-"-O" scan in parse_args (which routes any standalone -O<n> to gcc_opt).
    cmd = [sys.executable, __file__, "--csv", "--no-cache", "--suite", suite,
           "-j", str(jobs), f"--tcc-opt={tcc_opt}", gcc_opt]
    if dump_dir:
        cmd += ["--dump-dir", dump_dir]
    # stderr is left INHERITED (not captured) so the child's per-test progress
    # ("[i/N] suite/test ... OK/SKIP") streams to the terminal / CI log in real
    # time instead of appearing all at once when the multi-minute run finishes.
    # Only stdout (the CSV) is captured.
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, text=True, env=env)
    # A crashed child (e.g. the test-corpus import failing because pytest is not
    # installed in the interpreter running this) writes no CSV to stdout. Without
    # this check that silently became "0 funcs" recorded with exit 0 -- the DB
    # (and Grafana) showed zeros for every run. Fail loudly instead.
    if proc.returncode != 0:
        raise RuntimeError(
            f"regression_disasm.py --csv (suite={suite}) exited "
            f"{proc.returncode}; no code-size data collected")
    lines = [line for line in proc.stdout.splitlines() if re.match(r"^(suite|ir|float|bug|func-sections|gnu89-inline|pic-tds|gcc-compile|gcc-execute),", line) or line.startswith("suite,")]
    return "\n".join(lines)


def run_cache_check(data, no_cache, overwrite=False):
    if no_cache:
        return
    cache = DisasmCache()
    if overwrite:
        cache.data = {}
    func_results = []
    for key in data["all_entries"]:
        test_key, func_name = key.split("::", 1)
        tcc_n = data["func_tcc"][key]
        gcc_n = data["func_gcc"][key]
        func_results.append((key, tcc_n, gcc_n))

    report = cache.check_regressions([(k, t, g) for k, t, g in func_results], mutate=True)
    if overwrite:
        cache.save()
        cache.save_pending()
    else:
        cache.save_pending()
    cache.print_report(report)
    if overwrite:
        eprint(f"  Overwrote {cache.path.name} and {cache.pending_path.name}")
    else:
        eprint(f"  Staged cache to {cache.pending_path.name} — promote with --update-cache")
    return report


def parse_args():
    parser = argparse.ArgumentParser(description="Regression disassembly comparison")
    parser.add_argument("-O0", "-O1", "-O2", "-O3", "-Os", dest="gcc_opt", action="store_const", const=lambda x: x)
    parser.add_argument("-j", type=int, default=os.cpu_count() or 4, help="parallel jobs")
    parser.add_argument("--suite", default="all", help="test suite filter")
    parser.add_argument("--save", metavar="NAME", help="save baseline")
    parser.add_argument("--diff", metavar="NAME", help="diff against baseline")
    parser.add_argument("--csv", action="store_true", help="output CSV")
    parser.add_argument("--dump-dir", help="save dumps")
    parser.add_argument("--graph", nargs="+", help="graph mode: --graph REV [REV2]")
    parser.add_argument("--no-cache", action="store_true", help="disable cache tracking")
    parser.add_argument("--overwrite", action="store_true", help="force overwrite all cache entries in both main and pending cache")
    parser.add_argument("--update-cache", action="store_true",
                        help="promote the pending cache file to the main cache and exit without re-running")
    parser.add_argument("--discard-pending", action="store_true",
                        help="delete the pending cache file and exit")
    parser.add_argument("--tcc-opt", default="-O2",
                        help="TCC optimization level for the corpus compile "
                             "(default -O2). The bare -O<n> on the command line "
                             "still sets the GCC level; use --tcc-opt=-O1 to vary "
                             "the TCC level independently.")

    raw = sys.argv[1:]
    gcc_opt = "-O2"
    cleaned = []
    i = 0
    while i < len(raw):
        if raw[i] in ("-O0", "-O1", "-O2", "-O3", "-Os"):
            gcc_opt = raw[i]
        else:
            cleaned.append(raw[i])
        i += 1

    args = parser.parse_args(cleaned)
    args.gcc_opt = gcc_opt
    return args


if __name__ == "__main__":
    args = parse_args()

    if args.update_cache:
        if DisasmCache.promote_pending():
            eprint(f"Promoted {DisasmCache().pending_path.name} -> {DisasmCache().path.name}")
        else:
            eprint(f"No pending cache file to promote (expected at {DisasmCache().pending_path}).")
            sys.exit(1)
        sys.exit(0)

    if args.discard_pending:
        if DisasmCache.discard_pending():
            eprint(f"Discarded {DisasmCache().pending_path.name}")
        else:
            eprint(f"No pending cache file to discard.")
        sys.exit(0)

    TCC = get_tcc_path()

    if not TCC.exists() or not os.access(TCC, os.X_OK):
        eprint(f"ERROR: TCC not found at {TCC} — run 'make cross' first")
        sys.exit(1)
    for tool in ("arm-none-eabi-gcc", "arm-none-eabi-objdump", "arm-none-eabi-nm"):
        if shutil.which(tool) is None:
            eprint(f"ERROR: {tool} not found in PATH")
            sys.exit(1)

    if args.graph:
        for tool in ("git", "make"):
            if shutil.which(tool) is None:
                eprint(f"ERROR: {tool} not found in PATH (required for --graph)")
                sys.exit(1)

        left = args.graph[0]
        right = args.graph[1] if len(args.graph) > 1 else "working"
        eprint(f"Graph mode: comparing '{left}' vs '{right}' ...")

        left_tcc = str(TCC) if left == "working" else None
        right_tcc = str(TCC) if right == "working" else None
        left_build = None
        right_build = None

        if left_tcc is None:
            left_tcc, left_build = build_tcc_at_rev(left, args.j)
        if right_tcc is None:
            right_tcc, right_build = build_tcc_at_rev(right, args.j)

        eprint(f"Running disasm for '{left}' ...")
        left_csv = run_csv_mode(args.gcc_opt, args.dump_dir, args.suite, args.j, left_tcc)
        eprint(f"Running disasm for '{right}' ...")
        right_csv = run_csv_mode(args.gcc_opt, args.dump_dir, args.suite, args.j, right_tcc)

        print_graph(left_csv, right_csv, left, right, args.gcc_opt)

        if left_build:
            shutil.rmtree(left_build, ignore_errors=True)
        if right_build:
            shutil.rmtree(right_build, ignore_errors=True)
        sys.exit(0)

    tests = collect_tests(args.suite)
    if not tests:
        eprint("ERROR: No tests discovered. Check Python imports.")
        sys.exit(1)

    results, errors = run_all(tests, args.j, args.gcc_opt, args.dump_dir or "", args.suite, args.tcc_opt)
    eprint("Collecting data ...")
    data = collect_data(results)

    if args.csv:
        output_csv(data, args.gcc_opt, args.tcc_opt)
    elif args.save:
        save_baseline(data, args.save, args.gcc_opt)
    elif args.diff:
        diff_baseline(data, args.diff, args.gcc_opt)
    else:
        print_summary(data, args.gcc_opt)

    eprint("Updating cache ...")
    run_cache_check(data, args.no_cache, args.overwrite)

    compile_failures = [s for s in data["skipped"] if "compile failed" in s]
    no_common = [s for s in data["skipped"] if "no common functions" in s]
    no_funcs = [s for s in data["skipped"] if "no functions" in s and "no common" not in s]
    other_skips = [s for s in data["skipped"]
                   if "compile failed" not in s and "no common functions" not in s
                   and "no functions" not in s]

    if no_common:
        eprint(f"\nInfo: {len(no_common)} test(s) skipped (no common functions)")
    if no_funcs:
        eprint(f"\nInfo: {len(no_funcs)} test(s) skipped (no functions in object)")

    failed = errors + compile_failures + other_skips
    if failed:
        eprint(f"\nFAILED: {len(failed)} test(s):")
        for f in failed:
            eprint(f"  {f}")
        sys.exit(1)
