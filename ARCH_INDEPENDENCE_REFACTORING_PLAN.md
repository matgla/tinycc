# TCC IR Architecture Independence Refactoring Plan

## Executive Summary

The `tccir.c` file (8,267 lines) currently contains significant architecture-dependent code that violates the separation between the target-independent IR layer and the target-dependent code generation layer. This document provides a detailed analysis of these violations and a step-by-step plan for refactoring.

---

## Current Architecture Violations Analysis

### 1. Scratch Register Allocation (Architecture-Dependent)

**Problem:** The IR layer directly manages scratch register allocation with architecture-specific flags.

**Current Violations:**
```c
// tccir.c lines 2870, 2964, 3049, 3104, etc.
TCCMachineScratchRegs scratch = {0};
tcc_machine_acquire_scratch(&scratch, scratch_flags);
```

**Architecture-Specific Flags Used:**
- `TCC_MACHINE_SCRATCH_NEEDS_PAIR` - ARM register pairs for 64-bit
- `TCC_MACHINE_SCRATCH_PREFERS_FLOAT` - ARM VFP registers
- `TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS` - ARM R0-R3 exclusion
- `TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH` - ARM R11/R12 exclusion

**Impact:** 12 call sites in tccir.c directly allocate scratch registers.

### 2. Value Materialization (Architecture-Dependent)

**Problem:** The IR layer decides HOW to materialize values based on architecture-specific encoding limits.

**Current Violations:**
```c
// tccir.c line 3037 - Checking ARM instruction encoding limits
if (tcc_machine_can_encode_stack_offset_with_param_adj(frame_offset, is_param, test_reg))
    return; /* Backend can encode this offset directly */
```

**Materialization Functions Called:**
| Function | Count | Purpose |
|----------|-------|---------|
| `tcc_machine_load_spill_slot` | 6 | Load from spill slot |
| `tcc_machine_store_spill_slot` | 2 | Store to spill slot |
| `tcc_machine_addr_of_stack_slot` | 2 | Compute stack addresses |
| `tcc_machine_load_constant` | 2 | Materialize constants |
| `tcc_machine_load_cmp_result` | 1 | Load comparison flags |
| `tcc_machine_load_jmp_result` | 1 | Load jump target |

### 3. Stack Frame Layout Knowledge (Architecture-Dependent)

**Problem:** IR knows about frame pointer, parameter offsets, and stack layout.

**Current Violations:**
```c
// Accessing codegen_instruction_idx for liveness
ir->codegen_instruction_idx

// Architecture-specific stack layout assumptions
ir->call_outgoing_base  // FP-relative outgoing args area
ir->codegen_materialize_scratch_flags  // ARM scratch preferences
```

### 4. Inline Assembly (Architecture-Dependent)

**Problem:** IR stores inline asm blocks which are architecture-specific.

**Current Violations:**
```c
#ifdef CONFIG_TCC_ASM
TCCIRInlineAsm *inline_asms;  // ARM-specific inline asm storage
#endif
```

### 5. Spill Cache (Questionable)

**Problem:** `SpillCache` tracks register-stack mappings during codegen.

**Analysis:** This is borderline - it could be generic but currently embeds architecture assumptions about spill slot addressing.

---

