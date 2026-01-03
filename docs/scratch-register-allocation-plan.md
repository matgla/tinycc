# Dynamic Scratch Register Allocation for TinyCC ARM Thumb

## Goal
Replace hardcoded LR (R14) and IP (R12) scratch registers with dynamic allocation using liveness information in both IR and non-IR code paths.

## Current Problem
- Only 2 scratch registers (R12, R14) are hardcoded in `architecture_config`
- 64-bit operations may need 3-4 scratch registers
- Registers R12/R14 are wasted when other registers are free

## Implementation Plan

### Phase 1: Add Scratch Register API

**New file: `tcc-scratch.h`**
```c
typedef struct ScratchContext {
  int regs[4];       // Up to 4 scratch registers
  int count;         // Number allocated
  uint8_t spilled;   // Did we need to spill?
} ScratchContext;

// Acquire N scratch registers dynamically
ScratchContext scratch_acquire(int count);

// Acquire register pair for 64-bit ops
ScratchContext scratch_acquire_pair(void);

// Release scratch registers
void scratch_release(ScratchContext *ctx);
```

**New file: `tcc-scratch.c`**
- For IR mode: Query `ls->registers_map` bitmap for free registers
- For non-IR mode: Scan `vstack` to find unused registers (like `get_reg()`)
- Fallback: Use `get_reg()` which spills if needed

### Phase 2: IR Mode Integration

**File: `tccir.c` (lines 74-127)**

Replace in `tcc_ir_preload_spills()`:
```c
// Before:
q->src1.pr0 = architecture_config.scratch_register;

// After:
ScratchContext ctx = scratch_acquire(1);
q->src1.pr0 = ctx.regs[0];
```

Key locations:
- Line 89: `q->src1.pr0 = architecture_config.scratch_register`
- Line 101: `architecture_config.second_scratch_register`
- Line 111: `q->dest.pr0 = architecture_config.scratch_register`
- Line 125: `store(architecture_config.scratch_register, ...)`

### Phase 3: Non-IR Mode Integration

**File: `arm-thumb-gen.c`**

Replace hardcoded `base = 14` patterns:

| Line | Current | Replace With |
|------|---------|--------------|
| 1390 | `load_to_reg(base = 14, -1, &v1)` | `scratch_acquire(1)` |
| 2083 | `load_to_reg(base = 14, -1, &v1)` | `scratch_acquire(1)` |
| 2107 | `load_to_reg(base = 14, -1, &v1)` | `scratch_acquire(1)` |
| 2131 | `load_to_reg(base = 14, -1, &v1)` | `scratch_acquire(1)` |

Replace hardcoded `R12` patterns:

| Line | Current | Description |
|------|---------|-------------|
| 2784 | `load(R12, &op->src2)` | Immediate operand loading |
| 3209-3232 | `load_full_const(R12, ...)` | FP negation sign bit |
| 3618 | `load(R12, &op->src1)` | VFP constant loading |
| 3738 | `load(R12, dest)` | Function pointer calls |
| 3998-4002 | `load(R12, &arg->src1)` | Stack argument preservation |

### Phase 4: 64-bit Operation Support

For operations needing multiple scratch registers:

```c
ScratchContext ctx = scratch_acquire_pair();
if (ctx.count < 2) {
  // Spill strategy: use stack for intermediate
  ctx = scratch_acquire_with_spill(2);
}
```

Affected operations:
- 64-bit multiply (UMULL)
- 64-bit add/sub with carry
- Soft-float double operations

## Files to Modify

| File | Changes |
|------|---------|
| `tcc-scratch.h` | New - API declarations |
| `tcc-scratch.c` | New - Implementation |
| `arm-thumb-gen.c` | Replace R12/R14 hardcoding |
| `tccir.c` | Update `tcc_ir_preload_spills()` |
| `tccls.h` | Export `ls->registers_map` access helpers |
| `Makefile` | Add `tcc-scratch.c` to build |

## Implementation Order

1. Create `tcc-scratch.{c,h}` with basic API
2. Implement IR mode scratch allocation using `ls->registers_map`
3. Implement non-IR mode using vstack scanning (like `get_reg()`)
4. Convert one simple usage site in `arm-thumb-gen.c` (line 2784)
5. Run tests to verify
6. Convert remaining usage sites incrementally
7. Add 64-bit pair allocation
8. Remove `architecture_config.scratch_register` fields

## Testing Strategy

- Run `tests/ir_tests/` suite after each change
- Run `tests/tests2/*.c` for non-IR mode
- Add specific test for high register pressure scenario
- Test 64-bit operations with all registers busy