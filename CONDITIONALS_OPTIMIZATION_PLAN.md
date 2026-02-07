# Conditionals Benchmark Optimization Plan V2

## Progress Summary

| Phase | Status | Cycles | Improvement |
|-------|--------|--------|-------------|
| Initial | Done | ~49,000 | Baseline |
| Phase 1 (Const Branch + IMOD Folding) | ✅ Done | ~20,000 | **2.5x faster** |
| Phase 2 (Const Comparison Folding) | ✅ Done | ~42,000 | Fixed O(n²) perf bug |
| Phase 2b (NOP Removal) | TODO | ~35,000 | Remove wasted cycles |
| Phase 2c (Redundant Jump Elimination) | ✅ Done | ~35,000 | Simplify control flow |
| Phase 3 (Loop-Invariant Hoisting) | TODO | ~4,700 | Match GCC |

**Current: TCC ~35,000 cycles vs GCC ~4,000 cycles (8.75x gap)**

### Phase 2 Completion Notes (2026-02-04)

CMP folding is now working correctly:
- Fixed `is_merge_point()` O(n²) performance bug by precomputing merge points
- Re-enabled Pattern 3 in `tcc_ir_opt_value_tracking()`
- CMP instructions at IR lines 22 and 26 are now folded to NOP

**Current TCC -O1 Assembly (28 bytes - after Phase 2c Jump Threading):**
```asm
bench_conditionals:
   0:   movs    r1, #0              ; result = 0
   2:   movs    r2, #0              ; i = 0
   4:   cmp     r2, r0              ; i vs iterations
   6:   bge.n   18                  ; exit if i >= iterations
   8:   b.n     e                   ; jump to loop body
   a:   adds    r2, #1              ; i++ [HOT]
   c:   b.n     4                   ; back to loop check [HOT]
   e:   movs    r3, #42             ; v2 = 42 (LOOP INVARIANT!)
  10:   movw    r1, #1234           ; result = 1234 (LOOP INVARIANT!)
  14:   subs    r1, #42             ; result = 1192 (LOOP INVARIANT!)
  16:   b.n     a                   ; back to increment [HOT]
  18:   mov     r0, r1              ; return result
  1a:   bx      lr
```

**GCC -O1 Assembly (16 bytes):**
```asm
bench_conditionals:
   0:   cmp     r0, #0
   2:   ble.n   12                  ; return 0 if iterations <= 0
   4:   movs    r3, #0              ; counter = 0
   6:   adds    r3, #1              ; counter++ [HOT - 2 insns only!]
   8:   cmp     r0, r3              ; [HOT]
   a:   bne.n   6                   ; [HOT]
   c:   mov.w   r0, #1192           ; GCC computed result at compile time!
  10:   bx      lr
  12:   movs    r0, #0
  14:   bx      lr
```

### Remaining Gap Analysis

**Why TCC is still 10x slower:**

1. **No LICM (Loop-Invariant Code Motion)**: Instructions at IR 8, 9, 19 compute `v2=42`, `result=1234`, `result=1192` EVERY iteration, but the result is always the same!

2. **NOPs in generated code**: Dead IR instructions become NOP assembly, wasting cycles

3. **Extra branches**: TCC's loop structure has redundant jumps

**To match GCC, we need Phase 3: Loop-Invariant Code Motion (LICM)**

---

## Current State Analysis

### TCC -O1 Current IR After Optimizations:
```
0000: R1(V0) <-- #0 [ASSIGN]
0001: R2(V1) <-- #0 [ASSIGN]
0002: CMP R2(V1),R0(P0)
0003: JMP to 30  if ">=S"
0004: JMP to 8
0005: NOP
0006: R2(V1) <-- R2(V1) ADD #1          ; Loop counter increment [HOT]
0007: JMP to 2                          ; Back to loop check [HOT]
0008: R3(V2) <-- #42 [ASSIGN]           ; i = 42 (LOOP INVARIANT!) [HOT]
0009: R1(V0) <-- #1234 [ASSIGN]         ; r = 1234 (LOOP INVARIANT!) [HOT]
0010-0018: NOP                          ; (optimized away)
0019: R1(V0) <-- R1(V0) SUB #42         ; r -= 42 (LOOP INVARIANT!) [HOT]
0020: JMP to 22
0021: NOP
0022: CMP R1(V0),#1000000               ; 1192 vs 1000000 (CONSTANT CMP!) [HOT]
0023: JMP to 26  if "<=S"               ; ALWAYS TAKEN [HOT]
0024: R1(V0) <-- R1(V0) SAR #3          ; DEAD CODE
0025: JMP to 29
0026: CMP R1(V0),#-1000000              ; 1192 vs -1000000 (CONSTANT CMP!) [HOT]
0027: JMP to 29  if ">=S"               ; ALWAYS TAKEN [HOT]
0028: R1(V0) <-- #0 SUB R1(V0)          ; DEAD CODE
0029: JMP to 5                          ; Back to loop [HOT]
0030: R0(T10) <-- R1(V0) [LOAD]
0031: RETURNVALUE R0(T10)
```