## Proposed New Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     FRONTEND (tccgen.c)                         │
│              C Parsing, Type Checking, Semantic Analysis        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              IR LAYER (tccir.c) - PURE ARCH-INDEPENDENT         │
│  - IR Construction (SSA-like operations)                        │
│  - Target-Independent Optimizations (CSE, DCE, etc.)            │
│  - Liveness Analysis (target-agnostic algorithm)                │
│  - NO: scratch allocation, materialization decisions, stack     │
│    layout assumptions, architecture-specific flags              │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│         OPTIMIZATION MODULE (tccopt.c) - PLUGGABLE              │
│  - Constant Folding                                             │
│  - Dead Code Elimination                                        │
│  - Common Subexpression Elimination                             │
│  - Strength Reduction                                           │
│  - Architecture-aware peepholes (via callbacks)                 │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│         MACHINE INTERFACE (tccmachine.h) - ABSTRACT API         │
│  - Scratch register allocation interface                        │
│  - Value materialization requests (not decisions)               │
│  - Stack frame abstraction                                      │
│  - Instruction encoding hints                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│           BACKEND (arm-thumb-gen.c, x86-gen.c, etc.)            │
│  - Machine-specific code generation                             │
│  - Register allocation (physical registers)                     │
│  - Instruction selection and encoding                           │
│  - Stack frame layout                                           │
└─────────────────────────────────────────────────────────────────┘
```

---

## Detailed Refactoring Plan

### Phase 1: Create Machine Interface Abstraction (1-2 days)

**File: `tccmachine.h`** (New)

```c
/* Machine Interface - Abstract architecture-dependent operations */

#ifndef TCC_MACHINE_H
#define TCC_MACHINE_H

#include "tccir_operand.h"  /* Only for basic types */

/* Opaque scratch register handle - implementation hidden */
typedef struct TCCScratchHandle TCCScratchHandle;

/* Materialization request types - IR asks, machine decides */
typedef enum TCCMatRequest {
    TCC_MAT_LOAD_SPILL,      /* Load value from spill slot */
    TCC_MAT_STORE_SPILL,     /* Store value to spill slot */
    TCC_MAT_ADDR_STACK,      /* Compute address of stack slot */
    TCC_MAT_LOAD_CONST,      /* Load constant to register */
    TCC_MAT_LOAD_CMP,        /* Load comparison result */
    TCC_MAT_LOAD_JMP,        /* Load jump target */
} TCCMatRequest;

/* Materialization context - passed to machine layer */
typedef struct TCCMatContext {
    int vreg;                /* Virtual register to materialize */
    int frame_offset;        /* Stack offset (if applicable) */
    int is_param;            /* Is this a parameter slot? */
    int is_64bit;            /* Is this a 64-bit value? */
    /* ... more as needed */
} TCCMatContext;

/* Machine interface function table (vtable pattern) */
typedef struct TCCMachineInterface {
    /* Scratch register management */
    TCCScratchHandle* (*acquire_scratch)(int needs_pair, int avoid_call_regs);
    void (*release_scratch)(TCCScratchHandle* handle);
    int (*scratch_get_reg)(TCCScratchHandle* handle, int idx);
    
    /* Materialization - IR requests, machine fulfills */
    int (*can_materialize_directly)(const TCCMatContext* ctx);
    void (*request_materialization)(const TCCMatContext* ctx, TCCScratchHandle* dest);
    
    /* Stack frame queries (abstracted) */
    int (*get_spill_slot_offset)(int vreg);
    int (*get_stack_slot_size)(void);
    
} TCCMachineInterface;

/* Global machine interface pointer - set by backend during init */
extern const TCCMachineInterface* tcc_machine;

/* Convenience macros */
#define tcc_machine_acquire_scratch(needs_pair, avoid_call) \
    tcc_machine->acquire_scratch(needs_pair, avoid_call)
#define tcc_machine_request_materialization(ctx, dest) \
    tcc_machine->request_materialization(ctx, dest)

#endif
```

### Phase 2: Extract Optimizations to Separate Module (2-3 days)

**File: `tccopt.c` / `tccopt.h`** (New)

Move target-independent optimizations from tccir.c:

```c
/* tccopt.h */
#ifndef TCC_OPT_H
#define TCC_OPT_H

#include "tccir.h"

/* Optimization pass structure */
typedef struct TCCOptPass {
    const char* name;
    int (*run)(TCCIRState* ir);
    int enabled_by_default;
} TCCOptPass;

/* Target-independent optimization passes */
int tcc_opt_dead_code_elimination(TCCIRState* ir);
int tcc_opt_constant_folding(TCCIRState* ir);
int tcc_opt_common_subexpression_elimination(TCCIRState* ir);
int tcc_opt_strength_reduction(TCCIRState* ir);
int tcc_opt_copy_propagation(TCCIRState* ir);

