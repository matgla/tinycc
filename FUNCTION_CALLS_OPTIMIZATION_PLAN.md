# Function Calls Benchmark Optimization Plan

## Problem Statement

The `function_calls` benchmark shows TCC -O1 is **~13.8x slower** than GCC -O1:

| Compiler | Cycles/iter | Ratio |
|----------|-------------|-------|
| TCC -O1  | 56,049      | 1377.8% |
| GCC -O1  | 4,068       | baseline |

**Latest benchmark (Feb 2026):**
See [tests/benchmarks/FUNCTION_CALLS_ANALYSIS.md](tests/benchmarks/FUNCTION_CALLS_ANALYSIS.md) for detailed disassembly comparison.

## Benchmark Code

```c
static int NOINLINE func_a(int x) { return x * 3 + 7; }
static int NOINLINE func_b(int x) { return x * 5 - 3; }
static int NOINLINE func_c(int x) { return (x << 2) + 1; }

int bench_function_calls(int iterations)
{
  int result = 0;
  for (int n = 0; n < iterations; n++)
  {
    result = func_a(100);
    result = func_b(result);
    result = func_c(result);
    result = func_a(result);
    result = func_b(result);
  }
  return result;
}
```

## Root Cause Analysis

### Issue 1: No Loop-Invariant Code Motion (LICM) - CRITICAL

**Impact: ~13x slowdown**

GCC recognizes that the entire function call chain produces the same result every iteration (starts with constant `100`, functions are pure), and hoists everything outside the loop:

```asm
# GCC -O1 - calls happen ONCE, then just counts iterations
20003b20:   movs    r0, #100
20003b22:   bl      func_a       ; Called once
20003b26:   bl      func_b       ; Called once
20003b2a:   bl      func_c       ; Called once
20003b2e:   bl      func_a       ; Called once
20003b32:   bl      func_b       ; Called once
20003b36:   movs    r3, #0
20003b38:   adds    r3, #1       ; Empty loop just counts
20003b3a:   cmp     r4, r3
20003b3c:   bne.n   20003b38
```

TCC calls all functions inside the loop on every iteration:

```asm
# TCC -O1 - calls happen on EVERY iteration
20003d82:   movs    r0, #100
20003d84:   bl      func_a
...
20003daa:   b.n     20003d7e     ; Loop back
```

### Issue 2: Redundant Register Moves - MEDIUM

**Impact: ~5 extra instructions per iteration**

TCC generates unnecessary mov instructions between calls:

```asm
# TCC -O1 - unnecessary moves
20003d88:   mov     r5, r0       ; Save result to r5
20003d8a:   mov     r0, r5       ; Immediately copy r5 back to r0 (unnecessary!)
20003d8c:   bl      func_b
```

GCC chains calls directly:

```asm
# GCC -O1 - r0 flows through
20003b22:   bl      func_a       ; Result in r0
20003b26:   bl      func_b       ; Uses r0 directly as arg
```

### Issue 3: Suboptimal Multiply-by-Constant - LOW

**Impact: 2 extra instructions per function**

GCC uses strength reduction for multiply:

```asm
# GCC func_a (x * 3 + 7)
add.w   r0, r0, r0, lsl #1   ; r0 = r0 * 3 using barrel shifter
adds    r0, #7
bx      lr
; 3 instructions
```

TCC uses slow MUL instruction:

```asm
# TCC func_a (x * 3 + 7)
movs    r2, #3
mul.w   r1, r0, r2           ; Slow MUL
adds    r2, r1, #7
mov     r0, r2               ; Extra move
bx      lr
; 5 instructions
```

---

## Optimization Plan

### Phase 1: Loop-Invariant Code Motion (LICM) for Function Calls

**Priority: CRITICAL**
**Expected Improvement: ~10-13x for this benchmark**

#### 1.1 Pure Function Detection

Mark functions as "pure" (no side effects, result depends only on arguments):