### Key Problems Identified:

1. **Instructions 22-27**: `CMP R1(V0),#1000000` where R1 = 1192 (constant after SUB)
   - This is NOT folded because R1 has multiple definitions
   - Existing const_prop only tracks single-definition variables

2. **Instructions 8-9, 19**: Loop-invariant assignments and arithmetic
   - `R3 <-- #42` - constant, same every iteration
   - `R1 <-- #1234` - constant, same every iteration
   - `R1 <-- R1 SUB #42` - result is always 1192

---

## Phase 2: Full Constant Comparison Folding

### Problem Statement

The current `tcc_ir_opt_const_prop()` only propagates constants for variables with **exactly one definition**. This misses cases where:

1. A variable is assigned a constant (`R1 <-- #1234`)
2. Then modified with a constant (`R1 <-- R1 SUB #42`)
3. Then compared with a constant (`CMP R1, #1000000`)

In this case, R1 has value `1234 - 42 = 1192`, which is still a compile-time constant!

### Solution: Value Tracking Through Arithmetic

Implement **forward dataflow analysis** that tracks constant values through arithmetic operations, not just direct assignments.

### Current Status

✅ **Phase 2 Complete** - CMP folding now works:
- Precomputed merge points in O(n) instead of O(n²) per-instruction
- Back-edge targets correctly identified as merge points
- `tcc_ir_opt_value_tracking()` Pattern 3 enabled
- CMP at IR lines 22 and 26 now fold to NOP

**Next: Phase 3 - Loop-Invariant Code Motion (LICM)**

### Implementation Plan

#### Step 2.1: Add Value Tracking State (Est. 1 hour)

**File:** `ir/opt.c`

```c
/* Track constant values for vregs through arithmetic */
typedef struct {
  int is_constant;       /* 1 = value is known constant */
  int64_t value;         /* The constant value */
  int def_instruction;   /* Instruction that defined this value */
} VRegConstState;

/* Initialize state for all vregs */
static VRegConstState *vreg_const_state_init(TCCIRState *ir, int *max_vreg)
{
  int n = ir->next_instruction_index;
  int max_pos = 0;

  /* Find max vreg position */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int32_t vr = irop_get_vreg(dest);
    if (vr > 0 && TCCIR_DECODE_VREG_POSITION(vr) > max_pos)
      max_pos = TCCIR_DECODE_VREG_POSITION(vr);
  }

  *max_vreg = max_pos;
  return tcc_mallocz(sizeof(VRegConstState) * (max_pos + 1));
}
```

#### Step 2.2: Implement Forward Value Propagation (Est. 2 hours)

**File:** `ir/opt.c`

