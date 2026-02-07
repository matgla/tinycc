# Array Sum Optimization Plan

## Executive Summary

TCC -O1 is **2x slower** than GCC -O1 on the `array_sum` benchmark. This document analyzes the root causes and proposes specific optimizations to close the gap.

## Benchmark Code

```c
int bench_array_sum(int iterations)
{
  int arr[256];
  int sum = 0;

  for (int i = 0; i < 256; i++) {
    arr[i] = i * 7 + 13;
  }

  for (int n = 0; n < iterations; n++) {
    sum = 0;
    for (int i = 0; i < 256; i++) {
      sum += arr[i];
    }
  }

  return sum;
}
```

---

## Analysis: Inner Loop Comparison

### GCC -O1 Inner Loop (4 instructions)

```asm
2e:   ldr.w   r2, [r3, #4]!    ; load arr[i] AND increment pointer
32:   add     r0, r2           ; sum += arr[i]
34:   cmp     r3, r1           ; compare pointer to end
36:   bne.n   2e               ; loop back
```

**Characteristics:**
- Pre-indexed addressing: `[r3, #4]!` loads AND increments in one instruction
- Pointer-based iteration: no index calculation
- Condition at bottom: one branch per iteration
- 3 registers used: r0 (sum), r2 (temp), r3 (pointer)

### TCC -O1 Inner Loop (9 instructions)

```asm
48:   cmp.w   r4, #256         ; compare i with 256
4c:   bge.n   40               ; exit if >= 256
4e:   b.n     54               ; jump to body (EXTRA!)
50:   adds    r4, #1           ; i++
52:   b.n     48               ; back to compare (EXTRA!)
54:   mov.w   r5, r4, lsl #2   ; r5 = i * 4
58:   adds    r6, r3, r5       ; addr = arr + i*4
5a:   ldr.w   ip, [r6]         ; load arr[i]
5e:   add     r1, ip           ; sum += arr[i]
60:   b.n     50               ; back to increment
```

**Problems:**
1. Index calculation every iteration (`i * 4`)
2. Three branches per iteration instead of one
3. Condition at top with split structure
4. 6 registers used

### Instruction Count per Iteration

| Compiler | Instructions | Branches | Est. Cycles |
|----------|--------------|----------|-------------|
| GCC -O1  | 4            | 1        | ~4          |
| TCC -O1  | 9            | 3        | ~9-10       |

---

## Root Cause Analysis

### Problem 1: No Induction Variable Optimization (Strength Reduction)

**Current TCC IR (inner loop):**
```
0029: R5(T10) <-- R4(V3) SHL #2          ; i * 4 EVERY iteration
0030: R6(T11) <-- R3(T15) ADD R5(T10)    ; arr + offset
0031: R1(V0) <-- R1(V0) ADD R6(T11)***DEREF***
```

**What GCC does:**
Instead of computing `arr + i*4` each iteration, GCC:
1. Initializes a pointer `ptr = arr`
2. Increments the pointer: `ptr += 4` (or uses `[ptr, #4]!`)
3. Compares pointer to end address

This is **induction variable strength reduction** - replacing `base + i*stride` with `ptr += stride`.

### Problem 2: Suboptimal Loop Structure

**TCC generates:**
```
LOOP_HEADER:
  CMP i, 256
  BGE EXIT
  B BODY           ; extra branch!
INCREMENT:
  i++
  B LOOP_HEADER    ; extra branch!
BODY:
  ... loop body ...
  B INCREMENT      ; third branch!
EXIT:
```

**GCC generates (loop inversion):**
```
  ; preheader: check if iterations > 0
LOOP_BODY:
  ... loop body ...
  ptr++
  CMP ptr, end
  BNE LOOP_BODY    ; single branch!
```

### Problem 3: No Pre/Post-Indexed Addressing

ARM Thumb-2 supports powerful addressing modes:
- `LDR r0, [r1, #4]!` - Pre-indexed: r1 += 4, then load from r1
- `LDR r0, [r1], #4` - Post-indexed: load from r1, then r1 += 4

TCC currently only generates:
- `LDR r0, [r1]` - Simple load (no auto-increment)

---

## Proposed Optimizations

### Phase 1: Induction Variable Strength Reduction (HIGH IMPACT)

