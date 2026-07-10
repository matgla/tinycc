# Handoff: reduce gvn-relaxation regressions (branch `legacyOptRemoval`)

## State (2026-07-07, session 2)
- Removed (calls+bodies+tests): `bool_cse`, `cse_param_add`, `local_alu_cse`, `ptr_load_cse`, `lea_cse`.
  `strength_reduction` call removed.
- `lea_cse` → `ssa:gvn` (`gvn_try_lea` in `ir/opt/ssa_opt_gvn.c`): value-numbers `TCCIR_OP_LEA` of a
  vreg-backed stack slot (STACKOFF `vreg < -1`), dominator-scoped, `vreg == -1` left to `lea_fold`.
  net 0/416. Key must use `irop_get_stack_offset()` not `u.imm32` (STRUCT operands pack `ctype_idx`).
- `ssa:gvn` phi-operand exclusion relaxed (3 `def_phi_block >= 0` `continue`s deleted)
  **plus two profitability guards** in `ir/opt/ssa_opt_gvn.c`:
  1. **Backwards-def guard**: decline CSE when `existing->def_idx > i`. A dominating def at a
     later linear position (rotated-loop guard block) makes the reused value's live range wrap
     the whole loop (`[0,end]`, xcall=1) → callee-saved pressure + spills. Was 182 (+32),
     178 (+12), qsort (+8), bubble_sort (+4).
  2. **Un-fusing guard**: decline CSE of `phi SHL/SHR/SAR/ROR #imm` (barrel/indexed fusion)
     or `phi MUL` feeding a single ADD (MLA fusion) when either side is single-use — CSE to
     multi-use materializes an op the backend would have folded for free. **Exception**
     (`gvn_consumer_would_cse`): allow when the single consumer would itself CSE after
     substitution and its dest is a CSE-able TEMP — the whole address chain collapses
     (dijkstra `arr[i].a/.b` shared-base pattern, −44). Was matrix_test_simple (+16),
     bench_matrix_mul (+12), funcptr_fifth_arg (+4).
- `./ssaretire.sh check`: test-ir **13491 passed**, delta 28/416 changed,
  **.text net −368** (was −260), **4 LARGER** (was 11):
  `bug_mla64_non_inplace +4 · double_deref_test +4 · mibench_rijndael +12 · z_int_24769 +4`
- Fuzz NOT yet run this session — user runs `./ssaretire.sh fuzz N`.

## Residual LARGER (accepted, diagnosed)
- `bug_mla64_non_inplace +4`: CSE of `i+1` degrades `adds r2,#1` → `mov r12,r3` + wide CMP;
  copy survives regalloc, pure encoding-width loss.
- `double_deref_test +4`: two SUBs deleted (win) but downstream load_cse/RA interplay adds
  one wide indexed load + pool entry.
- `mibench_rijndael +12`: `phi ADD #imm` CSE materializes addresses that folded into
  store addressing modes, partially offset by enabled store-load forwards. A single-use-deref
  guard on `phi ADD/SUB #imm` was tried and REVERTED: it blocked rijndael's profitable
  round-key address CSEs → +128.
- `z_int_24769 +4`: branch/pool-shift noise from a marginal CSE, not chased.

## Failed approaches (do not retry)
- v1 "cheap `phi±imm` cross-block + live-range-extension" guard: fixed nothing (net −208);
  the harmful CSEs were the backwards-def class, not forward range extension.
- Unconditional (non-phi) single-use shift guard: perturbs reference behavior, new
  regressions (244 +20, 94 +4, binary_search +4).
- `phi ADD/SUB #imm` deref-consumer guard: rijndael +128 (see above).

## Diagnosis method that worked
Temp env gate (`GVN_PHI_STRICT`) re-enabling the old exclusions in one binary, then
`-dump-ir` A/B diff per file. Note: when final flat IR looks identical, diff the
`make CFLAGS+='-DTCC_LOG_LS=1'` regalloc logs — the 182 culprit was only visible as a
live-range diff (`T119 [227,228]` vs `[0,228]`).

## Then
Tackle `globalsym_cse` (still 1 call in `tccgen.c`, 11-file win): CSE of inline SYMREF
address materialization (repeated `ldr rN,[pc,#off]`). Not a gvn shape — needs a new
`ssa:symaddr_cse` rematerialization rule.

## Rules
`make test-ir` (not `make test`). Don't rebuild tcc while reducers run.
Tracker: `docs/plan_legacy_flat_ir_ssa_retire.md`.
