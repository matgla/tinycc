# Loop Unrolling Design

## Goal

Unroll small constant-trip-count loops to eliminate branch overhead and enable
further optimizations (constant folding, dead code elimination).

## Motivating Example

```c
const char *str = "hello";
int sum = 0;
for (int i = 0; i < 5; i++) {
    sum += strlen(str);
}
```

After strlen folding, the IR loop body becomes `V1 = V1 + #5` repeated 5 times.
The actual optimized IR before unrolling (from dump_ir.txt):

```
0000: V0 <-- GlobalSym(268435461) [ASSIGN]   ; str = "hello"
0001: V1 <-- #0 [ASSIGN]                      ; sum = 0
0002: V2 <-- #0 [ASSIGN]                      ; i = 0
0003: CMP V2, #5                               ; HEADER: i < 5?
0004: JMP to 14  if ">=S"                      ; EXIT: jump past loop
0005: JMP to 11                                ; jump to body (skip latch on first iter)
0006: T0 <-- V2 [ASSIGN]                       ; LATCH: save old i
0007: V2 <-- T0 ADD #1                         ;        i++
0008: JMP to 3                                 ;        back to header
0009: NOP
0010: NOP                                      ; (folded PARAM — was strlen arg)
0011: NOP                                      ; (folded CALL — strlen folded to #5)
0012: V1 <-- V1 ADD #5                         ; BODY: sum += 5
0013: JMP to 6                                 ; jump to latch
0014: ...                                      ; EXIT TARGET: printf etc.
```

Loop structure detected by `tcc_ir_detect_loops()`:
- Backward jump: instruction 8 (`JMP to 3`) — this is the latch
- `header_idx = 3`, `start_idx = 3`, `end_idx = 8`
- Body extends to 13 via forward jump analysis (instr 5 jumps to 11, instr 13 jumps to 6)
- `preheader_idx = 2` (the `V2 <-- #0` instruction before header)

With full unrolling, this becomes:

```
0001: V1 <-- #0
0012: V1 <-- V1 ADD #5    ; iteration 0
      V1 <-- V1 ADD #5    ; iteration 1
      V1 <-- V1 ADD #5    ; iteration 2
      V1 <-- V1 ADD #5    ; iteration 3
      V1 <-- V1 ADD #5    ; iteration 4
```

And the existing iterative constant propagation (Phase 1) collapses it to `V1 <-- #25`.

## Scope

**Full unrolling only** for loops where:
- Trip count is a compile-time constant
- Trip count <= threshold (16)
- Loop body is small (<= 32 non-NOP instructions)
- No nested loops (single-level only)
- Simple exit condition: `CMP IV, #N` followed by conditional jump
- Total expanded size: `trip_count * body_insn_count <= 128`

Partial unrolling (unroll-by-factor) is out of scope for the initial
implementation.

## Where It Fits in the Pipeline

In `tccgen.c` (around line 23991), between dead store elimination and LICM:

```
Phase 4:   Store-load forwarding, redundant/dead store elimination  (existing, ~line 23963-23990)
Phase 5a:  Loop unrolling                                           (NEW)
Phase 5a': Re-run Phase 1 iterative const prop + DCE               (NEW — collapse unrolled code)
Phase 5:   LICM                                                     (existing, disabled, ~line 23992)
Phase 6:   IV strength reduction                                    (existing, ~line 24008)
```

The key is that loop unrolling runs **after** strlen/constant folding has
simplified the body and **before** IV strength reduction (which would be
confused by an unrolled loop). After unrolling, we re-run the Phase 1 iterative
loop so constant propagation can collapse `0 + 5 + 5 + 5 + 5 + 5 → 25`.

## Data Structures

No new data structures. Reuse existing ones:

| Structure | Defined in | Used for |
|-----------|-----------|----------|
| `IRLoop` | `ir/licm.h:28` | Loop bounds: header_idx, start_idx, end_idx, preheader_idx |
| `IRLoops` | `ir/licm.h:41` | Collection of detected loops |
| `InductionVar` | `ir/opt.c:7991` | IV: vreg, init_val, step, def_idx, init_idx |

## Algorithm — Detailed

### Phase 1: Detect loops and find candidates

```c
int tcc_ir_opt_loop_unroll(TCCIRState *ir)
{
    IRLoops *loops = tcc_ir_detect_loops(ir);
    // Process innermost loops first (highest start_idx)
    // For each loop, call try_unroll_loop()
}
```