```c
int tcc_ir_opt_value_tracking(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;
  int max_vreg;
  VRegConstState *state = vreg_const_state_init(ir, &max_vreg);

  /* Forward pass: track values through the IR */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP) continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand src1 = tcc_ir_op_get_src1(ir, q);
    IROperand src2 = tcc_ir_op_get_src2(ir, q);

    int32_t dest_vr = irop_get_vreg(dest);
    int dest_pos = (dest_vr > 0) ? TCCIR_DECODE_VREG_POSITION(dest_vr) : -1;

    /* Pattern 1: Direct constant assignment */
    if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1)) {
      if (dest_pos >= 0 && dest_pos <= max_vreg) {
        state[dest_pos].is_constant = 1;
        state[dest_pos].value = irop_get_imm64_ex(ir, src1);
        state[dest_pos].def_instruction = i;
      }
      continue;
    }

    /* Pattern 2: Arithmetic with constant operand */
    if (q->op == TCCIR_OP_ADD || q->op == TCCIR_OP_SUB) {
      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr > 0) ? TCCIR_DECODE_VREG_POSITION(src1_vr) : -1;

      /* Check if src1 is a known constant AND src2 is immediate */
      if (src1_pos >= 0 && src1_pos <= max_vreg &&
          state[src1_pos].is_constant && irop_is_immediate(src2)) {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);
        int64_t result = (q->op == TCCIR_OP_ADD) ? val1 + val2 : val1 - val2;

        if (dest_pos >= 0 && dest_pos <= max_vreg) {
          state[dest_pos].is_constant = 1;
          state[dest_pos].value = result;
          state[dest_pos].def_instruction = i;
        }
      }
      continue;
    }

    /* Pattern 3: CMP with constant vreg - FOLD IT */
    if (q->op == TCCIR_OP_CMP && i + 1 < n) {
      IRQuadCompact *jump_q = &ir->compact_instructions[i + 1];
      if (jump_q->op != TCCIR_OP_JUMPIF) continue;

      int32_t src1_vr = irop_get_vreg(src1);
      int src1_pos = (src1_vr > 0) ? TCCIR_DECODE_VREG_POSITION(src1_vr) : -1;

      /* Check if src1 is known constant AND src2 is immediate */
      int src1_const = (src1_pos >= 0 && src1_pos <= max_vreg && state[src1_pos].is_constant);
      int src2_const = irop_is_immediate(src2);

      if (src1_const && src2_const) {
        int64_t val1 = state[src1_pos].value;
        int64_t val2 = irop_get_imm64_ex(ir, src2);

        IROperand cond = tcc_ir_op_get_src1(ir, jump_q);
        int tok = (int)irop_get_imm64_ex(ir, cond);
        int result = evaluate_compare_condition(val1, val2, tok);

        if (result >= 0) {
          IROperand jmp_dest = tcc_ir_op_get_dest(ir, jump_q);

          if (result) {
            /* Branch always taken - convert to unconditional JUMP */
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_JUMP;
            tcc_ir_set_dest(ir, i + 1, jmp_dest);
#ifdef DEBUG_IR_GEN
            printf("VALUE_TRACK: CMP vreg=%lld,#%lld -> always taken, JUMP to %d\n",
                   (long long)val1, (long long)val2, (int)jmp_dest.u.imm32);
#endif
          } else {
            /* Branch never taken - eliminate both */
            q->op = TCCIR_OP_NOP;
            jump_q->op = TCCIR_OP_NOP;
#ifdef DEBUG_IR_GEN
            printf("VALUE_TRACK: CMP vreg=%lld,#%lld -> never taken, eliminated\n",
                   (long long)val1, (long long)val2);
#endif
          }
          changes++;
        }
      }
      continue;
    }

    /* Any other instruction that defines dest_vr invalidates the constant */
    if (dest_pos >= 0 && dest_pos <= max_vreg && irop_config[q->op].has_dest) {
      state[dest_pos].is_constant = 0;
    }
  }

  tcc_free(state);

  /* Run DCE to remove code after eliminated branches */
  if (changes)
    changes += tcc_ir_opt_dce(ir);

  return changes;
}
```

#### Step 2.3: Handle Basic Block Boundaries (Est. 1 hour)

The above implementation is **intra-block** only. For full correctness:

1. Reset constant state at loop back-edges (instruction 29: `JMP to 5`)
2. Don't propagate constants across JUMP targets that have multiple predecessors

```c
/* Check if instruction is a jump target with multiple predecessors */
static int is_merge_point(TCCIRState *ir, int instr_idx)
{
  int n = ir->next_instruction_index;
  int predecessor_count = 0;

  /* Count instructions that jump to this target */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if ((int)dest.u.imm32 == instr_idx)
        predecessor_count++;
    }
    /* Fall-through predecessor */
    if (i + 1 == instr_idx && q->op != TCCIR_OP_JUMP &&
        q->op != TCCIR_OP_RETURNVALUE && q->op != TCCIR_OP_RETURNVOID)
      predecessor_count++;
  }

  return predecessor_count > 1;
}

/* In the main loop, clear state at merge points */
if (is_merge_point(ir, i)) {
  for (int v = 0; v <= max_vreg; v++)
    state[v].is_constant = 0;
}
```

#### Step 2.4: Integrate with Optimization Pipeline (Est. 30 min)

**File:** `ir/core.c` or wherever the optimization pipeline is called

