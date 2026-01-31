#!/usr/bin/env python3
"""
Unified script to build, upload, run, and compare TCC vs GCC benchmarks on RP2350.

Usage:
    python3 run_benchmark.py <host_ip_or_hostname> [options]
    python3 run_benchmark.py 192.168.0.113
    python3 run_benchmark.py user@192.168.0.113 --identity ~/.ssh/id_rsa
"""

import argparse
import os
import subprocess
import sys
import re
import tempfile
from pathlib import Path
from dataclasses import dataclass
from typing import Optional, Dict, List, Tuple

try:
    import paramiko
except ImportError:
    print("Error: paramiko not installed. Run: pip install paramiko")
    sys.exit(1)


@dataclass
class BenchmarkResult:
    name: str
    iterations: int
    cycles_per_iter: float
    result: int
    verify: str
    raw_output: str


@dataclass
class CompilerResult:
    compiler: str  # "TCC" or "GCC"
    build_success: bool
    build_size: Dict[str, int]
    benchmarks: List[BenchmarkResult]
    raw_output: str


def run_command(cmd: List[str], cwd: Optional[Path] = None, capture: bool = True,
                env: Optional[dict] = None) -> Tuple[int, str, str]:
    """Run a shell command and return exit code, stdout, stderr."""
    if capture:
        result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env)
        return result.returncode, result.stdout, result.stderr
    else:
        result = subprocess.run(cmd, cwd=cwd, env=env)
        return result.returncode, "", ""


def get_binary_size(elf_path: Path) -> Dict[str, int]:
    """Get binary size info using arm-none-eabi-size."""
    code, stdout, stderr = run_command(["arm-none-eabi-size", str(elf_path)])
    if code != 0:
        return {}

    # Parse output like: "   text    data     bss     dec     hex filename"
    lines = stdout.strip().split('\n')
    if len(lines) >= 2:
        parts = lines[1].split()
        if len(parts) >= 4:
            return {
                'text': int(parts[0]),
                'data': int(parts[1]),
                'bss': int(parts[2]),
                'dec': int(parts[3]),
            }
    return {}


def build_compiler(compiler: str, ssh_host: str, opt_level: str = "1") -> Tuple[bool, Optional[Path], Dict[str, int]]:
    """Build benchmark for specified compiler (tcc or gcc)."""
    print(f"\n{'='*50}")
    print(f"Building {compiler.upper()} version -O{opt_level}")
    print(f"{'='*50}")

    script_dir = Path(__file__).parent
    # Use separate build directories for each optimization level to speed up recompilation
    build_dir = script_dir / f"build_pico_{compiler.lower()}_O{opt_level}"
    pico_sdk_path = (script_dir / "libs" / "pico-sdk").resolve()

    # Create build directory
    build_dir.mkdir(parents=True, exist_ok=True)

    # Set environment with PICO_SDK_PATH
    env = os.environ.copy()
    env["PICO_SDK_PATH"] = str(pico_sdk_path)
    print(f"PICO_SDK_PATH={pico_sdk_path}")

    # Run cmake
    print(f"Running cmake...")
    cmake_cmd = [
        "cmake", "..",
        f"-DPICO_SDK_PATH={pico_sdk_path}",
        "-DPICO_PLATFORM=rp2350",  # Force RP2350 for ARMv8-M
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DBENCHMARK_COMPILER={compiler.upper()}",
        f"-DBENCHMARK_OPT_LEVEL={opt_level}"
    ]
    code, stdout, stderr = run_command(cmake_cmd, cwd=build_dir, env=env)
    if code != 0:
        print(f"CMake failed:\n{stderr}")
        return False, None, {}

    # Run make
    print(f"Running make...")
    code, stdout, stderr = run_command(["make", "-j4"], cwd=build_dir, env=env)
    if code != 0:
        print(f"Make failed:\n{stderr}")
        return False, None, {}

    # Check ELF file
    elf_file = build_dir / f"minimal_uart_picosdk_{compiler.lower()}.elf"
    if not elf_file.exists():
        print(f"Build failed: {elf_file} not found")
        return False, None, {}

    # Get binary size
    size_info = get_binary_size(elf_file)
    print(f"Build successful!")
    print(f"  Text: {size_info.get('text', 0)} bytes")
    print(f"  Data: {size_info.get('data', 0)} bytes")
    print(f"  BSS:  {size_info.get('bss', 0)} bytes")
    print(f"  Total: {size_info.get('dec', 0)} bytes")

    # Return ELF file for OpenOCD (ELF is supported, BIN is not)
    return True, elf_file, size_info