For each loop, `try_unroll_loop()` performs these checks:

#### 1a. Find the induction variable

Reuse `find_induction_vars()` (ir/opt.c:8021). This function:
- Scans `[loop->start_idx, loop->end_idx]` for `V = V + const` pattern
- Verifies V has exactly 1 definition inside the loop
- Looks for initialization `V = #const` in preheader (up to 5 instructions back)
- Returns `InductionVar { vreg, init_val, step, def_idx, init_idx }`

**Requirement**: exactly 1 basic IV found (multi-IV loops are too complex).

#### 1b. Find the exit condition

Scan from `loop->header_idx` forward (at most 2 instructions) for:

```
CMP  Viv, #limit
JMP  to exit_target  if COND
```

Where:
- `Viv` is the IV vreg from step 1a
- `#limit` is an immediate constant
- `COND` is one of: `>=S` (for `i < N`), `>S` (for `i <= N`), `==` (for `i != N`)
- `exit_target > loop->end_idx` (jumps past the loop)

Extract: `cmp_idx`, `jmpif_idx`, `exit_target`, `limit`, `cond_token`.

#### 1c. Compute trip count

```c
switch (cond_token) {
    case TOK_GE:  // >=S means loop runs while <
        trip_count = (limit - init_val + step - 1) / step;  // ceiling division
        break;
    case TOK_GT:  // >S means loop runs while <=
        trip_count = (limit - init_val) / step + 1;
        break;
    case TOK_NE:  // != means loop runs until equality
        if ((limit - init_val) % step != 0) return 0;  // infinite loop risk
        trip_count = (limit - init_val) / step;
        break;
}
```

**Bail if**: `trip_count <= 0`, `trip_count > 16`, or `step <= 0`.

#### 1d. Identify the body instructions

The "body" is everything between the exit conditional jump and the back-edge
jump that is NOT:
- The CMP instruction (`cmp_idx`)
- The conditional exit JMP (`jmpif_idx`)
- The IV increment (`iv.def_idx`)
- The back-edge JMP (latch jump to header)
- NOP instructions
- The `T0 <-- V2 [ASSIGN]` preceding the IV increment (save-old-IV pattern)

In the example IR:
```
Body instructions to clone = { 0012: V1 <-- V1 ADD #5 }
```

Count them: `body_insn_count`. **Bail if** `body_insn_count > 32` or
`trip_count * body_insn_count > 128`.

#### 1e. Check no nested loops

Scan body for backward JMP instructions (target < source). If any found,
bail — this is a nested loop.

#### 1f. Check no side effects that prevent unrolling

Scan body for instructions that are problematic:
- `FUNCCALLVAL` / `FUNCCALLVOID` — bail (calls can have side effects)
  - Exception: if we later add pure-function tracking, pure calls are OK
- `INLINE_ASM` — bail
- `SETJMP` / `LONGJMP` — bail

**Note**: `STORE` instructions are fine to unroll — they just happen N times to
different addresses (array writes). `LOAD` too.

### Phase 2: Emit unrolled code

Strategy: **in-place overwrite + `insert_instr_at()` for overflow**.

Since `insert_instr_at()` (ir/opt.c:8284) already exists and correctly updates
all jump targets, we can use it when the unrolled body doesn't fit in the
original loop's instruction slots.

However, to avoid the index-shifting complexity entirely for the common case,
use this two-tier approach:

#### 2a. NOP out the entire loop region

```c
for (int i = loop->start_idx; i <= loop_actual_end; i++)
    ir->compact_instructions[i].op = TCCIR_OP_NOP;
```

Also NOP the IV initialization in the preheader (`iv.init_idx`).

Also NOP the forward-jump into the body (`instr 5: JMP to 11` in our example)
if it's within the loop region.

#### 2b. Compute write positions

Available NOP slots: count NOPs in `[loop->start_idx, loop_actual_end]`.
Needed slots: `trip_count * body_insn_count`.

- If `needed <= available`: write in-place starting at `loop->start_idx`
- If `needed > available`: write what fits in-place, then use `insert_instr_at()`
  to insert remaining instructions at `loop_actual_end + 1`

#### 2c. Clone body instructions for each iteration

