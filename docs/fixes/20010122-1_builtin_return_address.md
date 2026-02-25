# Fix: `__builtin_return_address` / `__builtin_frame_address` Broken on ARM Thumb-2

**Test case**: `gcc.c-torture/execute/20010122-1.c`
**Symptom**: Exit code 1 (abort) with `-O0 -g`.

## Test Case Summary

The test validates that `__builtin_return_address(0)` returns a consistent value
regardless of surrounding code (calls to `dummy()` before/after), and that
`__builtin_return_address(1)` correctly walks one frame up.

```c
void NOINLINE *test1 (void) {
  return __builtin_return_address(0);  // leaf — no other calls
}
void NOINLINE *test2 (void) {
  dummy();
  return __builtin_return_address(0);  // call before
}
void NOINLINE *test3 (void) {
  void *t = __builtin_return_address(0);
  dummy();
  return t;                            // call after
}
// test4a–test6a: __builtin_return_address(1) from nested call via alloca
// main checks: test1() == test2() == test3()  → abort if not
```

## Root Cause

Three interrelated bugs in how `__builtin_return_address` is implemented.

### Bug 1: Hardcoded offset `2 * PTR_SIZE` doesn't match frame layout

`tccgen.c:7164-7176` adds `2 * PTR_SIZE = 8` to the frame pointer to locate the
saved LR. This generates IR `StackLoc[8] [LOAD]`, meaning "load from FP + 8."

But the actual prologue (`arm-thumb-gen.c:5881-5898`) does a single push of all
registers then `mov r7, sp`, placing FP at the bottom of the push area. ARM push
stores registers in ascending register-number order, so for
`push {r4, r5, r7, r12, lr}`:

```
[FP + 16]  = lr  (r14)        ← return address
[FP + 12]  = r12 (alignment pad)
[FP + 8]   = r7  (old FP)
[FP + 4]   = r5
[FP + 0]   = r4               ← FP points here
```

The offset from FP to LR = `offset_to_args - 4`, which varies per function.
The hardcoded `8` is almost never correct.

### Bug 2: Leaf functions don't save LR to stack

`arm-thumb-gen.c:5811`: LR is only pushed for non-leaf functions. `test1` is a
leaf → LR never pushed → `StackLoc[8]` reads garbage → `test1() != test2()` →
abort.

### Bug 3: Frame chain walk broken for level >= 1

For level >= 1, the code dereferences FP (`*FP`) expecting old FP. But since
FP = bottom of push area, `[FP + 0]` = lowest-numbered pushed register (e.g.
r4), NOT the saved old FP. Frame walking is impossible.

## Fix: Standard Thumb Frame Record via Two-Phase Push

Restructure the prologue so FP always points to a standard `{old_FP, LR}` frame
record, matching GCC's ARM Thumb convention. This fixes all three bugs.

### New stack layout

```
Higher addresses
─────────────────────────────────
  caller's stack args       FP + 8 + N
─────────────────────────────────
  saved LR                  FP + 4     ← __builtin_return_address(0)
  saved r7 (old FP)         FP + 0     ← *FP = parent frame pointer
═══════════════ FP (r7) ══════════
  callee-saved r11          FP - 4     ┐
  callee-saved r5           FP - 8     │ callee_push_size bytes
  callee-saved r4           FP - 12    ┘
─────────────────────────────────
  locals / spills            FP - callee_push_size - 4 ...
─────────────────────────────────
                             SP
Lower addresses
```

Key invariants:
- `[FP + 0]` = saved old FP (always)
- `[FP + 4]` = saved LR (always)
- `offset_to_args = 8` (always — the frame record `{r7, lr}` is exactly 8 bytes)
- Local/spill at IR offset `X` → physical address `FP + X - callee_push_size`

### Step 1: Add `force_lr_save` flag

**File: `tcc.h` (line ~1116)**

Add a new flag next to `force_frame_pointer`:

```c
uint8_t force_frame_pointer; /* required for VLA/dynamic SP even if omit_frame_pointer */
uint8_t force_lr_save;       /* __builtin_return_address needs LR saved even in leaf */
```

**File: `tccgen.c` (line ~11413)**

Reset the flag at function start, alongside `force_frame_pointer`:

```c
tcc_state->force_frame_pointer = 0;
tcc_state->need_frame_pointer = 0;
tcc_state->force_lr_save = 0;
```

### Step 2: Set flags in `__builtin_return_address` handler

**File: `tccgen.c` (line ~7143)**

At the start of the `TOK_builtin_frame_address` / `TOK_builtin_return_address`
case, force both frame pointer and LR save:

```c
case TOK_builtin_frame_address:
case TOK_builtin_return_address:
{
    int tok1 = tok;
    tcc_state->force_frame_pointer = 1;
    if (tok1 == TOK_builtin_return_address)
        tcc_state->force_lr_save = 1;
    // ... rest of handler
```

This ensures:
- The function gets a frame pointer (standard two-push layout)
- LR is pushed even if the function is a leaf

### Step 3: Fix offset from `2 * PTR_SIZE` to `PTR_SIZE`

**File: `tccgen.c` (line ~7168)**

```c
// BEFORE:
#ifdef TCC_TARGET_ARM
      vpushi(2 * PTR_SIZE);
// AFTER:
#ifdef TCC_TARGET_ARM
      vpushi(PTR_SIZE);
```

Because `[FP + 4] = LR` in the new layout (was `[FP + 8]` assumption before).

### Step 4: Restructure prologue

**File: `arm-thumb-gen.c`, function `tcc_gen_machine_prolog` (line ~5794)**

Add a new global to track the callee-saved push size:

```c
int callee_push_size = 0;         /* bytes pushed BELOW FP (callee-saved regs) */
uint32_t callee_saved_regs = 0;   /* register mask for second push */
```

In `tcc_gen_machine_prolog`, replace the current single-push logic:

```c
// ── Phase 1: Determine which registers need saving ──
uint16_t frame_regs = 0;      // {r7, lr} — the frame record
uint16_t callee_regs = 0;     // everything else (r4-r6, r8-r11)
int callee_count = 0;

// Frame record: always r7; lr if non-leaf or force_lr_save
frame_regs = (1 << R_FP);
if (!leaffunc || tcc_state->force_lr_save) {
    frame_regs |= (1 << R_LR);
}

// Callee-saved: r4-r11 as determined by used_registers
for (int i = R4; i <= R11; ++i) {
    if (tcc_state->text_and_data_separation && i == R9) continue;
    if (i == R_FP) continue;  // r7 is in frame_regs
    if (used_registers & (1ULL << i)) {
        callee_regs |= (1 << i);
        callee_count++;
    }
}
// Add R10 for nested function static chain if needed
if (extra_prologue_regs & (1u << ARM_R10)) {
    if (!(callee_regs & (1u << ARM_R10))) {
        callee_regs |= (1u << ARM_R10);
        callee_count++;
    }
}
// Pad callee-saved to even count for 8-byte alignment
if (callee_count % 2 != 0) {
    callee_regs |= (1 << R12);
    callee_count++;
}

// ── Phase 2: need_frame_pointer decision ──
// (same as current logic but also force when force_lr_save is set)
if (func_var || tcc_state->force_lr_save)
    tcc_state->need_frame_pointer = 1;
const int need_fp = (tcc_state->force_frame_pointer
                     || tcc_state->need_frame_pointer
                     || (stack_size > 0));
tcc_state->need_frame_pointer = need_fp;

// ── Phase 3: Emit pushes ──
if (need_fp) {
    // ── Two-phase push ──
    // Phase A: frame record
    ot_check(th_push(frame_regs));
    ot_check(th_mov_reg(R_FP, R_SP, ...));  // mov r7, sp
    // Phase B: callee-saved (below FP)
    if (callee_count > 0)
        ot_check(th_push(callee_regs));

    callee_push_size = callee_count * 4;
    callee_saved_regs = callee_regs;

    // offset_to_args: distance from FP to caller's stack args
    // With standard frame record: always 8 (the {r7, lr} pair)
    offset_to_args = 8;

    pushed_registers = frame_regs | callee_regs;  // for dry-run tracking
} else {
    // ── No frame pointer: single push of callee-saved + LR ──
    // (same as current behavior for trivial functions)
    uint16_t regs = callee_regs;
    int count = callee_count;
    if (!leaffunc || tcc_state->force_lr_save) {
        regs |= (1 << R_LR);
        count++;
    }
    if (count % 2 != 0) { regs |= (1 << R12); count++; }
    if (count > 0) ot_check(th_push(regs));
    callee_push_size = 0;
    callee_saved_regs = 0;
    offset_to_args = count * 4;
    pushed_registers = regs;
}

// ── Phase 4: Allocate locals ──
if (stack_size & 7) stack_size = (stack_size + 7) & ~7;
allocated_stack_size = stack_size;
if (stack_size > 0) gadd_sp(-stack_size);
```

**Important**: The `extra_prologue_regs & (1u << R_LR)` check (line ~5818) for
dry-run LR discovery also needs updating. When need_fp = 1, LR is always in
`frame_regs`, so the dry-run can only add it to the non-FP case.

### Step 5: Restructure epilogue

**File: `arm-thumb-gen.c`, function `tcc_gen_machine_epilog` (line ~6190)**

Replace the current single-pop epilogue:

```c
ST_FUNC void tcc_gen_machine_epilog(int leaffunc)
{
    int lr_saved = pushed_registers & (1 << R_LR);

    if (tcc_state->need_frame_pointer) {
        // ── Two-phase pop (mirrors two-phase push) ──

        if (callee_push_size > 0) {
            // SP = FP - callee_push_size (point to callee-saved area)
            // Works correctly even with alloca/VLA since FP is stable
            ot_check(th_sub_imm(R_SP, R_FP, callee_push_size, ...));
            // Restore callee-saved registers
            ot_check(th_pop(callee_saved_regs));
            // SP now = FP (pointing at frame record)
        } else {
            // No callee-saved: just restore SP from FP
            ot_check(th_mov_reg(R_SP, R_FP, ...));
        }

        if (lr_saved) {
            // Pop frame record: restore old FP into r7, return via PC
            ot_check(th_pop((1 << R_FP) | (1 << R_PC)));
        } else {
            // Leaf function with frame pointer but no LR saved
            ot_check(th_pop(1 << R_FP));
            ot_check(th_bx_reg(R_LR));
        }
    } else {
        // ── No frame pointer: existing behavior ──
        if (allocated_stack_size > 0)
            gadd_sp(allocated_stack_size);
        if (lr_saved) {
            pushed_registers |= (1 << R_PC);
            pushed_registers &= ~(1 << R_LR);
            ot_check(th_pop(pushed_registers));
        } else {
            if (pushed_registers > 0) ot_check(th_pop(pushed_registers));
            ot_check(th_bx_reg(R_LR));
        }
    }

    // Common cleanup
    thumb_gen_state.generating_function = 0;
    th_literal_pool_generate();
    thumb_free_call_sites();
}
```

### Step 6: Adjust FP-relative local/spill offsets

With callee-saved registers pushed below FP, all FP-relative local accesses
must account for the gap. A local at IR offset `-4` is now physically at
`FP - callee_push_size - 4`.

**Approach**: Create a helper and apply it at every FP-relative local access
point. Do NOT adjust param accesses (those are above FP and already correct).

```c
// New helper in arm-thumb-gen.c:
static inline int fp_adjust_local_offset(int frame_offset, int is_param)
{
    // Params are above FP (positive direction), no adjustment needed
    // Locals/spills are below FP and must skip past callee-saved area
    if (!is_param && tcc_state->need_frame_pointer)
        return frame_offset - callee_push_size;
    return frame_offset;
}
```

**Apply at these locations** (all in `arm-thumb-gen.c`):

1. **`tcc_machine_load_spill_slot`** (line ~2104): spill slots are always locals
   ```c
   frame_offset = fp_adjust_local_offset(frame_offset, 0);
   ```

2. **`tcc_machine_store_spill_slot`** (line ~2122): same
   ```c
   frame_offset = fp_adjust_local_offset(frame_offset, 0);
   ```

3. **`tcc_machine_addr_of_stack_slot`** (line ~2852): has `is_param` flag
   ```c
   frame_offset = fp_adjust_local_offset(frame_offset, is_param);
   ```

4. **`tcc_machine_can_encode_stack_offset_for_reg`** (line ~2080): used for
   encoding checks — apply adjustment before the check

5. **`tcc_machine_can_encode_stack_offset_with_param_adj`** (line ~2094):
   applies offset_to_args for params, also needs local adjustment

6. **IROP_TAG_STACKOFF handling** in the main codegen (line ~3244):
   ```c
   int frame_offset = irop_get_stack_offset(src);
   // Apply callee-saved gap for locals
   if (!src.is_param)
       frame_offset = fp_adjust_local_offset(frame_offset, 0);
   // Then apply offset_to_args for params (existing code)
   if (src.is_param && frame_offset >= 0)
       frame_offset += offset_to_args;
   ```

7. **LEA operations** (line ~6450+): same pattern as IROP_TAG_STACKOFF

8. **FP offset cache** (`get_cached_stack_addr_reg`, line ~4551): cache keys
   must use adjusted offsets. Adjust before lookup:
   ```c
   if (!op.is_param)
       frame_offset = fp_adjust_local_offset(frame_offset, 0);
   if (op.is_param)
       frame_offset += offset_to_args;
   ```

