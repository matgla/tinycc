"""
Shared utilities for disassembly comparison scripts.

Provides compilation, disassembly, instruction counting, function extraction,
and a best-known-result cache for TCC vs GCC code size tracking.
"""

import json
import os
import re
import signal
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

SOURCES_DIR = Path(__file__).resolve().parent
SCRIPT_DIR = SOURCES_DIR.parent
TCC_DIR = SCRIPT_DIR.parent
DEFAULT_TCC = TCC_DIR / "armv8m-tcc"
CACHE_FILE = SCRIPT_DIR / ".disasm_cache.json"
PENDING_CACHE_FILE = SCRIPT_DIR / ".disasm_cache.pending.json"
BASELINE_DIR = TCC_DIR / "metrics" / "baselines"

_HEADER_RE = re.compile(r'^[0-9a-f]+ <.*>:$')
_HEADER_NAME_RE = re.compile(r'<(.+)>:')
_INST_RE = re.compile(r'^\s+[0-9a-f]+:')
_GCC_CLONE_SUFFIXES = ('.part.', '.constprop.', '.isra.', '.cold.')
_DATA_DIRECTIVES = ('.word', '.short', '.byte')
_ALIGNMENT_MNEMONICS = ('nop',)


def _is_mapping_symbol(name):
    """ARM AAELF mapping symbols: $t/$a/$d, optionally suffixed ($t.2)."""
    return (name and name[0] == '$' and len(name) >= 2 and name[1] in 'tad'
            and (len(name) == 2 or name[2] == '.'))


def get_mapping_data_ranges(obj_file):
    """[(start, end)] address ranges covered by $d..$t/$a mapping symbols.

    These are the data-in-text regions (literal pools, switch tables) that a
    disassembler treats as data.  Used to tell a properly-marked `.word` run
    from a real disassembly desync.  A `$d` with no following code-mapping
    symbol extends to the end of its section (end-of-function pool)."""
    result = run(["arm-none-eabi-objdump", "-t", "--special-syms", str(obj_file)])
    per_section = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        # e.g. "00000390 l  .text  00000000 $d"
        if len(parts) < 5 or not _is_mapping_symbol(parts[-1]):
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        sec, kind = parts[-3], parts[-1][:2]
        per_section.setdefault(sec, []).append((addr, kind))
    ranges = []
    for sec, syms in per_section.items():
        syms.sort()
        open_d = None
        for addr, kind in syms:
            if kind == '$d':
                if open_d is None:
                    open_d = addr
            elif open_d is not None:
                ranges.append((open_d, addr))
                open_d = None
        if open_d is not None:
            ranges.append((open_d, float('inf')))
    return sorted(ranges)


def _addr_in_ranges(addr, ranges):
    for start, end in ranges:
        if start <= addr < end:
            return True
        if start > addr:
            break
    return False


def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


# Per-compile/disasm subprocess timeout. CI hardware (aarch64 Pi) is far slower
# than a dev laptop, so the default is generous and env-overridable: a heavy
# torture test (e.g. tests2/101_cleanup at -O2) can take well over 30s there.
SUBPROCESS_TIMEOUT = int(os.environ.get("DISASM_SUBPROCESS_TIMEOUT", "120"))


def run(cmd, **kwargs):
    timeout = kwargs.pop("timeout", SUBPROCESS_TIMEOUT)
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, start_new_session=True, **kwargs,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        raise
    return subprocess.CompletedProcess(cmd, proc.returncode, stdout, stderr)


def get_tcc_path():
    return Path(os.environ.get("TCC_OVERRIDE", DEFAULT_TCC))


_IRTESTS_DIR = TCC_DIR / "tests" / "ir_tests"
_TCC_INCLUDE_FLAGS = [
    "-nostdinc",
    "-I", str(_IRTESTS_DIR / "libc_includes"),
    "-I", str(_IRTESTS_DIR / "libc_imports"),
    "-I", str(_IRTESTS_DIR / "libc_includes" / "newlib"),
    "-I", str(TCC_DIR / "include"),
]


def compile_tcc(src, output, tcc=None, opt="-O2", extra_flags=None):
    tcc = tcc or get_tcc_path()
    ef = extra_flags.split() if extra_flags else []
    return run([str(tcc), opt, *ef, *_TCC_INCLUDE_FLAGS, "-c", str(src), "-o", str(output)])


