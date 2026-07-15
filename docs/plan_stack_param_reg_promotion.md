# Stack-passed parameter register promotion

## Problem (real corpus case: FIR filter)

A stack-passed parameter used loop-invariantly is reloaded from its incoming
stack home on **every** loop iteration, instead of being held in a register.

```
tcc -O2 fir inner loop:   ldr r9,[sp,#40]   ; reload `taps` (5th param) each iter
                          cmp r6, r9
gcc -O2:                  keeps loop bound in a register; no reload
```

Per-iteration cost: 1 extra load. On the FIR inner loop this is the full
tcc(3 loads/iter) vs gcc(2 loads/iter) gap.

## Root cause: the allocator promotes it, then three passes discard it

The linear scan **does** assign `taps` (P4) a register:
`[LS] P4 [0,35] -> R12` (interval correctly extended across both loops by the
back-edge pass). But R12 never appears in the emitted body — three sites reset
a stack-passed param's allocation back to `PREG_NONE`, because the prologue was
never taught to load a stack param into its assigned register:

1. `tcc_ir_avoid_spilling_stack_passed_params` — `ls->r0 = PREG_NONE`
   (source/backend/generators/regalloc.c:384; body ir/codegen.c:401)
2. `tcc_ir_register_allocation_params` — `allocation.r0 = PREG_NONE`
   (driver line 621; body ir/codegen.c:253)
3. Prologue param loop — `allocation.r0 = PREG_NONE` (arm-thumb-gen.c:9883)

Then materialization (ir/machine_op.c:214) sees `PREG_NONE` and emits
`MACH_OP_PARAM_STACK` = reload from `[sp + original_offset + offset_to_args]`.

Because the allocator already reserved R12 for P4's whole `[0,35]` interval,
the register is **reserved but wasted** — promoting it costs zero extra pressure.

## Chosen strategy: B — pre-RA IR promotion (lower codegen risk than A)

Insert `Tx <- P` (ASSIGN) at function entry for a stack-passed, non-addr-taken,
non-64-bit param used inside a loop; rewrite the in-loop uses of `P` to `Tx`.
The ASSIGN materializes as one load-from-home into `Tx`'s register; every
existing mechanism (RA, scratch, dry-run, 64-bit) handles a normal TEMP with no
new prologue/dry-run code. `P`'s own interval collapses to `[0, def(Tx)]`, so
net long-lived pressure is unchanged.

Rejected strategy A (keep the allocator's register + emit a prologue load):
fixes the waste directly but must coordinate all three resets AND add a prologue
load whose size must stay dry-run/real-run consistent, plus R12-as-scratch
aliasing — miscompile-class risks that make test-ir may not catch without fuzz.

### Placement & gating
- Run as a dedicated pass **immediately before `tcc_ir_ssa_regalloc`** so no
  later copy-prop can forward `P` back into the rewritten uses.
- Stack-passed detection: replicate the AAPCS argno walk from
  `tcc_ir_avoid_spilling_stack_passed_params`.
- Gate: param used inside a detected loop (`tcc_ir_detect_loops`), `!addrtaken`,
  scalar 32-bit (skip double/llong in v1). Insert one `Tx<-P` per promoted param.

### Validation
Rebuild clean (no LS log), confirm FIR keeps `taps` in a register (no per-iter
reload), `make test-ir` 13623/0, object-diff delta over corpus.

## Implemented (ir/licm.c `tcc_ir_promote_loop_stack_params`, wired pre-RA in
## source/backend/generators/regalloc.c)

- Detects stack-passed scalar-32-bit non-addr-taken params (AAPCS argno walk).
- Promotes only params all of whose uses are by-value (is_lval|is_local OK;
  is_llocal = pointer deref → skip) and that are used inside a loop.
- **Call-crossing gate (critical):** a promoted value held across a call needs a
  callee-saved reg; several in a call-heavy function flood the set → spills.
  Gate: `span_end < first_call`, where span_end extends each use to its
  containing loop's back-edge (so a call nested in the loop after the use still
  counts as crossed). This fixed torture 920625-1 `synth` (+297B → 0) and
  930421-1 `f` (call in nested loop, +6 → 0).
- Phase 1 decides (loop info valid), phase 2 rewrites uses to a fresh temp +
  inserts `Tx <- P` at entry (index 0; insert_instruction_before renumbers
  jump/switch targets). IJUMP present → whole pass skipped.

### Result (make test-ir 13623/0)
Speed optimization: hot-loop reloads drop out of the loop body (FIR 3→2
loads/iter = gcc parity; pr113787:foo −22, byte_match_count2 −14). Static size
is roughly net-neutral — a few leaf functions regress +2..+12 where promotion
adds one callee-saved push whose cost is invisible pre-RA (matmul_i4 +10,
proc4WithoutFDFE +12). Compile time −16.8% (fewer reload materializations).
No pre-RA gate cleanly separates the pressure-induced leaf regressions from the
wins (both are call-free multi-use), so the size/speed trade is the user's call.
Toggle: `TCC_DISABLE_PASS=ra:stack_param_promote`. Gate per
[[no-fuzz-runs-user-verifies]] / [[validate-with-make-test]].
