# Plan: Omit Frame Pointer When Safe

**Goal**: Eliminate unnecessary frame pointer (R7) setup in functions where SP
is statically known, saving 2-3 instructions per function and freeing R7 for
register allocation.

**Current state**: GCC `-O2` omits the frame pointer for `main` in
`hello_inline.txt` (16 instructions), while TCC always emits it (20 instructions).

## Problem

In `arm-thumb-gen.c:6828`, the frame pointer decision is:

```c
const int need_fp = (tcc_state->force_frame_pointer
                  || tcc_state->need_frame_pointer
                  || (stack_size > 0));  // <-- too conservative
```

Any function with locals or spills gets a frame pointer. The `stack_size > 0`
condition exists because **SP moves dynamically** during function calls:

- `func_call_mop` does `gadd_sp(-stack_size)` before each call to reserve
  outgoing stack args, then `gadd_sp(stack_size)` after (lines 8574-8577,
  8644-8648).
- Nested call preservation pushes R0-R3 onto the stack (lines 8566-8569).

When SP moves, SP-relative offsets to locals become invalid. The frame pointer
provides a stable base. Without it, removing `stack_size > 0` causes widespread
test failures.

## Key Insight

The IR already pre-computes the maximum outgoing call argument area:

- `ir->call_outgoing_size` — max bytes needed across all calls (`tccir.h:454`)
- `ir->call_outgoing_base` — frame offset of the reserved area (`tccir.h:453`)
- `ir/codegen.c:1329-1336` reserves this space in the stack frame layout

But the backend ignores this and still does per-call dynamic SP adjustments.

## Implementation Plan

### Phase 1: Use Pre-Reserved Outgoing Area for Stack Args

**Files**: `arm-thumb-gen.c`

1. **Replace `gadd_sp(-stack_size)` with offset-based stores in `func_call_mop`**
   - Currently (line 8574): `gadd_sp(-stack_size)` lowers SP, then
     `store_word_to_stack(reg, stack_offset)` stores relative to the new SP.
   - Change: compute `outgoing_base = ir->call_outgoing_base` (FP-relative
     offset). Store stack args at `[base_reg + outgoing_base + stack_offset]`
     where `base_reg` is FP or SP depending on `need_frame_pointer`.
   - Remove the `gadd_sp(-stack_size)` / `gadd_sp(stack_size)` pair.

2. **Adapt `store_word_to_stack` and `place_stack_arg_*` functions**
   - These currently store at `[SP + offset]` assuming SP was already lowered.
   - Change them to accept a base register + base offset, or pass the outgoing
     base through the `CallGenContext`.

3. **Handle nested call R0-R3 preservation without PUSH/POP**
   - Currently `th_push(arg_regs_push_mask)` / `th_pop(...)` dynamically moves SP.
   - Option A: Reserve slots for R0-R3 preservation in the frame (alongside
     outgoing area). Store/load explicitly instead of push/pop.
   - Option B: Move the nested-call saves to callee-saved spill slots allocated
     during register allocation. (More complex, may not be needed initially.)

### Phase 2: Remove `stack_size > 0` from Frame Pointer Decision

**Files**: `arm-thumb-gen.c`

4. **Update the `need_fp` condition** (line 6828):
   ```c
   const int need_fp = (tcc_state->force_frame_pointer
                     || tcc_state->need_frame_pointer);
   ```
   The remaining conditions (`force_frame_pointer`, variadic, `force_lr_save`)
   already cover the cases that truly need FP.

5. **Verify `fp_adjust_local_offset`** (line 192):
   - This adjusts local offsets by `callee_push_size` for FP-relative access.
   - When FP is omitted, locals are SP-relative. The offset calculation changes:
     SP points at the bottom of the frame (below outgoing area), so local offset
     from SP = `stack_size + local_offset` (where `local_offset` is negative
     from frame top).
   - Verify that all ~15 sites using `tcc_state->need_frame_pointer ? R_FP : R_SP`
     compute the correct offset in the SP case.

### Phase 3: Account for Outgoing Area in SP-Relative Offsets

6. **When `need_fp == 0` and `call_outgoing_size > 0`**:
   - SP is at `frame_bottom - call_outgoing_size` after prologue.
   - All SP-relative local accesses need an additional
     `+ call_outgoing_size` offset.
   - This adjustment should happen in `fp_adjust_local_offset` or at each
     `base_reg` selection site.