For each iteration `k = 0 .. trip_count - 1`:
  For each body instruction `orig`:
  
  1. Copy the instruction: `new.op = orig.op`
  2. Copy operands from the original (read src1, src2, dest from pool)
  3. **Remap operands**:
     - If src1/src2 references the IV vreg → replace with constant
       `#(init_val + k * step)` — but only if the IV is used as a value,
       not being defined
     - If dest is the IV vreg → this is the IV increment, already excluded
     - VAR vregs defined inside the body: for each iteration k > 0,
       allocate fresh TMPs via `tcc_ir_vreg_alloc_temp(ir)` and remap
       all references to them within that iteration's copy
  4. Write to the next available slot using:
     ```c
     ir->compact_instructions[write_pos].op = new_op;
     ir->compact_instructions[write_pos].operand_base = tcc_ir_pool_add(ir, dest);
     tcc_ir_pool_add(ir, src1);
     tcc_ir_pool_add(ir, src2);
     ```
  5. Clear `is_jump_target` on cloned instructions

#### 2d. Patch the entry

The original `JMP to exit if >=S` at `jmpif_idx` was NOPed. We need the
code to flow from the preheader into the first unrolled instruction.

Since we write the unrolled body starting at `loop->start_idx` (which is the
header), the preheader naturally falls through into it. No patching needed —
the NOP'd header is replaced by the first unrolled body instruction.

But we need to handle the `exit_target`: make sure the last unrolled
instruction falls through to `exit_target`. If the unrolled code ends before
`exit_target`, insert `JMP to exit_target` as the final instruction.

#### 2e. Concrete example walkthrough

For our test case (trip_count=5, body=[`V1 <-- V1 ADD #5`]):

Original slots 3–13 (11 slots) get NOPed. We need 5 instructions.

Write at positions 3–7:
```
0003: V1 <-- V1 ADD #5    ; iteration 0
0004: V1 <-- V1 ADD #5    ; iteration 1
0005: V1 <-- V1 ADD #5    ; iteration 2
0006: V1 <-- V1 ADD #5    ; iteration 3
0007: V1 <-- V1 ADD #5    ; iteration 4
0008: NOP                   ; (remaining slots stay NOP)
...
0013: NOP
0014: ...                   ; EXIT TARGET (unchanged)
```

Falls through to 0014 naturally. Phase 1 re-run folds:
```
V1 = 0; V1 = V1+5; V1 = V1+5; ... → V1 = 25
```

### Phase 3: Re-run constant propagation

After unrolling, call the Phase 1 iterative loop again:

```c
if (unrolled_count > 0) {
    int iter2 = 0;
    int ch2;
    do {
        ch2 = 0;
        if (tcc_state->opt_dce) ch2 += tcc_ir_opt_dce(ir);
        if (tcc_state->opt_const_prop) ch2 += tcc_ir_opt_const_prop(ir);
        if (tcc_state->opt_const_prop) ch2 += tcc_ir_opt_const_prop_tmp(ir);
        if (tcc_state->opt_const_prop) ch2 += tcc_ir_opt_branch_folding(ir);
    } while (ch2 > 0 && ++iter2 < 10);
}
```

## File-by-file Implementation Plan

### Step 1: Add flag — `tcc.h` and `libtcc.c`

**tcc.h** (~line 1147, after `opt_iv_strength_red`):
```c
unsigned char opt_loop_unroll;   /* -floop-unroll: full unroll small loops */
```

**libtcc.c** (~line 1724, in flag table after `iv-strength-red`):
```c
{offsetof(TCCState, opt_loop_unroll), 0, "loop-unroll"},
```

**libtcc.c** (~line 2279, in -O1 block):
```c
s->opt_loop_unroll = 1;         /* Full-unroll small constant-trip-count loops */
```

### Step 2: Declare API — `ir/opt.h`

Add declarations (near the other loop optimization declarations):
```c
int tcc_ir_opt_loop_unroll(TCCIRState *ir);
int tcc_ir_opt_loop_unroll_with_loops(TCCIRState *ir, IRLoops *loops);
```

### Step 3: Implement — `ir/opt.c`

Add a new section after the IV strength reduction code (~line 8570).

