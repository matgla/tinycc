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
    build_dir = script_dir / f"build_pico_{compiler.lower()}"
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
        "-DPICO_PLATFORM=rp2350",
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
    """Parse benchmark output and extract results."""
    results = []

    # Look for benchmark table lines
    # Format: name iterations cycles_per_iter result
    lines = output.split('\n')

    for line in lines:
        line = line.strip()
        # Match benchmark result lines
        # Example: "fibonacci_recursive      1000       45.23          0"
        match = re.match(r'^(\S+)\s+(\d+)\s+([\d.]+)\s+(-?\d+)$', line)
        if match:
            name = match.group(1)
            iterations = int(match.group(2))
            cycles_per_iter = float(match.group(3))
            result = int(match.group(4))
            results.append(BenchmarkResult(
                name=name,
                iterations=iterations,
                cycles_per_iter=cycles_per_iter,
                result=result,
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

# Configure serial port  
stty -F $SERIAL 115200 cs8 -cstopb -parenb raw -echo 2>/dev/null || true

# Clear previous output
rm -f /tmp/serial_out.txt
touch /tmp/serial_out.txt

# Open serial port for reading using file descriptor (keeps port open)
exec 3<$SERIAL

# Clear any pending data
(timeout 0.1 cat <&3 >/dev/null 2>&1 || true)

# Start capturing serial output in background from FD 3
# Using dd for better TTY handling
dd if=/dev/fd/3 of=/tmp/serial_out.txt bs=1 2>/dev/null &
SERIAL_PID=$!

# Give serial capture time to start
sleep 0.1

# Run OpenOCD - program ELF and run (OpenOCD supports ELF directly)
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "adapter speed 5000" \\
    -c "init" \\
    -c "reset halt" \\
    -c "program $ELF verify reset exit" \\
    -c "shutdown" 2>&1 &

OPENOCD_PID=$!

# Wait for benchmark completion signals (90s timeout)
echo "Waiting for benchmark output..."
TIMEOUT=90
ELAPSED=0
COMPLETED=0

while [ $ELAPSED -lt $TIMEOUT ]; do
    # Check for completion signals
    if grep -q "benchmark stopped" /tmp/serial_out.txt 2>/dev/null; then
        echo "✓ Benchmark stopped signal received!"
        COMPLETED=1
        break
    fi
    if grep -q "Benchmark completed" /tmp/serial_out.txt 2>/dev/null; then
        echo "✓ Benchmark completed!"
        COMPLETED=1
        break
    fi
    if grep -q "Benchmark failed" /tmp/serial_out.txt 2>/dev/null; then
        echo "✗ Benchmark failed!"
        COMPLETED=1
        break
    fi
    
    # Check if OpenOCD is still running
    if ! kill -0 $OPENOCD_PID 2>/dev/null; then
        # OpenOCD exited, give a bit more time to capture output
        sleep 1
        # Check one more time for completion
        if grep -qE "(benchmark stopped|Benchmark completed|Benchmark failed)" /tmp/serial_out.txt 2>/dev/null; then
            echo "✓ Benchmark finished!"
            COMPLETED=1
        fi
        break
    fi
    
    sleep 0.5
    ELAPSED=$((ELAPSED + 1))
done

if [ $ELAPSED -ge $TIMEOUT ]; then
    echo "Timeout after ${{TIMEOUT}}s"
fi

# Small delay to ensure all output is captured
sleep 0.5

# Kill serial capture
kill $SERIAL_PID 2>/dev/null || true
wait $SERIAL_PID 2>/dev/null || true

# Kill OpenOCD if still running
kill $OPENOCD_PID 2>/dev/null || true
wait $OPENOCD_PID 2>/dev/null || true

# Output captured data
echo ""
echo "===SERIAL_OUTPUT_START==="
cat /tmp/serial_out.txt 2>/dev/null
echo "===SERIAL_OUTPUT_END==="
'''
    remote_combined = "/tmp/run_test.sh"
    sftp.putfo(__import__("io").BytesIO(combined_script.encode()), remote_combined)
    ssh.exec_command(f"chmod +x {remote_combined}")

    print("Running benchmark on target...")
    stdin, stdout, stderr = ssh.exec_command(remote_combined, timeout=70)
    output = stdout.read().decode()
    errors = stderr.read().decode()

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
    """Print comparison between -O0 and -O1 for the same compiler."""
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
    
    # Performance comparison
    print("\n--- Performance Comparison (cycles per iteration) ---")
    print(f"{'Benchmark':<25} {'-O0':>12} {'-O1':>12} {'O1/O0 %':>12} {'Speedup':>10}")
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*10}")
    
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
        
        if o0_cycles > 0 and o1_cycles > 0:
            ratio = (o1_cycles / o0_cycles * 100)
            speedup = o0_cycles / o1_cycles if o1_cycles > 0 else 0
            total_o0 += o0_cycles
            total_o1 += o1_cycles
        else:
            ratio = 0
            speedup = 0
        
        o0_str = f"{o0_cycles:.2f}" if o0_cycles > 0 else "N/A"
        o1_str = f"{o1_cycles:.2f}" if o1_cycles > 0 else "N/A"
        ratio_str = f"{ratio:.1f}%" if ratio > 0 else "N/A"
        speedup_str = f"{speedup:.2f}x" if speedup > 0 else "N/A"
        
        print(f"{name:<25} {o0_str:>12} {o1_str:>12} {ratio_str:>12} {speedup_str:>10}")
    
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*10}")
    
    # Overall summary
    if total_o0 > 0 and total_o1 > 0:
        overall_ratio = (total_o1 / total_o0 * 100)
        overall_speedup = total_o0 / total_o1
        print(f"\n{'OVERALL':<25} {total_o0:>12.2f} {total_o1:>12.2f} {overall_ratio:>11.1f}% {overall_speedup:>9.2f}x")
    
    print("="*80)


def print_comparison(tcc_result: CompilerResult, gcc_result: CompilerResult):
    """Print comparison table of TCC vs GCC results."""
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

    # Benchmark results
    print("\n--- Performance Comparison (cycles per iteration) ---")
    print(f"{'Benchmark':<25} {'TCC':>12} {'GCC':>12} {'TCC/GCC %':>12} {'Winner':>8}")
    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*8}")

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

        if tcc_cycles > 0 and gcc_cycles > 0:
            ratio = (tcc_cycles / gcc_cycles * 100)
            winner = "TIE" if abs(ratio - 100) < 5 else ("TCC" if tcc_cycles < gcc_cycles else "GCC")
            if winner == "TCC":
                tcc_wins += 1
            elif winner == "GCC":
                gcc_wins += 1
            total_tcc += tcc_cycles
            total_gcc += gcc_cycles
        else:
            ratio = 0
            winner = "N/A"

        tcc_str = f"{tcc_cycles:.2f}" if tcc_cycles > 0 else "N/A"
        gcc_str = f"{gcc_cycles:.2f}" if gcc_cycles > 0 else "N/A"
        ratio_str = f"{ratio:.1f}%" if ratio > 0 else "N/A"

        print(f"{name:<25} {tcc_str:>12} {gcc_str:>12} {ratio_str:>12} {winner:>8}")

    print(f"{'-'*25} {'-'*12} {'-'*12} {'-'*12} {'-'*8}")

    # Overall summary
    if total_gcc > 0 and total_tcc > 0:
        overall_ratio = (total_tcc / total_gcc * 100)
        overall_winner = "TCC" if total_tcc < total_gcc else "GCC"
        print(f"\n{'OVERALL':<25} {total_tcc:>12.2f} {total_gcc:>12.2f} {overall_ratio:>11.1f}% {overall_winner:>8}")

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
                elf_path = Path(__file__).parent / f"build_pico_tcc/minimal_uart_picosdk_tcc.elf"
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
                else:
                    print(f"\nTCC run failed:\n{output[:1000]}")

        # Build and run GCC
        if not args.only or args.only == "gcc":
            if not args.skip_build:
                success, elf_path, size_info = build_compiler("gcc", args.host, opt_level)
                if not success:
                    print("GCC build failed!")
                    if args.only == "gcc":
                        sys.exit(1)
            else:
                elf_path = Path(__file__).parent / f"build_pico_gcc/minimal_uart_picosdk_gcc.elf"
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
                else:
                    print(f"\nGCC run failed:\n{output[:1000]}")
        
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
