# Plan Review: Materialization Refactor

> **Note (2026-03-06):** Much of this review describes findings made *before* implementation started. Several items are now moot:
> - `ir/mat.c` (1096 lines) — **deleted** (Phase 4 ✅)
> - `ir/operand.h` + `ir/operand.c` — **deleted** (Phase 4 ✅)
> - SValue materialization path — **deleted** (Phase 0 ✅)
> - `tcc_ir_codegen_generate()` at 2331 lines — now **1767 lines** after Phase 6 consolidated dispatch loops
> - Dry-run constraint collection — **implemented** as `dry_insn_scratch[]`/`dry_insn_saves[]` arrays (Phase 3 ✅)
> - Dispatch loop consolidation — **done** (Phase 6 ✅): single `for (pass=0; pass<2)` loop; −339 lines (~16%)
> - All backend handlers now use `_mop` variants exclusively (Phase 5o ✅)
> - `pr0_reg`/`pr1_reg` fields removed from `IROperand` (Phase 5p ✅): struct shrunk from 10→9 bytes; `irop_phys_r0()`/`irop_phys_r1()` helpers read interval table
> - All legacy `_ir` wrapper functions deleted (Phase 5q ✅): `load_to_dest_ir`, `store_ex_ir`, `store_ir`, `th_store_resolve_base_ir`, `irop_phys_r0`/`irop_phys_r1`; `tcc_gen_mach_load_to_reg` rewritten for direct-dest loading

Review of `plan.md` against the actual codebase state (original analysis). Based on reading `ir/codegen.c` (1767 lines), `arm-thumb-gen.c` (8055 lines), `tccir_operand.h` (560 lines), `tccir_operand.c` (844 lines), `ir/machine_op.c` (328 lines), `svalue.h`, and `ir/stack.h`. *(Note: `ir/mat.c`, `ir/operand.h` deleted in Phase 4.)*

---

## Key Finding 1: The Plan's "Current Pattern" Pseudocode Is Inaccurate

**Plan says** the backend (`arm-thumb-gen.c`) calls `tcc_ir_materialize_value_ir()` etc. directly.

**Reality:** `arm-thumb-gen.c` does **NOT** call any `tcc_ir_materialize_*` or `tcc_ir_mat_*` APIs. Zero calls. The materialization happens in `ir/codegen.c`'s dispatch loop *before* calling into the backend. The backend receives already-filled `IROperand` values and then does its **own** scratch+load pattern via `get_scratch_reg_with_save()` (66 calls) and `load_to_reg_ir()` (63 calls).

**Impact on plan:** The architecture is worse than described — there are **two independent materialization layers** running in series, not one. The plan's proposed change is still the right fix, but the migration path is different:
- We're not replacing materialize calls *in the backend* — we're removing the `ir/codegen.c` materialize layer and making the backend's existing load pattern the sole path.
- The `mach_*` helpers are essentially a clean API over what `arm-thumb-gen.c` already does informally.

**Action taken:** Phase 2 step file corrected to reflect actual architecture.

---

## Key Finding 2: Dry Run Already Exists

**Plan says** Phase 3 introduces a dry-run pass — "Run the backend twice."

**Reality:** `ir/codegen.c::tcc_ir_codegen_generate()` already runs a dry run followed by a real run. It calls `tcc_gen_machine_dry_run_begin()`, runs the full dispatch loop, calls `tcc_gen_machine_dry_run_end()`, analyzes branch offsets, then re-runs for real emission.

**Impact on plan:** Phase 3 is not "add a dry run" — it's "extend the existing dry run with constraint collection." This is a smaller, less risky change than described.

**Action taken:** Phase 3 step file corrected to frame this as an extension, not a new feature.

---

## Key Finding 3: Three Parallel APIs in `ir/mat.c`

**Plan mentions** two parallel paths (SValue and IROperand).

**Reality:** There are **three** layers:
1. Legacy SValue API: `tcc_ir_materialize_value()`, `_const_to_reg()`, `_addr()`, `_dest()`
2. IROperand API: `tcc_ir_materialize_value_ir()`, `_const_to_reg_ir()`, `_addr_ir()`, `_dest_ir()`
3. New wrapper API: `tcc_ir_mat_value()`, `_const()`, `_addr()`, `_dest()` (with `TCCMatValue`/`TCCMatAddr`/`TCCMatDest` types)

Layer 3 wraps layer 1. The active codegen path uses layer 2.

**Impact on plan:** Phase 0 (SValue elimination) should delete layers 1 and 3 (both SValue-based). Layer 2 is the one that stays until Phase 4.

---

## Key Finding 4: Duplicate Operand Headers

**Not mentioned in the original plan.**

`tccir_operand.h` (567 lines) and `ir/operand.h` (539 lines) are near-duplicate headers with divergent position field widths (17-bit vs 18-bit). This is a maintenance hazard — a fix applied to one may not be applied to the other.

**Impact on plan:** Added to Phase 5 as a cleanup step. Should arguably be fixed earlier to prevent bugs during the refactor.

---

## Key Finding 5: `ir/codegen.c` Has Multiple Dispatch Paths

The file contains **4 occurrences** of `case TCCIR_OP_ADD:`, suggesting multiple switch statements. Investigation shows:

1. **Lines ~1335–1435:** Operand need classification (sets `need_src1_value`, etc.)
2. **Lines ~1530–1610:** Main dispatch to backend `tcc_gen_machine_*_op()` functions
3. **Lines ~1820+:** Possibly a 64-bit or alternative dispatch path
4. **Lines ~1960+:** Possibly a legacy SValue dispatch path

This complexity is exactly what the refactor aims to eliminate. However, migrating requires understanding all 4 paths and ensuring none are silently active.

**Recommendation:** Before Phase 2, audit which paths execute under which conditions. Mark dead paths for removal. This could be a sub-step of Phase 0.

---

## Overall Assessment

| Aspect | Rating | Notes |
|---|---|---|
| **Problem diagnosis** | Accurate | The dual-materialization problem is real and well-identified |
| **Proposed solution** | Sound | MachineOperand + backend-driven materialization is the right approach |
| **Architecture understanding** | Partially inaccurate | Backend doesn't call mat APIs; dry run already exists |
| **Phase ordering** | Good | Dependencies are correct: 0→1→2→3→4→5 |
| **Risk assessment** | Understated | Duplicate operand headers and multiple dispatch paths add risk |
| **Estimated effort** | Reasonable | Phase 2 (convert ~14 instruction handlers) is the largest effort |

### Recommendations

1. **Phase 0 should include an audit of all 4 dispatch paths** in `ir/codegen.c` to determine which are active and which are dead.

2. **Consolidate operand headers early** (could be Phase 0.5) to prevent bugs during refactor where the wrong header is edited.

3. **Phase 2 conversion order should match instruction frequency** in the test suite. Convert the most-exercised handlers first to get maximum test coverage early.

4. **Add a "parallel validation" step** in Phase 1 where both old and new paths run and results are compared with assertions. This was added to the Phase 1 step file.

5. **Consider whether `machine_op_from_ir()` should read directly from the allocator** rather than from the filled `IROperand` flags. This would bypass `tcc_ir_fill_registers_ir()` entirely, making Phase 1 independent of the fill logic and reducing the risk of flag-encoding bugs.
