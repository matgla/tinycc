# TCC IR Architecture Independence Refactoring Plan

**Status**: Partially Implemented - Revision 2 (Updated: 2026-02-01)

---

## Executive Summary

This document describes the architecture independence refactoring for the TCC IR layer. The original goal was to separate architecture-independent IR operations from architecture-dependent code generation.

**What Was Achieved:**
- ✅ IR Modularization: Monolithic `tccir.c` (8,267 lines) split into focused modules
- ✅ Code Organization: Clear separation of concerns (types, pool, vregs, stack, etc.)
- ✅ Build System: `IR_FILES` variable, `tccir.c` removed from build

**What Was NOT Achieved:**
- ❌ Full Architecture Independence: IR layer still couples to ARM backend
- ❌ Machine Interface Integration: Abstract interface created but not adopted
- ❌ Backend Abstraction: ARM-specific calls remain in `ir/mat.c`

---

## Current State (Post-Modularization)

### File Structure

```
tcc/
├── IR Modules (ir/)
│   ├── core.c/h          # IR block lifecycle (allocate, reset, free)
│   ├── type.c/h          # Type management (float, llong, reg_type)
│   ├── pool.c/h          # Operand pool management
│   ├── vreg.c/h          # Virtual register allocation
│   ├── stack.c/h         # Stack slot management
│   ├── live.c/h          # Liveness analysis
│   ├── mat.c/h           # Value materialization (⚠️ ARM-coupled)
│   ├── codegen.c/h       # Code generation (⚠️ ARM-coupled)
│   ├── opt.c/h           # Optimization passes
│   └── dump.c/h          # IR debugging/dumping
│
├── Prototyped but Not Integrated
│   ├── tccmachine.h/c    # Machine interface abstraction (unused)
│   └── tccopt.h/c        # Optimization module (unused)
│
└── Backends (ARM-specific)
    ├── arm-thumb-gen.c   # ARM code generation
    ├── arm-thumb-scratch.c # Scratch register management
    └── arm-link.c        # ARM linking
```

### What Works Well

| Module | Lines | Status | Description |
|--------|-------|--------|-------------|
| core.c | ~1,500 | ✅ Clean | Block allocation, instruction emission |
| type.c | ~200 | ✅ Clean | Type tracking for vregs |
| pool.c | ~200 | ✅ Clean | Operand pool management |
| vreg.c | ~800 | ✅ Clean | Virtual register allocation |
| stack.c | ~600 | ✅ Clean | Stack slot assignment |
| live.c | ~1,200 | ✅ Clean | Liveness analysis |
| opt.c | ~2,400 | ✅ Clean | Optimization passes |
| dump.c | ~800 | ✅ Clean | IR dumping/debugging |

### Remaining Architecture Coupling

**`ir/mat.c` (~1,500 lines)** - Direct ARM backend calls:
```c
// Direct scratch allocation with ARM flags
tcc_machine_acquire_scratch(&scratch, scratch_flags);

// Direct materialization calls
tcc_machine_load_spill_slot(reg, frame_offset);
tcc_machine_addr_of_stack_slot(target_reg, frame_offset, is_param);
tcc_machine_load_constant(reg, hi_reg, value, is_64bit, NULL);
```

**`ir/codegen.c` (~2,200 lines)** - ARM-specific codegen:
- Assumes ARM instruction set
- Uses ARM scratch register conventions
- ARM-specific inline assembly handling

---

## Revised Goals

Given the current state, we have three options:

### Option A: Complete Architecture Independence (High Effort)

**Goal**: Full separation of IR from backend, enabling multi-architecture support

**Work Required**:
1. Create `arch/arm-thumb-machine.c` implementing `TCCMachineInterface`
2. Refactor `ir/mat.c` to use abstract interface (~500 lines changed)
3. Refactor `ir/codegen.c` to use abstract interface (~1000 lines changed)
4. Integrate `tccopt.c` or remove it in favor of `ir/opt.c`
5. Add `tcc_machine_register()` call in ARM backend init

**Estimated Effort**: 2-3 weeks
**Benefit**: True multi-architecture support, testable IR without backend

### Option B: Accept ARM-Coupled Modularity (Current State)

**Goal**: Maintain current modular structure, document ARM coupling

**Work Required**:
1. Remove unused `tccmachine.c/h` (or mark as experimental)
2. Remove unused `tccopt.c/h` (or integrate with `ir/opt.c`)
3. Document architecture dependencies in `ir/mat.c` and `ir/codegen.c`
4. Add comments explaining coupling points

**Estimated Effort**: 1-2 days
**Benefit**: Clean codebase, honest documentation of limitations

### Option C: Hybrid Approach (Recommended)

**Goal**: Partial abstraction - keep modularity, create minimal abstraction layer

**Work Required**:
1. Keep modular IR structure (current state)
2. Simplify `tccmachine.h` to only abstract materialization calls
3. Create minimal ARM implementation in existing files
4. Keep optimizations in `ir/opt.c` (working well)
5. Remove or consolidate `tccopt.c`

**Estimated Effort**: 3-5 days
**Benefit**: Cleaner than B, less work than A, enables future multi-arch

---

## Recommendation: Option C - Hybrid Approach

### Rationale

1. **This is an ARMv8-M fork**: Primary goal is Cortex-M33 support, not multi-architecture
2. **Modularization provides value**: Even with ARM coupling, modules are maintainable
3. **Full abstraction is overkill**: Significant effort for theoretical benefit
4. **Partial cleanup is worthwhile**: Removing dead code improves clarity

### Detailed Plan for Option C