**Goal:** Replace `base + i*stride` pattern with pointer increment.

**Implementation Location:** `ir/opt.c` or new file `ir/iv.c`

**Algorithm:**
1. Detect loop induction variables (variables incremented by constant each iteration)
2. Find uses of pattern: `base + IV * constant`
3. Create a new pointer variable: `ptr = base` in preheader
4. Replace `base + IV * stride` with `ptr`
5. Add `ptr += stride` at end of loop body

**IR Transformation:**
```
; BEFORE
0024: R4(V3) <-- #0 [ASSIGN]              ; i = 0
...
0029: R5(T10) <-- R4(V3) SHL #2           ; i * 4
0030: R6(T11) <-- R3(T15) ADD R5(T10)     ; arr + i*4
0031: R1(V0) <-- R1(V0) ADD R6(T11)***DEREF***
...
0029: R4(V3) <-- R4(V3) ADD #1            ; i++

; AFTER
0024: R4(V3) <-- #0 [ASSIGN]              ; i = 0
0024b: R6(ptr) <-- R3(T15) [ASSIGN]       ; ptr = arr (in preheader)
...
0031: R1(V0) <-- R1(V0) ADD R6(ptr)***DEREF***
0031b: R6(ptr) <-- R6(ptr) ADD #4         ; ptr += 4
...
0029: R4(V3) <-- R4(V3) ADD #1            ; i++ (can be DCE'd if only used for address calc)
```

**Expected Impact:** ~30% improvement (eliminates 2 instructions per iteration)

**Files to Modify:**
- `ir/opt.c` - Add `tcc_ir_opt_iv_strength_reduction()`
- `ir/opt.h` - Declare new function
- `ir/licm.c` - Reuse loop detection infrastructure

---

### Phase 2: Loop Structure Optimization (MEDIUM IMPACT)

**Goal:** Generate tighter loop structure with condition at bottom.

**Implementation Location:** `tccgen.c` (C parser) or `ir/opt.c` (IR level)

#### Option A: Fix at C Parser Level (tccgen.c)

Change how `for` loops are lowered to IR:

```
; Current structure (condition at top)
HEADER:
  CMP condition
  BGE EXIT
  B BODY
LATCH:
  increment
  B HEADER
BODY:
  ...
  B LATCH

; Target structure (condition at bottom)
PREHEADER:
  CMP condition  ; check if should enter loop at all
  BGE EXIT
BODY:
  ...
  increment
  CMP condition
  BNE BODY
EXIT:
```

#### Option B: IR-Level Loop Rotation

Transform the loop structure in an optimization pass after IR generation.

**Expected Impact:** ~20% improvement (eliminates 2 branches per iteration)

**Files to Modify:**
- `tccgen.c` - Modify `for_loop()` and `while_loop()` generation
- OR `ir/opt.c` - Add loop rotation pass

---

### Phase 3: Pre/Post-Indexed Addressing (MEDIUM IMPACT)

**Goal:** Use ARM's pre/post-indexed addressing modes.

**Implementation Location:** `arm-thumb-gen.c`

**Pattern Recognition in Code Generator:**
```
; Detect this pattern:
  LDR Rx, [Rbase]
  ADD Rbase, #stride   ; or any instruction that adds constant to base

; Replace with:
  LDR Rx, [Rbase], #stride   ; post-indexed
```

**Alternatively**, if pointer increment comes before load:
```
; Detect:
  ADD Rbase, #stride
  LDR Rx, [Rbase]

; Replace with:
  LDR Rx, [Rbase, #stride]!   ; pre-indexed
```

**Expected Impact:** ~10% improvement (eliminates 1 instruction per iteration)

**Files to Modify:**
- `arm-thumb-gen.c` - Pattern matching in code generator
- `arm-thumb-opcodes.c` - Ensure pre/post-indexed opcodes are available

---

### Phase 4: Dead Code Elimination for Induction Variables (LOW IMPACT)

After strength reduction, the original induction variable `i` may only be used for loop termination. If we switch to pointer comparison:

```c
// Before: compare i < 256
// After: compare ptr < end_ptr
```

Then `i` can be completely eliminated.

**Expected Impact:** ~5% improvement

---

## Implementation Priority

