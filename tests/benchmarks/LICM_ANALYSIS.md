# LICM (Loop-Invariant Code Motion) Analysis

## Current Status: Enabled but Limited

LICM is **enabled at -O1** (`opt_licm = 1` in [libtcc.c#L2097](libtcc.c#L2097)), but the current implementation cannot optimize the `function_calls` benchmark due to specific limitations.

## Why LICM Doesn't Help function_calls Benchmark

### The Problem

The benchmark calls `func_a`, `func_b`, `func_c` - static functions defined in the same file with `NOINLINE` attribute:

```c
static int NOINLINE func_a(int x) { return x * 3 + 7; }
static int NOINLINE func_b(int x) { return x * 5 - 3; }
static int NOINLINE func_c(int x) { return (x << 2) + 1; }
```

### Current LICM Logic (from [ir/licm.c](ir/licm.c))

The LICM implementation has **three hoisting mechanisms**:

1. **Stack Address Hoisting** (lines 440-640): Hoists `Addr[StackLoc[offset]]` computations
2. **Constant Expression Hoisting** (lines 660-840): Hoists arithmetic with constant operands
3. **Pure Function Call Hoisting** (lines 880-1450): Hoists calls to pure/const functions

### Why Pure Function Hoisting Fails

The pure function detection in `tcc_ir_get_func_purity()` (lines 975-1050) checks:

1. **Well-known pure functions** - a hardcoded table of libc functions (strlen, abs, sqrt, etc.)
2. **`__attribute__((const))`** - explicit const attribute
3. **`__attribute__((pure))`** - explicit pure attribute

**The benchmark functions `func_a`, `func_b`, `func_c` have NONE of these!**

They're marked as `NOINLINE` but not `pure` or `const`. The LICM sees them as **unknown purity** and conservatively treats them as impure.

```c
// From tcc_ir_get_func_purity():
/* Conservative default: unknown = IMPURE (can't hoist) */
return TCC_FUNC_PURITY_IMPURE;  // <-- This is what happens!
```

## GCC's Advantage: Interprocedural Analysis

GCC can **see the function bodies** and infer purity:
- `func_a` only uses its parameter `x`, performs arithmetic, and returns
- No stores to memory, no global reads, no calls to impure functions
- Therefore: implicitly `const`

TCC's LICM doesn't perform this interprocedural analysis - it only looks at attributes and a hardcoded table.

---

## LICM Architecture (Current)

```
tcc_ir_opt_licm() [main entry]
    │
    ├── tcc_ir_detect_loops()     - Find loops via backward jump detection
    │
    ├── tcc_ir_hoist_pure_calls() - Hoist pure function calls (Phase 1)
    │   │
    │   ├── For each loop:
    │   │   ├── Skip if preheader inside another loop
    │   │   ├── Skip if loop contains VLA_ALLOC
    │   │   └── For each FUNCCALLVAL/VOID:
    │   │       ├── tcc_ir_is_hoistable_call()
    │   │       │   ├── Get function symbol
    │   │       │   ├── tcc_ir_get_func_purity() ← BOTTLENECK!
    │   │       │   │   ├── Check well-known pure function table
    │   │       │   │   ├── Check func_const attribute
    │   │       │   │   ├── Check func_pure attribute
    │   │       │   │   └── Default: IMPURE ← func_a/b/c end up here
    │   │       │   └── Check all args are loop-invariant
    │   │       └── Hoist call + params to preheader
    │
    └── tcc_ir_hoist_loop_invariants() - Hoist stack addrs & const exprs
        │
        └── For each loop:
            ├── Skip if loop contains function calls ← BLOCKS OPTIMIZATION!
            └── Hoist stack addresses and const exprs
```

### Key Insight: Double-Blocking

1. `func_a/b/c` can't be hoisted by `tcc_ir_hoist_pure_calls()` because they're not recognized as pure
2. Other optimizations in `tcc_ir_hoist_loop_invariants()` are **blocked** because the loop contains function calls

---

## Fix Options

### Option 1: Mark Benchmark Functions as Pure (Easy, ~5 min)

Add `__attribute__((const))` to the benchmark:

```c
static int NOINLINE __attribute__((const)) func_a(int x) { return x * 3 + 7; }
static int NOINLINE __attribute__((const)) func_b(int x) { return x * 5 - 3; }
static int NOINLINE __attribute__((const)) func_c(int x) { return (x << 2) + 1; }
```

**Pros:** Immediate fix, no compiler changes
**Cons:** Requires source code changes, not automatic

### Option 2: Infer Purity from Function Body (Medium, ~3-5 days)

When compiling a function, analyze its IR to determine purity:

```c
TCCFuncPurity tcc_ir_infer_func_purity(TCCIRState *ir) {
  // Scan all instructions
  for (int i = 0; i < ir->next_instruction_index; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];

    // STORE to non-stack memory → not pure
    if (q->op == TCCIR_OP_STORE) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (!is_stack_or_param(dest))
        return TCC_FUNC_PURITY_IMPURE;
    }

    // LOAD from non-stack/param → not const (but may be pure)
    if (q->op == TCCIR_OP_LOAD) {
      IROperand src = tcc_ir_op_get_src1(ir, q);
      if (!is_stack_or_param(src))
        is_const = false;  // Still could be pure
    }

    // Call to impure function → not pure
    if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID) {
      int callee_purity = get_callee_purity(ir, q);
      if (callee_purity < TCC_FUNC_PURITY_PURE)
        return TCC_FUNC_PURITY_IMPURE;
    }
  }

  return is_const ? TCC_FUNC_PURITY_CONST : TCC_FUNC_PURITY_PURE;
}
```

**Pros:** Automatic purity detection
**Cons:** Requires two-pass compilation or caching

### Option 3: Loop Independence Detection (Medium, ~2-3 days)

Detect that the loop body doesn't use the loop counter:

```c
// In bench_function_calls:
for (int n = 0; n < iterations; n++) {
  result = ...;  // 'n' is never used here!
}
```

If `n` isn't used in the body and `result` is completely overwritten, the body is **idempotent** - executing it once gives the same result as 1000 times.

**Pros:** Doesn't require purity analysis
**Cons:** Narrower scope, only helps specific patterns

### Option 4: Same-TU Function Purity Cache (Recommended, ~1-2 days)

When compiling functions in the same translation unit:
1. After generating IR for a function, infer and cache its purity
2. When LICM encounters a call to a same-TU function, look up cached purity

```c
// In tcc.h or tccir.h
typedef struct FuncPurityCache {
  int token;           // Function name token
  TCCFuncPurity purity;
} FuncPurityCache;

// In TCCState or TCCIRState
FuncPurityCache *func_purity_cache;
int func_purity_cache_size;
```

**Pros:** Works for same-file static functions (like the benchmark)
**Cons:** Doesn't help cross-TU calls

---

## Recommended Implementation Plan

### Phase 1: Quick Test (Option 1) - 5 minutes
Add `__attribute__((const))` to benchmark functions and verify LICM works.

### Phase 2: Purity Cache (Option 4) - 1-2 days
1. Add `FuncPurityCache` to `TCCIRState`
2. After `gen_func_body()`, call `tcc_ir_infer_and_cache_purity()`
3. In `tcc_ir_get_func_purity()`, check cache before returning IMPURE

### Phase 3: Full Inference (Option 2) - 3-5 days
Implement full purity inference for all functions.

---

## Test: Verify LICM Is Working

To test if LICM activates with explicit attributes:

```c
// test_licm_pure.c
static int __attribute__((const)) pure_add(int x) { return x + 1; }

int test(int n) {
  int r = 0;
  for (int i = 0; i < n; i++) {
    r = pure_add(100);  // Should be hoisted!
  }
  return r;
}
```

Compile with:
```bash
./armv8m-tcc -O1 -dump-ir -c test_licm_pure.c
```

Look for the `pure_add` call to be BEFORE the loop, not inside it.

---

## Files Involved

| File | Role |
|------|------|
| [ir/licm.c](ir/licm.c) | Main LICM implementation |
| [ir/licm.h](ir/licm.h) | LICM API |
| [tcc.h](tcc.h) | `opt_licm` flag, TCCState |
| [libtcc.c](libtcc.c) | Enables LICM at -O1 |
| [tccgen.c](tccgen.c) | Calls `tcc_ir_opt_licm()` |

---

## Summary

**Current state:** LICM is enabled and works for:
- ✅ Stack address hoisting
- ✅ Constant expression hoisting
- ✅ Calls to well-known pure functions (strlen, abs, etc.)
- ✅ Calls with explicit `__attribute__((pure/const))`

**Not working for:**
- ❌ User-defined functions without explicit attributes
- ❌ The `function_calls` benchmark (func_a/b/c are not marked pure)

**Fix:** Either mark functions as pure, or implement purity inference from function bodies.