```c
// In IR or symbol table
typedef enum {
  FUNC_ATTR_NONE = 0,
  FUNC_ATTR_PURE = (1 << 0),      // No side effects, reads only args
  FUNC_ATTR_CONST = (1 << 1),     // Pure + no memory reads
  FUNC_ATTR_NOINLINE = (1 << 2),
} FuncAttr;
```

**Detection heuristics:**
- Function only uses parameters (no globals, no pointer derefs)
- Function has no calls to impure functions
- Function has no stores to memory
- Conservative: start with explicit `__attribute__((const))` or `__attribute__((pure))`

#### 1.2 Loop-Invariant Expression Detection

In the IR optimization pass, identify expressions whose operands are:
1. Constants
2. Loop-invariant variables (defined outside loop, not modified inside)
3. Results of pure function calls with loop-invariant arguments

#### 1.3 Code Motion

Move loop-invariant instructions to the loop preheader:

```
Before:
  loop_header:
    r1 = CONST 100
    r2 = CALL func_a(r1)    ; Loop-invariant!
    r3 = CALL func_b(r2)    ; Loop-invariant!
    ...
    branch loop_header

After:
  preheader:
    r1 = CONST 100
    r2 = CALL func_a(r1)    ; Hoisted
    r3 = CALL func_b(r2)    ; Hoisted
  loop_header:
    ...                      ; Just loop counter logic
    branch loop_header
```

#### 1.4 Implementation Location

File: `ir/licm.c` (Loop-Invariant Code Motion)

```c
// Pseudocode structure
typedef struct {
  IRBlock *header;
  IRBlock *preheader;      // Insert hoisted code here
  IRBlock **body_blocks;
  int num_body_blocks;
  Set *invariant_instrs;
} LoopInfo;

void licm_optimize(IRFunction *func) {
  // 1. Build loop tree (find natural loops)
  // 2. For each loop (innermost first):
  //    a. Identify loop-invariant instructions
  //    b. Check if safe to hoist (no side effects, dominates all exits)
  //    c. Move to preheader
}

bool is_loop_invariant(IRInstr *instr, LoopInfo *loop) {
  // Instruction is loop-invariant if:
  // - All operands are defined outside the loop, OR
  // - All operands are themselves loop-invariant
  // AND
  // - Instruction has no side effects (or is a pure function call)
}
```

---

### Phase 2: Redundant Move Elimination

**Priority: MEDIUM**
**Expected Improvement: ~10-15% for call-heavy code**

#### 2.1 Copy Propagation

Replace uses of a copy with the original value:

```
Before:
  r5 = MOV r0
  r0 = MOV r5    ; r0 = r5 = r0 (redundant)
  CALL func_b

After:
  CALL func_b    ; r0 already has the value
```

#### 2.2 Implementation

In `ir/opt.c`, add copy propagation pass:

```c
void copy_propagation(IRFunction *func) {
  // For each MOV instruction:
  //   Track that dst = src
  //   Replace subsequent uses of dst with src (if src still valid)
  //   If dst is never used again, delete the MOV
}
```

#### 2.3 Register Allocator Improvement

The register allocator should prefer to keep values in their natural locations:
- Function arguments stay in r0-r3
- Return values stay in r0
- Avoid unnecessary spills to callee-saved registers across calls when not needed

---

### Phase 3: Strength Reduction for Multiply

**Priority: LOW**
**Expected Improvement: ~5% for multiply-heavy code**

#### 3.1 Pattern Matching for Common Multipliers

In code generation, recognize multiply by small constants:

| Multiplier | Replacement |
|------------|-------------|
| 2          | `lsl r, #1` |
| 3          | `add r, r, r, lsl #1` |
| 4          | `lsl r, #2` |
| 5          | `add r, r, r, lsl #2` |
| 6          | `add r, r, r, lsl #1` then `lsl #1` |
| 7          | `rsb r, r, r, lsl #3` |
| 8          | `lsl r, #3` |
| 9          | `add r, r, r, lsl #3` |
| 10         | `add r, r, r, lsl #2` then `lsl #1` |

#### 3.2 Implementation Location

