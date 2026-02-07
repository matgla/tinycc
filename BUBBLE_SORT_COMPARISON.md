# Bubble Sort: GCC vs TCC Codegen Comparison

## Code Size Summary

| Compiler | Size (bytes) | Instructions |
|----------|-------------|--------------|
| GCC -O1  | 76 bytes    | ~25 instructions |
| TCC -O1  | 108 bytes   | ~54 instructions |
| **Ratio** | **1.42x** | **2.16x** |

## GCC -O1 Disassembly (76 bytes)

```asm
bubble_sort:
   0:   add.w   ip, r1, #-1          ; ip = n - 1 (outer loop limit)
   4:   cmp.w   ip, #0               ; early exit if n <= 1
   8:   ble.n   48
   a:   push    {lr}
   c:   mov     lr, ip               ; lr = outer loop counter
   e:   add.w   ip, r0, ip, lsl #2   ; ip = &arr[n-1] (end pointer)
  12:   cmp.w   lr, #0
  16:   itt     gt
  18:   movgt   r3, r0               ; r3 = arr (current pointer)
  1a:   ble.n   3e

  ; INNER LOOP - only 8 instructions!
  1c:   ldr     r2, [r3, #0]         ; r2 = arr[j]
  1e:   ldr.w   r1, [r3, #4]!        ; r1 = arr[j+1], r3++ (POST-INCREMENT!)
  22:   cmp     r2, r1               ; compare
  24:   itt     gt                   ; IT block for conditional swap
  26:   strgt.w r1, [r3, #-4]        ; conditional store arr[j]
  2a:   strgt   r2, [r3, #0]         ; conditional store arr[j+1]
  2c:   cmp     r3, ip               ; loop until end
  2e:   bne.n   1c

  30:   sub.w   ip, ip, #4           ; shrink end pointer
  34:   subs.w  lr, lr, #1           ; decrement outer counter
  38:   bne.n   12
  3a:   ldr.w   pc, [sp], #4         ; return
  3e:   ...                          ; edge case handling
  48:   bx      lr
```

## TCC -O1 Disassembly (108 bytes)

```asm
bubble_sort:
   0:   stmdb   sp!, {r4, r5, r6, r8, r9, sl, ip, lr}  ; MANY registers saved
   4:   movs    r2, #0               ; i = 0
   6:   subs    r3, r1, #1           ; n - 1
   8:   cmp     r2, r3               ; outer loop check
   a:   bge.n   68
   c:   b.n     12
   e:   adds    r2, #1               ; i++
  10:   b.n     6                    ; RECOMPUTE n-1 each iteration!

  12:   movs    r4, #0               ; j = 0
  14:   subs    r5, r1, #1           ; n - 1 AGAIN!
  16:   subs    r6, r5, r2           ; n - 1 - i
  18:   cmp     r4, r6               ; inner loop check
  1a:   bge.n   e
  1c:   b.n     22
  1e:   adds    r4, #1               ; j++
  20:   b.n     14                   ; RECOMPUTE n-1-i each iteration!

  ; INNER LOOP BODY - bloated address computation
  22:   mov.w   r6, r4, lsl #2       ; j * 4
  26:   add.w   r8, r0, r6           ; &arr[j]
  2a:   adds    r6, r4, #1           ; j + 1
  2c:   mov.w   r9, r6, lsl #2       ; (j+1) * 4
  30:   add.w   r6, r0, r9           ; &arr[j+1]
  34:   ldr.w   ip, [r8]             ; arr[j]
  38:   ldr.w   lr, [r6]             ; arr[j+1]
  3c:   cmp     ip, lr
  3e:   ble.n   1e                   ; branch if no swap

  ; SWAP - recomputes addresses AGAIN!
  40:   mov.w   r6, r4, lsl #2       ; REDUNDANT: j * 4
  44:   add.w   r8, r0, r6           ; REDUNDANT: &arr[j]
  48:   ldr.w   r9, [r8]             ; temp = arr[j]
  4c:   mov     r8, r6
  4e:   add.w   r6, r0, r8
  52:   add.w   r8, r4, #1
  56:   ldr.w   sl, [r0, r8, lsl #2] ; arr[j+1]
  5a:   str.w   sl, [r6]             ; arr[j] = arr[j+1]
  5e:   mov     r6, r8
  60:   mov     r8, r9
  62:   str.w   r8, [r0, r6, lsl #2] ; arr[j+1] = temp
  66:   b.n     1e
  68:   ldmia.w sp!, {r4, r5, r6, r8, r9, sl, ip, pc}
```

