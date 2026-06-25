# Register Allocator Improvement Opportunities

## Current State (25 vs 19 instructions for bench_array_sum)

The remaining 6-instruction gap is entirely register allocation and stack layout quality:

| Gap | TCC | GCC | Root Cause |
|---|---|---|---|
| 2 instr | `push/pop {r4}` | no callee-save | r4 used for inner loop temp; r12 not available |
| 2 instr | `add r3,sp,#8; add.w r3,#1024` | `add r1,sp,#1020` | End pointer computed in 2 instructions |
| 1 instr | `mov r0, r1` | sum already in r0 | Return value not in r0 |
| 1 instr | `subw sp,#1036` (wide) | `sub.w sp,#1024` | 12 extra bytes frame padding |

---

## 1. R12 (IP) for Allocation

### Goal
Add r12 to the allocator pool as a caller-saved register. This gives 5 caller-saved registers (r0-r3, r12) instead of 4, eliminating callee-save push/pop when register pressure is 5.

### Current Blocker
~30 places in `arm-thumb-gen.c` hardcode `R_IP`/`R12`/`ARM_R12` without going through the scratch allocator. These would clobber any value the allocator placed in r12.

### Hardcoded R12 uses that need conversion to scratch allocator:

**Stack manipulation (prologue/epilogue):**
- `arm-thumb-gen.c:3116-3117` — `MOV R_IP, R_SP` for dynamic stack alloc
- `arm-thumb-gen.c:3131-3132` — Load via R_IP for stack restore
- `arm-thumb-gen.c:7881-7892` — Argument area setup uses R12 directly
- `arm-thumb-gen.c:7910-7912` — Vararg store uses R_IP

**Struct handling:**
- `arm-thumb-gen.c:8577-8590` — `get_struct_base_addr_mop` defaults to ARM_R12
- `arm-thumb-gen.c:9035` — Same pattern in store path
- `arm-thumb-gen.c:9106` — Returns R_IP as fallback

**Direct scratch use:**
- `arm-thumb-gen.c:8100` — `int temp = R_IP` for parameter copy
- `arm-thumb-gen.c:9654-9655` — Stack load uses ARM_R12 for offset

**PIC/GOT/text-data separation:**
- `arm-thumb-gen.c:6721,7298,7376` — POP uses R12 for GOT reload

### Required changes:
1. Convert each hardcoded R12 use to call `get_scratch_reg_with_save()` instead
2. Ensure each converted site properly saves/restores if r12 is live
3. Add r12 to `caller_saved_registers` bitmap
4. Change `registers_for_allocator = 13`
5. Cap `tcc_ls_assign_callee_saved_register` to r4-r11 (exclude r12)
6. Update `tcc_ls_assign_any_register` allocation order: r0-r3, r12, r4-r11

### Risk
High — each hardcoded site needs careful analysis of what registers are excluded and whether the scratch save/restore interacts with the surrounding code correctly.

---

## 2. Return Value Precolor Priority (Eviction)

### Goal
When the allocator processes a precolored interval (e.g., return value hinted to r0) and the preferred register is already taken by an uncolored interval, evict the uncolored interval to a different register.

### Current Blocker
Linear scan processes intervals in start-point order. The return value vreg (V0, start=10) is processed AFTER the loop counter (V3, start=9). V3 gets r0 first. When V0 tries r0, it's taken and falls back to r1. Result: `mov r0, r1` at return.

### Failed Approach: Retroactive Eviction
Attempted: when precolored V0 can't get r0, find V3 in the active set, release r0, and reassign V3 to a different register.

**Why it fails:** Retroactive reassignment changes the register for V3's ENTIRE interval. If another interval (V2) was assigned r1 during [7,12] while V3 was in r0 during [9,21], moving V3 to r1 creates an overlap [9,12] where both V3 and V2 are in r1. This produces incorrect codegen.

### Correct Approaches (not yet implemented):

**A. Interval Splitting:**
Split the conflicting interval at the eviction point. V3 stays in r0 for [9, eviction_point], then moves to r1 for [eviction_point, 21]. Requires inserting a MOV at the split point and managing two sub-intervals.

**B. Priority-Based Sorting:**
Sort intervals so precolored ones are processed first among those with the same start point. Doesn't help when start points differ (V3=9 vs V0=10).

**C. Second-Chance Allocation:**
After all intervals are processed, scan for precolored intervals that didn't get their preferred register. Try to swap with the conflicting interval if safe (no overlap with other intervals in the new register).

**D. Graph Coloring:**
Replace linear scan with a graph-coloring allocator that handles preferences natively. Significant complexity increase.

### Recommendation
Approach C (second-chance) is safest and simplest. After the main allocation loop, for each precolored interval that missed its hint:
1. Find the interval currently holding the desired register
2. Check if the desired register is free for the blocker's entire range (scan all intervals)
3. If safe, swap registers
4. If not safe, leave as-is

---

## 3. Loop Bound Rematerialization Without Calls

### Goal
The inner sum loop computes `end = SP+8+1024` in 2 instructions and keeps it in r3 for the entire loop. If rematerialized inside the loop (1 instruction per iteration), r3 is freed for the loaded value, avoiding r4 (callee-save).

### Current State
`tcc_ir_opt_loop_bound_remat` only fires for loops containing function calls. The inner sum loop has no calls, so it's skipped.

### Required Change
Relax the `has_calls` guard to also allow remat when register pressure exceeds caller-saved capacity (>4 simultaneous live values). Requires estimating live count at the IR level before register allocation.

### Trade-off
Adds 1 instruction per inner loop iteration (the remat ADD) but saves 2 instructions total (push/pop r4). Net benefit depends on loop trip count — beneficial for loops with many iterations.
