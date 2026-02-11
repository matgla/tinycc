# Floating Point Library Structure

TinyCC modular floating point library supporting multiple ARM architectures and FPU configurations.

## Directory Layout

```
lib/fp/
├── fp_abi.h                      # Common ABI definitions and helpers
├── Makefile                      # Master build script
├── README.md                     # This file
│
├── soft/                         # Software-only floating point
│   ├── fadd.c                   # Addition (float, double)
│   ├── fmul.c                   # Multiplication (float, double)
│   ├── fdiv.c                   # Division (float, double)
│   ├── fcmp.c                   # Single-precision comparison
│   ├── dcmp.c                   # Double-precision comparison
│   ├── conv.c                   # Integer/float conversions
│   ├── fmt.c                    # Format conversions (f2d, d2f)
│   └── Makefile
│
└── arm/                          # ARM architecture specific
    ├── vfpv4-sp/                # Cortex-M4F (single-precision FPU)
    │   ├── fops.c               # Float ops (VADD.F32, VMUL.F32, etc.)
    │   ├── fcmp.c               # Float comparison
    │   ├── conv.c               # Float conversions
    │   ├── dops_soft.c          # Double ops (delegated to soft)
    │   └── Makefile
    │
    ├── vfpv5-dp/                # Cortex-M7 (double-precision FPU)
    │   ├── ops.c                # Float and double arithmetic
    │   ├── cmp.c                # Float and double comparison
    │   ├── conv.c               # All conversions (HW)
    │   └── Makefile
    │
    └── rp2350/                  # RP2350 (double coprocessor)
        ├── dcp_init.c           # Coprocessor initialization
        ├── dcp_ops.c            # Double ops via DCP
        ├── dcp_cmp.c            # Double comparison via DCP
        ├── dcp_conv.c           # Double conversions via DCP
        └── Makefile
```

## Building

### Build for specific FPU

```bash
# Pure software FP (Cortex-M0, M0+, M3)
cd lib/fp && make FPU=soft

# VFPv4 single-precision (Cortex-M4F)
cd lib/fp && make FPU=vfpv4-sp

# VFPv5 double-precision (Cortex-M7)
cd lib/fp && make FPU=vfpv5-dp

# RP2350 with double coprocessor
cd lib/fp && make FPU=rp2350
```

### Build all variants

```bash
cd lib/fp && make all-variants
```

Output libraries (short canonical names, usable with `-l` flags):
- `libsoftfp.a` / `libsoftfp.so` - Software floating point (`-lsoftfp`)
- `libvfpv4sp.a` / `libvfpv4sp.so` - VFPv4 single-precision (`-lvfpv4sp`)
- `libvfpv5dp.a` / `libvfpv5dp.so` - VFPv5 double-precision (`-lvfpv5dp`)
- `librp2350fp.a` / `librp2350fp.so` - RP2350 double coprocessor (`-lrp2350fp`)

Backward-compatible symlinks are also created:
- `libtcc1-fp-soft-$(TARGET).a` → `libsoftfp.a`
- `libtcc1-fp-vfpv4-sp-$(TARGET).a` → `libvfpv4sp.a`
- etc.

### Build shared libraries (for YasOS dynamic linking)

```bash
cd lib/fp && make FPU=soft build-shared
cd lib/fp && make FPU=vfpv4-sp build-shared
# ... or all at once:
cd lib/fp && make all-shared
```

(Where `$(TARGET)` is the target architecture specified during build, e.g., `armv8m`, `arm`)

## Architecture Notes

### Soft Float (`soft/`)
Pure C implementations of ARM EABI FP functions. Used when no hardware FPU available.

**Implements:**
- `__aeabi_fadd`, `__aeabi_fsub`, `__aeabi_fmul`, `__aeabi_fdiv` (float)
- `__aeabi_dadd`, `__aeabi_dsub`, `__aeabi_dmul`, `__aeabi_ddiv` (double)
- `__aeabi_cfcmple`, `__aeabi_cfcmplt`, etc. (float comparison)
- `__aeabi_cdcmple`, `__aeabi_cdcmplt`, etc. (double comparison)
- `__aeabi_f2iz`, `__aeabi_i2f`, `__aeabi_f2d`, etc. (conversions)

### VFPv4-sp (Cortex-M4F)
Hardware single-precision FPU with software double-precision fallback.

**Features:**
- Uses VFP instructions for float operations: `VADD.F32`, `VMUL.F32`, `VDIV.F32`
- Uses hardware comparison: `VCMP.F32`, `VMRS`
- Hardware conversions: `VCVT.S32.F32`, `VCVT.F32.S32`
- Double operations delegated to soft float library

### VFPv5-dp (Cortex-M7)
Full hardware support for both single and double-precision.

**Features:**
- Hardware float: `VADD.F32`, `VMUL.F32`, `VDIV.F32`
- Hardware double: `VADD.F64`, `VMUL.F64`, `VDIV.F64`
- Hardware comparison for both precisions
- Hardware conversions between int/float/double
- Supports FMA (fused multiply-add) for better precision

### RP2350 (DCP - Double Coprocessor)
RP2350 has dedicated double-precision coprocessor for efficient 64-bit float operations.

**Features:**
- Uses coprocessor instructions via MCR/MCRR/MRC/MRRC
- Separate DCP from main processor
- Requires initialization (`rp2350_dcp_init()`)
- Single-precision may use VFPv4-sp or software

## ARM EABI Floating Point ABI

All implementations conform to ARM EABI Floating Point ABI:

### Call Convention
- Float arguments in `r0`, `r1`, `r2`, `r3` (software ABI)
- Float arguments in `s0`-`s15` (hardware ABI with `-mfloat-abi=hard`)
- Double arguments in `r0:r1`, `r2:r3` or `d0`, `d1` registers

### Comparison Results
Comparison functions return CPSR flags in `r0`:
- **N** (bit 31): Less than
- **Z** (bit 30): Equal
- **C** (bit 29): Greater than
- **V** (bit 28): Unordered (NaN)

## Implementation Notes

### Incomplete Stubs
Most soft float functions are currently TODO stubs. Priority implementations:
1. Basic arithmetic (add, sub, mul, div)
2. Comparisons
3. int/float conversions
4. float/double conversions

### Soft Float Algorithm Suggestions
- **Significand**: Stored as normalized 24-bit (float) or 53-bit (double)
- **Exponent**: Biased format (127 for float, 1023 for double)
- **Special Cases**: NaN, Inf, denormalized, zero
- **Rounding**: Round-to-nearest-even (banker's rounding)

### Optimization Opportunities
1. Use assembly stubs instead of inline asm for better optimization
2. Cache DCP status in RP2350 implementation
3. SIMD operations for vector float operations
4. Fast paths for common cases (normalized numbers)

## Testing

Test with IR tests in `tests/ir_tests/`:
- `71_float_simple.c` - Basic float operations
- `71_double_simple.c` - Basic double operations
- `72_float_result.c` - Float results and conversions
- `73_double_printf.c` - Double precision with printf

## References

- ARM EABI: https://github.com/ARM-software/abi-aa/releases/download/2023Q3/aapcs32.pdf
- ARM VFP: ARM Cortex-M4 Devices Generic User Guide
- RP2350: https://datasheets.raspberrypi.org/rp2350/rp2350-datasheet.pdf