## Key Missing Optimizations in TCC

### 1. **Loop-Invariant Code Motion (LICM)** - HIGH IMPACT

**Problem**: TCC recomputes `n - 1` on EVERY iteration of both loops.

```c
// TCC computes this every time:
for (int i = 0; i < n - 1; i++)       // n-1 computed each outer iteration
    for (int j = 0; j < n - 1 - i; j++) // n-1-i computed each inner iteration
```

**GCC**: Hoists `n-1` computation to loop preheader, uses pointer-based end detection.

**Impact**: ~6 extra instructions per outer loop iteration.

---

### 2. **Pointer-Based Loop Iteration** - HIGH IMPACT

**Problem**: TCC uses index `j` and recomputes `&arr[j]` every iteration.

**GCC**: Uses pointer increment with post-increment addressing:
```asm
ldr.w   r1, [r3, #4]!    ; Load and increment pointer in ONE instruction
```

**TCC**: Computes address from scratch each time:
```asm
mov.w   r6, r4, lsl #2   ; j * 4
add.w   r8, r0, r6       ; arr + j*4
```

**Impact**: ~4 extra instructions per inner loop iteration.

---

### 3. **Common Subexpression Elimination (CSE) in Swap Block** - MEDIUM IMPACT

**Problem**: TCC computes `&arr[j]` and `&arr[j+1]` twice - once for comparison, again for swap.

```
; Compare block: computes &arr[j], &arr[j+1]
22:   mov.w   r6, r4, lsl #2       ; j * 4
26:   add.w   r8, r0, r6           ; &arr[j]
...
; Swap block: RECOMPUTES the same addresses!
40:   mov.w   r6, r4, lsl #2       ; j * 4 AGAIN
44:   add.w   r8, r0, r6           ; &arr[j] AGAIN
```

**GCC**: Reuses the loaded values `r2` and `r1`, stores back to same locations.

**Impact**: ~4 redundant instructions in swap path.

---

### 4. **IT Block Conditional Execution** - MEDIUM IMPACT

**Problem**: TCC uses branch for conditional swap.

**GCC**: Uses IT (If-Then) block for predicated execution:
```asm
itt     gt
strgt.w r1, [r3, #-4]    ; No branch, predicated store
strgt   r2, [r3, #0]
```

**TCC**: Branches over the swap code:
```asm
ble.n   1e               ; Branch if no swap
...                       ; Full swap code
b.n     1e               ; Branch back
```

**Impact**: Extra branch overhead, worse pipeline utilization.

---

### 5. **Post-Increment Addressing Modes** - MEDIUM IMPACT

**Problem**: TCC doesn't use `LDR Rd, [Rn, #offset]!` (pre-increment) or `LDR Rd, [Rn], #offset` (post-increment).

**GCC** uses:
```asm
ldr.w   r1, [r3, #4]!    ; Load and increment in one instruction
```

**TCC** requires separate operations.

**Impact**: 1-2 instructions per iteration.

---

### 6. **Register Pressure / Spilling** - LOW IMPACT

**TCC** saves 8 registers: `{r4, r5, r6, r8, r9, sl, ip, lr}`
**GCC** saves 1 register: `{lr}`

