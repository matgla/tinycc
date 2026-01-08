# Function Parameter Handling Refactor Plan

## Goal
Make function-call argument handling **explicit, stable, and target-independent** at the IR boundary.

In particular:

- `TCCIR_OP_FUNCPARAM*` must stop being a “marker” that backends have to rediscover by scanning IR.
- Backends must not infer which `FUNCPARAM` belong to which `FUNCCALL` by walking instruction streams.
- The callsite’s argument list must be represented **once** (in IR), then consumed consistently by:
  - liveness extension (keep arg vregs live until the owning call)
  - register allocation constraints (if needed)
  - machine code generation

This reduces bug surface (nested calls, optimizations, reordering) and removes backend-only assumptions.

## Current behavior (2026-01-08)

### How arguments are emitted
In the front-end ([tccgen.c](../tccgen.c)):

- Argument expressions are evaluated, and `TCCIR_OP_FUNCPARAMVAL` is emitted with `src2.c.i = param_num` (0-based).
- For `nb_args > 4`, additional `FUNCPARAMVAL` are emitted after parsing using `vtop` (so emission order is not strictly the same as evaluation order).
- `TCCIR_OP_FUNCCALL{VAL,VOID}` is emitted for the call.

### How arguments are consumed

- IR liveness uses `tcc_ir_extend_param_intervals()` ([tccir.c](../tccir.c)) which scans backward from each `FUNCCALL*` to find matching `FUNCPARAM*` while skipping nested calls.
- ARM Thumb backend generates calls in `tcc_gen_machine_func_call_op()` ([arm-thumb-gen.c](../arm-thumb-gen.c)) and also scans backward from the call to find its params, then sorts them by `param_num`.

### Concrete failure modes / “bugprone” spots

1) **Backend re-scans IR to recover args**
   - The call generator performs a backward scan with `nested_call_depth` and a “seen params” set.
   - This duplicates logic that IR already has (liveness extension), increasing drift risk.

2) **Hard limit: `uint32_t params_found` breaks for >32 arguments**
   - The backend uses `params_found |= (1 << param_num)`.
   - Any call with `param_num >= 32` is undefined/incorrect (shift overflow / collisions).

3) **Zero-arg `FUNCCALLVOID` may have no explicit delimiter**
   - The `FUNCPARAMVOID` marker is emitted in one code path but not uniformly for all call kinds.
   - Without a delimiter or an explicit `argc`, backward-scanning can accidentally “steal” params from a prior call.

4) **Optimizer coupling**
   - Any pass that deletes/moves `FUNCPARAM*` (or introduces unrelated instructions between params and call) can silently mis-bind args.
   - This is especially risky as the project grows more IR-level transformations.

## Target end-state contract (IR → backend)

- A `FUNCCALL{VAL,VOID}` has a **direct, explicit** argument list available via IR metadata.
- Backends **never** scan for `FUNCPARAM*`.
- `TCCIR_OP_FUNCPARAM*` becomes either:
  - an IR-only construct eliminated before machine codegen, or
  - a debug-only artifact that does not affect correctness.

## Design (recommended): IR Callsite Table

Introduce an IR-owned callsite representation:

```c
// concept
typedef struct IRCallSite {
  int call_instr_index;        // current index (or orig_index)
  int argc;
  int *arg_instr_indices;      // indices of FUNCPARAMVAL ops (Phase 1)
  // Phase 2: store arg descriptors independent of FUNCPARAM instructions
} IRCallSite;
```

and a mapping in `TCCIRState` so codegen can do:

- `IRCallSite *cs = tcc_ir_callsite_for_call(ir, call_idx);`

### Key detail: key by `orig_index` (recommended)
Because IR compaction/DCE can reorder instruction indices, `orig_index` on `TACQuadruple` is the stable key.

- Store the callsite table keyed by `call->orig_index`.
- Optionally store both `orig_index` and current `call_idx` for debug.

## Staged rollout plan

### Phase 0 — tighten invariants and add debug checks (small, low risk)