def compile_gcc(src, output, opt="-O2", extra_flags=None):
    cmd = [
        "arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", opt,
        "-std=gnu11", "-Wno-implicit-int", "-Wno-incompatible-pointer-types",
        "-Wno-int-conversion", "-Wno-implicit-function-declaration",
        "-c", str(src), "-o", str(output),
    ]
    if extra_flags:
        cmd[4:4] = extra_flags
    return run(cmd)


def disassemble(obj_file):
    # --special-syms makes ARM mapping symbols ($t/$d) visible as block labels,
    # letting count_all_functions distinguish a properly-marked data-in-text
    # region (pool, switch table) from a disassembly desync.
    result = run(["arm-none-eabi-objdump", "-d", "--special-syms", str(obj_file)])
    return result.stdout


def get_functions(obj_file, include_local=False):
    result = run(["arm-none-eabi-nm", str(obj_file)])
    funcs = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sym_type = parts[1]
        name = parts[2]
        if sym_type == 'T' or (include_local and sym_type == 't'):
            if not any(name.find(s) >= 0 for s in _GCC_CLONE_SUFFIXES):
                funcs.add(name)
    return funcs


_BL_RE = re.compile(r'\bblx?\s+[0-9a-f]+\s+<([^+>]+)>')
_MNEMONIC_RE = re.compile(r'^\s+[0-9a-f]+:\s+(?:[0-9a-f]{4}\s+)+\s*(\S+)')


def _is_non_instruction(line):
    if any(x in line for x in _DATA_DIRECTIVES):
        return True
    m = _MNEMONIC_RE.match(line)
    if m and m.group(1) in _ALIGNMENT_MNEMONICS:
        return True
    return False


def count_instructions(dump_text, func_name):
    in_func = False
    count = 0
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            in_func = f"<{func_name}>:" in line
        elif in_func and _INST_RE.match(line) and not _is_non_instruction(line):
            count += 1
    return count


def count_instructions_with_clones(dump_text, func_name):
    in_func = False
    count = 0
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            m = _HEADER_NAME_RE.search(line)
            if m:
                name = m.group(1)
                in_func = (name == func_name or
                           any(name.startswith(func_name + s) for s in _GCC_CLONE_SUFFIXES))
            else:
                in_func = False
        elif in_func and _INST_RE.match(line) and not _is_non_instruction(line):
            count += 1
    return count


def _strip_clone_suffix(name):
    for s in _GCC_CLONE_SUFFIXES:
        idx = name.find(s)
        if idx >= 0:
            return name[:idx]
    return name


def count_all_functions(dump_text, func_names, with_clones=False, data_ranges=None):
    """Count instructions for all functions in a single pass over the dump text.

    `data_ranges` (from get_mapping_data_ranges) enables a desync tripwire: a
    data directive at an address NOT covered by a $d mapping range, followed by
    more decoded instructions, means objdump hit unmarked data-in-text and lost
    sync — those counts cannot be trusted, so the affected functions are
    reported on stderr.  TCC emits $d/$t mapping symbols for every data-in-text
    region (literal pools, switch tables); a warning here means some emission
    path lost them (this exact failure once mis-scored pr50310::foo by ~90
    instructions).  With data_ranges=None the tripwire is off.

    Rare-binutils compatibility: `<$d>:`/`<$t>:` block labels, when a
    disassembler prints them, are treated as region toggles rather than
    function boundaries."""
    wanted = set(func_names)
    counts = {f: 0 for f in wanted}
    current_func = None
    in_marked_data = False
    saw_unmarked_data = False
    desynced = set()
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            m = _HEADER_NAME_RE.search(line)
            name = m.group(1) if m else None
            if _is_mapping_symbol(name):
                # data/code toggle inside the current function, not a new function
                in_marked_data = name.startswith('$d')
                saw_unmarked_data = False
                continue
            current_func = None
            in_marked_data = False
            saw_unmarked_data = False
            if name:
                if name in wanted:
                    current_func = name
                elif with_clones:
                    base = _strip_clone_suffix(name)
                    if base != name and base in wanted:
                        current_func = base
        elif current_func and not in_marked_data:
            if any(x in line for x in _DATA_DIRECTIVES):
                if data_ranges is not None:
                    try:
                        addr = int(line.split(':', 1)[0].strip(), 16)
                    except ValueError:
                        addr = None
                    if addr is not None and not _addr_in_ranges(addr, data_ranges):
                        saw_unmarked_data = True
            elif _INST_RE.match(line) and not _is_non_instruction(line):
                if saw_unmarked_data:
                    desynced.add(current_func)
                    saw_unmarked_data = False
                counts[current_func] += 1
    if desynced:
        print(f"# WARNING: disassembly desync (code after unmarked data) in: "
              f"{', '.join(sorted(desynced))} — counts for these functions are unreliable "
              f"(missing $d/$t mapping symbols?)", file=sys.stderr)
    return counts