| Phase | Optimization | Impact | Complexity | Priority |
|-------|-------------|--------|------------|----------|
| 1 | IV Strength Reduction | HIGH (~30%) | Medium | **P0** |
| 2 | Loop Structure | MEDIUM (~20%) | Medium | **P1** |
| 3 | Pre/Post-Indexed Addressing | MEDIUM (~10%) | Low | **P1** |
| 4 | IV Dead Code Elimination | LOW (~5%) | Low | **P2** |

---

## Detailed Implementation: Phase 1 (IV Strength Reduction)

### Data Structures

```c
/* Induction variable descriptor */
typedef struct {
  int vreg;           /* Virtual register holding IV */
  int init_val;       /* Initial value (usually 0) */
  int step;           /* Increment per iteration */
  int def_idx;        /* Instruction index where IV is incremented */
} InductionVar;

/* Derived induction variable (e.g., arr + i*4) */
typedef struct {
  int base_vreg;      /* Base address register */
  int iv_idx;         /* Index into InductionVar array */
  int multiplier;     /* Multiplier (e.g., 4 for int[]) */
  int use_idx;        /* Instruction index where this is used */
} DerivedIV;
```

### Algorithm Pseudocode

```
function iv_strength_reduction(loop):
    # Step 1: Find basic induction variables
    ivs = []
    for instr in loop.body:
        if instr matches "Vx = Vx + constant":
            ivs.append(InductionVar(vreg=Vx, step=constant))
    
    # Step 2: Find derived induction variables
    derived = []
    for instr in loop.body:
        if instr matches "Ty = base + Vx * constant":
            if Vx is in ivs:
                derived.append(DerivedIV(base, iv_idx, constant, instr.idx))
    
    # Step 3: Create strength-reduced versions
    for div in derived:
        iv = ivs[div.iv_idx]
        stride = iv.step * div.multiplier
        
        # Insert in preheader: ptr = base + iv.init_val * div.multiplier
        insert_before(loop.header, "Vptr = base")
        
        # Replace use: Ty = Vptr (instead of base + Vx * constant)
        replace(div.use_idx, "Ty = Vptr")
        
        # Insert after IV increment: Vptr += stride
        insert_after(iv.def_idx, "Vptr = Vptr + stride")
```

### Test Cases

```c
// Test 1: Simple array access
for (int i = 0; i < n; i++) {
    sum += arr[i];  // arr + i*4 -> ptr++
}

// Test 2: Strided access
for (int i = 0; i < n; i += 2) {
    sum += arr[i];  // arr + i*4 -> ptr += 8
}

// Test 3: Multiple derived IVs
for (int i = 0; i < n; i++) {
    sum += arr1[i] + arr2[i];  // Two pointers
}

// Test 4: Nested loops
for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
        sum += matrix[i][j];
    }
}
```

---

## Expected Results

After implementing Phase 1 and Phase 2:

**Target TCC Inner Loop:**
```asm
LOOP:
  ldr.w   r2, [r3]         ; load *ptr
  add     r0, r2           ; sum += *ptr
  adds    r3, #4           ; ptr += 4
  cmp     r3, r1           ; compare ptr to end
  bne.n   LOOP             ; 5 instructions
```

With Phase 3 (pre/post-indexed):
```asm
LOOP:
  ldr.w   r2, [r3], #4     ; load *ptr++
  add     r0, r2           ; sum += value
  cmp     r3, r1           ; compare ptr to end
  bne.n   LOOP             ; 4 instructions - matches GCC!
```

---

## Validation Plan

1. **Correctness:** Run full test suite (`make test`)
2. **Performance:** Re-run `run_benchmark.py` for array_sum
3. **Code size:** Compare binary sizes before/after
4. **Regression:** Ensure no slowdown in other benchmarks

---

## References

- [Loop Optimization](https://en.wikipedia.org/wiki/Loop_optimization)
- [Strength Reduction](https://en.wikipedia.org/wiki/Strength_reduction)
- [ARM Thumb-2 Instruction Set](https://developer.arm.com/documentation/ddi0406/c/Application-Level-Architecture/Instruction-Details/Alphabetical-list-of-instructions)
- GCC source: `gcc/tree-scalar-evolution.c`, `gcc/tree-ssa-loop-ivopts.c`