/* Run all enabled optimizations */
void tcc_optimize_ir(TCCIRState* ir, int level);

/* Register a target-specific optimization pass */
void tcc_opt_register_pass(TCCOptPass* pass);

#endif
```

**Optimizations to Move:**
1. Dead code elimination (currently embedded in IR construction)
2. Constant folding
3. Copy propagation
4. Strength reduction
5. FP offset caching (currently in tccir.c)

### Phase 3: Refactor tccir.c - Remove Arch Dependencies (3-4 days)

#### Step 3.1: Remove Direct Scratch Allocation

**Before:**
```c
// tccir.c
TCCMachineScratchRegs scratch = {0};
unsigned scratch_flags = (is_64bit ? TCC_MACHINE_SCRATCH_NEEDS_PAIR : 0);
tcc_machine_acquire_scratch(&scratch, scratch_flags);
```

**After:**
```c
// tccir.c - request materialization abstractly
TCCMatContext ctx = {
    .vreg = vreg_num,
    .is_64bit = is_64bit,
    .frame_offset = offset,
};
TCCScratchHandle* scratch = tcc_machine_request_materialization(&ctx, NULL);
```

#### Step 3.2: Remove Materialization Decisions

**Before:**
```c
// tccir.c
if (tcc_machine_can_encode_stack_offset_with_param_adj(frame_offset, is_param, test_reg))
    return;
// ... compute and call tcc_machine_addr_of_stack_slot
```

**After:**
```c
// tccir.c - just request the address
TCCMatContext ctx = {
    .request = TCC_MAT_ADDR_STACK,
    .frame_offset = offset,
    .is_param = is_param,
};
if (!tcc_machine->can_materialize_directly(&ctx)) {
    // Request explicit materialization via scratch
    tcc_machine_request_materialization(&ctx, scratch);
}
```

#### Step 3.3: Move Liveness Analysis

Currently `LSLiveIntervalState ls` is embedded in TCCIRState. Options:

**Option A:** Keep liveness in IR (it's algorithmically generic)
**Option B:** Move to optimization module

Recommendation: **Keep in IR** but make it more abstract:

```c
// tccir.h
typedef struct TCCLivenessState TCCLivenessState;  /* Opaque */

TCCIRState {
    // ...
    TCCLivenessState* liveness;  /* Pointer to allow different implementations */
    // ...
};
```

### Phase 4: Backend Adaptation (2-3 days)

**File: `arm-thumb-machine.c`** (New)

Implement the machine interface for ARM:

```c
/* arm-thumb-machine.c */

#include "tccmachine.h"
#include "arm-thumb-defs.h"

static TCCScratchHandle* arm_acquire_scratch(int needs_pair, int avoid_call_regs) {
    // ARM-specific scratch allocation
    unsigned flags = 0;
    if (needs_pair) flags |= TCC_MACHINE_SCRATCH_NEEDS_PAIR;
    if (avoid_call_regs) flags |= TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS;
    
    ARMScratchHandle* handle = tcc_malloc(sizeof(*handle));
    // ... ARM-specific implementation
    return (TCCScratchHandle*)handle;
}

static int arm_can_materialize_directly(const TCCMatContext* ctx) {
    // ARM-specific encoding checks
    if (ctx->request == TCC_MAT_ADDR_STACK) {
        return tcc_machine_can_encode_stack_offset_with_param_adj(
            ctx->frame_offset, ctx->is_param, /*dest=*/0);
    }
    return 0;
}

static void arm_request_materialization(const TCCMatContext* ctx, TCCScratchHandle* dest) {
    switch (ctx->request) {
    case TCC_MAT_LOAD_SPILL:
        tcc_machine_load_spill_slot(dest->regs[0], ctx->frame_offset);
        break;
    case TCC_MAT_ADDR_STACK:
        tcc_machine_addr_of_stack_slot(dest->regs[0], ctx->frame_offset, ctx->is_param);
        break;
    // ... etc
    }
}

