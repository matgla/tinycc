# Scalar Evolution / Loop Accumulator Optimization Design

## Goal

Recognize simple accumulation patterns in loops and replace them with a
closed-form computation, eliminating the loop entirely without unrolling.

## Motivating Example

After strlen folding, the loop:

```c
int sum = 0;
for (int i = 0; i < 5; i++) {
    sum += 5;  // strlen("hello") folded to 5
}
```

produces IR:

```
V1 <-- #0            ; sum = 0
V2 <-- #0            ; i = 0
loop:
  CMP V2, #5
  JMP exit if >=S
  V1 <-- V1 ADD #5   ; sum += 5
  V2 <-- V2 ADD #1   ; i++
  JMP loop
exit:
  ... use V1 ...
```

Scalar evolution recognizes that `V1` has the closed form:
`V1_final = init + trip_count * stride = 0 + 5 * 5 = 25`

The entire loop is replaced with:

```
V1 <-- #25
```

## Relationship to Loop Unrolling

These are complementary optimizations:

| | Loop Unrolling | Scalar Evolution |
|---|---|---|
| Approach | Replicate body N times | Compute final value directly |
| When better | Body has side effects, memory ops | Body is pure accumulation |
| Code size | Grows with trip count | Constant (1-2 instructions) |
| Generality | Works for any small loop | Only for reducible patterns |

Scalar evolution is strictly better when applicable, but applies to fewer cases.
Loop unrolling is more general and also enables scalar evolution indirectly
(by exposing constant patterns to the existing constant propagation).

**Recommended order**: Try scalar evolution first; if it fails, fall back to
loop unrolling.

## Scope

**Patterns recognized** (initial implementation):

1. **Constant accumulation**: `acc += constant` over N iterations
   - Result: `acc = init + N * constant`
2. **Linear induction final value**: `i = 0; i < N; i += step`
   - Result: `i_final = N` (or `init + trip_count * step`)
3. **Constant assignment in loop**: `x = constant` repeated N times
   - Result: `x = constant` (one assignment)

**Not in scope** (future work):
- Polynomial induction (`sum += i` → triangular number)
- Reduction with non-constant stride (`sum += arr[i]`)
- Floating-point accumulation (precision semantics differ)
- Multiple exit loops

## Where It Fits in the Pipeline

```
Phase 1:  Constant propagation + strlen folding  (existing)
Phase 5a: Scalar evolution / loop replacement     (NEW)
Phase 5b: Loop unrolling (for remaining loops)    (NEW)
Phase 1': Re-run constant prop + DCE              (collapse results)
Phase 5:  LICM                                    (existing, disabled)
Phase 6:  IV strength reduction                   (existing)
```

Runs in the same slot as loop unrolling, just before it.

## Algorithm

### Step 1: Loop analysis

For each detected loop (reuse `tcc_ir_detect_loops()`):

1. Identify all **basic induction variables** (reuse `find_induction_vars()`)
2. Determine **trip count** (same as loop unrolling: constant init, limit, step)
3. Verify **single exit** from loop header

### Step 2: Classify loop body vregs

Scan all non-NOP instructions in the loop body. For each VAR vreg `V` defined
in the loop, classify it:

- **Basic IV**: `V = V + const_step` (already identified)
- **Constant accumulator**: `V = V + const` or `V = V - const`
  (where const does not depend on any loop-variant value)
- **Constant overwrite**: `V = const` (same constant every iteration)
- **Non-reducible**: anything else (memory store, function call, etc.)

A loop is **fully reducible** if:
- Every instruction is either a NOP, an IV increment, a reducible accumulator
  update, or a branch instruction (CMP/JMP) for loop control
- There are no STORE, CALL, or other side-effecting instructions

### Step 3: Compute closed-form values

For each reducible accumulator:

| Pattern | Closed Form |
|---------|------------|
| `V = V + C` (accumulator) | `V_final = V_init + trip_count * C` |
| `V = V - C` | `V_final = V_init - trip_count * C` |
| `V = C` (overwrite) | `V_final = C` |
| IV `V += step` | `V_final = V_init + trip_count * step` |

Compute `trip_count * C` at compile time (both are constants). If the result
overflows 32 bits, bail out (preserve runtime semantics).

### Step 4: Replace loop with assignments

1. NOP out all instructions from loop preheader through loop end
2. At the loop start position, emit:
   - For each reducible VAR: `V <-- #closed_form_value`
   - Fall through to the original exit target
3. If any VAR is used after the loop, make sure its final value is set

### Step 5: Dead IV cleanup

The IV initialization and any IV-only uses become dead. Existing DCE handles
this automatically.

## API

```c
/* In ir/opt.h */

/* Attempt to replace loops with closed-form scalar computations.
 * Returns number of loops eliminated. */
int tcc_ir_opt_scalar_evolution(TCCIRState *ir);

/* Variant using pre-detected loops */
int tcc_ir_opt_scalar_evolution_with_loops(TCCIRState *ir, IRLoops *loops);
```

## Data Structures

```c
/* Accumulator pattern found in a loop body */
typedef struct LoopAccumulator {
    int vreg;         /* VAR vreg being accumulated */
    int init_val;     /* Initial value (from preheader) */
    int stride;       /* Constant added per iteration */
    int init_idx;     /* Instruction index of initialization */
    int update_idx;   /* Instruction index of accumulation in loop */
    enum {
        ACCUM_ADD,    /* V = V + C */
        ACCUM_SUB,    /* V = V - C */
        ACCUM_ASSIGN, /* V = C (constant overwrite) */
    } kind;
} LoopAccumulator;

#define MAX_ACCUMULATORS 8
```

## Configuration

Reuse `opt_loop_unroll` flag or add a separate `opt_scalar_evol` flag.
Enable at `-O1`.

## Testing Strategy

1. **Primary test**: `100_pure_func_strlen.c` - loop eliminated, sum = 25
2. **New tests**:
   - `sum += 3` over 10 iterations → sum = 30
   - `sum += i` (NOT reducible with initial impl - should fall through to
     unrolling or remain as loop)
   - Two accumulators in same loop: `sum1 += 2; sum2 += 3;`
   - Loop with memory store in body (should NOT be eliminated)
   - Trip count = 0 (loop never executes, preserve init values)
   - Accumulator with negative stride: `sum -= 1`
   - Overflow edge case: `sum += 0x40000000` over 8 iterations

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Incorrect trip count for edge conditions | Handle `<`, `<=`, `!=` separately; test boundary values |
| Overflow semantics mismatch | Use 32-bit wrapping arithmetic (matches C unsigned); bail for signed overflow |
| Dead code after elimination | Existing DCE handles cleanup |
| Interaction with IV strength reduction | Eliminated loops have no IVs; SR skips them naturally |
| Missing a side effect in the loop | Conservative: any STORE/CALL/volatile makes loop non-reducible |

## Implementation Steps

1. Write `tcc_ir_opt_scalar_evolution()` in `ir/opt.c`:
   a. Detect loops, find IVs, compute trip counts
   b. Scan body for accumulator patterns
   c. Check full reducibility (no side effects)
   d. Compute closed-form values
   e. Replace loop with constant assignments
2. Wire into pipeline before loop unrolling
3. Re-run Phase 1 constant prop after both passes
4. Add tests
5. Verify no regressions
