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

import pexpect
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

CURRENT_DIR = Path(__file__).parent

was_cleaned = False


@dataclass
class ProfileConfig:
    """Configuration for compiler profiling."""
    tool: str = "none"  # "none", "heaptrack", "time"
    output_dir: Optional[Path] = None
    output_prefix: str = ""  # prefix for output files (e.g., test name)

    def get_wrapper_cmd(self) -> str:
        """Get the CC_WRAPPER command for make."""
        if self.tool == "none" or self.output_dir is None:
            return ""

        if self.tool == "heaptrack":
            out_file = self.output_dir / f"heaptrack_{self.output_prefix}"
            return f"heaptrack --record-only -o {out_file}"
        elif self.tool == "time":
            out_file = self.output_dir / f"time_{self.output_prefix}.txt"
            return f"/usr/bin/time -v -a -o {out_file}"
        else:
            return ""


@dataclass
class CompileConfig:
    """Configuration for compilation."""
    compiler: Optional[Path] = None  # None = use default armv8m-tcc
    extra_cflags: str = ""
    profiler: Optional[ProfileConfig] = None
    clean_before_build: bool = True
    output_dir: Optional[Path] = None  # None = use default build dir

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
    # Binary size metrics
    text_size: int = 0
    data_size: int = 0
    bss_size: int = 0
    total_size: int = 0
    error: str = ""


def _as_file_list(test_file):
    if isinstance(test_file, (list, tuple)):
        return list(test_file)
    return [test_file]


def _primary_file(test_file):
    files = _as_file_list(test_file)
    if not files:
        raise ValueError("test_file list is empty")
    return files[0]


def get_test_output_file(test_name, output_dir=None):
    primary = _primary_file(test_name)
    if output_dir is None:
        output_dir = CURRENT_DIR / "build"
    return output_dir / f"{Path(primary).stem}.elf"


def build_make_command(test_file, machine, compiler, output_dir=None, cflags=None, cc_wrapper=None):
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
        f"TARGET={get_test_output_file(test_file, output_dir)}",
    ]
    if cflags:
        cmd.append(f"EXTRA_CFLAGS={cflags}")
    if cc_wrapper:
        cmd.append(f"CC_WRAPPER={cc_wrapper}")
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
                        list(config.profiler.output_dir.glob(f"time_{prefix}.txt")):
            old_file.unlink()

    # Build make command
    make_command = build_make_command(
        test_file, machine, str(config.compiler),
        output_dir=output_dir,
        cflags=config.extra_cflags or None,
        cc_wrapper=cc_wrapper
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

    elf_file = get_test_output_file(test_file, output_dir)
    output_lines = []
    if result.stdout:
        output_lines.extend(result.stdout.decode().splitlines())
    if result.stderr:
        output_lines.extend(result.stderr.decode().splitlines())

    compile_result = CompileResult(
        success=(result.returncode == 0),
        elf_file=elf_file,
        output_lines=output_lines,
        compile_time_s=elapsed,
    )

    if result.returncode != 0:
        compile_result.error = (result.stderr.decode() if result.stderr else "") + \
                               (result.stdout.decode() if result.stdout else "")
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

    return compile_result


def prepare_test(machine, kernel_file, args=None):
    qemu_command = build_qemu_command(machine, kernel_file, args)
    # Use a wide pseudo-terminal so long lines aren't wrapped
    sut = pexpect.spawn(qemu_command)
    sut.setwinsize(200, 1000)
    return sut


def run_test(test_file, machine, args=None, cflags=None, config=None):
    """
    Compile and prepare a test for QEMU execution.

    Args:
        test_file: Source file(s) to compile
        machine: QEMU machine type
        args: Arguments to pass to the test program
        cflags: Extra CFLAGS (deprecated, use config)
        config: CompileConfig for compilation options

    Returns:
        Tuple of (pexpect.spawn, output_lines)
    """
    primary = _primary_file(test_file)
    test_name = Path(primary).stem

    test_files = [CURRENT_DIR / Path(f) for f in _as_file_list(test_file)]

    # Use new compile_testcase with config
    if config is None:
        config = CompileConfig()
    if cflags:
        config.extra_cflags = cflags

    compile_result = compile_testcase(test_files, machine, config=config)

    if not compile_result.success:
        raise RuntimeError(f"Build failed: {compile_result.error}")

    sut = prepare_test(machine, compile_result.elf_file, args)

    # Enable logging to file
    log_file = open(f"{CURRENT_DIR}/build/{test_name}_output.log", "wb")
    sut.logfile = log_file

    return sut, compile_result.output_lines


# Legacy function signature for backwards compatibility
def reset_clean_state():
    """Reset the global clean state (useful for test isolation)."""
    global was_cleaned
    was_cleaned = False