**Helper: `find_loop_exit_condition()`**
```c
/* Scan from header_idx for: CMP Viv, #limit; JUMPIF exit_target COND
 * Returns 1 if found, fills out_cmp_idx, out_jmpif_idx, out_limit, out_cond,
 * out_exit_target. */
static int find_loop_exit_condition(TCCIRState *ir, IRLoop *loop,
    int iv_vreg,
    int *out_cmp_idx, int *out_jmpif_idx,
    int *out_limit, int *out_cond, int *out_exit_target);
```

Scan instructions `[header_idx, header_idx+3]`:
- Find `CMP` where one operand is `iv_vreg` and the other is immediate
- Find `JUMPIF` immediately after the CMP
- Extract condition token from the JUMPIF
- Extract exit target (must be > loop->end_idx to be an exit)

**Helper: `compute_trip_count()`**
```c
static int compute_trip_count(int init_val, int limit, int step, int cond_token);
```

Handle:
- `>=S` (generated by `i < N`): `trip_count = ceil((limit - init_val) / step)`
  with `ceil(a/b) = (a + b - 1) / b` for positive values
- `>S` (generated by `i <= N`): `trip_count = (limit - init_val) / step + 1`
- Validate: `trip_count >= 0`, `(limit - init_val)` is exact multiple of step
  for `!=` conditions

**Helper: `collect_body_instructions()`**
```c
/* Collect non-control-flow, non-IV body instructions to clone.
 * Returns count, fills body_indices[] array. */
static int collect_body_instructions(TCCIRState *ir, IRLoop *loop,
    int iv_vreg, int cmp_idx, int jmpif_idx, int iv_def_idx,
    int *body_indices, int max_body);
```

Walk `[loop->start_idx, loop_actual_end]`, skip:
- NOP instructions
- CMP at cmp_idx
- JUMPIF at jmpif_idx
- All JMP (unconditional) instructions
- IV increment at iv_def_idx
- ASSIGN that copies IV to a temp (pattern: `T = Viv` where T is only
  used by the IV increment on the next line)

**Main: `try_unroll_loop()`**
```c
static int try_unroll_loop(TCCIRState *ir, IRLoop *loop)
{
    InductionVar ivs[MAX_IV];
    int num_ivs = find_induction_vars(ir, loop, ivs, MAX_IV);
    if (num_ivs != 1) return 0;

    InductionVar *iv = &ivs[0];
    int cmp_idx, jmpif_idx, limit, cond, exit_target;
    if (!find_loop_exit_condition(ir, loop, iv->vreg,
            &cmp_idx, &jmpif_idx, &limit, &cond, &exit_target))
        return 0;

    int trip_count = compute_trip_count(iv->init_val, limit, iv->step, cond);
    if (trip_count <= 0 || trip_count > 16) return 0;

    int body_indices[128];
    int body_count = collect_body_instructions(ir, loop, iv->vreg,
            cmp_idx, jmpif_idx, iv->def_idx, body_indices, 128);
    if (body_count <= 0 || body_count > 32) return 0;
    if (trip_count * body_count > 128) return 0;

    // Check no nested loops (backward jumps in body)
    // Check no CALL/ASM instructions in body

    // === EMIT ===
    // NOP out entire loop region [start_idx .. actual_end] + IV init
    // Write trip_count copies of body at start_idx
    // Add JMP to exit_target at the end if needed

    return 1;
}
```

**Vreg remapping during clone:**

For each body instruction being cloned for iteration k:
- Read original dest, src1, src2
- If src1 or src2 has vreg == iv_vreg: replace with `irop_make_imm32(-1, init_val + k * step, VT_INT)`
- For VAR vregs defined in the body (not the IV): need per-iteration copies.
  But since we use full unrolling and the accumulator pattern is `V = V + const`,
  we do NOT remap — the same V is accumulated across iterations. This is correct:
  ```
  V1 = V1 + 5   ; iter 0: V1 goes from 0 → 5
  V1 = V1 + 5   ; iter 1: V1 goes from 5 → 10
  ```

The only remapping needed is: uses of the IV as a value (e.g., `arr[i] = i`
where i appears as src). The IV definition itself is excluded from the body.

**Writing an instruction in-place at a NOP slot:**
```c
static void write_instr_at(TCCIRState *ir, int pos, TccIrOp op,
                           IROperand dest, IROperand src1, IROperand src2)
{
    IRQuadCompact *q = &ir->compact_instructions[pos];
    q->op = op;
    q->is_jump_target = 0;
    q->operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
}
```