def extract_function_disasm(dump_text, func_name):
    lines = []
    in_func = False
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            m = _HEADER_NAME_RE.search(line)
            name = m.group(1) if m else None
            if _is_mapping_symbol(name):
                # $t/$d block label inside the function, not a boundary
                if in_func:
                    lines.append(line)
                continue
            if in_func:
                break
            in_func = f"<{func_name}>:" in line
        if in_func:
            lines.append(line)
        if in_func and line.strip() == '':
            break
    return lines


def extract_all_function_disasm(dump_text, func_names):
    """Extract the disasm line-block (header through trailing blank line) for
    many functions in a single pass.  Returns {func_name: [lines]}.  Mirrors
    extract_function_disasm but avoids one full scan per function."""
    wanted = set(func_names)
    result = {f: [] for f in wanted}
    current = None
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            m = _HEADER_NAME_RE.search(line)
            name = m.group(1) if m else None
            if _is_mapping_symbol(name):
                if current is not None:
                    result[current].append(line)
                continue
            current = name if name in wanted else None
            if current is not None:
                result[current].append(line)
        elif current is not None:
            result[current].append(line)
            if line.strip() == '':
                current = None
    return result


def get_common_functions(tcc_obj, gcc_obj, include_local=False):
    tcc_funcs = get_functions(tcc_obj, include_local)
    gcc_funcs = get_functions(gcc_obj, include_local)
    return sorted(tcc_funcs & gcc_funcs), tcc_funcs, gcc_funcs


def get_callees_from_disasm(dump_text, func_name):
    in_func = False
    callees = set()
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            in_func = f"<{func_name}>:" in line
        elif in_func:
            m = _BL_RE.search(line)
            if m:
                callees.add(m.group(1))
    return callees


def build_callee_map(dump_text):
    """Map every function name -> set of callee names, in a single pass.

    Avoids re-scanning the whole dump once per function during the
    transitive-callee BFS (the dumps can be 100k+ lines)."""
    callee_map = {}
    current = None
    for line in dump_text.splitlines():
        if _HEADER_RE.match(line):
            m = _HEADER_NAME_RE.search(line)
            current = m.group(1) if m else None
            if current is not None and current not in callee_map:
                callee_map[current] = set()
        elif current is not None:
            m = _BL_RE.search(line)
            if m:
                callee_map[current].add(m.group(1))
    return callee_map


def get_transitive_callees(dump_text, root_funcs, available_funcs):
    callee_map = build_callee_map(dump_text)
    visited = set()
    queue = list(root_funcs)
    while queue:
        func = queue.pop()
        if func in visited:
            continue
        visited.add(func)
        if func not in available_funcs:
            continue
        for callee in callee_map.get(func, ()):
            if callee in available_funcs and callee not in visited:
                queue.append(callee)
    return visited & available_funcs


def compare_functions(tcc_dump, gcc_dump, common_funcs):
    # Single pass over each dump rather than one full scan per function
    # (the dumps can be 100k+ lines for macro-heavy tests).
    tcc_counts = count_all_functions(tcc_dump, common_funcs, with_clones=False)
    gcc_counts = count_all_functions(gcc_dump, common_funcs, with_clones=True)
    return [(func, tcc_counts[func], gcc_counts[func]) for func in common_funcs]


# ── Cache ──