### Phase 4: Prologue/Epilogue Updates

7. **Prologue** (around line 6894):
   - When `need_fp == 0`: skip `MOV R7, SP` and R7 push.
   - Still emit `SUB SP, #stack_size` for locals + outgoing area.

8. **Epilogue** (around line 7298):
   - When `need_fp == 0`: skip `MOV SP, R7` restore.
   - Use `ADD SP, #stack_size` instead.

## Risks and Edge Cases

- **VLA / `alloca`**: Already covered by `force_frame_pointer = 1` in `tccgen.c`.
- **Variadic functions**: Already force FP via `func_var` check (line 6821).
- **`__builtin_return_address`**: Already forces FP via `force_lr_save` (line 6825).
- **Debug info (DWARF)**: `tccdbg.c:2969` checks `need_frame_pointer` for CFA
  tracking. Needs testing — CFA may need to switch to SP-based when FP is omitted.
- **Nested functions / static chain**: Use R10 for chain, may reference FP for
  parent frame access. Check `tcc_gen_machine_set_chain`.
- **Scratch register saves**: `get_scratch_reg_with_save` does PUSH/POP of
  scratch registers mid-function. These also move SP. If these happen while
  accessing locals, SP offsets break. Need to verify these never overlap with
  local accesses, or track their adjustment.
- **Software FP library calls**: Lines 6025-6332 do `sub sp` for softfloat call
  frames. These are internal helpers and may need the same treatment.

## Testing Strategy

1. `make test -j16` — IR test suite (primary)
4. Manual inspection of `hello_inline.txt` output to verify FP is omitted
5. Compare instruction counts before/after across the full test suite

## TODO

### Phase 1: Use Pre-Reserved Outgoing Area
- [ ] Add `outgoing_base` field to `CallGenContext` sourced from `ir->call_outgoing_base`
- [ ] Change `place_stack_arg_32bit` / `place_stack_arg_64bit` / `place_stack_arg_struct` to store at `[base_reg + outgoing_base + stack_offset]` instead of `[SP + stack_offset]`
- [ ] Remove `gadd_sp(-stack_size)` / `gadd_sp(stack_size)` from `func_call_mop`
- [ ] Replace R0-R3 nested call `th_push`/`th_pop` with explicit STR/LDR to reserved frame slots
- [ ] Remove `used_stack_size` tracking (no longer needed)
- [ ] Adapt softfloat helper call frames (lines 6025-6332) to use reserved area

### Phase 2: Remove `stack_size > 0` Condition
- [ ] Change `need_fp` condition at line 6828 to `(force_frame_pointer || need_frame_pointer)`
- [ ] Verify all `force_frame_pointer = 1` sites in `tccgen.c` cover VLA/alloca/varargs

### Phase 3: Fix SP-Relative Offsets
- [ ] Update `fp_adjust_local_offset` to add `call_outgoing_size` when FP is omitted
- [ ] Audit all ~15 `need_frame_pointer ? R_FP : R_SP` sites for correct offset math
- [ ] Handle `MACH_OP_PARAM_STACK` offset calculation (incoming args above frame)

### Phase 4: Prologue/Epilogue
- [ ] Skip R7 push/pop and `MOV R7, SP` / `MOV SP, R7` when `need_fp == 0`
- [ ] Use `ADD SP, #stack_size` in epilogue instead of `MOV SP, R7`
- [ ] Update DWARF CFA tracking in `tccdbg.c` for SP-based frames

### Phase 5: Edge Cases
- [ ] Audit `get_scratch_reg_with_save` PUSH/POP — verify no local access overlap
- [ ] Test nested functions / static chain with FP omitted
- [ ] Verify R9 (GOT base) save/restore in yasos text-data-separation mode

### Phase 6: Testing
- [ ] `make test -j16` — IR tests pass
- [ ] `make test-asm -j16` — assembly tests pass
- [ ] `make test-gcc-torture-compile` — GCC torture tests pass
- [ ] Verify `hello_inline.txt` shows FP omitted for `main`
- [ ] Compare instruction count regressions across test suite

## Expected Impact

- Saves 2-4 instructions per non-leaf function (push/pop R7 + MOV R7,SP + MOV SP,R7)
- Frees R7 for general register allocation (significant for register pressure)
- Closer parity with GCC `-O2` output