```c
/* Add to optimization pipeline after const_prop */
int tcc_ir_optimize(TCCIRState *ir)
{
  int changes = 0;
  int iteration = 0;

  do {
    changes = 0;
    changes += tcc_ir_opt_const_prop(ir);
    changes += tcc_ir_opt_value_tracking(ir);    /* NEW */
    changes += tcc_ir_opt_branch_folding(ir);
    changes += tcc_ir_opt_dce(ir);
    changes += tcc_ir_opt_dse(ir);
    iteration++;
  } while (changes > 0 && iteration < 10);

  return changes;
}
```

#### Step 2.5: Testing (Est. 1 hour)

**File:** `tests/ir_tests/98_value_tracking.c`

```c
/* Test value tracking through arithmetic */
#include <stdio.h>

int test_value_track_sub() {
    int x = 1234;
    x = x - 42;  /* x = 1192, should be tracked */
    if (x > 1000000) return 1;  /* Always false, should be eliminated */
    if (x < -1000000) return 2; /* Always false, should be eliminated */
    return x;  /* Should return 1192 */
}

int test_value_track_add() {
    int x = 100;
    x = x + 50;  /* x = 150 */
    if (x > 200) return 1;  /* Always false */
    return x;
}

int test_chained_arithmetic() {
    int x = 10;
    x = x + 5;   /* x = 15 */
    x = x * 2;   /* x = 30 - MUL may not be tracked, that's OK */
    if (x == 0) return 1;
    return x;
}

int main() {
    printf("test_value_track_sub: %d\n", test_value_track_sub());
    printf("test_value_track_add: %d\n", test_value_track_add());
    printf("test_chained_arithmetic: %d\n", test_chained_arithmetic());
    return 0;
}
```

**Expected output:**
```
test_value_track_sub: 1192
test_value_track_add: 150
test_chained_arithmetic: 30
```

### Expected IR After Phase 2 (Full)

```
0000: R1(V0) <-- #0 [ASSIGN]
0001: R2(V1) <-- #0 [ASSIGN]
0002: CMP R2(V1),R0(P0)
0003: JMP to 30  if ">=S"
0004: JMP to 8
0005: NOP
0006: R2(V1) <-- R2(V1) ADD #1
0007: JMP to 2
0008: R3(V2) <-- #42 [ASSIGN]           ; Still here (not yet hoisted)
0009: R1(V0) <-- #1234 [ASSIGN]         ; Still here (not yet hoisted)
0010-0018: NOP
0019: R1(V0) <-- R1(V0) SUB #42         ; Still here, but value known = 1192
0020: JMP to 22
0021: NOP
0022: NOP                               ; CMP eliminated! (was: CMP R1, #1000000)
0023: JMP to 26                         ; Now unconditional! (1192 <= 1000000)
0024: NOP                               ; SAR eliminated (dead code)
0025: NOP                               ; JMP eliminated (dead code)
0026: NOP                               ; CMP eliminated! (was: CMP R1, #-1000000)
0027: JMP to 29                         ; Now unconditional! (1192 >= -1000000)
0028: NOP                               ; SUB eliminated (dead code)
0029: JMP to 5
0030: R0(T10) <-- R1(V0) [LOAD]
0031: RETURNVALUE R0(T10)
```

### Expected Disassembly After Phase 2 (Full)

```asm
bench_conditionals:
    movs    r1, #0
    movs    r2, #0
    cmp     r2, r0
    bge.w   exit
    b.n     body
increment:
    adds    r2, #1              ; [HOT]
    b.n     check_loop          ; [HOT]
body:
    movs    r3, #42             ; [HOT] - still in loop (Phase 3 will fix)
    movw    r1, #1234           ; [HOT] - still in loop (Phase 3 will fix)
    subs    r1, #42             ; [HOT] - still in loop (Phase 3 will fix)
    ; CMP + branch eliminated!
    ; CMP + branch eliminated!
    b.n     increment           ; [HOT]
exit:
    mov     r0, r1
    bx      lr
```

**Estimated cycles: ~10,000-15,000** (down from ~20,000)

---

## Phase 2 Implementation Checklist

- [x] **2.1** Add `VRegConstState` structure and initialization
- [x] **2.2** Implement `tcc_ir_opt_value_tracking()` with:
  - [x] Track ASSIGN with constant
  - [x] Track ADD/SUB with constant result
  - [ ] Fold CMP + JUMPIF when src1 vreg has known constant value (DISABLED - needs loop fix)
