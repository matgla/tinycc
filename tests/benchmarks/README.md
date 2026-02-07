# RP2350 Benchmark Suite - TCC vs GCC

Benchmark suite for comparing TCC (TinyCC) vs GCC performance on Raspberry Pi RP2350 (ARM Cortex-M33).

## Quick Start

```bash
# Build, run and compare both compilers on RP2350
python3 run_benchmark.py <rp2350_ip_address>

# Example
python3 run_benchmark.py 192.168.0.113

# With SSH key authentication
python3 run_benchmark.py user@192.168.0.113 -i ~/.ssh/id_rsa
```

## Usage

### Basic Usage

```bash
# Compare TCC vs GCC with -O1 optimization (default)
python3 run_benchmark.py 192.168.0.113

# Test with -O0 optimization
python3 run_benchmark.py 192.168.0.113 -O 0

# Test both -O0 and -O1 and compare optimization impact
python3 run_benchmark.py 192.168.0.113 -O both
```

### Options

```bash
python3 run_benchmark.py [host] [options]

Options:
  -O {0,1,both}        Optimization level (default: 1)
  --only {tcc,gcc}     Run only one compiler
  --skip-build         Skip build, use existing binaries
  -o OUTPUT            Save results to file
  -i IDENTITY          SSH identity file
  -p PORT              SSH port (default: 22)
```

### Examples

```bash
# Run only TCC with -O0
python3 run_benchmark.py 192.168.0.113 --only tcc -O 0

# Run both optimization levels and save results
python3 run_benchmark.py 192.168.0.113 -O both -o results.txt

# Skip rebuild (use existing binaries)
python3 run_benchmark.py 192.168.0.113 --skip-build
```

## Setup

### Requirements

- CMake 3.13+, arm-none-eabi-gcc, armv8m-tcc
- Python 3 with paramiko: `pip install -r requirements.txt`
- RP2350 board with CMSIS-DAP probe
- Remote Linux host with OpenOCD and SSH access

See [RP2350_README.md](RP2350_README.md) for detailed setup instructions.

## Benchmarks

### Core Micro-benchmarks

| Benchmark | Description |
|-----------|-------------|
| integer_math | Integer arithmetic (mul, shift, xor) |
| float_math | Floating point operations (soft-float) |
| array_sum | Memory access patterns |
| function_calls | Function call overhead |
| conditionals | Branch prediction |
| switch_stmt | Jump table performance |
| strcpy | String copy |
| memcpy | Memory copy |
| strcmp | String comparison |
| fibonacci | Recursive function calls |
| bubble_sort | Nested loops |
| linked_list | Pointer chasing |

### MiBench Suite (Real-world Benchmarks)

| Benchmark | Category | Description |
|-----------|----------|-------------|
| mibench_sha | Security | SHA-1 cryptographic hash |
| mibench_bitcount | Automotive | Bit counting algorithms |
| mibench_crc32 | Telecomm | CRC32 checksum computation |

See [MIBENCH_INTEGRATION.md](MIBENCH_INTEGRATION.md) for full MiBench integration plan.

## Files

- `run_benchmark.py` - Main build/run/compare script
- `CMakeLists.txt` - Build configuration
- `minimal_uart_picosdk.c` - RP2350 entry point
- `benchmark_main.c` - Benchmark harness
- `bench_*.c` - Individual benchmarks
- `cycle_counter.c` - ARM DWT cycle counter
- `benchmarks.h` - Common header
- `rp2350_ram.ld` - Linker script

## Build Directories

- `build_pico_tcc/` - TCC build artifacts
- `build_pico_gcc/` - GCC build artifacts
- `libs/pico-sdk/` - Pico SDK

Clean builds: `rm -rf build_pico_tcc build_pico_gcc`

## Output Example

```
BENCHMARK COMPARISON: TCC vs GCC

--- Binary Size Comparison ---
Section                  TCC          GCC    TCC/GCC %
text                   35036        42356       82.7%

--- Performance (cycles per iteration) ---
Benchmark                       TCC          GCC    TCC/GCC %   Winner
integer_math                  45.23        38.12      118.6%      GCC
float_math                   123.45       156.78       78.7%      TCC
...

OVERALL                      1234.5       1456.7       84.7%      TCC

TCC wins: 8
GCC wins: 4
```

## Documentation

- [RP2350_README.md](RP2350_README.md) - Detailed setup guide
- [RP2350_DEBUG_GUIDE.md](RP2350_DEBUG_GUIDE.md) - Troubleshooting
