"""
QEMU test runner and compiler profiling utilities.

This module provides:
- Compilation of test cases using TinyCC or GCC
- QEMU execution of compiled binaries
- Profiling support (heaptrack, GNU time)
- Binary size reporting via arm-none-eabi-size

Usage for testing:
    from qemu_run import run_test
    sut, logs = run_test("test.c", "mps2-an505")

Usage for profiling:
    from qemu_run import compile_testcase, CompileConfig, ProfileConfig
    config = CompileConfig(profiler=ProfileConfig(tool="heaptrack", output_dir=Path("./profile")))
    result = compile_testcase(["test.c"], "mps2-an505", config=config)
"""

import os
import pexpect
import re
import shlex
import shutil
import sys
import time
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

CURRENT_DIR = Path(__file__).parent

was_cleaned = False


class SubprocessSUT:
    """Minimal pexpect-like interface for reading QEMU output without PTYs.

    This avoids Python 3.13+ warnings (and potential flakiness) around
    forkpty() in multi-threaded processes on macOS.
    """

    def __init__(self, command: str):
        argv = shlex.split(command)
        self._proc = subprocess.Popen(
            argv,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        if self._proc.stdout is None:
            raise RuntimeError("Failed to spawn process with stdout pipe")
        self._fd = self._proc.stdout.fileno()
        self._buffer = ""
        self.match = None
        self.exitstatus = None
        self.logfile = None

    def setwinsize(self, *_args, **_kwargs):
        # No PTY; nothing to do.
        return

    def _append_output(self, data: bytes):
        if not data:
            return
        if self.logfile is not None:
            try:
                self.logfile.write(data)
                self.logfile.flush()
            except Exception:
                # Best-effort logging; don't break tests due to logging.
                pass
        text = data.decode("utf-8", errors="replace")
        # Normalize CRLF/CR to LF for more predictable matching.
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        self._buffer += text
        # Keep buffer bounded (large enough for regex searching and debugging).
        if len(self._buffer) > 256_000:
            self._buffer = self._buffer[-128_000:]

    def expect(self, pattern, timeout: int = 1):
        if isinstance(pattern, (bytes, bytearray)):
            pattern = pattern.decode("utf-8", errors="replace")
        regex = pattern if hasattr(pattern, "search") else re.compile(pattern)

        deadline = time.monotonic() + float(timeout)
        while True:
            m = regex.search(self._buffer)
            if m is not None:
                self.match = m
                return m

            # If process exited and no more output is coming, bail out.
            if self._proc.poll() is not None:
                # Drain any remaining bytes.
                try:
                    while True:
                        chunk = os.read(self._fd, 4096)
                        if not chunk:
                            break
                        self._append_output(chunk)
                except OSError:
                    pass
                m = regex.search(self._buffer)
                if m is not None:
                    self.match = m
                    return m
                raise TimeoutError(f"Pattern not found before process exit: {pattern!r}")

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Timeout waiting for pattern: {pattern!r}")

            # Wait for stdout to become readable, then read a chunk.
            import select

            r, _, _ = select.select([self._fd], [], [], min(0.05, remaining))
            if not r:
                continue
            try:
                chunk = os.read(self._fd, 4096)
            except OSError:
                chunk = b""
            if chunk:
                self._append_output(chunk)

    def wait(self, timeout: Optional[int] = None):
        rc = self._proc.wait(timeout=timeout)
        self.exitstatus = rc
        return rc


@dataclass
class ProfileConfig:
    """Configuration for compiler profiling."""
    tool: str = "none"  # "none", "heaptrack", "time", "perf", "xctrace"
    output_dir: Optional[Path] = None
    output_prefix: str = ""  # prefix for output files (e.g., test name)
    perf_frequency: int = 99  # sampling frequency for perf (Hz)
    measure_memory: bool = True  # For perf: also capture memory usage via /usr/bin/time

    def get_wrapper_cmd(self) -> str:
        """Get the CC_WRAPPER command for make."""
        if self.tool == "none" or self.output_dir is None:
            return ""

        if self.tool == "heaptrack":
            if sys.platform == "darwin":
                raise RuntimeError("heaptrack is not available on macOS; use --profiler time")
            out_file = self.output_dir / f"heaptrack_{self.output_prefix}"
            return f"heaptrack --record-only -o {out_file}"
        elif self.tool == "time":
            out_file = self.output_dir / f"time_{self.output_prefix}.txt"
            if sys.platform == "darwin":
                timewrap = CURRENT_DIR / "timewrap.py"
                return f"{sys.executable} {timewrap} -a -o {out_file} --"
            return f"/usr/bin/time -v -a -o {out_file}"
        elif self.tool == "perf":
            if sys.platform == "darwin":
                raise RuntimeError("perf profiling is Linux-only; use --profiler time on macOS")
            perf_file = self.output_dir / f"perf_{self.output_prefix}.data"
            if self.measure_memory:
                # Wrap perf with time to get memory metrics too
                time_file = self.output_dir / f"time_{self.output_prefix}.txt"
                return f"/usr/bin/time -v -a -o {time_file} perf record -F {self.perf_frequency} -g --call-graph dwarf -o {perf_file}"
            else:
                return f"perf record -F {self.perf_frequency} -g --call-graph dwarf -o {perf_file}"
        elif self.tool == "xctrace":
            if sys.platform != "darwin":
                raise RuntimeError("xctrace profiling is macOS-only")
            # Ensure xctrace is available (usually requires full Xcode).
            probe = subprocess.run(
                ["xcrun", "-f", "xctrace"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if probe.returncode != 0:
                raise RuntimeError(
                    "xctrace not found. Install Xcode (not just Command Line Tools), open it once, "
                    "accept the license, then run: sudo xcode-select -s /Applications/Xcode.app/Contents/Developer"
                )
            trace_file = self.output_dir / f"xctrace_{self.output_prefix}.trace"
            # Note: `xctrace` is provided by Xcode Command Line Tools.
            # We keep it minimal and record an Allocations trace for the compiler invocation.
            return f"xcrun xctrace record --template Allocations --output {trace_file} --launch --"
        else:
            return ""


@dataclass
class CompileConfig:
    """Configuration for compilation."""
    compiler: Optional[Path] = None  # None = use default armv8m-tcc
    extra_cflags: str = ""
    dump_ir: bool = False  # Pass -dump-ir to the compiler (TinyCC only)
    two_phase: bool = False  # Use two-phase compilation (reduces memory)
    defines: Optional[list] = None  # List of defines, e.g. ["FOO", "BAR=1"]
    profiler: Optional[ProfileConfig] = None
    clean_before_build: bool = True
    output_dir: Optional[Path] = None  # None = use default build dir
    output_prefix: str = ""  # Prefix to add to output filename (e.g. "O0_")
    output_suffix: str = ""  # Suffix to add to output filename (e.g. "_tag")

    def __post_init__(self):
        if self.compiler is None:
            self.compiler = CURRENT_DIR / "../../armv8m-tcc"


@dataclass
class CompileResult:
    """Result of a compilation."""
    success: bool
    elf_file: Path
    output_lines: list
    # Profiling metrics (populated if profiler was used)
    compile_time_s: float = 0.0
    user_time_s: float = 0.0
    sys_time_s: float = 0.0
    max_rss_kb: int = 0
    heap_peak_kb: int = 0
    heap_allocations: int = 0
    heap_temporary_allocs: int = 0
    profile_file: str = ""
    flamegraph_file: str = ""  # SVG flamegraph (for perf profiling)
    perf_samples: int = 0  # Number of perf samples collected
    # Binary size metrics
    text_size: int = 0
    data_size: int = 0
    bss_size: int = 0
    total_size: int = 0
    error: str = ""
    make_command: list = None  # The make command that was executed


def _as_file_list(test_file):
    if isinstance(test_file, (list, tuple)):
        return list(test_file)
    return [test_file]


def _primary_file(test_file):
    files = _as_file_list(test_file)
    if not files:
        raise ValueError("test_file list is empty")
    return files[0]


def get_test_output_file(test_name, output_dir=None, prefix="", suffix=""):
    primary = _primary_file(test_name)
    if output_dir is None:
        output_dir = CURRENT_DIR / "build"
    return output_dir / f"{prefix}{Path(primary).stem}{suffix}.elf"


def build_make_command(test_file, machine, compiler, output_dir=None, cflags=None, defines=None, cc_wrapper=None, two_phase=False, output_prefix="", output_suffix=""):
    """Build the make command for compiling a test case."""
    make_dir = CURRENT_DIR / 'qemu' / machine
    test_files = [str(f) for f in _as_file_list(test_file)]
    test_files_value = " ".join(test_files)

    if output_dir is None:
        output_dir = CURRENT_DIR / "build"

    cmd = [
        "make",
        "-C",
        str(make_dir),
        f"OUTPUT={output_dir}",
        f"TEST_FILES={test_files_value}",
        f"CC={compiler}",
        f"TARGET={get_test_output_file(test_file, output_dir, prefix=output_prefix, suffix=output_suffix)}",
    ]
    # Build EXTRA_CFLAGS from cflags and defines
    extra_cflags_parts = []
    if cflags:
        extra_cflags_parts.append(cflags)
    if defines:
        for d in defines:
            extra_cflags_parts.append(f"-D{d}")
    if extra_cflags_parts:
        cmd.append(f"EXTRA_CFLAGS={' '.join(extra_cflags_parts)}")
    if cc_wrapper:
        cmd.append(f"CC_WRAPPER={cc_wrapper}")
    if two_phase:
        cmd.append("TWO_PHASE=1")
    return cmd


def build_qemu_command(machine, kernel_file, args=None):
    cmd = f'qemu-system-arm -machine {machine} -nographic -semihosting -kernel {kernel_file}'
    if args:
        cmd += ' -append "' + ' '.join(args) + '"'
    return cmd


def get_binary_size(elf_file):
    """Get binary size metrics using arm-none-eabi-size."""
    metrics = {'text': 0, 'data': 0, 'bss': 0, 'total': 0}

    if not Path(elf_file).exists():
        return metrics

    result = subprocess.run(
        ["arm-none-eabi-size", str(elf_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if result.returncode == 0:
        output = result.stdout.decode()
        lines = output.strip().split('\n')
        if len(lines) >= 2:
            # Format: text    data     bss     dec     hex filename
            parts = lines[1].split()
            if len(parts) >= 4:
                metrics['text'] = int(parts[0])
                metrics['data'] = int(parts[1])
                metrics['bss'] = int(parts[2])
                metrics['total'] = int(parts[3])

    return metrics


def parse_time_output(time_file):
    """Parse GNU time -v output."""
    metrics = {'user_time': 0.0, 'sys_time': 0.0, 'max_rss_kb': 0}

    if not time_file.exists():
        return metrics

    content = time_file.read_text()
    max_rss_values = []

    for line in content.split('\n'):
        if 'User time' in line:
            match = re.search(r'(\d+\.?\d*)', line)
            if match:
                metrics['user_time'] += float(match.group(1))
        elif 'System time' in line:
            match = re.search(r'(\d+\.?\d*)', line)
            if match:
                metrics['sys_time'] += float(match.group(1))
        elif 'Maximum resident set size' in line:
            match = re.search(r'(\d+)', line)
            if match:
                max_rss_values.append(int(match.group(1)))

    if max_rss_values:
        metrics['max_rss_kb'] = max(max_rss_values)

    return metrics


def parse_perf_output(perf_data_file, generate_flamegraph=True):
    """Parse perf data and optionally generate a flamegraph SVG.

    Requires:
    - perf (Linux perf tools)
    - For flamegraphs: either 'flamegraph' CLI tool or FlameGraph scripts
    """
    metrics = {'samples': 0, 'flamegraph_file': ''}

    perf_file = Path(perf_data_file)
    if not perf_file.exists():
        return metrics

    # Get sample count from perf report
    result = subprocess.run(
        ["perf", "report", "-i", str(perf_file), "--stdio", "--header"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if result.returncode == 0:
        output = result.stdout.decode(errors='replace')
        for line in output.split('\n'):
            if 'sample' in line.lower() and ('event' in line.lower() or 'of' in line.lower()):
                match = re.search(r'(\d+)\s+sample', line.lower())
                if match:
                    metrics['samples'] = int(match.group(1))
                    break

    if not generate_flamegraph:
        return metrics

    # Generate flamegraph
    flamegraph_svg = perf_file.with_suffix('.svg')

    # Try using 'flamegraph' CLI tool first (cargo install flamegraph)
    result = subprocess.run(
        ["which", "flamegraph"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if result.returncode == 0:
        # Use flamegraph CLI - it reads perf.data directly
        result = subprocess.run(
            ["flamegraph", "--perfdata", str(perf_file), "-o", str(flamegraph_svg)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode == 0 and flamegraph_svg.exists():
            metrics['flamegraph_file'] = str(flamegraph_svg)
            return metrics

    # Fallback: use perf script + FlameGraph scripts
    # perf script -> stackcollapse-perf.pl -> flamegraph.pl
    perf_script_result = subprocess.run(
        ["perf", "script", "-i", str(perf_file)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    if perf_script_result.returncode != 0:
        return metrics

    # Try different collapse tools
    collapse_result = None
    collapse_tools = ["stackcollapse-perf.pl", "inferno-collapse-perf"]
    for tool in collapse_tools:
        try:
            collapse_result = subprocess.run(
                [tool],
                input=perf_script_result.stdout,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if collapse_result.returncode == 0:
                break
        except FileNotFoundError:
            continue

    if collapse_result is None or collapse_result.returncode != 0:
        return metrics

    # Try different flamegraph tools
    fg_tools = [
        ["flamegraph.pl", "--title", perf_file.stem],
        ["inferno-flamegraph", "--title", perf_file.stem],
    ]
    for tool_cmd in fg_tools:
        try:
            fg_result = subprocess.run(
                tool_cmd,
                input=collapse_result.stdout,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if fg_result.returncode == 0:
                flamegraph_svg.write_bytes(fg_result.stdout)
                metrics['flamegraph_file'] = str(flamegraph_svg)
                break
        except FileNotFoundError:
            continue

    return metrics


def parse_heaptrack_output(heaptrack_prefix):
    """Parse heaptrack output using heaptrack_print."""
    metrics = {'heap_peak_kb': 0, 'allocations': 0, 'temporary_allocs': 0}

    parent = heaptrack_prefix.parent
    # Try both .zst (newer) and .gz (older) extensions
    matches = list(parent.glob(heaptrack_prefix.name + "*.zst"))
    if not matches:
        matches = list(parent.glob(heaptrack_prefix.name + "*.gz"))

    if not matches:
        return metrics, ""

    all_files = sorted(matches)
    result_file = str(all_files[-1])

    total_allocations = 0
    total_temporary = 0
    max_peak = 0

    for gzf in all_files:
        result = subprocess.run(
            ["heaptrack_print", str(gzf)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        output = result.stdout.decode(errors='replace')

        for line in output.split('\n'):
            if 'peak heap memory consumption' in line.lower():
                match = re.search(r'(\d+\.?\d*)\s*([KMGBkmgb])', line)
                if match:
                    value = float(match.group(1))
                    unit = match.group(2).upper()
                    if unit == 'K':
                        pass
                    elif unit == 'M':
                        value *= 1024
                    elif unit == 'G':
                        value *= 1024 * 1024
                    elif unit == 'B':
                        value /= 1024
                    max_peak = max(max_peak, int(value))
            elif line.startswith('calls to allocation functions:'):
                match = re.search(r':\s*(\d+)', line)
                if match:
                    total_allocations += int(match.group(1))
            elif line.startswith('temporary memory allocations:'):
                match = re.search(r':\s*(\d+)', line)
                if match:
                    total_temporary += int(match.group(1))

    metrics['heap_peak_kb'] = max_peak
    metrics['allocations'] = total_allocations
    metrics['temporary_allocs'] = total_temporary

    return metrics, result_file


def compile_testcase(test_file, machine, compiler=None, cflags=None, config=None):
    """
    Compile a test case with optional profiling.

    Args:
        test_file: Source file(s) to compile
        machine: QEMU machine type (e.g., "mps2-an505")
        compiler: Path to compiler (deprecated, use config.compiler)
        cflags: Extra CFLAGS (deprecated, use config.extra_cflags)
        config: CompileConfig with all options

    Returns:
        CompileResult with compilation outcome and metrics
    """
    global was_cleaned

    # Handle legacy arguments
    if config is None:
        config = CompileConfig()
    if compiler is not None:
        config.compiler = Path(compiler)
    if cflags is not None:
        config.extra_cflags = cflags

    # Convenience: allow callers to request IR dumping without manually
    # threading -dump-ir through extra_cflags.
    if getattr(config, "dump_ir", False):
        if "-dump-ir" not in (config.extra_cflags or ""):
            config.extra_cflags = (config.extra_cflags + " -dump-ir").strip()

    # Determine output directory
    output_dir = config.output_dir or (CURRENT_DIR / "build")
    output_dir.mkdir(parents=True, exist_ok=True)

    # Setup profiler
    cc_wrapper = None
    if config.profiler and config.profiler.tool != "none":
        if config.profiler.output_dir is None:
            config.profiler.output_dir = output_dir
        config.profiler.output_dir.mkdir(parents=True, exist_ok=True)
        if not config.profiler.output_prefix:
            config.profiler.output_prefix = Path(_primary_file(test_file)).stem
        cc_wrapper = config.profiler.get_wrapper_cmd()

        # Clean old profiler output files
        prefix = config.profiler.output_prefix
        for old_file in list(config.profiler.output_dir.glob(f"heaptrack_{prefix}*.zst")) + \
                        list(config.profiler.output_dir.glob(f"heaptrack_{prefix}*.gz")) + \
                        list(config.profiler.output_dir.glob(f"time_{prefix}.txt")) + \
                        list(config.profiler.output_dir.glob(f"perf_{prefix}.data")) + \
                        list(config.profiler.output_dir.glob(f"perf_{prefix}.svg")):
            old_file.unlink()

        # xctrace outputs a directory ending with .trace
        old_trace = config.profiler.output_dir / f"xctrace_{prefix}.trace"
        if old_trace.exists():
            shutil.rmtree(old_trace, ignore_errors=True)

    # Build make command
    make_command = build_make_command(
        test_file, machine, str(config.compiler),
        output_dir=output_dir,
        cflags=config.extra_cflags or None,
        defines=config.defines,
        cc_wrapper=cc_wrapper,
        two_phase=config.two_phase,
        output_prefix=config.output_prefix,
        output_suffix=config.output_suffix
    )

    # Clean if needed
    if config.clean_before_build and not was_cleaned:
        result = subprocess.run(make_command + ["clean"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode != 0:
            raise RuntimeError(f"Clean failed with exit code {result.returncode}")
        was_cleaned = True

    # Compile
    import time
    start = time.perf_counter()
    result = subprocess.run(make_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    elapsed = time.perf_counter() - start

    elf_file = get_test_output_file(test_file, output_dir, prefix=config.output_prefix, suffix=config.output_suffix)
    output_lines = []
    if result.stdout:
        output_lines.extend(result.stdout.decode('utf-8', errors='replace').splitlines())
    if result.stderr:
        output_lines.extend(result.stderr.decode('utf-8', errors='replace').splitlines())

    compile_result = CompileResult(
        success=(result.returncode == 0),
        elf_file=elf_file,
        output_lines=output_lines,
        compile_time_s=elapsed,
        make_command=make_command,
    )

    if result.returncode != 0:
        compile_result.error = (result.stderr.decode('utf-8', errors='replace') if result.stderr else "") + \
                               (result.stdout.decode('utf-8', errors='replace') if result.stdout else "")
        return compile_result

    # Get binary size
    size_metrics = get_binary_size(elf_file)
    compile_result.text_size = size_metrics['text']
    compile_result.data_size = size_metrics['data']
    compile_result.bss_size = size_metrics['bss']
    compile_result.total_size = size_metrics['total']

    # Parse profiler output
    if config.profiler and config.profiler.tool != "none":
        prefix = config.profiler.output_prefix
        if config.profiler.tool == "heaptrack":
            ht_file = config.profiler.output_dir / f"heaptrack_{prefix}"
            ht_metrics, profile_file = parse_heaptrack_output(ht_file)
            compile_result.heap_peak_kb = ht_metrics['heap_peak_kb']
            compile_result.heap_allocations = ht_metrics['allocations']
            compile_result.heap_temporary_allocs = ht_metrics['temporary_allocs']
            compile_result.profile_file = profile_file
        elif config.profiler.tool == "time":
            time_file = config.profiler.output_dir / f"time_{prefix}.txt"
            time_metrics = parse_time_output(time_file)
            compile_result.user_time_s = time_metrics['user_time']
            compile_result.sys_time_s = time_metrics['sys_time']
            compile_result.max_rss_kb = time_metrics['max_rss_kb']
            compile_result.profile_file = str(time_file)
        elif config.profiler.tool == "perf":
            perf_file = config.profiler.output_dir / f"perf_{prefix}.data"
            perf_metrics = parse_perf_output(perf_file, generate_flamegraph=True)
            compile_result.perf_samples = perf_metrics['samples']
            compile_result.profile_file = str(perf_file)
            compile_result.flamegraph_file = perf_metrics['flamegraph_file']
            # Also parse time output if measure_memory was enabled
            if getattr(config.profiler, 'measure_memory', True):
                time_file = config.profiler.output_dir / f"time_{prefix}.txt"
                if time_file.exists():
                    time_metrics = parse_time_output(time_file)
                    compile_result.user_time_s = time_metrics['user_time']
                    compile_result.sys_time_s = time_metrics['sys_time']
                    compile_result.max_rss_kb = time_metrics['max_rss_kb']
        elif config.profiler.tool == "xctrace":
            trace_file = config.profiler.output_dir / f"xctrace_{prefix}.trace"
            if trace_file.exists():
                compile_result.profile_file = str(trace_file)

    return compile_result


def prepare_test(machine, kernel_file, args=None):
    qemu_command = build_qemu_command(machine, kernel_file, args)
    # Prefer pipe-based execution when possible.
    #
    # - On macOS we avoid pty.forkpty() warnings/flakiness in multi-threaded
    #   processes (Python 3.13+).
    # - On Python 3.14+ a DeprecationWarning is emitted when forkpty() is used
    #   from a multi-threaded process (common under pytest), so avoid PTYs by
    #   default there as well.
    force_pexpect = os.environ.get("TINYCC_IRTEST_USE_PEXPECT", "")
    if force_pexpect.strip() not in {"1", "true", "TRUE"}:
        if sys.platform == "darwin" or sys.version_info >= (3, 14):
            return SubprocessSUT(qemu_command)

    # Otherwise, use a wide pseudo-terminal so long lines aren't wrapped.
    sut = pexpect.spawn(qemu_command)
    sut.setwinsize(200, 1000)
    return sut


def run_test(test_file, machine, args=None, cflags=None, defines=None, config=None):
    """
    Compile and prepare a test for QEMU execution.

    Args:
        test_file: Source file(s) to compile
        machine: QEMU machine type
        args: Arguments to pass to the test program
        cflags: Extra CFLAGS (deprecated, use config)
        defines: List of defines (deprecated, use config)
        config: CompileConfig for compilation options

    Returns:
        Tuple of (pexpect.spawn, output_lines)
    """
    test_files = [CURRENT_DIR / Path(f) for f in _as_file_list(test_file)]

    # Use new compile_testcase with config
    if config is None:
        config = CompileConfig()
    if cflags:
        config.extra_cflags = cflags
    if defines:
        config.defines = defines

    compile_result = compile_testcase(test_files, machine, config=config)

    if not compile_result.success:
        raise RuntimeError(f"Build failed: {compile_result.error}")

    sut = prepare_test(machine, compile_result.elf_file, args)

    # Enable logging to file (name follows built ELF, so it naturally includes prefix/suffix)
    log_path = compile_result.elf_file.with_name(f"{compile_result.elf_file.stem}_output.log")
    log_file = open(log_path, "wb")
    if config and config.extra_cflags:
        log_file.write(f"=== EXTRA_CFLAGS: {config.extra_cflags} ===\n".encode())
    sut.logfile = log_file

    return sut, compile_result.output_lines


# Legacy function signature for backwards compatibility
def reset_clean_state():
    """Reset the global clean state (useful for test isolation)."""
    global was_cleaned
    was_cleaned = False