/* The machine interface vtable for ARM */
static const TCCMachineInterface arm_machine_interface = {
    .acquire_scratch = arm_acquire_scratch,
    .release_scratch = arm_release_scratch,
    .scratch_get_reg = arm_scratch_get_reg,
    .can_materialize_directly = arm_can_materialize_directly,
    .request_materialization = arm_request_materialization,
    .get_spill_slot_offset = arm_get_spill_slot_offset,
    .get_stack_slot_size = arm_get_stack_slot_size,
};

/* Called during ARM backend initialization */
void arm_machine_init(void) {
    tcc_machine = &arm_machine_interface;
}
```

---

## File Structure After Refactoring

```
tcc/
├── Core IR (Architecture-Independent)
│   ├── tccir.c          # Reduced from ~8300 to ~5000 lines
│   ├── tccir.h          # Clean arch-independent interface
│   ├── tccir_operand.c  # Operand handling (already clean)
│   └── tccir_operand.h
│
├── Optimizations (Pluggable)
│   ├── tccopt.c         # New: optimization passes
│   ├── tccopt.h
│   └── passes/
│       ├── opt_dce.c    # Dead code elimination
│       ├── opt_constfold.c
│       └── opt_cse.c
│
├── Machine Interface (Abstract)
│   ├── tccmachine.h     # New: machine interface definition
│   └── tccmachine.c     # Default/fallback implementations
│
└── Backends (Architecture-Specific)
    ├── arm/
    │   ├── arm-thumb-gen.c      # Reduced complexity
    │   ├── arm-thumb-machine.c  # New: implements machine interface
    │   ├── arm-thumb-opcodes.c
    │   └── arm-thumb-defs.h
    │
    └── x86/
        └── (similar structure)
```

---

## Migration Strategy

### Step-by-Step Approach

1. **Week 1: Create Abstractions**
   - Create `tccmachine.h` with abstract interface
   - Create `tccopt.h` with optimization pass structure
   - Add hooks to existing code without changing behavior

2. **Week 2: Migrate Optimizations**
   - Move FP offset cache to tccopt.c
   - Move DCE to tccopt.c
   - Move constant folding to tccopt.c
   - Keep original code as fallback (#ifdef USE_NEW_OPT)

3. **Week 3: Migrate Machine Dependencies**
   - Create `arm-thumb-machine.c` with ARM implementation
   - Migrate one materialization path at a time
   - Test after each migration

4. **Week 4: Cleanup and Testing**
   - Remove old code paths
   - Run full test suite
   - Performance benchmarking

### Backward Compatibility

During migration, use feature flags:

```c
/* tcc.h */
#define TCC_USE_NEW_MACHINE_INTERFACE 1  /* Set to 0 to use old code */
#define TCC_USE_NEW_OPTIMIZATION_MODULE 1
```

---

## Benefits of Refactoring

### Immediate Benefits

1. **Portability**: New architectures only implement machine interface
2. **Testability**: IR can be tested without a backend
3. **Maintainability**: Clear separation of concerns

### Long-term Benefits

1. **Optimization Reuse**: Optimizations work across all architectures
2. **IR Serialization**: Clean IR can be serialized/deserialized
3. **JIT Potential**: Abstract machine interface enables JIT compilation
4. **Parallel Development**: Frontend and backend teams can work independently

---

## Risks and Mitigation

| Risk | Mitigation |
|------|------------|
| Performance regression | Benchmark at each step, keep old code as fallback |
| Test failures | Run full test suite after each phase |
| Code bloat | Temporary duplication during migration, cleanup at end |
| Schedule slip | Phased approach allows stopping at any milestone |

---

## Next Steps

1. **Review this plan** with stakeholders
2. **Create feature branch** for refactoring
3. **Implement Phase 1** (Machine Interface) as proof of concept
4. **Measure impact** on code size and performance
5. **Decide** on full implementation based on results