File: `arm-thumb-gen.c`, in the MUL lowering:

```c
void lower_mul_const(IRInstr *instr, int constant) {
  if (is_power_of_2(constant)) {
    emit_lsl(dst, src, log2(constant));
  } else if (is_power_of_2(constant - 1)) {
    // x * (2^n + 1) = x + (x << n)
    emit_add_shifted(dst, src, src, LSL, log2(constant - 1));
  } else if (is_power_of_2(constant + 1)) {
    // x * (2^n - 1) = (x << n) - x
    emit_rsb_shifted(dst, src, src, LSL, log2(constant + 1));
  } else {
    // Fall back to MUL
    emit_mul(dst, src, constant);
  }
}
```

---

## Implementation Order

1. **Phase 1.1-1.2**: Pure function detection and loop-invariant analysis
   - Start with conservative approach (explicit attributes only)
   - Build infrastructure for loop analysis

2. **Phase 1.3-1.4**: LICM implementation
   - Create preheader blocks
   - Hoist invariant instructions

3. **Phase 2**: Copy propagation
   - Quick win, relatively simple to implement

4. **Phase 3**: Strength reduction
   - Lower priority, can be done later

---

## Testing

### Unit Tests

Add to `tests/ir_tests/`:
- `loop_invariant_hoist.c` - Test LICM for various patterns
- `pure_function_calls.c` - Test pure function detection
- `copy_propagation.c` - Test redundant move elimination

### Benchmark Validation

Run `function_calls` benchmark after each phase to measure improvement:

```bash
cd tests/benchmarks
./run_benchmark.py --filter function_calls
```

Target: Achieve < 10,000 cycles/iter (within 2.5x of GCC)

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| LICM moves code that has side effects | Conservative pure function analysis; only hoist what's provably safe |
| Preheader insertion breaks CFG | Careful dominance tree maintenance |
| Copy propagation removes necessary moves | Validate register liveness after optimization |
| Increased compile time | Limit optimization iterations; use efficient data structures |

---

## References

- [ir/licm.c](ir/licm.c) - Existing LICM infrastructure (partial, has bugs)
- [ir/opt.c](ir/opt.c) - Current optimization passes
- [arm-thumb-gen.c](arm-thumb-gen.c) - ARM code generation
- [LICM_IMPLEMENTATION_STATUS.md](LICM_IMPLEMENTATION_STATUS.md) - Known LICM bugs
- [LICM_IMPLEMENTATION_PLAN.md](LICM_IMPLEMENTATION_PLAN.md) - Original LICM plan
- [tests/benchmarks/FUNCTION_CALLS_ANALYSIS.md](tests/benchmarks/FUNCTION_CALLS_ANALYSIS.md) - Detailed disassembly analysis
- GCC's tree-ssa-loop-im.c - Reference LICM implementation

---

## Current LICM Status & Known Bugs

The existing LICM implementation in `ir/licm.c` has these issues:

### Bug 1: Function Call Parameter Tracking
When LICM inserts instructions, it shifts subsequent instructions, breaking `call_id` tracking for function parameters.

**Symptom:**
```
error: compiler_error: missing FUNCPARAMVAL for call_id=0 arg=0
```

### Bug 2: Incomplete Operand Replacement
Hoisted values are created but not always used - the original instruction still computes the address directly.

### Recommended Fix Approach

1. **Skip hoisting for now if function calls exist in loop** (safe fallback)
2. **Fix call_id tracking**: When inserting instructions, update all `call_id` references in subsequent instructions
3. **Fix operand replacement**: Ensure all uses of hoisted value are updated with the new temporary

---

## Quick Win: Idempotent Loop Detection (2-3 days)

A simpler optimization specifically for the benchmark pattern:

### Pattern Detection
```c
for (int n = 0; n < iterations; n++) {
  result = expression_not_using_n;  // Constant result
}
```

### Transform To
```c
if (iterations > 0) {
  result = expression_not_using_n;  // Execute once
}
for (int n = 0; n < iterations; n++) {
  // Empty counting loop (or eliminate entirely)
}
```