def parse_benchmark_output(output: str) -> List[BenchmarkResult]:
    """Parse benchmark output and extract results.

    Only parses benchmarks from the LAST complete run in the output,
    ignoring any leftover data from previous runs in the serial buffer.
    """
    results = []

    # Find the LAST occurrence of the benchmark header to ignore stale data
    # from previous runs that might be in the serial buffer
    lines = output.split('\n')

    # Look for the last "Running X benchmarks" line to find start of current run
    start_idx = 0
    for i, line in enumerate(lines):
        if 'Running' in line and 'benchmarks' in line:
            start_idx = i

    # Also check for header separators to find the last run
    for i, line in enumerate(lines):
        if '========================================' in line and i > start_idx:
            # Check if next few lines contain "ARMv8-M Benchmark Suite"
            for j in range(i, min(i+5, len(lines))):
                if 'ARMv8-M Benchmark Suite' in lines[j]:
                    start_idx = i
                    break

    # Parse benchmark results only from start_idx onwards
    seen_names = set()  # Track names to avoid duplicates

    for line in lines[start_idx:]:
        line = line.strip()

        # Stop at end markers
        if 'benchmark stopped' in line.lower() or 'Benchmark completed' in line:
            break

        # Match benchmark result lines with cycle counter (5 columns)
        # Example: "fibonacci              10    47066.00         6765     PASS"
        match = re.match(r'^(\S+)\s+(\d+)\s+([\d.]+)\s+(-?\d+)\s+(\S+)$', line)
        if match:
            name = match.group(1)
            # Skip if we've already seen this benchmark (duplicate from stale data)
            if name in seen_names:
                continue
            seen_names.add(name)

            iterations = int(match.group(2))
            cycles_per_iter = float(match.group(3))
            result = int(match.group(4))
            verify = match.group(5)
            results.append(BenchmarkResult(
                name=name,
                iterations=iterations,
                cycles_per_iter=cycles_per_iter,
                result=result,
                verify=verify,
                raw_output=line
            ))
            continue

        # Match benchmark result lines without cycle counter (4 columns)
        # Example: "fibonacci              10         6765     PASS"
        match = re.match(r'^(\S+)\s+(\d+)\s+(-?\d+)\s+(\S+)$', line)
        if match:
            name = match.group(1)
            # Skip if we've already seen this benchmark
            if name in seen_names:
                continue
            seen_names.add(name)

            iterations = int(match.group(2))
            result = int(match.group(3))
            verify = match.group(4)
            results.append(BenchmarkResult(
                name=name,
                iterations=iterations,
                cycles_per_iter=0.0,  # No cycle counter data
                result=result,
                verify=verify,
                raw_output=line
            ))

    return results