This reuses the existing `tcc_ir_pool_add()` to allocate operand pool entries.
The old operand pool entries for the NOPed instructions become garbage but are
harmless (the pool only grows; it's freed when the IR block is freed).

### Step 4: Wire into pipeline — `tccgen.c`

At ~line 23991, after dead store elimination, before LICM:

```c
  /* Phase 5a: Loop Unrolling - fully unroll small constant-trip-count loops */
  int unrolled_count = 0;
  if (tcc_state->opt_loop_unroll)
    unrolled_count = tcc_ir_opt_loop_unroll(ir);

  /* Phase 5a': After unrolling, re-run iterative constant propagation + DCE
   * to collapse the expanded constant arithmetic (e.g. 0+5+5+5+5+5 → 25) */
  if (unrolled_count > 0)
  {
    int iter2 = 0, ch2;
    do {
      ch2 = 0;
      if (tcc_state->opt_dce)        ch2 += tcc_ir_opt_dce(ir);
      if (tcc_state->opt_const_prop)  ch2 += tcc_ir_opt_const_prop(ir);
      if (tcc_state->opt_const_prop)  ch2 += tcc_ir_opt_const_prop_tmp(ir);
      if (tcc_state->opt_const_prop)  ch2 += tcc_ir_opt_branch_folding(ir);
      if (tcc_state->opt_const_prop)  ch2 += tcc_ir_opt_value_tracking(ir);
    } while (ch2 > 0 && ++iter2 < 10);
  }
```

### Step 5: Add tests

**Test 1**: Existing `100_pure_func_strlen.c` — verify with `--dump-ir` that
the loop is eliminated and `V1 <-- #25` appears in the optimized IR.
Update the expect file if output changes (it shouldn't — same result, less work).

**Test 2**: New `101_loop_unroll_basic.c`:
```c
#include <stdio.h>
int main() {
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += 10;
    printf("%d\n", sum);       // expect: 40
    return sum != 40;
}
```

**Test 3**: New `102_loop_unroll_no_unroll.c`:
```c
#include <stdio.h>
int main() {
    int sum = 0;
    int n = 100;
    for (int i = 0; i < n; i++) sum += 1;   // n not const — don't unroll
    printf("%d\n", sum);
    return sum != 100;
}
```

**Test 4**: New `103_loop_unroll_with_array.c`:
```c
#include <stdio.h>
int main() {
    int arr[4];
    for (int i = 0; i < 4; i++) arr[i] = i * 10;
    printf("%d %d %d %d\n", arr[0], arr[1], arr[2], arr[3]);
    return 0;
}
```

Add all to `TEST_FILES` in `tests/ir_tests/test_qemu.py`.

### Step 6: Validate

```bash
make cross && make test -j16          # IR tests (must all pass)
make test-asm -j16                    # ASM tests (no regressions)
# Optionally:
make test-gcc-torture-compile         # GCC torture compile tests
```

## Edge Cases

| Case | Expected behavior |
|------|-------------------|
| `for (i=0; i<0; i++)` | trip_count=0, NOP out loop, keep init values |
| `for (i=0; i<1; i++)` | trip_count=1, emit body once (no loop overhead) |
| `for (i=5; i<10; i+=2)` | trip_count=ceil(5/2)=3, emit 3 copies with IV=5,7,9 |
| `for (i=0; i<17; i++)` | trip_count=17 > threshold, skip |
| Body has `if/else` | Body contains JUMPIF → forward jumps within body. These need target remapping per iteration. Complex — bail for v1 |
| IV used after loop | Keep IV final value: `V2 = init + trip_count * step` assigned before exit |

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Code size explosion | Conservative threshold: trip_count * body_size <= 128 |
| Instruction index corruption (like LICM bug) | Write into NOP slots — no shifting. Only use insert_instr_at() as fallback |
| Incorrect vreg remapping | Keep it simple: V accumulators aren't remapped (correct for `V=V+C`). IV uses get constant substitution. Fresh TMPs only for TMP vregs defined in body |
| Interactions with IV strength reduction | Unrolling eliminates the loop; IV SR detects no loops (safe) |
| Register pressure increase | Unrolled code reuses same VARs; linear scan handles spills |
| Body with internal branches | v1: bail on bodies containing JUMPIF (revisit later) |
| Operand pool growth | Pool only grows, old entries become dead — acceptable for small unrolls |