### Implementation Steps

1. **Detect loop induction variable** (the counter `n`)
2. **Check if loop body uses induction variable** - if not, body is iteration-independent
3. **Check if result is overwritten each iteration** - if yes, only last iteration matters
4. **Hoist entire body** before loop (leave empty counter)

This bypasses the complex LICM machinery and directly targets the benchmark pattern.

---

## Implementation Plan: Automatic Purity Inference

This is the **recommended fix** that makes the benchmark optimization automatic without requiring source code changes.

### Overview

When TCC compiles a function, analyze its IR to determine purity and cache the result. When LICM encounters a call to a same-TU function, look up the cached purity instead of defaulting to IMPURE.

### Phase 1: Purity Inference Engine (1 day)

**File:** `ir/licm.c` (add new function)

```c
/* Infer function purity by analyzing its IR
 * Called after IR generation for each function
 * Returns: TCC_FUNC_PURITY_CONST, TCC_FUNC_PURITY_PURE, or TCC_FUNC_PURITY_IMPURE
 */
TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir)
{
  int is_const = 1;  /* Assume const until proven otherwise */

  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];

    switch (q->op) {
    case TCCIR_OP_STORE:
    case TCCIR_OP_STORE_INDEXED:
      /* Store to non-stack memory → IMPURE */
      {
        IROperand dest = tcc_ir_op_get_dest(ir, q);
        if (!is_stack_or_param_addr(ir, dest))
          return TCC_FUNC_PURITY_IMPURE;
      }
      break;

    case TCCIR_OP_LOAD:
    case TCCIR_OP_LOAD_INDEXED:
      /* Load from non-stack/param → not CONST (could still be PURE) */
      {
        IROperand src = tcc_ir_op_get_src1(ir, q);
        if (!is_stack_or_param_addr(ir, src))
          is_const = 0;
      }
      break;

    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      /* Call to impure function → IMPURE */
      {
        IROperand src1 = tcc_ir_op_get_src1(ir, q);
        Sym *callee = irop_get_sym_ex(ir, src1);
        int callee_purity = tcc_ir_get_func_purity(ir, callee);
        if (callee_purity == TCC_FUNC_PURITY_IMPURE)
          return TCC_FUNC_PURITY_IMPURE;
        if (callee_purity == TCC_FUNC_PURITY_PURE)
          is_const = 0;
      }
      break;

    default:
      break;
    }
  }

  return is_const ? TCC_FUNC_PURITY_CONST : TCC_FUNC_PURITY_PURE;
}
```

**Helper function needed:**

```c
/* Check if an operand refers to stack or parameter memory */
static int is_stack_or_param_addr(TCCIRState *ir, IROperand op)
{
  int tag = irop_get_tag(op);

  /* Stack offsets are always local */
  if (tag == IROP_TAG_STACKOFF)
    return 1;

  /* VREGs that hold stack addresses */
  /* This requires tracking - conservative: return 0 for vregs */
  if (tag == IROP_TAG_VREG)
    return 0;  /* Conservative: unknown pointer */

  return 0;
}
```

### Phase 2: Purity Cache (0.5 day)

**File:** `tcc.h` - Add cache structure

```c
/* Function purity cache for LICM optimization */
typedef struct FuncPurityEntry {
  int token;              /* Function name token (v field of Sym) */
  int purity;             /* TCC_FUNC_PURITY_* value */
} FuncPurityEntry;

#define FUNC_PURITY_CACHE_SIZE 256

/* In TCCState: */
FuncPurityEntry func_purity_cache[FUNC_PURITY_CACHE_SIZE];
int func_purity_cache_count;
```

**File:** `ir/licm.c` - Add cache functions