- [x] **2.3** Handle basic block boundaries (merge points)
- [x] **2.4** Integrate into optimization pipeline (run iteratively)
- [x] **2.5** Add test case `tests/ir_tests/98_value_tracking.c`
- [x] **2.6** Run full test suite: `make test -j16`
- [ ] **2.7** Benchmark: `python run_benchmark.py conditionals`

### Known Issues

The CMP folding causes HardFaults in loops (e.g., `test_fp_offset_cache.c`). The issue is that constant state is not properly cleared at loop back-edges. Need to:
1. Properly detect loop headers
2. Clear state when entering loops from back-edges
3. Test with nested loops

### Next Steps to Fix CMP Folding

```c
/* Better approach: Track basic block boundaries */
static void clear_state_at_loop_headers(TCCIRState *ir, VRegConstState *state, int max_vreg)
{
  int n = ir->next_instruction_index;

  /* Find all loop headers (targets of backward jumps) */
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF) {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)dest.u.imm32;
      /* Backward jump = loop back-edge */
      if (target < i) {
        /* Clear state at loop header */
        for (int v = 0; v <= max_vreg; v++)
          state[v].is_constant = 0;
      }
    }
  }
}
```

---

## Phase 2b: NOP Removal in Code Generator

### Problem

After IR optimizations, many instructions become NOP but are still emitted as actual `nop` assembly instructions, wasting cycles.

**Current IR:**
```
0010: NOP
0011: NOP
0012: JMP to 16
0013: NOP
0014: NOP
...
0021: NOP
0022: NOP
```

**Current Assembly (wasted cycles):**
```asm
  14:   nop                         ; wasted cycle
  16:   subs    r1, #42
  18:   nop                         ; wasted cycle
  1a:   nop                         ; wasted cycle
  1c:   nop                         ; wasted cycle
  1e:   b.n     a
```

### Solution

The code generator should skip NOP instructions entirely instead of emitting them.

### Implementation Plan

#### TODO 2b.1: Skip NOPs in Code Generator (Est. 30 min)
**File:** `arm-thumb-gen.c`

```c
/* In the main code generation loop */
for (int i = 0; i < n; i++) {
  IRQuadCompact *q = &ir->compact_instructions[i];

  /* Skip NOP instructions - don't emit anything */
  if (q->op == TCCIR_OP_NOP)
    continue;

  /* ... rest of code generation ... */
}
```

#### TODO 2b.2: Update Jump Target Resolution (Est. 1 hour)
**File:** `arm-thumb-gen.c`

When NOPs are skipped, jump targets need adjustment:
- [ ] Build a mapping from IR index to actual code offset
- [ ] Resolve jump targets using the mapping
- [ ] Handle forward and backward jumps correctly

```c
/* Build IR index -> code offset mapping */
int *ir_to_code_offset = tcc_mallocz(n * sizeof(int));
int code_offset = 0;

for (int i = 0; i < n; i++) {
  ir_to_code_offset[i] = code_offset;
  if (ir->compact_instructions[i].op != TCCIR_OP_NOP)
    code_offset += instruction_size(ir, i);
}

/* When emitting a jump to target T, use ir_to_code_offset[T] */
```

#### TODO 2b.3: Handle Label References (Est. 30 min)
- [ ] Ensure labels still work correctly
- [ ] Update any debug info that references IR indices

### Phase 2b Checklist

- [ ] **2b.1** Skip NOP instructions in code generator
- [ ] **2b.2** Update jump target resolution for skipped NOPs
- [ ] **2b.3** Handle labels and debug info
- [ ] **2b.4** Test: verify no NOPs in generated assembly
- [ ] **2b.5** Run full test suite

---

## Phase 2c: Redundant Jump Elimination

### Problem

After optimizations, the IR has redundant jumps:
```
0012: JMP to 16       ; Jump over NOPs
0013: NOP
0014: NOP
0015: NOP
0016: NOP             ; Target of jump (also NOP!)
0017: NOP
0018: NOP
0019: R1 <-- R1 SUB #42
0020: JMP to 22       ; Jump to next non-NOP
0021: NOP
0022: NOP             ; Target (NOP!)
0023: JMP to 26       ; Jump to next non-NOP
```

These patterns waste cycles:
1. **JMP to NOP**: Jump target is a NOP - should jump to next real instruction
2. **JMP to next instruction**: Unconditional jump that falls through anyway
3. **Chain of jumps**: JMP to JMP to JMP...

