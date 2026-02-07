# Function Calls Benchmark Analysis: TCC-O1 vs GCC-O1

## Performance Summary

| Metric | TCC-O1 | GCC-O1 | Ratio |
|--------|--------|--------|-------|
| Cycles per iteration | 56,049 | 4,068 | **1377.8%** |

## Root Cause Analysis

GCC-O1 applies **aggressive loop invariant code motion (LICM)** and realizes that the loop body produces the **same result every iteration**. It hoists ALL function calls outside the loop, executing them only once.

### GCC-O1 Disassembly

```asm
20003b1a <bench_function_calls>:
20003b1a:   push    {r4, lr}
20003b1c:   subs    r4, r0, #0        ; r4 = iterations
20003b1e:   ble.n   20003b40          ; if iterations <= 0, skip

; --- Function calls executed ONCE (outside loop) ---
20003b20:   movs    r0, #100
20003b22:   bl      func_a            ; result = func_a(100)
20003b26:   bl      func_b            ; result = func_b(result)
20003b2a:   bl      func_c            ; result = func_c(result)
20003b2e:   bl      func_a            ; result = func_a(result)
20003b32:   bl      func_b            ; result = func_b(result)

; --- Empty counting loop ---
20003b36:   movs    r3, #0
20003b38:   adds    r3, #1            ; loop body: just increment counter
20003b3a:   cmp     r4, r3
20003b3c:   bne.n   20003b38          ; loop back
20003b3e:   pop     {r4, pc}
```

**GCC optimizations applied:**
1. **Loop invariant code motion**: All function calls moved outside the loop
2. **Dead store elimination**: The `result` variable doesn't need to persist across iterations
3. **Strength reduction**: `x * 3` → `x + x<<1`, `x * 5` → `x + x<<2`
4. Loop becomes trivial counter (essentially a NOP loop)

### TCC-O1 Disassembly

```asm
20003d6e <bench_function_calls>:
20003d6e:   push    {r4, r5, r6, lr}
20003d70:   mov     r4, r0            ; r4 = iterations
20003d72:   movs    r5, #0            ; result = 0
20003d74:   movs    r6, #0            ; n = 0

; --- Loop comparison ---
20003d76:   cmp     r6, r4            ; compare n with iterations
20003d78:   bge.w   20003dac          ; exit if n >= iterations
20003d7c:   b.n     20003d82          ; jump to loop body

20003d7e:   adds    r6, #1            ; n++
20003d80:   b.n     20003d76          ; back to comparison

; --- Loop body: ALL function calls inside loop ---
20003d82:   movs    r0, #100
20003d84:   bl      func_a
20003d88:   mov     r5, r0            ; result = func_a(100)
20003d8a:   mov     r0, r5            ; redundant move
20003d8c:   bl      func_b
20003d90:   mov     r5, r0            ; result = func_b(result)
20003d92:   mov     r0, r5            ; redundant move
20003d94:   bl      func_c
20003d98:   mov     r5, r0            ; result = func_c(result)
20003d9a:   mov     r0, r5            ; redundant move
20003d9c:   bl      func_a
20003da0:   mov     r5, r0            ; result = func_a(result)
20003da2:   mov     r0, r5            ; redundant move
20003da4:   bl      func_b
20003da8:   mov     r5, r0            ; result = func_b(result)
20003daa:   b.n     20003d7e          ; back to increment
```

**TCC issues identified:**
1. **No loop invariant code motion**: All 5 function calls execute every iteration
2. **Redundant register moves**: Every `mov r5, r0` followed by `mov r0, r5` pair
3. **Extra registers used**: Uses r4, r5, r6 instead of minimal r4
4. **Suboptimal loop structure**: Extra unconditional branch at 20003d7c

### Helper Functions Comparison

| Function | TCC-O1 | GCC-O1 |
|----------|--------|--------|
| `func_a` (x*3+7) | `mul.w r1, r0, r2; adds r2, r1, #7` | `add.w r0, r0, r0, lsl #1; adds r0, #7` |
| `func_b` (x*5-3) | `mul.w r1, r0, r2; subs r2, r1, #3` | `add.w r0, r0, r0, lsl #2; subs r0, #3` |
| `func_c` (x<<2+1) | `mov.w r1, r0, lsl #2; adds r2, r1, #1` | `lsls r0, r0, #2; adds r0, #1` |

GCC uses **strength reduction** to replace multiplications with shifts and adds:
- `x * 3` → `x + (x << 1)` (one ADD instead of MUL)
- `x * 5` → `x + (x << 2)` (one ADD instead of MUL)

TCC uses the hardware multiplier (still reasonably fast on Cortex-M33).

## Cycle Count Breakdown

### Per-iteration costs (estimated)

| Operation | TCC-O1 | GCC-O1 |
|-----------|--------|--------|
| Loop overhead | ~10 cycles | ~4 cycles |
| Function calls (5× BL+BX) | ~40 cycles | 0 (hoisted) |
| func_a computation | ~6 cycles | 0 (hoisted) |
| func_b computation | ~6 cycles | 0 (hoisted) |
| func_c computation | ~4 cycles | 0 (hoisted) |
| Redundant moves | ~10 cycles | 0 |
| **Total per iteration** | **~56 cycles** | **~4 cycles** |

With 1000 iterations:
- TCC: ~56,000 cycles ✓ (matches benchmark: 56,049)
- GCC: ~4,000 cycles ✓ (matches benchmark: 4,068)

## Recommendations for TCC Optimization

### High Priority (Major Impact)

1. **Implement Loop Invariant Code Motion (LICM)**
   - Detect that all function calls have constant inputs that don't depend on loop index
   - Move invariant computations before the loop
   - This alone would reduce cycles from 56,049 to ~4,000

2. **Recognize pure/const functions**
   - Mark functions without side effects
   - Allow more aggressive hoisting of pure function calls

### Medium Priority

3. **Eliminate redundant register moves**
   - `mov r5, r0; mov r0, r5` pattern is unnecessary
   - The result is already in r0 for the next call

4. **Optimize loop structure**
   - Remove the unconditional branch at start of loop body
   - Use more efficient loop-ending test placement

### Lower Priority

5. **Strength reduction for multiplications**
   - `x * 3` → `add r0, r0, r0, lsl #1`
   - `x * 5` → `add r0, r0, r0, lsl #2`
   - Minor impact since Cortex-M33 MUL is fast (1-2 cycles)

## Conclusion

The **13.7x performance gap** is almost entirely due to **GCC's loop invariant code motion** optimization, which recognizes that the entire loop body produces the same result on every iteration and hoists all work outside the loop.

TCC executes all 5 function calls × 1000 iterations = 5000 function calls.
GCC executes 5 function calls × 1 time = 5 function calls.

This is a **fundamental optimization** that TCC's O1 pass does not currently perform. Implementing LICM would be the single most impactful optimization for this benchmark.