def upload_and_run(elf_path: Path, host: str, port: int = 22,
                   username: str = "mateusz", identity: Optional[str] = None,
                   password: Optional[str] = None) -> Tuple[bool, str]:
    """Upload and run ELF on target via SSH using OpenOCD."""

    print(f"\nConnecting to {username}@{host}...")

    # Connect via SSH
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    connect_kwargs = {"hostname": host, "port": port, "username": username}
    if identity:
        connect_kwargs["key_filename"] = identity
    elif password:
        connect_kwargs["password"] = password

    try:
        ssh.connect(**connect_kwargs)
        print(f"✓ Connected")
    except Exception as e:
        print(f"✗ Connection failed: {e}")
        return False, ""

    sftp = ssh.open_sftp()

    # Upload ELF file (OpenOCD supports ELF directly)
    remote_elf = f"/tmp/{elf_path.name}"
    print(f"Uploading {elf_path.name} to {remote_elf}...")
    sftp.put(str(elf_path), remote_elf)

    # Find serial port
    stdin, stdout, stderr = ssh.exec_command("ls /dev/ttyACM* 2>/dev/null | head -1")
    serial_port = stdout.read().decode().strip()
    if not serial_port:
        stdin, stdout, stderr = ssh.exec_command("ls /dev/ttyUSB* 2>/dev/null | head -1")
        serial_port = stdout.read().decode().strip()
    if not serial_port:
        print("Warning: No serial port found, trying /dev/ttyACM0")
        serial_port = "/dev/ttyACM0"
    else:
        print(f"Using serial port: {serial_port}")

    # Create run script - now waits for "benchmark stopped" signal
    combined_script = f'''#!/bin/bash
set -e

SERIAL="{serial_port}"
ELF="{remote_elf}"

echo "Configuring serial port..."
# Configure serial port with proper flush settings
stty -F $SERIAL 460800 cs8 -cstopb -parenb raw -echo 2>/dev/null || true

# Clear any pending data in serial port using multiple methods
echo "Flushing serial port..."
# Method 1: drain using cat with timeout (increased for reliability)
timeout 1.0 cat $SERIAL >/dev/null 2>&1 || true
# Method 2: use stty to flush
cat $SERIAL > /dev/null 2>&1 &
FLUSH_PID=$!
sleep 0.3
kill $FLUSH_PID 2>/dev/null || true
wait $FLUSH_PID 2>/dev/null || true

# Clear previous output file
rm -f /tmp/serial_out.txt
rm -f /tmp/serial_raw.txt
touch /tmp/serial_out.txt
touch /tmp/serial_raw.txt

# Open serial port for reading using file descriptor (keeps port open)
exec 3<$SERIAL

# Final flush of any buffered data
echo "Final serial flush..."
(timeout 1.0 cat <&3 >/dev/null 2>&1 || true)

# Start capturing ALL serial output in background (including sync)
dd if=/dev/fd/3 of=/tmp/serial_raw.txt bs=1 2>/dev/null &
SERIAL_PID=$!

# Give serial capture time to start
sleep 0.2

echo "Running OpenOCD with reset..."
# Run OpenOCD - reset target first, then program and run
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "adapter speed 5000" \\
    -c "init" \\
    -c "reset halt" \\
    -c "reset" \\
    -c "sleep 100" \\
    -c "program $ELF verify" \\
    -c "reset run" \\
    -c "shutdown" 2>&1 &

OPENOCD_PID=$!

# Wait for benchmark completion signals (300s timeout - increased for longer benchmarks)
echo "Waiting for benchmark output..."
TIMEOUT=300
ELAPSED=0
COMPLETED=0

while [ $ELAPSED -lt $TIMEOUT ]; do
    # Check for completion signals in raw output
    if grep -q "benchmark stopped" /tmp/serial_raw.txt 2>/dev/null; then
        echo "✓ Benchmark stopped signal received!"
        COMPLETED=1
        break
    fi
    if grep -q "Benchmark completed" /tmp/serial_raw.txt 2>/dev/null; then
        echo "✓ Benchmark completed!"
        COMPLETED=1
        break
    fi
    if grep -q "Benchmark failed" /tmp/serial_raw.txt 2>/dev/null; then
        echo "✗ Benchmark failed!"
        COMPLETED=1
        break
    fi

    # Check if OpenOCD is still running
    if ! kill -0 $OPENOCD_PID 2>/dev/null; then
        # OpenOCD exited, give a bit more time to capture output
        sleep 1
        # Check one more time for completion
        if grep -qE "(benchmark stopped|Benchmark completed|Benchmark failed)" /tmp/serial_raw.txt 2>/dev/null; then
            echo "✓ Benchmark finished!"
            COMPLETED=1
        fi
        break
    fi

    sleep 0.5
    ELAPSED=$((ELAPSED + 1))
done

if [ $ELAPSED -ge $TIMEOUT ]; then
    echo "TIMEOUT: Benchmark did not complete within ${{TIMEOUT}} seconds"
    echo "This may indicate the benchmark is stuck or needs more iterations"
fi

# Give time for final output to be captured
echo "Waiting for final output..."
sleep 1.0

# Flush serial buffer one more time to get remaining data
cat /dev/fd/3 >/dev/null 2>&1 &
FLUSH2_PID=$!
sleep 0.5
kill $FLUSH2_PID 2>/dev/null || true
wait $FLUSH2_PID 2>/dev/null || true

# Final delay for data to be written
sleep 0.5

# Kill serial capture
kill $SERIAL_PID 2>/dev/null || true
wait $SERIAL_PID 2>/dev/null || true

# Kill OpenOCD if still running
kill $OPENOCD_PID 2>/dev/null || true
wait $OPENOCD_PID 2>/dev/null || true

# Extract clean output: everything after ===SYNC_START=== marker
# This discards any garbage from power-up or previous runs
echo ""
echo "===SERIAL_OUTPUT_START==="
if grep -q "===SYNC_START===" /tmp/serial_raw.txt 2>/dev/null; then
    # Extract everything after the SYNC marker
    sed -n '/===SYNC_START===/,$p' /tmp/serial_raw.txt | tail -n +2
else
    # Fallback: output everything if no sync marker found
    echo "WARNING: No sync marker found, outputting raw data"
    cat /tmp/serial_raw.txt 2>/dev/null
fi
echo "===SERIAL_OUTPUT_END==="
'''
    remote_combined = "/tmp/run_test.sh"
    sftp.putfo(__import__("io").BytesIO(combined_script.encode()), remote_combined)
    ssh.exec_command(f"chmod +x {remote_combined}")

    print("Running benchmark on target...")
    stdin, stdout, stderr = ssh.exec_command(remote_combined, timeout=300)
    # Use errors='replace' to handle garbage bytes in serial output
    output = stdout.read().decode(errors='replace')
    errors = stderr.read().decode(errors='replace')

    # Split output
    if "===SERIAL_OUTPUT_START===" in output:
        parts = output.split("===SERIAL_OUTPUT_START===")
        ocd_output = parts[0]
        serial_part = parts[1].split("===SERIAL_OUTPUT_END===")[0]
    else:
        ocd_output = output
        serial_part = ""

    # Check for issues
    success = True
    if "Resource busy" in ocd_output:
        print("!!! ERROR: CMSIS-DAP probe is busy !!!")
        print("Stop any running OpenOCD, picoprobe, or serial monitor first")
        success = False
    elif "Error:" in ocd_output and "completed" not in ocd_output:
        print("!!! OpenOCD reported errors !!!")

    # Cleanup
    sftp.close()
    ssh.close()

    if serial_part:
        return success, serial_part
    else:
        return False, ocd_output + "\n" + errors


