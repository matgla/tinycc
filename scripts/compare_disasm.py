#!/usr/bin/env python3
"""
Compare disassemblies between TCC -O2 and GCC at a selectable opt level.

Usage:
  ./scripts/compare_disasm.py [options] [test_file.c|bubble|fibonacci] [function_name]

Options (must come before positional args):
  -O0 / -O1 / -O2 / -O3   GCC optimization level (default: -O2)
  --no-cache               disable cache updates
  --update-cache           promote pending cache to main, no rerun
  --discard-pending        delete pending cache file, no rerun

Two-phase cache workflow:
  Each measurement run stages the would-be cache to .disasm_cache.pending.json.
  The main cache (.disasm_cache.json) is only replaced when you later invoke
  with --update-cache, which renames the pending file without re-measuring.
"""

import sys
import tempfile
from pathlib import Path

from disasm_common import (
    DisasmCache,
    TCC_DIR,
    compile_gcc,
    compile_tcc,
    compare_functions,
    disassemble,
    eprint,
    extract_all_function_disasm,
    get_common_functions,
    get_tcc_path,
    get_transitive_callees,
)

PRESET_BUBBLE = """\
/* Bubble sort from benchmarks - tests nested loops and array access */
void bubble_sort(int *arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}
"""

PRESET_FIBONACCI = """\
/* Fibonacci from benchmarks - tests recursion */
static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int fibonacci(int n) {
    return fib(n);
}
"""

PRESET_DEFAULT = """\
// Test functions for disassembly comparison

int sum_array(int *p, int n) {
    int sum = 0;
    while (n-- > 0)
        sum += *p++;
    return sum;
}

int dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int fibonacci(int n) {
    if (n <= 1) return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
}

int max(int a, int b) {
    return (a > b) ? a : b;
}

int absolute(int x) {
    return (x < 0) ? -x : x;
}
"""


def parse_args():
    raw = sys.argv[1:]
    gcc_opt = "-O2"
    no_cache = False
    update_cache = False
    discard_pending = False
    positional = []

    for arg in raw:
        if arg in ("-O0", "-O1", "-O2", "-O3", "-Os"):
            gcc_opt = arg
        elif arg == "--no-cache":
            no_cache = True
        elif arg == "--update-cache":
            update_cache = True
        elif arg == "--discard-pending":
            discard_pending = True
        else:
            positional.append(arg)

    test_file = positional[0] if positional else None
    func_filter = positional[1] if len(positional) > 1 else None

    return gcc_opt, test_file, func_filter, no_cache, update_cache, discard_pending


def prepare_test_file(test_arg):
    if test_arg == "bubble":
        path = Path("/tmp/disasm_bubble_sort.c")
        path.write_text(PRESET_BUBBLE)
        print("Using bubble sort example (from benchmarks)")
        return path, "bubble_sort"
    elif test_arg == "fibonacci":
        path = Path("/tmp/disasm_fibonacci.c")
        path.write_text(PRESET_FIBONACCI)
        print("Using fibonacci example (from benchmarks)")
        return path, "fib"
    elif test_arg:
        path = Path(test_arg)
        if not path.exists():
            eprint(f"ERROR: File not found: {path}")
            sys.exit(1)
        return path, None
    else:
        path = Path("/tmp/disasm_test.c")
        if not path.exists():
            path.write_text(PRESET_DEFAULT)
            print(f"Created default test file: {path}")
        return path, None


def print_usage():
    print("Usage: compare_disasm.py [options] [test_file.c|bubble|fibonacci] [function_name]")
    print()
    print("Options:")
    print("  -O0 / -O1 / -O2 / -O3   GCC optimization level (default: -O2)")
    print("  --no-cache               disable cache updates")
    print("  --update-cache           promote pending cache to main, no rerun")
    print("  --discard-pending        delete pending cache file, no rerun")
    print()
    print("Examples:")
    print("  compare_disasm.py                          # Use default test file")
    print("  compare_disasm.py mytest.c                 # Use your own C file")
    print("  compare_disasm.py mytest.c my_function     # Compare specific function")
    print("  compare_disasm.py bubble                   # Use bubble sort benchmark")
    print("  compare_disasm.py fibonacci                # Use fibonacci benchmark")
    print()


def resolve_cache_key_prefix(cache, basename):
    """Return the cache prefix matching <basename>, mirroring regression_disasm's
    '<suite>/<basename>::<func>' layout. Falls back to the bare basename when no
    matching prefix is present in the cache."""
    for key in cache.data:
        if "::" not in key:
            continue
        prefix = key.split("::", 1)[0]
        if prefix == basename or prefix.endswith(f"/{basename}"):
            return prefix
    return basename


def print_summary_table(func_results, gcc_opt):
    print("========================================")
    print(f"  Summary: TCC -O2 vs GCC {gcc_opt}")
    print("========================================")
    print()
    print(f"  {'Function':<30} {'TCC':>8} {'GCC':>8} {'Ratio':>8}")
    print(f"  {'-'*30} {'-'*8} {'-'*8} {'-'*8}")

    total_tcc = 0
    total_gcc = 0
    for func, tc, gc in func_results:
        total_tcc += tc
        total_gcc += gc
        ratio = f"{tc / gc:.2f}x" if gc > 0 else "N/A"
        print(f"  {func:<30} {tc:>8} {gc:>8} {ratio:>8}")

    print(f"  {'-'*30} {'-'*8} {'-'*8} {'-'*8}")
    total_ratio = f"{total_tcc / total_gcc:.2f}x" if total_gcc > 0 else "N/A"
    print(f"  {'TOTAL':<30} {total_tcc:>8} {total_gcc:>8} {total_ratio:>8}")
    print()


