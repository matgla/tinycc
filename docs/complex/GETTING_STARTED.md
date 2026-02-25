# Complex Number Support - Getting Started Guide

This guide helps you get started implementing complex number support in TinyCC.

## Prerequisites

Before starting, ensure you have:
- Working TinyCC build environment
- ARM cross-compiler (`arm-none-eabi-gcc`) for comparison
- Python 3 with pytest for testing

```bash
# Verify build works
make clean && make cross -j$(nproc)

# Verify tests run
make test-venv
make test-prepare
cd tests/ir_tests && python run.py -c 01_hello_world.c
```

## IMPORTANT: Read This First

**⚠️ CRITICAL:** Before starting Phase 1, you MUST complete Phase 0 (Research) to make a fundamental design decision. The current VT_BTYPE mask (0x000f) only supports values 0-15, but we need value 16 for VT_CDOUBLE.

**Two paths forward:**
1. **Expand VT_BTYPE mask** to 0x001f (requires auditing ~50-100 code locations)
2. **Use struct-based approach** (map complex to struct early, simpler but loses type info)

See README.md Phase 0 for details.

## Quick Start: Phase 1 (Type System)

**Prerequisites:** Phase 0 complete, design decision made.

### Step 1: Expand VT_BTYPE Mask (if chosen)

Edit `tcc.h` around line 1000:

```c
/* BEFORE: */
#define VT_BTYPE    0x000f  /* mask for basic type */

/* AFTER: */
#define VT_BTYPE    0x001f  /* mask for basic type (expanded for complex) */
```

**Then run tests:**
```bash
make clean && make cross -j$(nproc)
make test -j16  # Verify no regressions
```

### Step 2: Add Type Constants

Edit `tcc.h` around line 1185:

```c
#define VT_BOOL 11      /* ISOC99 boolean type */
/* 12 is available for future use */
#define VT_QLONG 13     /* 128-bit integer */
#define VT_QFLOAT 14    /* 128-bit float */
#define VT_CFLOAT 15    /* float _Complex */
#define VT_CDOUBLE 16   /* double _Complex (requires VT_BTYPE=0x001f) */
```

### Step 3: Update Parser

Edit `tccgen.c` function `parse_btype()`. Find the `TOK_COMPLEX` case around line 5886:

**Current:**
```c
case TOK_COMPLEX:
    tcc_error("_Complex is not yet supported");
```

**Change to:**
```c
case TOK_COMPLEX:
    complex_modifier = 1;  /* Track that we saw _Complex */
    next();
    break;
```

Then modify the `TOK_FLOAT` and `TOK_DOUBLE` cases to check this flag.

### Step 4: Add Type Helpers

Edit `tcctype.h`:

```c
static inline int tcc_is_complex_type(int t)
{
    int bt = t & VT_BTYPE;
    return (bt == VT_CFLOAT || bt == VT_CDOUBLE);
}
```

### Step 5: Test

Create minimal test:

```c
/* test_complex.c */
#include <stdio.h>

int main(void)
{
    _Complex float cf;
    _Complex double cd;
    
    printf("sizeof(cf) = %d\n", (int)sizeof(cf));
    printf("sizeof(cd) = %d\n", (int)sizeof(cd));
    return 0;
}
```

Compile:
```bash
./armv8m-tcc -c test_complex.c -o test_complex.o
arm-none-eabi-objdump -h test_complex.o
```

**Success:** No compilation error, object file created.

## Debugging Tips

### Enable Parser Debug

```bash
make clean
make CFLAGS+='-DPARSE_DEBUG' cross 2>&1 | head -100
```

### View IR Output

```bash
./armv8m-tcc -dump-ir -c test_complex.c
```

### Compare with GCC

```bash
# See what GCC generates
arm-none-eabi-gcc -O1 -S -mcpu=cortex-m33 test_complex.c -o test_complex.s
cat test_complex.s
```

### Use GDB

```bash
# Compile with debug info
./armv8m-tcc -g -c test_complex.c -o test_complex.o

# Debug the compiler itself
gdb ./armv8m-tcc
(gdb) break parse_btype
(gdb) run -c test_complex.c
```

## Common Issues

### Issue: "_Complex is not yet supported" still appears

**Cause:** Parser not reaching your new code or token not recognized.

**Debug:**
```c
case TOK_COMPLEX:
    fprintf(stderr, "DEBUG: Found TOK_COMPLEX\n");  /* Add this */
    complex_modifier = 1;
    next();
    break;
```

### Issue: Wrong sizeof results

**Cause:** Type size function not updated.

**Fix:** Update `tcc_get_basic_type_size()` in `tcctype.h`:

```c
case VT_CFLOAT:
    return 8;
case VT_CDOUBLE:
    return 16;
```

### Issue: IR shows wrong types

**Cause:** IROperand encoding not handling complex.

**Fix:** Add to `tccir_operand.c` functions that map VT_ to IROP_BTYPE_.

## Testing Your Changes

### Create Test File

```bash
cd tests/ir_tests
cat > 50_complex_types.c << 'EOF'
#include <stdio.h>

int main(void)
{
    _Complex float cf;
    _Complex double cd;
    
    if (sizeof(cf) != 8) {
        printf("FAIL: sizeof(float _Complex) = %d, expected 8\n", (int)sizeof(cf));
        return 1;
    }
    if (sizeof(cd) != 16) {
        printf("FAIL: sizeof(double _Complex) = %d, expected 16\n", (int)sizeof(cd));
        return 1;
    }
    printf("OK\n");
    return 0;
}
EOF

echo "OK" > 50_complex_types.expect
```

### Run Test

```bash
python run.py -c 50_complex_types.c
```

**Expected:** Test compiles and outputs "OK".

## Next Steps

After Phase 1 works:

1. Move to Phase 2: IR support (straightforward type encoding)
2. Phase 3: Code generation (most work, start with load/store)
3. Phase 4-8: Incrementally add features

See `README.md` for full phase descriptions and `IMPLEMENTATION_CHECKLIST.md` for detailed tasks.

## Resources

- C99 Standard: Section 6.2.5 (Types), 7.3 (Complex arithmetic)
- ARM AAPCS: Procedure Call Standard for ARM Architecture
- GCC Complex Docs: https://gcc.gnu.org/onlinedocs/gcc/Complex.html

## Getting Help

If stuck:
1. Check existing type implementations (VT_FLOAT, VT_DOUBLE) for patterns
2. Compare with GCC output
3. Add debug prints to understand flow
4. Check IR dump to see where things go wrong