9. **`tcc_machine_store_param_slot`** (line ~2157): already adds offset_to_args,
   no local adjustment needed (it's always for params)

10. **Parameter shuffle in prologue** (line ~5950+): accesses incoming stack
    params at `offset + offset_to_args`. Since offset_to_args is now 8 (not
    total push size), and these params are above the frame record, this is
    correct. No change needed.

### Step 7: Adjust variadic function handling

**File: `arm-thumb-gen.c` (line ~5935)**

Currently saves r0-r3 at `[FP - 16]` to `[FP - 4]`. With callee-saved below
FP, these fixed offsets collide with callee-saved registers.

Two options:

**Option A** (recommended): Reserve the variadic area as part of the callee-saved
region by saving r0-r3 AFTER the callee-saved push, at offsets relative to the
new SP:

```c
// The variadic save area must be below callee-saved registers
// Adjust offsets: old [FP - 16..FP - 4] → new [FP - callee_push_size - 16..FP - callee_push_size - 4]
tcc_gen_machine_store_to_stack(R0, -callee_push_size - 16);
tcc_gen_machine_store_to_stack(R1, -callee_push_size - 12);
tcc_gen_machine_store_to_stack(R2, -callee_push_size - 8);
tcc_gen_machine_store_to_stack(R3, -callee_push_size - 4);
```

The `tcc_gen_machine_store_to_stack` helper stores relative to FP, so these
adjusted offsets place the saves below the callee-saved area.

Similarly, the stack-args pointer at `[FP - 20]` becomes
`[FP - callee_push_size - 20]`, and the named-arg-bytes count at `[FP - 24]`
becomes `[FP - callee_push_size - 24]`.

**Option B**: Include the variadic save area in the IR's stack frame (negative
offsets from `loc`), so it gets the callee_push_size adjustment automatically
via `fp_adjust_local_offset`. This requires the IR to know about variadic layout
at allocation time, which may be complex.

### Step 8: Adjust static chain (nested functions)

**File: `arm-thumb-gen.c` (line ~5912)**

The static chain register (R10) is saved at `[FP - 4]` (CHAIN_SLOT_OFFSET).
With callee-saved below FP, adjust to `[FP - callee_push_size - 4]`.

Search for `CHAIN_SLOT_OFFSET` or `-4` used for the chain slot and update:

```c
// Old:
tcc_gen_machine_store_to_stack(R10, -4);  // chain at [FP - 4]
// New:
tcc_gen_machine_store_to_stack(R10, -callee_push_size - 4);
```

Also update the `resolve_chain_base` function (line ~219) which reads the chain
at `[FP - 4]`:
```c
load_from_base_ir(out_scratch->reg, ..., callee_push_size + 4 /* abs offset */,
                  1 /* sign: negative */, ...);
```

### Step 9: Verify `tcc_gen_machine_store_to_stack` helper

Confirm this helper stores relative to FP (not SP). If it uses the
`need_frame_pointer ? R_FP : R_SP` pattern, it should work as-is since we're
always in the need_fp = 1 case for two-push functions.

### Step 10: Handle dry-run codegen

The two-pass codegen system (dry-run then real emit) discovers additional
register pushes during pass 1. Key concern: the dry-run's `lr_push_count` and
`scratch_regs_pushed` tracking must work with the new push structure.

When the dry-run discovers LR needs saving (e.g. for a scratch push), this info
feeds into `extra_prologue_regs`. In the new layout, LR is always in the frame
record when need_fp = 1, so extra_prologue_regs only affects the no-FP case.

Review `arm-thumb-gen.c:784-798` where `lr_saved_in_prologue` is computed and
update to match the new push structure.

### Step 11: Edge case — `need_frame_pointer = 0`

When `need_fp = 0` (very simple leaf functions, no locals, no spills):
- No two-phase push — use the existing single-push behavior
- `callee_push_size = 0`
- `offset_to_args = count * 4` (number of pushed regs × 4)
- No FP-relative accesses (no locals exist)
- `__builtin_return_address` forces need_fp = 1 (via `force_frame_pointer`)

No changes needed for this case.

## Testing

```bash
# Primary test
cd tests/ir_tests
python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20010122-1.c --cflags="-O0 -g"
python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20010122-1.c --cflags="-O1 -g"

# Full regression suites
make test -j16               # IR tests
make test-asm -j16           # Assembly tests
make test-all                # IR + GCC torture
```

Key regression scenarios to watch:
- Variadic functions (printf, va_list)
- Nested functions with captured variables
- Functions with alloca/VLA
- Functions with many spills (large offset encoding)
- 64-bit operations (paired register spill/reload)
- Functions with no locals (need_fp = 0 path unchanged)

## Risk Assessment

**Medium-high risk.** This changes every function's prologue/epilogue and all
FP-relative offset calculations. The fix is architecturally correct (matches
GCC's Thumb convention), but the large surface area requires thorough testing.

The `fp_adjust_local_offset` approach centralizes the adjustment, minimizing
the chance of missing a location. The key risk is missing an offset adjustment
site in the backend, which would manifest as accessing the wrong stack slot
(likely a callee-saved register value instead of a local variable).