TCC uses many more registers due to not reusing values and poor CSE.

---

## IR Analysis

TCC IR shows the redundant computations clearly:

```
; First address computation (for compare)
0017: T7 <-- V1 SHL #2           ; j * 4
0018: T8 <-- P0 ADD T7           ; &arr[j]

; REDUNDANT - same computation again (for swap)
0024: T12 <-- V1 SHL #2          ; j * 4 AGAIN!
0025: T13 <-- P0 ADD T12         ; &arr[j] AGAIN!
```

The optimizer doesn't recognize that T7==T12 and T8==T13.

---

## Summary: Optimization Priority

| Optimization | Impact | Complexity | Instructions Saved |
|--------------|--------|------------|-------------------|
| **Pointer-based iteration** | High | High | ~8-10/iter |
| **LICM for loop bounds** | High | Medium | ~6/outer iter |
| **CSE across basic blocks** | Medium | Medium | ~4 in swap |
| **IT block generation** | Medium | Medium | ~2-3/swap |
| **Post-increment addressing** | Medium | Low | ~1-2/iter |

**Total potential savings**: ~30-40 instructions → could bring TCC to ~70-80 bytes, matching GCC.

---

## Implementation Plan

### Phase 1: Loop Bound Hoisting (LICM Enhancement) - HIGH PRIORITY (DONE)

**Current State**: LICM exists in `ir/licm.c` but only hoists:
- Stack address computations (`Addr[StackLoc[...]]`)
- Pure function calls

**Problem**: TCC recomputes `n-1` on every outer iteration and `n-1-i` on every inner iteration.

**Solution**: Extend LICM to hoist simple arithmetic expressions involving loop-invariant operands.

```
; Before (current TCC):
0001: T0 <-- P1 SUB #1       ; computed every outer iteration
0009: T3 <-- P1 SUB #1       ; computed every inner iteration
0010: T4 <-- T3 SUB V0       ; n-1-i computed every inner iteration

; After (with LICM):
; In preheader:
PRE1: T_limit <-- P1 SUB #1  ; hoisted, computed once

; In outer loop preheader:
PRE2: T_inner_limit <-- T_limit SUB V0  ; computed once per outer iteration
```

**Files to modify**:
- [ir/licm.c](ir/licm.c): Add `hoist_arith_exprs_from_loop()` function
- Detect SUB/ADD with constant RHS where LHS is loop-invariant (parameter or hoisted vreg)

**Complexity**: Medium (extend existing infrastructure)

---

### Phase 2: Global CSE Across Basic Blocks - HIGH PRIORITY

**Current State**: CSE in `ir/opt.c` only works within basic blocks (cleared on jumps).

**Problem**: Address computation for `arr[j]` happens twice:
```
; Compare block:
0017: T7 <-- V1 SHL #2       ; j * 4
0018: T8 <-- P0 ADD T7       ; &arr[j]

; Swap block (REDUNDANT):
0024: T12 <-- V1 SHL #2      ; j * 4 AGAIN
0025: T13 <-- P0 ADD T12     ; &arr[j] AGAIN
```

**Solution**: Implement dominator-based CSE or extend available expression analysis.

**Approach A - Quick Win**: Peephole at codegen level
- In `arm-thumb-gen.c`, detect when same computation appears in consecutive basic blocks
- Reuse register if still valid

**Approach B - Proper Solution**: Dominator-based CSE
- Build dominator tree
- For each instruction, check if equivalent computation available from dominator
- Replace with ASSIGN from existing vreg

**Files to modify**:
- [ir/opt.c](ir/opt.c): Extend `tcc_ir_opt_cse_arith()` with dominator-aware version
- New function: `tcc_ir_opt_cse_global()`

**Complexity**: High (requires dominator analysis)

---

### Phase 3: IT Block Generation for Conditional Stores - MEDIUM PRIORITY

**Current State**: TCC uses branches for conditional execution.