### Solution

Implement jump threading and redundant jump elimination at IR level.

### Implementation Plan

#### TODO 2c.1: Jump Target Forwarding (Est. 1 hour)
**File:** `ir/opt.c`

Forward jump targets through NOPs to the next real instruction:

```c
int tcc_ir_opt_jump_threading(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP && q->op != TCCIR_OP_JUMPIF)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)dest.u.imm32;

    /* Find first non-NOP instruction at or after target */
    int new_target = target;
    while (new_target < n && ir->compact_instructions[new_target].op == TCCIR_OP_NOP)
      new_target++;

    /* Also follow unconditional jumps (jump threading) */
    while (new_target < n && ir->compact_instructions[new_target].op == TCCIR_OP_JUMP) {
      IROperand next_dest = tcc_ir_op_get_dest(ir, &ir->compact_instructions[new_target]);
      new_target = (int)next_dest.u.imm32;
    }

    if (new_target != target) {
      dest.u.imm32 = new_target;
      tcc_ir_set_dest(ir, i, dest);
      changes++;
    }
  }

  return changes;
}
```

#### TODO 2c.2: Eliminate Fall-Through Jumps (Est. 30 min)
**File:** `ir/opt.c`

Remove unconditional jumps to the next instruction:

```c
int tcc_ir_opt_eliminate_fallthrough_jumps(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  for (int i = 0; i < n - 1; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];

    if (q->op != TCCIR_OP_JUMP)
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    int target = (int)dest.u.imm32;

    /* Find next non-NOP instruction */
    int next_real = i + 1;
    while (next_real < n && ir->compact_instructions[next_real].op == TCCIR_OP_NOP)
      next_real++;

    /* If jump target equals next real instruction, eliminate the jump */
    if (target == next_real) {
      q->op = TCCIR_OP_NOP;
      changes++;
    }
  }

  return changes;
}
```

#### TODO 2c.3: Integrate into Optimization Pipeline (Est. 30 min)
**File:** `tccgen.c`

Add jump threading after other optimizations:

```c
/* In optimization loop */
changes += tcc_ir_opt_jump_threading(ir);
changes += tcc_ir_opt_eliminate_fallthrough_jumps(ir);
```

### Expected IR After Phase 2c

```
0000: R1(V0) <-- #0 [ASSIGN]
0001: R2(V1) <-- #0 [ASSIGN]
0002: CMP R2(V1),R0(P0)
0003: JMP to 30  if ">=S"
0004: JMP to 8                          ; Direct to loop body
0005: NOP
0006: R2(V1) <-- R2(V1) ADD #1
0007: JMP to 2
0008: R3(V2) <-- #42 [ASSIGN]
0009: R1(V0) <-- #1234 [ASSIGN]
... NOPs ...
0019: R1(V0) <-- R1(V0) SUB #42
0020: JMP to 29                         ; Direct to back-edge (was: 20→22→26→29)
... NOPs ...
0029: JMP to 5
0030: R0(T10) <-- R1(V0) [LOAD]
0031: RETURNVALUE R0(T10)
```

### Phase 2c Implementation Notes (2026-02-04)

Jump threading optimization implemented in `ir/opt_jump_thread.c`:

1. **Jump Target Forwarding**: Jumps targeting NOPs are redirected to the next real instruction
2. **Jump Threading**: Chains of unconditional jumps are followed to find the ultimate target
3. **Fall-Through Elimination**: Unconditional jumps to the next instruction are removed

**Results:**
- Code size reduced from 36 bytes to 28 bytes for `bench_conditionals`
- All 494 IR tests pass
- Jump chains like `20→22→26→29→5` collapsed to direct `20→5`

### Phase 2c Checklist

- [x] **2c.1** Implement jump target forwarding (skip NOPs)
- [x] **2c.2** Implement jump threading (follow JMP chains)
- [x] **2c.3** Eliminate fall-through jumps
- [x] **2c.4** Integrate into optimization pipeline
- [x] **2c.5** Test with various control flow patterns
- [x] **2c.6** Run full test suite (494 tests passed)

---

## Phase 3: Loop-Invariant Code Motion (LICM)

### Goal
Move computations that produce the same result on every iteration out of the loop.

**Target improvement: ~42,000 cycles → ~4,000 cycles (10x)**

### Current Problem

