# RP2350 (Raspberry Pi Pico 2) Benchmarking Guide

This guide explains how to run TCC vs GCC benchmarks on actual RP2350 hardware with real cycle counter measurements.

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350-based)
- **Debug probe** (one of):
  - Raspberry Pi Debug Probe (CMSIS-DAP)
  - Pico Probe (another Pico running debug firmware)
  - J-Link or other ARM debugger
- **USB cable** for power and UART
- **Host computer** with Linux/macOS/Windows

## Software Requirements

### Required Tools

```bash
# Install OpenOCD (with RP2350 support)
# Ubuntu/Debian:
sudo apt-get install openocd

# Or build from source for latest RP2350 support:
git clone https://github.com/openocd-org/openocd.git
cd openocd
./bootstrap
./configure --enable-cmsis-dap
make
sudo make install

# Install picotool (alternative flashing method)
git clone https://github.com/raspberrypi/picotool.git
cd picotool
mkdir build && cd build
cmake ..
make
sudo make install

# Install Python dependencies
pip install pyserial
```

### Finding Your Serial Port

```bash
# Linux - list all serial ports
ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null

# Check which port appears when you plug in the Pico
dmesg | tail -20

# macOS
ls /dev/tty.usb*

# Windows - use Device Manager, typically COM3, COM4, etc.
```

## Quick Start

### 1. Build Benchmarks

```bash
cd tests/benchmarks

# Build for RP2350
make rp2350

# Verify binaries were created
ls -la build/
```

### 2. Run Automated Benchmark Script

The `rp2350_runner.py` script automates everything:

```bash
# Using OpenOCD (default - requires debug probe)
python rp2350_runner.py --port /dev/ttyACM0

# Using picotool (simpler - just USB)
python rp2350_runner.py --port /dev/ttyACM0 --picotool

# Specify custom OpenOCD config
python rp2350_runner.py \
    --port /dev/ttyACM0 \
    --openocd-cfg interface/cmsis-dap.cfg \
    --openocd-target target/rp2350.cfg
```

### 3. Manual Steps (if automated script doesn't work)

#### Option A: Using OpenOCD

```bash
# Terminal 1: Start OpenOCD server
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg

# Terminal 2: Flash and run
openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
    -c "program build/benchmark_tcc.elf verify reset exit"

# Terminal 2: Capture output
picocom -b 115200 /dev/ttyACM0
# or
minicom -D /dev/ttyACM0 -b 115200
```

#### Option B: Using picotool

```bash
# Put Pico in BOOTSEL mode (hold BOOTSEL while plugging in, or use picotool)
picotool reboot -f -u

# Flash the binary
picotool load build/benchmark_tcc.elf -f

# Reboot to run
picotool reboot -f

# Capture output
picocom -b 115200 /dev/ttyACM0
```

#### Option C: UF2 Drag & Drop

```bash
# Convert ELF to UF2
make uf2  # (requires elf2uf2-rs or picotool)

# Or manually:
elf2uf2-rs build/benchmark_tcc.elf build/benchmark_tcc.uf2

# Then copy UF2 to the RPI-RP2 drive that appears when in BOOTSEL mode
cp build/benchmark_tcc.uf2 /media/$USER/RPI-RP2/
```

## Understanding the Output

The benchmark outputs:

```
========================================
ARMv8-M Benchmark Suite
========================================
Compiler: TCC (0x544343)
Build: TINYCC
Optimization: -O1
Target: ARM Cortex-M33

Benchmark               Cycles/iter     Result
------------------------------------------------
integer_math               45.50          45
float_math               8999.00        8999
array_sum                  89.25          89
...

All benchmarks completed.
```

**Important**: The cycle counts are measured using the ARM DWT (Data Watchpoint and Trace) cycle counter, which provides accurate clock-cycle measurements on real hardware.

## Troubleshooting

### OpenOCD can't find the device

```bash
# Check USB device is detected
lsusb | grep -i "CMSIS\|Raspberry\|Debug"

# Check permissions (Linux)
sudo usermod -a -G dialout $USER
# Log out and back in

# Try with sudo temporarily
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
```

### No UART output

1. **Check baud rate**: RP2350 uses 115200 baud by default
2. **Check correct port**: Use `dmesg` to see which /dev/tty* appears when plugging in
3. **Check wiring**: UART0 TX is GPIO0 (pin 1), RX is GPIO1 (pin 2)
4. **Use a terminal program**:
   ```bash
   picocom -b 115200 /dev/ttyACM0
   # Press Ctrl+A then Ctrl+X to exit
   ```

### Binary won't run

1. **Check it's built for correct target**: 
   ```bash
   arm-none-eabi-readelf -h build/benchmark_tcc.elf | grep Machine
   # Should show: ARM
   ```

2. **Verify entry point**:
   ```bash
   arm-none-eabi-readelf -h build/benchmark_tcc.elf | grep Entry
   # Should be in 0x10000000 range for RP2350
   ```

### Cycle counter shows 0

The DWT cycle counter is enabled in `boot.S`. If it shows 0:
- Check the binary was compiled for RP2350 (not mps2-an505)
- The boot.S for RP2350 includes DWT enable code
- QEMU returns 0 for DWT (expected behavior)

## Benchmark Results Format

Saved results have this naming convention:
```
build/rp2350_gcc_20250130_114523.txt
build/rp2350_tcc_20250130_114545.txt
build/rp2350_summary_20250130_114545.txt
```

The summary file contains:
- Binary sizes
- Cycles per iteration for each benchmark
- GCC/TCC ratio (ratio > 1.0 means TCC is faster)

## Advanced Usage

### Run only one compiler

```bash
python rp2350_runner.py --gcc-only
python rp2350_runner.py --tcc-only
```

### Custom output directory

```bash
python rp2350_runner.py --output-dir /path/to/results
```

### Different debug probe

For J-Link:
```bash
python rp2350_runner.py \
    --openocd-cfg interface/jlink.cfg \
    --openocd-target target/rp2350.cfg
```

For picoprobe:
```bash
python rp2350_runner.py \
    --openocd-cfg interface/picoprobe.cfg \
    --openocd-target target/rp2350.cfg
```

## Technical Details

### Memory Map

- **Flash (XIP)**: 0x10000000 - 0x10400000 (4MB)
- **SRAM**: 0x20000000 - 0x20082000 (520KB)
- **Stack**: Top of SRAM (grows down from 0x20082000)

### Boot Process

1. RP2350 bootrom loads from flash
2. `boot.S` runs:
   - Copies .data from FLASH to RAM
   - Zeroes .bss section
   - Enables DWT cycle counter
   - Calls `main()`

### UART Output

The `minilibc_rp2350.c` provides minimal printf that writes to UART0:
- Assumes UART is initialized by bootrom
- No flow control
- Outputs at 115200 baud, 8N1

### DWT Cycle Counter

The ARMv8-M DWT is used for timing:
```c
// Enable
core_debug->DEMCR |= CORE_DEBUG_DEMCR_TRCENA_Msk;
dwt->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

// Read
cycles = dwt->CYCCNT;
```

This gives accurate clock-cycle counts (not wall time), perfect for compiler comparison.