**GCC generates**:
```asm
itt     gt
strgt.w r1, [r3, #-4]
strgt   r2, [r3, #0]
```

**TCC generates**:
```asm
ble.n   1e           ; branch over swap
... swap code ...
b.n     1e           ; branch back
```

**Solution**: Detect simple if-then patterns with 1-4 instructions that can use IT blocks.

**Pattern to detect**:
```
CMP a, b
JUMPIF label if <=
STORE ...     ; 1-4 instructions
STORE ...
JMP label
label:
```

**Implementation**:
1. In IR optimizer: Mark sequences suitable for IT block
2. In codegen: Generate IT instruction instead of branch

**Files to modify**:
- [ir/opt.c](ir/opt.c): Add `tcc_ir_mark_it_candidates()`
- [arm-thumb-gen.c](arm-thumb-gen.c): Detect marked sequences, emit IT blocks
- Already has IT generation at line 2913 for boolean operations

**Complexity**: Medium

---

### Phase 4: Strength Reduction (Index to Pointer) - MEDIUM PRIORITY

**Current State**: TCC uses index-based iteration with computed addresses.

**GCC uses**: Pointer-based iteration with post-increment.

```asm
; GCC: pointer iteration
ldr.w   r1, [r3, #4]!    ; load AND increment in one instruction

; TCC: index iteration
mov.w   r6, r4, lsl #2   ; j * 4
add.w   r8, r0, r6       ; arr + j*4
ldr.w   ip, [r8]         ; load
```

**Solution**: Strength reduction - convert `arr[i]` in loops to `*p++` pattern.

**Detection**:
- Loop with index variable `i` incremented by 1
- Array access `arr[i]` or `arr[i+1]` in loop body
- Can convert to pointer `p` initialized to `&arr[0]`, incremented by 4

**Files to modify**:
- [ir/opt.c](ir/opt.c): Add `tcc_ir_opt_strength_reduction()`
- Detect induction variables
- Replace index-based access with pointer increment

**Complexity**: High (requires induction variable analysis)

---

### Phase 5: Post-Increment Addressing Mode - LOW PRIORITY

**Current State**: TCC doesn't use `LDR Rd, [Rn, #offset]!` or `LDR Rd, [Rn], #offset`.

**Solution**: In codegen, detect sequential loads/stores that could use post-increment.

**Pattern**:
```
; Before:
LDR r2, [r3]
ADD r3, r3, #4

; After:
LDR r2, [r3], #4   ; post-increment
```

**Files to modify**:
- [arm-thumb-gen.c](arm-thumb-gen.c): Peephole optimizer to combine LDR+ADD

**Complexity**: Low (peephole at codegen level)

---

## Recommended Implementation Order

| Order | Phase | Expected Impact | LOC Estimate |
|-------|-------|-----------------|--------------|
| 1 | Phase 2: Global CSE | High - eliminates redundant addr calc | ~200 |
| 2 | Phase 1: Loop Bound LICM | High - hoists n-1 computation | ~150 |
| 3 | Phase 3: IT Blocks | Medium - removes branches | ~250 |
| 4 | Phase 5: Post-increment | Low - peephole only | ~100 |
| 5 | Phase 4: Strength Reduction | Medium - complex analysis | ~400 |

**Quick Win Strategy**: Phases 1 and 2 together should reduce bubble_sort from 108 to ~80-85 bytes.

---

## Test Plan

1. Run existing benchmark suite after each phase:
   ```bash
   make test -j16
   cd tests/benchmarks && ./run_benchmarks.sh
   ```

2. Verify bubble_sort specifically:
   ```bash
   ./armv8m-tcc -O1 -c /tmp/bubble_sort_compare.c -o /tmp/test.o
   arm-none-eabi-objdump -d /tmp/test.o | wc -c
   ```

3. Check for regressions in other benchmarks (copy_sum, dot_product, etc.)