```
Loop body (executed N times):
  0008: R3(V2) <-- #42 [ASSIGN]         ; Same every iteration!
  0009: R1(V0) <-- #1234 [ASSIGN]       ; Same every iteration!
  0019: R1(V0) <-- R1(V0) SUB #42       ; Always produces 1192!
```

These instructions should execute ONCE before the loop, not N times.

### Desired Output

```
Preheader (executed once):
  R1(V0) <-- #1192 [ASSIGN]             ; Computed at compile time!

Loop body (executed N times):
  R2(V1) <-- R2(V1) ADD #1              ; Just the counter
  CMP R2(V1), R0(P0)
  JMP to loop if "<S"
```

---

## Phase 3 Implementation Plan

### TODO 3.1: Loop Detection (Est. 2 hours)
**File:** `ir/opt.c` or new `ir/loop.c`

Identify natural loops in the IR:
- [ ] Find back-edges (jumps where target < source)
- [ ] Identify loop headers (targets of back-edges)
- [ ] Compute loop body (all instructions dominated by header that can reach the back-edge)
- [ ] Handle nested loops (inner loops are hoisted first)

```c
typedef struct {
  int header;           /* First instruction of loop */
  int back_edge_src;    /* Instruction with back-edge jump */
  int *body;            /* Array of instruction indices in loop */
  int body_count;
  int preheader;        /* Instruction before header (insertion point) */
} LoopInfo;

/* Find all natural loops */
int find_loops(TCCIRState *ir, LoopInfo **loops, int *loop_count);
```

### TODO 3.2: Loop-Invariant Detection (Est. 2 hours)
**File:** `ir/opt.c`

An instruction is loop-invariant if:
- [ ] All its operands are either:
  - Constants/immediates, OR
  - Defined outside the loop, OR
  - Defined by other loop-invariant instructions
- [ ] It has no side effects (no STORE, no CALL)
- [ ] Its destination is not used before being redefined in the loop

```c
/* Check if instruction i is loop-invariant in loop L */
int is_loop_invariant(TCCIRState *ir, int i, LoopInfo *loop, uint8_t *invariant_flags)
{
  IRQuadCompact *q = &ir->compact_instructions[i];

  /* Side-effect instructions cannot be hoisted */
  if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_FUNCCALL ||
      q->op == TCCIR_OP_FUNCCALLVAL)
    return 0;

  /* Check each operand */
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  if (!operand_is_invariant(ir, src1, loop, invariant_flags))
    return 0;
  if (!operand_is_invariant(ir, src2, loop, invariant_flags))
    return 0;

  return 1;
}

int operand_is_invariant(TCCIRState *ir, IROperand op, LoopInfo *loop, uint8_t *invariant_flags)
{
  /* Immediates are always invariant */
  if (irop_is_immediate(op))
    return 1;

  /* Find the definition of this vreg */
  int def_instr = find_single_definition(ir, op, loop);
  if (def_instr < 0)
    return 0;  /* Multiple definitions - not invariant */

  /* Defined outside loop - invariant */
  if (!is_in_loop(def_instr, loop))
    return 1;

  /* Defined inside loop - check if that definition is invariant */
  return invariant_flags[def_instr];
}
```

### TODO 3.3: Code Motion (Est. 3 hours)
**File:** `ir/opt.c`

Move invariant instructions to preheader:
- [ ] Create preheader block if it doesn't exist
- [ ] Move invariant instructions in dependency order
- [ ] Update jump targets that pointed to header
- [ ] Handle the case where dest vreg is live-in to the loop

```c
int tcc_ir_opt_licm(TCCIRState *ir)
{
  int changes = 0;
  LoopInfo *loops;
  int loop_count;

  if (!find_loops(ir, &loops, &loop_count))
    return 0;

  /* Process innermost loops first */
  for (int L = 0; L < loop_count; L++) {
    LoopInfo *loop = &loops[L];

    /* Find loop-invariant instructions */
    uint8_t *invariant = tcc_mallocz(ir->next_instruction_index);
    int found_invariant;

    do {
      found_invariant = 0;
      for (int i = 0; i < loop->body_count; i++) {
        int instr = loop->body[i];
        if (!invariant[instr] && is_loop_invariant(ir, instr, loop, invariant)) {
          invariant[instr] = 1;
          found_invariant = 1;
        }
      }
    } while (found_invariant);

    /* Move invariant instructions to preheader */
    for (int i = 0; i < loop->body_count; i++) {
      int instr = loop->body[i];
      if (invariant[instr]) {
        move_to_preheader(ir, instr, loop);
        changes++;
      }
    }

    tcc_free(invariant);
  }

  free_loops(loops, loop_count);
  return changes;
}
```