#### Step 1: Cleanup Dead Abstraction Code (Day 1)

**Remove or consolidate unused files:**

```bash
# Option C1: Remove unused machine interface
git rm tccmachine.c tccmachine.h

# Option C2: Keep simplified machine interface (minimal)
# Reduce tccmachine.h to only what's needed for mat.c
```

**Decision**: Option C2 - Keep simplified interface for future extensibility

#### Step 2: Document Architecture Coupling (Day 1)

Add header comments to coupled files:

```c
/*
 * ir/mat.c - Value Materialization
 *
 * ARCHITECTURE COUPLING WARNING:
 * This file contains direct calls to ARM-specific materialization functions:
 * - tcc_machine_load_spill_slot()
 * - tcc_machine_addr_of_stack_slot()
 * - tcc_machine_load_constant()
 *
 * To port to a new architecture, these calls must be abstracted.
 * See ARCH_INDEPENDENCE_REFACTORING_PLAN.md for details.
 */
```

#### Step 3: Consolidate Optimization Modules (Day 2)

**Current state**: Two optimization systems
- `ir/opt.c` - Working, used by `tccgen.c` (via `tcc_ir_opt_*`)
- `tccopt.c` - Unused, has nice structure but not integrated

**Options**:
1. Remove `tccopt.c/h` - Simplest, `ir/opt.c` works well
2. Merge `tccopt.c` structure into `ir/opt.c` - Keep good ideas
3. Replace `ir/opt.c` with `tccopt.c` - Risky, needs testing

**Recommendation**: Option 2 - Merge pass structure from `tccopt.h` into `ir/opt.h`

#### Step 4: Simplify Machine Interface (Day 2-3)

Reduce `tccmachine.h` to minimal functional interface:

```c
/* Minimal machine interface - enables future multi-arch support */
#ifndef TCC_MACHINE_H
#define TCC_MACHINE_H

/* Materialization function signatures */
typedef void (*tcc_mat_load_spill_fn)(int reg, int frame_offset);
typedef void (*tcc_mat_addr_stack_fn)(int reg, int offset, int is_param);
typedef void (*tcc_mat_load_const_fn)(int reg, int hi_reg, int64_t val, int is_64bit, void *ctx);

typedef struct TCCMachineOps {
    tcc_mat_load_spill_fn load_spill;
    tcc_mat_addr_stack_fn addr_stack;
    tcc_mat_load_const_fn load_const;
    /* ... other materialization ops ... */
} TCCMachineOps;

/* Set by backend during initialization */
extern const TCCMachineOps *tcc_machine_ops;

/* Convenience macros */
#define tcc_machine_load_spill_slot(reg, offset) \
    (tcc_machine_ops && tcc_machine_ops->load_spill ? \
     tcc_machine_ops->load_spill(reg, offset) : (void)0)

#endif
```

Update `ir/mat.c` to use `tcc_machine_ops->` instead of direct calls.

#### Step 5: Update ARM Backend (Day 4)

In `arm-thumb-gen.c` or initialization:

```c
static const TCCMachineOps arm_machine_ops = {
    .load_spill = arm_load_spill_slot,
    .addr_stack = arm_addr_of_stack_slot,
    .load_const = arm_load_constant,
    /* ... */
};

void arm_backend_init(void) {
    tcc_machine_ops = &arm_machine_ops;
    /* ... rest of init ... */
}
```

#### Step 6: Testing (Day 5)

- Full test suite run
- Verify no performance regression
- Document any issues

---

## Updated Benefits

### Immediate Benefits (Option C)

1. **Cleaner Codebase**: Remove truly dead code (`tccopt.c` if unused)
2. **Documented Coupling**: Clear understanding of what needs changing for new arch
3. **Minimal Abstraction**: Foundation laid for future multi-arch support
4. **No Regression**: Working compiler remains working

### Future Benefits (If Multi-Arch Needed)

1. **Clear Port Path**: Documented coupling points show exactly what to change
2. **Minimal Abstraction**: Function pointer table easier than full vtable
3. **Testable IR**: Could test IR generation without backend (with more work)

---

## Decision Matrix

| Factor | Option A (Full) | Option B (Accept) | Option C (Hybrid) |
|--------|-----------------|-------------------|-------------------|
| Effort | 2-3 weeks | 1-2 days | 3-5 days |
| Code Quality | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| Future Flexibility | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐⭐ |
| Risk | High (changes working code) | Low | Low-Medium |
| Value for ARMv8M fork | Low | Medium | High |

**Recommendation**: Option C - Best balance of effort and value

---

## Next Steps (Immediate Actions)

1. **Decision**: Confirm Option C approach
2. **Day 1**: Document architecture coupling in `ir/mat.c` and `ir/codegen.c`
3. **Day 1-2**: Decide fate of `tccopt.c` (remove or merge into `ir/opt.c`)
4. **Day 2-3**: Simplify `tccmachine.h` to minimal interface
5. **Day 3-4**: Update `ir/mat.c` to use function pointers
6. **Day 5**: Update ARM backend, test

---

## Appendix: Original Plan (For Reference)

The original plan proposed:
- Phase 1: Create `tccmachine.h` with full vtable abstraction ✅ (Created but not integrated)
- Phase 2: Extract optimizations to `tccopt.c` ✅ (Created but not integrated)
- Phase 3: Refactor `tccir.c` to remove arch dependencies ✅ (Done via modularization)
- Phase 4: Create `arm-thumb-machine.c` ❌ (Not done)

The modularization approach (creating `ir/` directory) was a successful alternative to Phase 3, but Phases 1, 2, and 4 remain incomplete.