def print_opt_comparison(compiler_name: str, o0_result: CompilerResult, o1_result: CompilerResult):
    """Print comparison between -O0 and -O1 for the same compiler with verification."""
    print("\n" + "="*80)
    print(f"OPTIMIZATION COMPARISON: {compiler_name} -O0 vs -O1")
    print("="*80)

    # Binary sizes
    print("\n--- Binary Size Comparison ---")
    print(f"{'Section':<15} {'-O0':>12} {'-O1':>12} {'O1/O0 %':>12}")
    print(f"{'-'*15} {'-'*12} {'-'*12} {'-'*12}")

    for section in ['text', 'data', 'bss', 'dec']:
        o0_size = o0_result.build_size.get(section, 0)
        o1_size = o1_result.build_size.get(section, 0)
        ratio = (o1_size / o0_size * 100) if o0_size > 0 else 0
        print(f"{section:<15} {o0_size:>12} {o1_size:>12} {ratio:>11.1f}%")

    # Performance comparison with verification status
    print("\n--- Performance Comparison (microseconds per iteration) ---")
    print(f"{'Benchmark':<25} {'-O0':>12} {'-O1':>12} {'O1/O0 %':>12} {'Speedup':>10} {'Verify':>8}")
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*10} {'-'*8}")

    o0_benches = {b.name: b for b in o0_result.benchmarks}
    o1_benches = {b.name: b for b in o1_result.benchmarks}
    all_names = sorted(set(o0_benches.keys()) | set(o1_benches.keys()))

    total_o0 = 0
    total_o1 = 0

    for name in all_names:
        o0_b = o0_benches.get(name)
        o1_b = o1_benches.get(name)

        o0_cycles = o0_b.cycles_per_iter if o0_b else 0
        o1_cycles = o1_b.cycles_per_iter if o1_b else 0
        o0_verify = o0_b.verify if o0_b else "N/A"
        o1_verify = o1_b.verify if o1_b else "N/A"

        # Check for failures
        o0_failed = o0_verify == "FAIL"
        o1_failed = o1_verify == "FAIL"

        if o0_failed or o1_failed:
            # Show FAILED for any that failed
            o0_str = "FAILED" if o0_failed else f"{o0_cycles:.2f}"
            o1_str = "FAILED" if o1_failed else f"{o1_cycles:.2f}"
            ratio_str = "N/A"
            speedup_str = "N/A"
        elif o0_cycles > 0 and o1_cycles > 0:
            ratio = (o1_cycles / o0_cycles * 100)
            speedup = o0_cycles / o1_cycles if o1_cycles > 0 else 0
            total_o0 += o0_cycles
            total_o1 += o1_cycles
            o0_str = f"{o0_cycles:.2f}"
            o1_str = f"{o1_cycles:.2f}"
            ratio_str = f"{ratio:.1f}%"
            speedup_str = f"{speedup:.2f}x"
        elif o0_cycles == 0 and o1_cycles > 0:
            ratio = 0
            speedup = float('inf') if o1_cycles > 0 else 0
            o0_str = "N/A"
            o1_str = f"{o1_cycles:.2f}"
            ratio_str = "N/A"
            speedup_str = "N/A"
        else:
            o0_str = f"{o0_cycles:.2f}" if o0_b else "N/A"
            o1_str = f"{o1_cycles:.2f}" if o1_b else "N/A"
            ratio_str = "N/A"
            speedup_str = "N/A"

        # Verification status
        if o0_failed or o1_failed:
            verify_status = "FAIL"
        elif o0_verify == "PASS" and o1_verify == "PASS":
            verify_status = "OK"
        else:
            verify_status = "SKIP"

        print(f"{name:<25} {o0_str:>12} {o1_str:>12} {ratio_str:>12} {speedup_str:>10} {verify_status:>8}")

    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*10} {'-'*8}")

    # Overall summary
    if total_o0 > 0 and total_o1 > 0:
        overall_ratio = (total_o1 / total_o0 * 100)
        overall_speedup = total_o0 / total_o1
        print(f"\n{'OVERALL (passed only)':<25} {total_o0:>12.2f} {total_o1:>12.2f} {overall_ratio:>11.1f}% {overall_speedup:>9.2f}x")

    print("="*80)


