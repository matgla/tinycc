# ARM Thumb `PREG_SPILLED` Cleanup Plan

## Objective
- Finish enforcing the "IR owns spills" contract by ensuring ARM Thumb codegen never sees or emits `PREG_SPILLED` sentinels.
- Turn remaining defensive checks into ordinary register allocation logic (or delete them) once callers always materialize real registers.
- Replace documentation that still mentions backend-side spill handling.

## Working Approach
1. Triage every `PREG_SPILLED` reference below and decide whether it should become:
   - a hard error (if seeing a spill would indicate a new IR bug),
   - an explicit materialization request (calling existing helpers), or
   - dead code that can be deleted because IR already enforces the invariant.
2. Remove code that quietly treats `PREG_SPILLED` as memory operands; keep/upgrade the few useful runtime assertions.
3. After each removal, rerun the Thumb QEMU regression plus targeted IR tests (64-bit ops, hard-float, varargs, VLA) to ensure behaviour stays intact.

## Occurrence Inventory
Each unchecked box represents at least one direct `PREG_SPILLED` touch point. Close every box (retaining a final assertion if needed) before declaring the cleanup complete.

1. - [x] **Lvalue store base selection** — [arm-thumb-gen.c#L1675-L1712](arm-thumb-gen.c#L1675-L1712)
      - `store()` now relies on `thumb_require_materialized_reg()` for address bases instead of hand-checking `PREG_SPILLED`.
2. - [x] **`load_to_dest()` pointer/reg guards** — [arm-thumb-gen.c#L2490-L2605](arm-thumb-gen.c#L2490-L2605)
      - Removed bespoke spill diagnostics and routed all pointer/source checks through the shared helper so backend only sees materialized registers.
3. - [x] **Register-requirement helpers** — [arm-thumb-gen.c#L2829-L2858](arm-thumb-gen.c#L2829-L2858)
      - Tightened `thumb_require_materialized_reg/pair` and `thumb_ensure_not_spilled` so they now treat any non-hardware register (including lingering `PREG_SPILLED` bits) as a compiler error, and added `thumb_exclude_mask_for_regs()` to simplify scratch exclusions.
4. - [ ] **Documentation comment** — [arm-thumb-gen.c#L3080-L3090](arm-thumb-gen.c#L3080-L3090)
     - The note still references backend-side spill loading. Update wording once the remaining sites are gone.
5. - [ ] **64-bit ADD/SUB helper guards** — [arm-thumb-gen.c#L3148-L3185](arm-thumb-gen.c#L3148-L3185)
     - `thumb_emit_add64/sub64` treat memory destinations and spilled sources specially. Remove the `dest_is_mem` flow or assert that the IR never leaves `pr*` as `PREG_SPILLED`.
6. - [x] **64-bit SHL immediate path** — [arm-thumb-gen.c#L3220-L3298](arm-thumb-gen.c#L3220-L3298)
      - Now enforces register materialization up front, borrows dest-high for implicit zero-extension, and only allocates scratch for carry bits (no more `PREG_SPILLED` fallbacks or memory destinations).
7. - [x] **64-bit SHR immediate path** — [arm-thumb-gen.c#L3301-L3382](arm-thumb-gen.c#L3301-L3382)
      - Mirror of SHL: asserts real registers, zero-extends via dest-high, and removes the spill/memory handling in favor of helper-based assertions.
8. - [x] **64-bit OR (imm + reg cases)** — [arm-thumb-gen.c#L3506-L3750](arm-thumb-gen.c#L3506-L3750)
      - OR paths now demand materialized operands/dest via the shared helpers, rely on scratch masks for immediates, and no longer try to fix spilled regs.
9. - [x] **64-bit AND (imm + reg cases)** — [arm-thumb-gen.c#L3839-L4025](arm-thumb-gen.c#L3839-L4025)
      - Immediate and register flows use the invariant helpers and treat 32-bit halves as zero without any backend-side spill recovery.
10. - [x] **64-bit XOR helper** — [arm-thumb-gen.c#L4264-L4310](arm-thumb-gen.c#L4264-L4310)
       - XOR follows the same pattern (helper assertions + scratch-limited immediates) and deletes all `PREG_SPILLED` checks.
11. - [x] **Generic data-processing fallback** — [arm-thumb-gen.c#L4579-L4665](arm-thumb-gen.c#L4579-L4665)
      - Enforced `thumb_require_materialized_reg()` for dest/src regs, removed memory-destination fallbacks, and added a `TEST_ZERO` guard so any lingering spill now trips an IR bug instead of being silently reloaded.
12. - [x] **Hard-float result write-back** — [arm-thumb-gen.c#L4781-L4835](arm-thumb-gen.c#L4781-L4835)
      - `store_fp_result_from_vfp()` now requires a materialized integer destination register (or true memory lvalue) and no longer treats `PREG_SPILLED` as a stack-backed destination.
13. - [ ] **Scalar store op** — [arm-thumb-gen.c#L5330-L5360](arm-thumb-gen.c#L5330-L5360)
      - `tcc_gen_machine_store_op()` still reloads source registers if `pr0` is `PREG_SPILLED`. Replace with `load_to_dest`-style materialization earlier.
14. - [ ] **Parameter shuffle in prolog** — [arm-thumb-gen.c#L5483-L5530](arm-thumb-gen.c#L5483-L5530)
      - Spilled parameters (allocation `r0 == PREG_SPILLED`) trigger stack stores. Ensure IR encodes stack slots explicitly so prolog no longer inspects the sentinel.
15. - [ ] **64-bit assign/move** — [arm-thumb-gen.c#L5649-L5685](arm-thumb-gen.c#L5649-L5685)
      - `tcc_gen_machine_assign_op()` reloads when either half equals `PREG_SPILLED`. Collapse into the materialize helpers and keep only guardrails.
16. - [ ] **LEA destination scratch path** — [arm-thumb-gen.c#L5806-L5885](arm-thumb-gen.c#L5806-L5885)
      - LEA allocates scratch registers whenever the destination was spilled and also checks for `PREG_SPILLED` during store-back. Replace with IR-managed stack slots.
17. - [x] **`load_to_register` / helper utilities** — [arm-thumb-gen.c#L6075-L6135](arm-thumb-gen.c#L6075-L6135)
      - Simplified to rely on the shared helper for cached-register moves; spilled cases now fall back to `load_to_reg()` without touching `PREG_SPILLED`.
18. - [ ] **Call lowering (stack + register args)** — [arm-thumb-gen.c#L6413-L6685](arm-thumb-gen.c#L6413-L6685)
      - Argument setup still inspects `PREG_SPILLED` for stack writes, lvalue remapping, and documentation comments (e.g., [arm-thumb-gen.c#L6677-L6682](arm-thumb-gen.c#L6677-L6682)). Move spill handling into IR materialization and leave only diagnostics here.
19. - [ ] **Return write-back** — [arm-thumb-gen.c#L6800-L6835](arm-thumb-gen.c#L6800-L6835)
      - Return-value store checks whether the destination virtual register is spilled. Ensure IR either keeps the value in R0/R1 or encodes a true stack lvalue.
20. - [ ] **VLA helpers** — [arm-thumb-gen.c#L6967-L6998](arm-thumb-gen.c#L6967-L6998)
      - VLA alloc/release paths still react to spilled operands when computing the size. Fold this into the IR-side materialization helpers.

## Next Actions
1. Pick one of the high-impact runtime paths (e.g., store/load helpers) and eliminate its `PREG_SPILLED` handling first.
2. After each removal, document the invariant in this file (mark the box) and update any related comments/tests.
3. Keep the QEMU regression plus targeted IR tests in the loop after every cluster of changes. (Last run: all Thumb + IR suites green.)
