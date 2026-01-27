# RP2350 UART Debugging Guide

## Key Differences from Pico (RP2040)

RP2350 has **different clock and reset defaults** compared to RP2040:

### 1. Clock Sources (CRITICAL!)
```
RP2040: Default boot uses 12MHz XOSC -> PLL -> 125MHz system
RP2350: Default boot may use ROSC (Ring Oscillator) or different PLL settings

The peripheral clock (clk_peri) must be enabled AND sourced correctly!
```

### 2. Reset Bits (DIFFERENT!)
```
RP2040: RESET_UART0 = bit 22
RP2350: May be different - check datasheet!
```

### 3. GPIO Bank for Pins 32-47 (HI GPIOs)
```
GPIO0-31:  IO_BANK0_BASE = 0x40028000
GPIO32-47: IO_BANK0_BASE + 0x100 (QSPI/HI bank!)
```

## GDB Checkpoints

### Step 1: Verify Clocks
```gdb
# Check if clk_peri is enabled
x/wx 0x40008048
# Should show bit 11 set (0x800 or higher)
# Bits: [11]=ENABLE, [16:14]=AUXSRC, [7:0]=SRC

# Check system clock source
x/wx 0x4000803c
# Should be 0x1 for XOSC, 0x2 for PLL, etc.
```

### Step 2: Verify Resets
```gdb
# Check reset status
x/wx 0x4000c000
# Check reset done
x/wx 0x4000c008

# UART0 reset bit should be 0 (not in reset)
# RESET_DONE bit 22 should be 1 (done)
```

### Step 3: Verify UART Registers Accessible
```gdb
# Try to read UART flags register
x/wx 0x40070018
# Should NOT bus fault - if it does, clock/reset is wrong

# Check UART control
x/wx 0x40070030
```

### Step 4: Check GPIO Configuration
```gdb
# GPIO32 control (HI bank offset!)
x/wx 0x40028104
# Should be 0x2 for UART function

# GPIO33 control
x/wx 0x4002810c
```

### Step 5: Check UART Baud Rate Settings
```gdb
# Integer baud rate divisor
x/wx 0x40070024
# Should be 81 for 115200 @ 150MHz

# Fractional baud rate divisor
x/wx 0x40070028
# Should be 24

# Line control
x/wx 0x4007002c
# Should be 0x70 (8 bits, FIFOs)

# Control register
x/wx 0x40070030
# Should have bits 0, 8, 9 set (UARTEN, TXE, RXE)
```

## Common Issues

### Issue 1: Peripheral Clock Not Running
If `CLK_PERI_CTRL` (0x40008048) is 0, the UART registers won't work.

**Fix:** Enable it with the correct source:
```c
// Source from pll_sys (default, most reliable)
CLK_PERI_CTRL = (1u << 11);  // Just enable, keep auxsrc=0

// OR explicitly set source
CLK_PERI_CTRL = (0u << 12) | (1u << 11);  // AUXSRC=0 (clk_sys), ENABLE=1
```

### Issue 2: Wrong Clock Frequency Assumption
The bootrom may have set a different clock than expected.

**Check:**
```gdb
# Read clock frequency (if available in bootrom)
# Or measure by toggling GPIO and checking timing
```

### Issue 3: GPIO32/33 Not in Right Bank
GPIO32+ are in the "HI" bank which has different control registers.

**Reference from pico-sdk:**
```c
// For GPIO0-29:  IO_BANK0_BASE + 0x04 + (pin * 8)
// For GPIO30-35: IO_BANK0_BASE + 0x100 + ((pin-30) * 8)
// Actually for RP2350, check the exact offset!
```

### Issue 4: UART Needs Full Configuration
Just enabling clock isn't enough - need proper init sequence:

1. Enable clock
2. Deassert reset
3. Wait for reset done
4. Disable UART (CR = 0)
5. Set baud rate
6. Set line control
7. Enable UART

## Minimal Working Example (from reference)

```c
// Simpler approach - just poll and output
void uart_putc(char c) {
    // Wait for TX FIFO not full
    while (*(volatile unsigned int*)0x40070018 & (1<<5));
    *(volatile unsigned int*)0x40070000 = c;
}

void init(void) {
    // Enable peripheral clock
    *(volatile unsigned int*)0x40008048 = (1<<11);
    
    // Deassert resets
    *(volatile unsigned int*)0x4000c000 &= ~((1<<22) | (1<<5));
    
    // Wait for reset done
    while ((*(volatile unsigned int*)0x4000c008 & ((1<<22) | (1<<5))) != ((1<<22) | (1<<5)));
    
    // Configure GPIO32 for UART0
    *(volatile unsigned int*)0x40028104 = 2;
    
    // Set baud rate (assuming 150MHz)
    *(volatile unsigned int*)0x40070024 = 81;  // IBRD
    *(volatile unsigned int*)0x40070028 = 24;  // FBRD
    *(volatile unsigned int*)0x4007002c = 0x70; // LCRH
    *(volatile unsigned int*)0x40070030 = 0x301; // CR (TXE + RXE + UARTEN)
}
```

## GDB Commands to Run

```bash
# Connect to OpenOCD
target remote localhost:3333

# Reset and halt
monitor reset halt

# Load binary
load

# Set breakpoint at main
break main

# Continue
continue

# At main, check these addresses:
# (Add these to a gdb script)
```

Create `debug.gdb`:
```gdb
target remote localhost:3333
monitor reset halt
load

# Check clocks
echo "CLK_PERI_CTRL: "
x/wx 0x40008048
echo "CLK_SYS_CTRL: "
x/wx 0x4000803c

# Check resets
echo "RESET: "
x/wx 0x4000c000
echo "RESET_DONE: "
x/wx 0x4000c008

# Check GPIO32
echo "GPIO32_CTRL: "
x/wx 0x40028104

# Check UART
echo "UART_CR: "
x/wx 0x40070030
echo "UART_IBRD: "
x/wx 0x40070024
echo "UART_FBRD: "
x/wx 0x40070028

# Set breakpoint and run
break main
continue
```

Run with: `arm-none-eabi-gdb -x debug.gdb build/benchmark_gcc_ram_debug.elf`