def print_comparison(tcc_result: CompilerResult, gcc_result: CompilerResult):
    """Print comparison table of TCC vs GCC results with verification status."""
    print("\n" + "="*80)
    print("BENCHMARK COMPARISON: TCC vs GCC")
    print("="*80)

    # Binary sizes
    print("\n--- Binary Size Comparison ---")
    print(f"{'Section':<15} {'TCC':>12} {'GCC':>12} {'TCC/GCC %':>12}")
    print(f"{'-'*15} {'-'*12} {'-'*12} {'-'*12}")

    for section in ['text', 'data', 'bss', 'dec']:
        tcc_size = tcc_result.build_size.get(section, 0)
        gcc_size = gcc_result.build_size.get(section, 0)
        ratio = (tcc_size / gcc_size * 100) if gcc_size > 0 else 0
        print(f"{section:<15} {tcc_size:>12} {gcc_size:>12} {ratio:>11.1f}%")

    # Benchmark results with verification
    print("\n--- Performance Comparison (microseconds per iteration) ---")
    print(f"{'Benchmark':<25} {'TCC':>12} {'GCC':>12} {'TCC/GCC %':>12} {'Winner':>8} {'Verify':>8}")
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*8} {'-'*8}")

    # Create lookup dicts
    tcc_benches = {b.name: b for b in tcc_result.benchmarks}
    gcc_benches = {b.name: b for b in gcc_result.benchmarks}

    all_names = sorted(set(tcc_benches.keys()) | set(gcc_benches.keys()))

    total_tcc = 0
    total_gcc = 0
    tcc_wins = 0
    gcc_wins = 0

    for name in all_names:
        tcc_b = tcc_benches.get(name)
        gcc_b = gcc_benches.get(name)

        tcc_cycles = tcc_b.cycles_per_iter if tcc_b else 0
        gcc_cycles = gcc_b.cycles_per_iter if gcc_b else 0
        tcc_verify = tcc_b.verify if tcc_b else "N/A"
        gcc_verify = gcc_b.verify if gcc_b else "N/A"

        # Determine verification status for display
        tcc_failed = tcc_verify == "FAIL"
        gcc_failed = gcc_verify == "FAIL"
        tcc_pass = tcc_verify == "PASS"
        gcc_pass = gcc_verify == "PASS"

        # Determine winner based on verification and performance
        if tcc_failed and not gcc_failed:
            # TCC failed, GCC passed or skipped -> GCC wins
            winner = "GCC"
            gcc_wins += 1
            ratio = 0
        elif gcc_failed and not tcc_failed:
            # GCC failed, TCC passed -> TCC wins
            winner = "TCC"
            tcc_wins += 1
            ratio = float('inf')
        elif tcc_cycles > 0 and gcc_cycles > 0:
            # Both have data, compare performance
            ratio = (tcc_cycles / gcc_cycles * 100)
            winner = "TIE" if abs(ratio - 100) < 5 else ("TCC" if tcc_cycles < gcc_cycles else "GCC")
            if winner == "TCC":
                tcc_wins += 1
            elif winner == "GCC":
                gcc_wins += 1
            total_tcc += tcc_cycles
            total_gcc += gcc_cycles
        elif tcc_cycles == 0 and gcc_cycles > 0:
            # TCC has no data, GCC has data - no winner
            ratio = 0
            winner = "N/A"
        elif gcc_cycles == 0 and tcc_cycles > 0:
            # GCC has no data, TCC has data - no winner
            ratio = float('inf')
            winner = "N/A"
        else:
            ratio = 0
            winner = "N/A"

        # Format output strings - show FAILED if verification failed
        if tcc_failed:
            tcc_str = "FAILED"
        else:
            tcc_str = f"{tcc_cycles:.2f}" if tcc_b else "N/A"

        if gcc_failed:
            gcc_str = "FAILED"
        else:
            gcc_str = f"{gcc_cycles:.2f}" if gcc_b else "N/A"

        ratio_str = f"{ratio:.1f}%" if ratio > 0 and ratio != float('inf') else ("N/A" if ratio == 0 else "INF")

        # Verification summary column
        if tcc_failed and gcc_failed:
            verify_status = "BOTH-FAIL"
        elif tcc_failed:
            verify_status = "TCC-FAIL"
        elif gcc_failed:
            verify_status = "GCC-FAIL"
        elif tcc_pass and gcc_pass:
            verify_status = "OK"
        else:
            verify_status = "SKIP"

        print(f"{name:<25} {tcc_str:>12} {gcc_str:>12} {ratio_str:>12} {winner:>8} {verify_status:>8}")

    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*8} {'-'*8}")

    # Overall summary (only for passed benchmarks)
    if total_gcc > 0 and total_tcc > 0:
        overall_ratio = (total_tcc / total_gcc * 100)
        overall_winner = "TCC" if total_tcc < total_gcc else "GCC"
        print(f"\n{'OVERALL (passed only)':<25} {total_tcc:>12.2f} {total_gcc:>12.2f} {overall_ratio:>11.1f}% {overall_winner:>8}")

    print(f"\n--- Summary ---")
    print(f"TCC wins: {tcc_wins}")
    print(f"GCC wins: {gcc_wins}")
    print(f"Ties/NA: {len(all_names) - tcc_wins - gcc_wins}")
    print("="*80)