class DisasmCache:
    def __init__(self, path=None, pending_path=None):
        self.path = Path(path) if path else CACHE_FILE
        self.pending_path = Path(pending_path) if pending_path else PENDING_CACHE_FILE
        self.data = {}
        self._load()

    def _load(self):
        if self.path.exists():
            try:
                self.data = json.loads(self.path.read_text())
            except (json.JSONDecodeError, OSError):
                self.data = {}

    def save(self):
        self.path.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n")

    def save_pending(self):
        """Stage the current data to the pending file.  The main cache is
        not touched; promote later via DisasmCache.promote_pending()."""
        self.pending_path.write_text(json.dumps(self.data, indent=2, sort_keys=True) + "\n")

    @staticmethod
    def promote_pending(main_path=None, pending_path=None):
        """Atomically replace the main cache file with the pending file.
        Returns True if a pending file existed and was promoted, False otherwise."""
        main = Path(main_path) if main_path else CACHE_FILE
        pending = Path(pending_path) if pending_path else PENDING_CACHE_FILE
        if not pending.exists():
            return False
        pending.replace(main)  # atomic on POSIX
        return True

    @staticmethod
    def discard_pending(pending_path=None):
        """Remove the pending file if present.  Returns True if removed."""
        pending = Path(pending_path) if pending_path else PENDING_CACHE_FILE
        if not pending.exists():
            return False
        pending.unlink()
        return True

    def get(self, key):
        return self.data.get(key)

    def update_if_better(self, key, tcc_count, gcc_count, mutate=True):
        entry = self.data.get(key)
        if entry is None or tcc_count < entry["tcc"]:
            if mutate:
                self.data[key] = {
                    "tcc": tcc_count,
                    "gcc": gcc_count,
                    "updated": datetime.now(timezone.utc).isoformat(),
                }
            return "improved"
        elif tcc_count > entry["tcc"]:
            if tcc_count <= gcc_count:
                if mutate:
                    self.data[key] = {
                        "tcc": tcc_count,
                        "gcc": gcc_count,
                        "updated": datetime.now(timezone.utc).isoformat(),
                    }
                return "overwritten"
            return "regression"
        return "unchanged"

    def check_regressions(self, func_results, key_prefix="", mutate=True):
        regressions = []
        improvements = []
        overwritten = []
        unchanged = 0
        new_funcs = 0
        suite_stats = {}

        for func, tcc_count, gcc_count in func_results:
            key = f"{key_prefix}::{func}" if key_prefix else func
            suite = key.split("/", 1)[0] if "/" in key else ""
            if suite and suite not in suite_stats:
                suite_stats[suite] = {"improved": 0, "regressed": 0, "unchanged": 0, "new": 0,
                                      "overwritten": 0,
                                      "improved_delta": 0, "regressed_delta": 0,
                                      "total_tcc": 0, "total_gcc": 0,
                                      "better": 0, "close": 0, "ok": 0, "warn": 0, "bad": 0}
            if suite:
                suite_stats[suite]["total_tcc"] += tcc_count
                suite_stats[suite]["total_gcc"] += gcc_count
                if gcc_count > 0:
                    r100 = tcc_count * 100 // gcc_count
                    if r100 < 100:
                        suite_stats[suite]["better"] += 1
                    elif r100 < 120:
                        suite_stats[suite]["close"] += 1
                    elif r100 < 150:
                        suite_stats[suite]["ok"] += 1
                    elif r100 < 200:
                        suite_stats[suite]["warn"] += 1
                    else:
                        suite_stats[suite]["bad"] += 1
            entry = self.data.get(key)
            old_tcc = entry["tcc"] if entry else None
            status = self.update_if_better(key, tcc_count, gcc_count, mutate=mutate)
            if status == "regression":
                cached = self.data[key]
                regressions.append((key, cached["tcc"], tcc_count))
                if suite:
                    suite_stats[suite]["regressed"] += 1
                    suite_stats[suite]["regressed_delta"] += tcc_count - old_tcc
            elif status == "overwritten":
                overwritten.append((key, old_tcc, tcc_count, gcc_count))
                if suite:
                    suite_stats[suite]["overwritten"] += 1
            elif status == "improved":
                delta = (old_tcc - tcc_count) if old_tcc is not None else 0
                improvements.append((key, old_tcc, tcc_count))
                if suite:
                    suite_stats[suite]["improved"] += 1
                    suite_stats[suite]["improved_delta"] += delta
            elif status == "unchanged":
                unchanged += 1
                if suite:
                    suite_stats[suite]["unchanged"] += 1
            else:
                new_funcs += 1
                if suite:
                    suite_stats[suite]["new"] += 1

        return {
            "regressions": regressions,
            "improvements": improvements,
            "overwritten": overwritten,
            "unchanged": unchanged,
            "new": new_funcs,
            "suite_stats": suite_stats,
        }

    def print_report(self, report):
        if report["regressions"]:
            eprint(f"\n  REGRESSIONS ({len(report['regressions'])} functions):")
            for key, cached_tcc, current_tcc in report["regressions"]:
                delta = current_tcc - cached_tcc
                eprint(f"    [!] {key}: {cached_tcc} -> {current_tcc} (+{delta})")

        if report.get("overwritten"):
            eprint(f"\n  OVERWRITTEN ({len(report['overwritten'])} functions, worse but <= GCC):")
            for key, old_tcc, current_tcc, gcc_count in report["overwritten"]:
                delta = current_tcc - old_tcc
                eprint(f"    [~] {key}: {old_tcc} -> {current_tcc} (+{delta}, gcc={gcc_count})")

        if report["improvements"]:
            eprint(f"\n  IMPROVEMENTS ({len(report['improvements'])} functions):")
            for key, old_tcc, tcc_count in report["improvements"]:
                delta = (old_tcc - tcc_count) if old_tcc is not None else 0
                eprint(f"    [+] {key}: {old_tcc} -> {tcc_count} (-{delta})")

        total_changes = (len(report["regressions"]) + len(report["improvements"])
                         + len(report.get("overwritten", [])))
        if total_changes > 0 or report["new"] > 0:
            eprint(f"\n  Summary: {len(report['improvements'])} improved, "
                   f"{len(report['regressions'])} regressed, "
                   f"{len(report.get('overwritten', []))} overwritten, "
                   f"{report['unchanged']} unchanged, "
                   f"{report['new']} new")

        suite_stats = report.get("suite_stats", {})
        if suite_stats:
            eprint(f"\n  --- Per-suite cache delta ---")
            eprint(f"  {'suite':<20}  {'improved':>8}  {'regressed':>9}  {'overwritten':>11}  {'unchanged':>9}  {'new':>5}  {'delta':>8}")
            eprint(f"  {'-'*20}  {'-'*8}  {'-'*9}  {'-'*11}  {'-'*9}  {'-'*5}  {'-'*8}")
            for s in sorted(suite_stats.keys()):
                ss = suite_stats[s]
                delta = ss["regressed_delta"] - ss["improved_delta"]
                sign = "+" if delta > 0 else ""
                eprint(f"  {s:<20}  {ss['improved']:>8}  {ss['regressed']:>9}  "
                       f"{ss.get('overwritten', 0):>11}  "
                       f"{ss['unchanged']:>9}  {ss['new']:>5}  {sign}{delta:>7}")

            eprint(f"\n  --- Per-suite TCC/GCC ratios ---")
            eprint(f"  {'suite':<20}  {'<1.0':>5}  {'1.0-1.2':>7}  {'1.2-1.5':>7}  "
                   f"{'1.5-2.0':>7}  {'>=2.0':>5}  {'TCC':>6}  {'GCC':>6}  {'ratio':>6}")
            eprint(f"  {'-'*20}  {'-'*5}  {'-'*7}  {'-'*7}  {'-'*7}  {'-'*5}  {'-'*6}  {'-'*6}  {'-'*6}")
            for s in sorted(suite_stats.keys()):
                ss = suite_stats[s]
                ratio = f"{ss['total_tcc'] / ss['total_gcc']:.2f}" if ss['total_gcc'] > 0 else "N/A"
                eprint(f"  {s:<20}  {ss['better']:>5}  {ss['close']:>7}  {ss['ok']:>7}  "
                       f"{ss['warn']:>7}  {ss['bad']:>5}  {ss['total_tcc']:>6}  {ss['total_gcc']:>6}  {ratio:>5s}x")
            eprint()