- Add a verifier pass (debug-only or always-on in `-run-ir` builds) that asserts:
  - every callsite has either `argc==0` or contains param numbers `0..argc-1` exactly once
  - param numbers are non-negative
  - nested call binding is consistent
- Add explicit `argc` computation during binding (don’t rely on `FUNCPARAMVOID`).

Acceptance: verifier catches malformed sequences early.

### Phase 1 — bind params once, remove backend scanning

1) Add `tcc_ir_build_callsites(ir)` in [tccir.c](../tccir.c)
   - Run it after IR emission and after any pass that can reorder/remove instructions that matter for calls.
   - It performs a single binding algorithm (similar to the existing backward scan) and produces an `IRCallSite` per `FUNCCALL*`.

2) Update liveness extension
   - Replace `tcc_ir_extend_param_intervals()` scanning with:
     - for each callsite, extend each argument vreg interval end to `call_idx`

3) Update ARM Thumb call codegen
   - `tcc_gen_machine_func_call_op()` consumes `IRCallSite` arguments, never scans the IR stream.
   - Replace `params_found` bitmask with a safe structure (vector/boolean array sized to `argc`).

Acceptance:
- behavior identical for existing tests
- nested calls still work
- calls with >32 args no longer corrupt binding

### Phase 2 — eliminate `FUNCPARAM*` as a correctness dependency

In Phase 1, a callsite may still reference `FUNCPARAMVAL` instructions by index.
Phase 2 removes that dependency so optimizations can freely drop/reorder `FUNCPARAM*`.

Approach:

- During binding, record arguments as **descriptors**, not instruction indices.
  - store a copy of the argument `SValue` (or a compact form)
  - store the vreg id (if any) for later physical-register lookup
  - store type/size classification needed for ABI lowering (64-bit, struct size, float/double)

Then:

- After binding, rewrite `FUNCPARAM*` instructions to `TCCIR_OP_NOP` (or remove them during compaction).

Acceptance:
- backend and liveness still function with `FUNCPARAM*` removed
- IR optimizations become safer around calls

### Phase 3 (optional) — make call lowering fully explicit IR

This is the “most robust” design long-term, but largest change.

- Lower a call into explicit IR ops:
  - stack arg stores
  - register arg moves
  - the call instruction itself

Pros:
- register allocator naturally sees clobbers/conflicts
- removes complex backend remap logic

Cons:
- requires IR-level representation for ABI moves and stack layout constraints

## Tests to add (must cover)

- **Nested calls:** `f(g(1), h(2), 3)` and deeper nesting.
- **>32 arguments:** a function with e.g. 40 `int` args, and a mixture of `long long` and structs.
- **0-arg void call:** ensure binding yields `argc==0` and does not steal params.
- **Indirect calls:** `fp(a,b)` where `fp` value lives in R0-R3 and args also use R0-R3.
- **Struct by value:** small and larger structs, both register and stack passing.
- **Soft-float helper calls:** paths through `tcc_ir_put_soft_call()` which emit `FUNCPARAMVAL`.

Prefer placing new focused cases under `tests/ir_tests/` (with `.expect`) if that’s the established pattern for IR correctness.

## Notes / Constraints

- ABI rules (AAPCS, VFP/hardfloat) remain backend-owned in Phase 1/2; this refactor only changes *how arguments are associated with calls*.
- Callsite binding must not assume arguments are contiguous in the instruction stream; it should rely on param numbers and nesting rules.

## Implementation checklist

- [ ] Add `IRCallSite` representation to `TCCIRState`.
- [ ] Implement `tcc_ir_build_callsites()` and a verifier.
- [ ] Switch liveness param extension to use callsites.
- [ ] Switch ARM Thumb call lowering to use callsites (remove backward scan + `params_found`).
- [ ] Add regression tests for >32 args + 0-arg void call + nesting.
- [ ] (Optional) Phase 2: store arg descriptors and NOP-out `FUNCPARAM*`.