def main():
    parser = argparse.ArgumentParser(
        description="Build, run and compare TCC vs GCC benchmarks on RP2350"
    )
    parser.add_argument("host", help="Target host IP or hostname (optionally user@host)")
    parser.add_argument("--port", "-p", type=int, default=22, help="SSH port (default: 22)")
    parser.add_argument("--identity", "-i", help="SSH identity file")
    parser.add_argument("--password", help="SSH password")
    parser.add_argument("--skip-build", action="store_true", help="Skip build, use existing binaries")
    parser.add_argument("--only", choices=["tcc", "gcc"], help="Only run one compiler")
    parser.add_argument("--output", "-o", help="Save comparison to file")
    parser.add_argument("--opt-level", "-O", choices=["0", "1", "both"], default="1",
                        help="Optimization level: 0, 1, or 'both' to compare (default: 1)")

    args = parser.parse_args()

    # Parse host
    if "@" in args.host:
        username, hostname = args.host.split("@", 1)
    else:
        username = "mateusz"
        hostname = args.host

    def run_single_opt(opt_level: str, label_suffix: str = "") -> tuple:
        """Run benchmarks for a single optimization level."""
        print("="*80)
        print(f"Running with -O{opt_level}{label_suffix}")
        print("="*80)

        tcc_result = None
        gcc_result = None

        # Build and run TCC
        if not args.only or args.only == "tcc":
            if not args.skip_build:
                success, elf_path, size_info = build_compiler("tcc", args.host, opt_level)
                if not success:
                    print("TCC build failed!")
                    if args.only == "tcc":
                        sys.exit(1)
            else:
                elf_path = Path(__file__).parent / f"build_pico_tcc_O{opt_level}/minimal_uart_picosdk_tcc.elf"
                size_info = get_binary_size(elf_path)

            if elf_path and elf_path.exists():
                success, output = upload_and_run(
                    elf_path, hostname, args.port, username, args.identity, args.password
                )
                benchmarks = parse_benchmark_output(output) if success else []
                tcc_result = CompilerResult(
                    compiler=f"TCC-O{opt_level}",
                    build_success=success,
                    build_size=size_info,
                    benchmarks=benchmarks,
                    raw_output=output
                )

                if success:
                    print(f"\nTCC-O{opt_level} Benchmarks ({len(benchmarks)} found):")
                    for b in benchmarks:
                        print(f"  {b.name}: {b.cycles_per_iter:.2f} cycles/iter")
                    if "TIMEOUT" in output:
                        print("\n⚠ WARNING: Benchmark timeout occurred!")
                else:
                    print(f"\nTCC run failed:\n{output[:1000]}")
                    if "TIMEOUT" in output:
                        print("\n⚠ FAILURE: Benchmark timed out - increase timeout or reduce iterations")

        # Build and run GCC
        if not args.only or args.only == "gcc":
            if not args.skip_build:
                success, elf_path, size_info = build_compiler("gcc", args.host, opt_level)
                if not success:
                    print("GCC build failed!")
                    if args.only == "gcc":
                        sys.exit(1)
            else:
                elf_path = Path(__file__).parent / f"build_pico_gcc_O{opt_level}/minimal_uart_picosdk_gcc.elf"
                size_info = get_binary_size(elf_path)

            if elf_path and elf_path.exists():
                success, output = upload_and_run(
                    elf_path, hostname, args.port, username, args.identity, args.password
                )
                benchmarks = parse_benchmark_output(output) if success else []
                gcc_result = CompilerResult(
                    compiler=f"GCC-O{opt_level}",
                    build_success=success,
                    build_size=size_info,
                    benchmarks=benchmarks,
                    raw_output=output
                )

                if success:
                    print(f"\nGCC-O{opt_level} Benchmarks ({len(benchmarks)} found):")
                    for b in benchmarks:
                        print(f"  {b.name}: {b.cycles_per_iter:.2f} cycles/iter")
                    if "TIMEOUT" in output:
                        print("\n⚠ WARNING: Benchmark timeout occurred!")
                else:
                    print(f"\nGCC run failed:\n{output[:1000]}")
                    if "TIMEOUT" in output:
                        print("\n⚠ FAILURE: Benchmark timed out - increase timeout or reduce iterations")

        return tcc_result, gcc_result

    print("="*80)
    print("RP2350 Benchmark Runner - TCC vs GCC")
    print("="*80)
    print(f"Target: {username}@{hostname}:{args.port}")
    print("")

    # Run based on optimization level selection
    if args.opt_level == "both":
        # Run both -O0 and -O1 and compare
        tcc_o0, gcc_o0 = run_single_opt("0", " (1/2)")
        print("\n")
        tcc_o1, gcc_o1 = run_single_opt("1", " (2/2)")

        # Print -O0 vs -O1 comparison for each compiler
        if tcc_o0 and tcc_o1:
            print("\n")
            print_opt_comparison("TCC", tcc_o0, tcc_o1)
        if gcc_o0 and gcc_o1:
            print("\n")
            print_opt_comparison("GCC", gcc_o0, gcc_o1)

        # Also print TCC vs GCC for -O1 (the default comparison)
        if tcc_o1 and gcc_o1:
            print("\n")
            print_comparison(tcc_o1, gcc_o1)
    else:
        # Run single optimization level
        tcc_result, gcc_result = run_single_opt(args.opt_level)

        # Print comparison if both results available
        if tcc_result and gcc_result and tcc_result.build_success and gcc_result.build_success:
            print_comparison(tcc_result, gcc_result)

    # Save to file if requested
    if args.output:
        with open(args.output, 'w') as f:
            f.write("="*80 + "\n")
            f.write("TCC vs GCC Benchmark Results\n")
            f.write("="*80 + "\n\n")

            if args.opt_level == "both":
                # Save results from both optimization levels
                if tcc_o0:
                    f.write(f"--- TCC -O0 Raw Output ---\n")
                    f.write(tcc_o0.raw_output)
                    f.write("\n\n")
                if gcc_o0:
                    f.write(f"--- GCC -O0 Raw Output ---\n")
                    f.write(gcc_o0.raw_output)
                    f.write("\n\n")
                if tcc_o1:
                    f.write(f"--- TCC -O1 Raw Output ---\n")
                    f.write(tcc_o1.raw_output)
                    f.write("\n\n")
                if gcc_o1:
                    f.write(f"--- GCC -O1 Raw Output ---\n")
                    f.write(gcc_o1.raw_output)
                    f.write("\n\n")
            else:
                # Save single optimization level results
                if tcc_result:
                    f.write("--- TCC Raw Output ---\n")
                    f.write(tcc_result.raw_output)
                    f.write("\n\n")
                if gcc_result:
                    f.write("--- GCC Raw Output ---\n")
                    f.write(gcc_result.raw_output)
                    f.write("\n\n")
        print(f"\nResults saved to: {args.output}")

    print("\nDone!")


if __name__ == "__main__":
    main()