```c
/* Add function purity to cache */
void tcc_ir_cache_func_purity(TCCState *s, int func_token, TCCFuncPurity purity)
{
  if (s->func_purity_cache_count >= FUNC_PURITY_CACHE_SIZE)
    return;  /* Cache full */

  s->func_purity_cache[s->func_purity_cache_count].token = func_token;
  s->func_purity_cache[s->func_purity_cache_count].purity = purity;
  s->func_purity_cache_count++;
}

/* Lookup function purity from cache */
int tcc_ir_lookup_func_purity(TCCState *s, int func_token)
{
  for (int i = 0; i < s->func_purity_cache_count; i++) {
    if (s->func_purity_cache[i].token == func_token)
      return s->func_purity_cache[i].purity;
  }
  return -1;  /* Not found */
}
```

### Phase 3: Integration (0.5 day)

**File:** `tccgen.c` - After IR generation, cache purity

```c
/* In gen_func_body() or wherever IR generation completes: */
if (tcc_state->opt_licm) {
  /* Infer and cache purity for this function */
  TCCFuncPurity purity = tcc_ir_infer_func_purity(ir);
  tcc_ir_cache_func_purity(tcc_state, cur_func->v, purity);

#ifdef DEBUG_IR_GEN
  printf("[PURITY] Function '%s' inferred as %s\n",
         get_tok_str(cur_func->v, NULL),
         purity == TCC_FUNC_PURITY_CONST ? "CONST" :
         purity == TCC_FUNC_PURITY_PURE ? "PURE" : "IMPURE");
#endif
}
```

**File:** `ir/licm.c` - Update `tcc_ir_get_func_purity()` to check cache

```c
int tcc_ir_get_func_purity(TCCIRState *ir, Sym *sym)
{
  if (!sym)
    return TCC_FUNC_PURITY_UNKNOWN;

  /* Check cache first */
  int cached = tcc_ir_lookup_func_purity(tcc_state, sym->v);
  if (cached >= 0) {
#ifdef DEBUG_IR_GEN
    printf("[LICM] Found cached purity for '%s': %d\n",
           get_tok_str(sym->v, NULL), cached);
#endif
    return cached;
  }

  /* ... rest of existing logic (check attributes, well-known table) ... */
}
```

### Phase 4: Testing (0.5 day)

**Test 1:** Verify purity inference

```c
// tests/ir_tests/purity_inference.c
static int pure_func(int x) { return x * 2 + 1; }  // Should infer CONST
static int impure_func(int x) { global = x; return x; }  // Should infer IMPURE

int test_pure(int n) {
  int r = 0;
  for (int i = 0; i < n; i++)
    r = pure_func(100);  // Should be hoisted
  return r;
}
```

**Test 2:** Verify benchmark improvement

```bash
cd tests/benchmarks
./run_benchmark.py --filter function_calls
# Expected: cycles/iter drops from ~56,000 to ~5,000
```

### Timeline

| Task | Effort | Cumulative |
|------|--------|------------|
| Phase 1: Purity inference engine | 1 day | 1 day |
| Phase 2: Purity cache | 0.5 day | 1.5 days |
| Phase 3: Integration | 0.5 day | 2 days |
| Phase 4: Testing | 0.5 day | 2.5 days |
| Buffer for edge cases | 0.5 day | **3 days** |

### Expected Results

| Metric | Before | After |
|--------|--------|-------|
| `function_calls` cycles/iter | 56,049 | ~5,000 |
| TCC-O1/GCC-O1 ratio | 1377% | ~125% |

### Edge Cases to Handle

1. **Recursive functions**: Mark as IMPURE (conservative) or implement fixpoint
2. **Indirect calls (function pointers)**: Already handled - marked IMPURE
3. **Variadic functions**: Mark as IMPURE (may have side effects)
4. **Inline assembly**: Mark as IMPURE
5. **Volatile accesses**: Mark as IMPURE

### Files to Modify

| File | Changes |
|------|---------|
| `tcc.h` | Add `FuncPurityEntry` struct and cache array to TCCState |
| `ir/licm.c` | Add `tcc_ir_infer_func_purity()`, cache functions |
| `ir/licm.h` | Declare new functions |
| `tccgen.c` | Call purity inference after IR generation |
| `tests/ir_tests/` | Add purity inference test |

---