def print_side_by_side(tcc_lines, gcc_lines, func_name, gcc_opt):
    if not tcc_lines and not gcc_lines:
        print("  (function not found in either output)")
        return

    print(f"  {'TCC -O2':<80} | GCC {gcc_opt}")
    print(f"  {'-'*80}-+-{'-'*80}")

    max_lines = max(len(tcc_lines), len(gcc_lines))
    for i in range(max_lines):
        tcc_line = tcc_lines[i].expandtabs() if i < len(tcc_lines) else ""
        gcc_line = gcc_lines[i].expandtabs() if i < len(gcc_lines) else ""
        print(f"  {tcc_line:<80.80} | {gcc_line}")

    print()


def main():
    gcc_opt, test_arg, func_filter, no_cache, update_cache, discard_pending = parse_args()

    if update_cache:
        if DisasmCache.promote_pending():
            eprint(f"Promoted {DisasmCache().pending_path.name} -> {DisasmCache().path.name}")
            sys.exit(0)
        eprint(f"No pending cache file to promote (expected at {DisasmCache().pending_path}).")
        sys.exit(1)

    if discard_pending:
        if DisasmCache.discard_pending():
            eprint(f"Discarded {DisasmCache().pending_path.name}")
        else:
            eprint(f"No pending cache file to discard.")
        sys.exit(0)

    if test_arg is None:
        print_usage()

    test_file, preset_func = prepare_test_file(test_arg)
    if func_filter is None and preset_func:
        func_filter = preset_func

    tcc = get_tcc_path()
    if not tcc.exists():
        eprint(f"ERROR: TCC not found at {tcc} — run 'make cross' first")
        sys.exit(1)

    with tempfile.TemporaryDirectory(prefix="compare_disasm.") as tmpdir:
        tmpdir = Path(tmpdir)
        tcc_obj = tmpdir / "tcc.o"
        gcc_obj = tmpdir / "gcc.o"

        print(f"=== Compiling {test_file} ===")
        print()

        r = compile_tcc(test_file, tcc_obj)
        if r.returncode != 0:
            eprint(f"TCC compilation failed:\n{r.stderr}")
            sys.exit(1)

        r = compile_gcc(test_file, gcc_obj, opt=gcc_opt)
        if r.returncode != 0:
            eprint(f"GCC compilation failed:\n{r.stderr}")
            sys.exit(1)

        tcc_dump = disassemble(tcc_obj)
        gcc_dump = disassemble(gcc_obj)

        common, tcc_funcs, gcc_funcs = get_common_functions(
            tcc_obj, gcc_obj, include_local=True
        )

        print("Available functions in TCC output:")
        for f in sorted(tcc_funcs):
            print(f"  {f}")
        print()
        print("Available functions in GCC output:")
        for f in sorted(gcc_funcs):
            print(f"  {f}")
        print()

        if func_filter:
            roots = [func_filter]
        else:
            roots = common

        if not roots:
            print("No functions to compare!")
            sys.exit(1)

        both = tcc_funcs & gcc_funcs
        tcc_reachable = get_transitive_callees(tcc_dump, roots, tcc_funcs)
        gcc_reachable = get_transitive_callees(gcc_dump, roots, gcc_funcs)
        funcs_to_compare = sorted((tcc_reachable | gcc_reachable) & both)

        func_results = compare_functions(tcc_dump, gcc_dump, funcs_to_compare)

        print_summary_table(func_results, gcc_opt)

        # Extract every function's disasm block once, not per-function in the loop.
        result_funcs = [func for func, _, _ in func_results]
        tcc_disasm = extract_all_function_disasm(tcc_dump, result_funcs)
        gcc_disasm = extract_all_function_disasm(gcc_dump, result_funcs)

        if not no_cache:
            cache = DisasmCache()
            key_prefix = resolve_cache_key_prefix(cache, test_file.stem)
            report = cache.check_regressions(func_results, key_prefix)
            cache.save_pending()
            cache.print_report(report)
            print(f"  Cache key prefix: {key_prefix}")
            print(f"  Staged cache to {cache.pending_path.name} — promote with --update-cache")

        for func, tcc_count, gcc_count in func_results:
            print("========================================")
            print(f"  Function: {func}")
            print("========================================")
            print()
            print(f"  TCC -O2:  {tcc_count:5d} instructions")
            print(f"  GCC {gcc_opt}:  {gcc_count:5d} instructions")
            if gcc_count > 0:
                ratio = tcc_count / gcc_count
                print(f"  Ratio:        {ratio:.2f} (TCC/GCC)")
            print()

            print_side_by_side(tcc_disasm.get(func, []), gcc_disasm.get(func, []), func, gcc_opt)

    print()
    print("========================================")
    print("  Dump files cleaned up (were in tmpdir)")
    print("========================================")


if __name__ == "__main__":
    main()
