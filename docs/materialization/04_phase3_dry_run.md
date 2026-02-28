# Phase 3: Dry-Run Integration

> **Status: ✅ COMPLETE** — committed `bc43b639 phase 3` + `c2569883 phase 3: enable dry-run scratch conflict fixup`

## Goal

Extend the existing dry-run pass in `ir/codegen.c` to collect per-instruction scratch register constraints using `MachineOperand`, and feed these constraints back to the register allocator.

## Current State (Important: Dry Run Already Exists)

**The original plan described this as a new feature, but a dry-run pass already exists.** The current `tcc_ir_codegen_generate()` in `ir/codegen.c` already runs the backend twice:

1. **Dry run:** Calls `tcc_gen_machine_dry_run_begin()`, runs the full dispatch loop (instruction handlers execute but `ot()` is a no-op), then calls `tcc_gen_machine_dry_run_end()`.
2. **Real run:** Restores `ind`/`loc` state and runs the dispatch loop again, this time emitting actual code.

The dry run currently serves to:
- Compute accurate code sizes for branch offset optimization (`tcc_gen_machine_branch_opt_analyze`)
- Detect whether LR was pushed in loops (to move it to prologue instead)
- Record scratch register usage patterns

**What's missing:** The dry run does not currently feed scratch constraints back to the register allocator. It runs *after* allocation is final.

## Proposed Extension

### Per-instruction constraint collection

During the dry run, each `mach_ensure_in_reg()` / `mach_alloc_scratch()` call records what it needs:

```c
typedef struct {
    int instruction_index;
    int scratch_regs_needed;      /* how many scratch regs this instruction needs */
    int scratch_reg_hints[4];     /* preferred scratch registers (if any) */
    bool needs_pair;              /* needs an even-aligned register pair */
    bool clobbers[16];            /* which physical registers this instruction clobbers */
} InstructionConstraints;
```

### Constraint-aware allocation

```
Current flow:
  liveness → allocator → dry run (for branch sizing) → real run

Proposed flow:
  liveness → allocator (initial) → dry run (collect constraints) → allocator (refined) → real run
```

The second allocator pass is lightweight — it only adjusts assignments where the dry run found conflicts (e.g., a vreg was allocated to a register that a specific instruction needs as scratch).

## Implementation Steps

### Step 3.1: Add constraint recording to `MachineCodegenContext`

**Action:** Extend the context struct (from Phase 2) with constraint tracking:

```c
typedef struct {
    // ... existing fields from Phase 2 ...

    /* Constraint recording (dry run only) */
    InstructionConstraints *constraints;
    int constraints_count;
    int constraints_capacity;
} MachineCodegenContext;
```

In dry-run mode, `mach_alloc_scratch()` records the scratch register it chose (or would choose) into `constraints[current_instruction]`.

### Step 3.2: Record constraints during dry run

**Action:** Modify the `mach_*` helpers to record scratch usage when `ctx->plan_mode == true`:

```c
static int mach_alloc_scratch(MachineCodegenContext *ctx, uint16_t exclude_mask)
{
    int reg;
    if (ctx->plan_mode) {
        // Record that this instruction needs a scratch register
        ctx->constraints[ctx->instruction_index].scratch_regs_needed++;
        // Still allocate (to detect conflicts), but don't emit PUSH/POP
        reg = get_scratch_reg_with_save(exclude_mask);
    } else {
        reg = get_scratch_reg_with_save(exclude_mask);
    }
    return reg;
}
```

### Step 3.3: Feed constraints to allocator

**Action:** After dry run, scan constraints for conflicts:

```c
void tcc_ir_apply_scratch_constraints(TCCIRState *ir,
                                       InstructionConstraints *constraints,
                                       int count)
{
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < 16; c++) {
            if (constraints[i].clobbers[c]) {
                // Mark register c as unavailable at instruction i
                // This creates a "clobber interval" that the allocator respects
                tcc_ls_add_clobber(ir, constraints[i].instruction_index, c);
            }
        }
    }
    // Re-run allocation with clobber intervals
    tcc_ls_reallocate_with_clobbers(ir);
}
```

**Design decision:** The second allocation pass should be *incremental* — only re-allocate vregs that conflict with newly-discovered clobbers. A full re-allocation is correct but slower.

### Step 3.4: Verify dry-run consistency

**Action:** Add assertions that the dry run and real run produce consistent scratch allocation:

```c
// After each instruction in real run:
if (DEBUG_VERIFY) {
    assert(ctx->current_scratch_count == constraints[i].scratch_regs_needed);
}
```

Any divergence indicates a bug in the constraint recording.

### Step 3.5: Incremental rollout

**Action:** Initially, skip the second allocator pass and just collect/log constraints. Verify that:

1. Constraint recording doesn't change behavior
2. Recorded constraints match actual scratch usage
3. Performance overhead is negligible

Then enable the constraint-aware re-allocation in a follow-up.

## Risk Assessment

- **Risk: Low for constraint recording.** The dry run already exists; we're just adding bookkeeping.
- **Risk: Medium for constraint-aware allocation.** Re-running the allocator requires careful handling of already-assigned registers.
- **Risk: Low for divergence.** The dry run is deterministic — if both passes use the same `MachineOperand` inputs, constraints must match.

## What Was Actually Built

The design diverged from the plan's proposal. The actual implementation is simpler and more effective:

### Per-instruction arrays (replaces `InstructionConstraints` struct)

```c
int      *dry_insn_scratch;   /* count of mach_alloc_scratch() calls per instruction */
uint16_t *dry_insn_saves;     /* bitmask of registers needing PUSH per instruction */
```

Allocated in `tcc_ir_codegen_generate()` for `ir->next_instruction_index` entries.

### Scratch recording (replaces `plan_mode` flag)

`arm-thumb-gen.c` uses two globals reset before each instruction:
```c
static int g_insn_scratch_count;        /* incremented in get_scratch_reg_with_save */
static uint16_t g_insn_scratch_saves;   /* OR'd with (1<<reg) when PUSH needed */
```

Queried via `tcc_gen_machine_insn_scratch_count()` and `tcc_gen_machine_insn_scratch_saves_mask()` after the dry-run handler executes.

### `try_reassign_scratch_conflict()` (replaces `tcc_ls_reallocate_with_clobbers()`)

When a vreg is assigned to a register that needs to call `get_scratch_reg_with_save()` (i.e., the register is live and thus must be PUSH'd during scratch allocation), this function finds an alternative callee-saved register with no live interval overlap and reassigns the vreg there.

**Key fix:** ARM frame pointer R7 (`R_FP`) and the static chain register R10 are excluded from the candidate set — they are never allocated to vregs but would otherwise appear "free" in the live-register bitmask.

### Consistency check

Under `TCC_LS_DEBUG`, a mismatch check compares dry-run scratch count against real-run scratch count per instruction, flagging unexpected divergence (expected only when the fixup was applied).

## Verification Checklist

- [x] `dry_insn_scratch[]` and `dry_insn_saves[]` arrays allocated and populated during dry run
- [x] Per-instruction scratch globals reset via `tcc_gen_machine_insn_scratch_reset()` before each instruction
- [x] `try_reassign_scratch_conflict()` reassigns conflicting vregs to callee-saved registers
- [x] R7 (R_FP) excluded from reassignment candidates
- [x] Static chain register excluded when `ir->has_static_chain`
- [x] `tcc_ls_reset_scratch_cache()` called after any fixup
- [x] Consistency check logging under `TCC_LS_DEBUG`
- [x] `make test -j16` passes (3310 tests, 0 failures)
- [x] `postmod-1` test passes at both -O0 and -O1
