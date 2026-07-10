# Optimization Architecture

TinyCC's optimizer operates in three sequential phases over flat IR,
before and after register allocation:

```
 ┌─────────────────────────────────────────────────────────────────┐
 │                     tccgen.c  (frontend)                        │
 │  IR generation → tccgen_pass1() → tccgen_pass2() → ...        │
 └─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
 ┌─────────────────────────────────────────────────────────────────┐
 │  PHASE 1: Legacy Pre-SSA Flat-IR Optimizations                 │
 │  ir/opt_pipeline.c  (tcc_ir_opt_run_default)                   │
 │  Pass groups: entry_store → propagation → memory → fusion →     │
 │              late_cleanup                                      │
 │  Runs at every -O level; pass groups selected by level.          │
 └─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
 ┌─────────────────────────────────────────────────────────────────┐
 │  PHASE 2: SSA Construction + SSA-Phase Optimizations            │
 │  ir/regalloc.c  (tcc_ir_optimize)                              │
 │  Loop transforms (flat IR) → CFG/SSA build → SSA opt engine    │
 │  Passes: var_const_fold, sccp, cprop, fold, branch, DCE, ...  │
 └─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
 ┌─────────────────────────────────────────────────────────────────┐
 │  PHASE 3: Register Allocation                                   │
 │  ir/regalloc.c  (ra_linear_scan)                               │
 │  Graph coalescing → linear scan → phi resolution → live regs   │
 └─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
 ┌─────────────────────────────────────────────────────────────────┐
 │  PHASE 4: Post-RA Optimizations                                │
 │  ir/regalloc.c  (tcc_ir_move_coalescing)                       │
 │  ir/opt_promote.c  (post_ra_forward_diamond)                   │
 │  arm-thumb-gen.c  (codegen peepholes, branch optimization)     │
 └─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                     Machine code emission
```

## Key Design Decisions

1. **Flat IR before SSA**: Loop transforms (rotation, LICM, reroll, const_sim,
   unroll, IV-SR, decrement-to-zero) operate on flat IR before CFG/SSA
   construction. This avoids the complexity of SSA-form loop analysis and
   reuses the proven flat-IR engines from the tccgen era.

2. **Two IR layers**: Pre-SSA passes work on `TCCIRState` flat IR with
   `IROptCtx`. SSA passes work on the same IR but with `IRSSAOptCtx` that
   maintains use-def chains. The pipeline driver (`opt_pipeline.c`) provides
   the pre-SSA pass groups; the SSA driver (`ssa_opt.c`) provides the
   per-opcode generator table.

3. **Generator model**: Both engines use a generator table — a list of
   `(opcode, function)` pairs dispatched per instruction. This mirrors the
   thumb-2 opcode builder model and avoids per-pass instruction scans.

4. **Cascade wrappers**: Some pass groups embed fixpoint loops (kb_cascade,
   const_prop_cascade, branch_cleanup_cascade, entry_store_cleanup) that
   run multiple sub-passes to a fixpoint within a single pipeline invocation.
   This ensures cross-dependencies between passes converge.

5. **Flag-gated passes**: Every pass has a `flag_offset` (a byte offset into
   `TCCState`) that gates it. The pipeline checks `*((unsigned char *)tcc_state
   + flag_offset)` before running each pass. This allows runtime disabling
   via `-fno-<flag>` or the `TCC_DISABLE_PASS` env var.

6. **Optimization levels**: Four pipelines are defined:
   - `O0`: DCE only (correctness)
   - `O1`: propagation + late_cleanup
   - `Os`: propagation + memory + late_cleanup (skip fusion for size)
   - `O2`: full pipeline including fusion

## File Map

| File | Role |
|------|------|
| `ir/opt_pipeline.c` | Pre-SSA pass groups, level presets, cascade wrappers |
| `ir/opt_engine.c` | Pre-SSA generator dispatch, `IROptCtx` |
| `ir/ssa.c` | SSA construction (phi placement) and renaming |
| `ir/opt/ssa_opt.c` | SSA generator dispatch, use-def chains, guard collapse |
| `ir/opt/ssa_opt.h` | SSA pass declarations |
| `ir/opt/*.c` | Individual pre-SSA passes (constprop, dce, etc.) |
| `ir/opt/ssa_opt_*.c` | Individual SSA passes |
| `ir/regalloc.c` | Register allocation + SSA pipeline driver + post-RA |
| `ir/opt_promote.c` | Post-RA diamond forwarding |
| `arch/arm/ssa_opt_arm.c` | ARM-specific SSA generators (MLA, indexed mem) |
| `arm-thumb-gen.c` | Codegen peepholes, branch optimization |
| `tccopt.h` | Public optimization API |

## Disable Mechanisms

- **Compile flag**: `-fno-<flag>` (e.g. `-fno-const-prop`)
- **Env var**: `TCC_DISABLE_PASS=<pass_name>` (e.g. `TCC_DISABLE_PASS=ssa:branch`)
- **Level**: Lower optimization levels skip entire groups