### TODO 3.4: Strength Reduction for Constant Results (Est. 1 hour)
**File:** `ir/opt.c`

When LICM hoists `R1 <-- #1234` followed by `R1 <-- R1 SUB #42`, combine them:
- [ ] Detect pattern: ASSIGN const + arithmetic with const
- [ ] Compute result at compile time
- [ ] Replace with single ASSIGN

```c
/* After LICM, in preheader:
   R1 <-- #1234
   R1 <-- R1 SUB #42

   Becomes:
   R1 <-- #1192
*/
int tcc_ir_opt_fold_preheader(TCCIRState *ir, LoopInfo *loop)
{
  /* Re-run value tracking on preheader to fold chained constants */
  return tcc_ir_opt_value_tracking_range(ir, loop->preheader, loop->header);
}
```

### TODO 3.5: Integration and Testing (Est. 2 hours)

- [ ] Add `tcc_ir_opt_licm()` to optimization pipeline
- [ ] Run after const_prop and value_tracking
- [ ] Test with nested loops
- [ ] Test with multiple loop-invariant instructions
- [ ] Test with dependencies between invariant instructions

**Test cases:**
```c
// tests/ir_tests/99_licm.c
int test_licm_simple(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int x = 100;        // Loop-invariant
        sum += x;
    }
    return sum;  // Should be n * 100
}

int test_licm_chained(int n) {
    int r = 0;
    for (int i = 0; i < n; i++) {
        int x = 50;         // Loop-invariant
        int y = x + 30;     // Loop-invariant (depends on x)
        r = y;
    }
    return r;  // Should be 80
}

int test_licm_with_dep(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int base = 10;      // Loop-invariant
        sum += base + i;    // base is invariant, i is not
    }
    return sum;
}
```

---

## Phase 3 Checklist

- [ ] **3.1** Implement loop detection
  - [ ] Find back-edges
  - [ ] Identify loop headers
  - [ ] Compute loop body
- [ ] **3.2** Implement loop-invariant detection
  - [ ] Check operand sources
  - [ ] Handle transitive invariance
  - [ ] Exclude side-effect instructions
- [ ] **3.3** Implement code motion
  - [ ] Create/find preheader
  - [ ] Move instructions in dependency order
  - [ ] Update IR structure
- [ ] **3.4** Fold chained constants in preheader
- [ ] **3.5** Testing
  - [ ] Add test cases
  - [ ] Run full test suite
  - [ ] Benchmark conditionals

---

## Alternative: Simplified LICM for This Benchmark

Instead of full LICM, we could implement a simpler optimization specific to this pattern:

### Simpler Approach: Constant Loop Body Detection

If the entire loop body (except the counter) produces a constant result:
1. Compute the constant at compile time
2. Replace loop body with just the counter increment
3. Set result after loop exit

```c
/* Detect: loop body always produces same value for V0 */
int tcc_ir_opt_constant_loop_body(TCCIRState *ir)
{
  /* Find loops where result variable is:
     1. Assigned a constant
     2. Modified only by constant operations
     3. Never read by control flow inside loop
  */
}
```

This would match GCC's optimization for this specific benchmark.

---

## Expected Final Result (After Phase 3)

**TCC -O1 Assembly:**
```asm
bench_conditionals:
    cmp     r0, #0
    ble.n   return_zero
    movs    r2, #0              ; counter = 0
    movw    r1, #1192           ; result = 1192 (computed at compile time!)
loop:
    adds    r2, #1              ; counter++ [HOT - 1 insn]
    cmp     r2, r0              ; [HOT - 1 insn]
    blt.n   loop                ; [HOT - 1 insn]
    mov     r0, r1
    bx      lr
return_zero:
    movs    r0, #0
    bx      lr
```

**Estimated cycles: ~4,000-5,000** (matching GCC!)

---

## Verification Commands

```bash
# Test current IR
./armv8m-tcc -dump-ir -O1 -c /tmp/bench_cond_simple.c -o /tmp/test.o 2>&1

# Run benchmark
cd tests/benchmarks && python run_benchmark.py <host> conditionals

# Run all tests
make test -j16
```

