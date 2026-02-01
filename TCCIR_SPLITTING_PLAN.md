# TCCIR Splitting Plan

## Current State

- **File**: `tccir.c` - 8,277 lines
- **Problem**: Monolithic file mixing multiple concerns
- **Goal**: Split into logical, manageable modules

## Proposed Module Structure

```
tccir/
├── tccir_core.c/h          # IR block lifecycle, basic operations
├── tccir_debug.c/h         # Debug dumping, tracing
├── tccir_type.c/h          # Type helpers (is_float, is_64bit, etc.)
├── tccir_pool.c/h          # IROperand pool management
├── tccir_vreg.c/h          # Virtual register management
├── tccir_live.c/h          # Live intervals, liveness analysis
├── tccir_stack.c/h         # Stack layout, spill slot management
├── tccir_materialize.c/h   # Value materialization (NEW - arch-dependent!)
├── tccir_opt.c/h           # Optimizations (DCE, CSE, etc.)
└── tccir_codegen.c/h       # Codegen helpers
```

---

## Detailed Module Breakdown

### 1. tccir_core.c/h (Core IR Operations)

**Current Lines**: ~1,200 (various sections)

**Contains**:
- `tcc_ir_allocate_block()` / `tcc_ir_release_block()`
- `tcc_ir_put()` - Main IR instruction insertion
- `tcc_ir_add_function_parameters()`
- `tcc_ir_inline_asms_*()` (if CONFIG_TCC_ASM)
- Block-level state management

**Public API**:
```c
TCCIRState *tcc_ir_allocate_block(void);
void tcc_ir_release_block(TCCIRState *ir);
int tcc_ir_put(TCCIRState *ir, TccIrOp op, SValue *src1, SValue *src2, SValue *dest);
void tcc_ir_add_function_parameters(TCCIRState *ir, CType *func_type);
```

**Dependencies**: tccir_pool, tccir_vreg, tccir_type

---

### 2. tccir_debug.c/h (Debug/Dump Functions)

**Current Lines**: ~270 (lines 60-330)

**Contains**:
- `tcc_dump_svalue_short_to()`
- `tcc_dump_quadruple_to()`
- `tcc_try_dump_thumb_with_objdump()`
- Debug color codes, formatting helpers

**Public API**:
```c
void tcc_ir_dump(TCCIRState *ir, FILE *out);
void tcc_ir_dump_instruction(TCCIRState *ir, int idx, FILE *out);
void tcc_ir_dump_svalue(const SValue *sv, FILE *out);
```

**Dependencies**: None (uses only core types)

---

### 3. tccir_type.c/h (Type Helpers)

**Current Lines**: ~70 (lines 333-403)

**Contains**:
- `tcc_ir_is_float_type()`
- `tcc_ir_is_double_type()`
- `tcc_ir_is_64bit_type()`
- `tcc_ir_is_spilled()`
- `tcc_ir_is_fpu_operation()`

**Public API**:
```c
int tcc_ir_is_float_type(int t);
int tcc_ir_is_double_type(int t);
int tcc_ir_is_64bit_type(int t);
int tcc_ir_is_spilled(SValue *sv);
int tcc_ir_is_64bit(int t);
```

**Dependencies**: None

---

### 4. tccir_pool.c/h (Operand Pool Management)

**Current Lines**: ~150 (lines 609-690, scattered)

**Contains**:
- `tcc_ir_iroperand_pool_add()`
- `tcc_ir_set_dest_jump_target()`
- `tcc_ir_pools_init()` / `tcc_ir_pools_free()`
- Pool capacity management

**Public API**:
```c
int tcc_ir_iroperand_pool_add(TCCIRState *ir, IROperand irop);
void tcc_ir_set_dest_jump_target(TCCIRState *ir, int instr_idx, int target);
void tcc_ir_pools_init(TCCIRState *ir);
void tcc_ir_pools_free(TCCIRState *ir);
```

**Dependencies**: None

---

### 5. tccir_vreg.c/h (Virtual Register Management)

**Current Lines**: ~200 (lines 498-560, 1769-1940)

**Contains**:
- `tcc_ir_get_vreg_temp()` - Allocate temp vreg
- `tcc_ir_get_vreg_var()` - Allocate variable vreg
- `tcc_ir_get_vreg_param()` - Allocate parameter vreg
- `tcc_ir_set_addrtaken()` / `tcc_ir_set_float_type()`
- `tcc_ir_set_llong_type()` / `tcc_ir_set_original_offset()`
- `tcc_ir_get_reg_type()` / `tcc_ir_is_vreg_valid()`
- `tcc_ir_mark_return_value_incoming_regs()`

**Public API**:
```c
int tcc_ir_get_vreg_temp(TCCIRState *ir);
int tcc_ir_get_vreg_var(TCCIRState *ir);
int tcc_ir_get_vreg_param(TCCIRState *ir);
void tcc_ir_set_float_type(TCCIRState *ir, int vreg, int is_float, int is_double);
void tcc_ir_set_llong_type(TCCIRState *ir, int vreg);
int tcc_ir_get_reg_type(TCCIRState *ir, int vreg);
int tcc_ir_is_vreg_valid(TCCIRState *ir, int vr);
```

**Dependencies**: tccir_type

---

### 6. tccir_live.c/h (Live Intervals & Liveness)

**Current Lines**: ~600 (lines 561-610, 1944-2430)

**Contains**:
- `tcc_ir_compute_live_intervals()`
- `tcc_ir_liveness_analysis()`
- `tcc_ir_patch_live_intervals_registers()`
- `tcc_ir_extend_param_intervals()`
- `tcc_ir_extend_intervals_for_backward_jumps()`
- `tcc_ir_init_interval_starts()` / `tcc_ir_clear_live_intervals()`
- `tcc_ir_avoid_spilling_stack_passed_params()`

**Public API**:
```c
void tcc_ir_compute_live_intervals(TCCIRState *ir);
void tcc_ir_liveness_analysis(TCCIRState *ir);
void tcc_ir_patch_live_intervals_registers(TCCIRState *ir);
void tcc_ir_extend_param_intervals(TCCIRState *ir);
```

**Dependencies**: tccir_vreg, tccir_core

---

### 7. tccir_stack.c/h (Stack Layout)

**Current Lines**: ~350 (lines 2459-2765)

**Contains**:
- `tcc_ir_build_stack_layout()`
- `tcc_ir_stack_layout_*` - hash table, slot management
- `tcc_ir_materialization_slot()` / `tcc_ir_materialization_offset()`
- `tcc_ir_assign_physical_register()`

**Public API**:
```c
void tcc_ir_build_stack_layout(TCCIRState *ir);
const TCCStackSlot *tcc_ir_materialization_slot(const TCCIRState *ir, const SValue *sv);
int tcc_ir_materialization_offset(const TCCIRState *ir, const SValue *sv);
void tcc_ir_assign_physical_register(TCCIRState *ir, int vreg, int offset, int r0, int r1);
```

**Dependencies**: tccir_vreg

---

### 8. tccir_materialize.c/h (Value Materialization) ⚠️ ARCH-DEPENDENT

**Current Lines**: ~750 (lines 2767-3525)

**⚠️ WARNING**: This module contains ARCHITECTURE-DEPENDENT code!

**Contains**:
- `tcc_ir_materialize_value()` - Uses tcc_machine_* calls
- `tcc_ir_materialize_const_to_reg()`
- `tcc_ir_materialize_addr()`
- `tcc_ir_materialize_dest()`
- `tcc_ir_materialize_*_ir()` variants for IROperand

**Options**:
1. **Move to backend** - Most correct, but breaks existing code
2. **Keep in IR with machine interface** - Use tccmachine.h abstractions
3. **Hybrid** - Keep high-level logic in IR, move arch-specific to backend

**Recommendation**: Option 2 - Use the new machine interface

**Public API**:
```c
void tcc_ir_materialize_value(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
void tcc_ir_materialize_const_to_reg(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
void tcc_ir_materialize_addr(TCCIRState *ir, SValue *sv, TCCMaterializedAddr *result, int dest_reg);
void tcc_ir_materialize_dest(TCCIRState *ir, SValue *dest, TCCMaterializedDest *result);
/* IROperand variants... */
```

**Dependencies**: tccmachine.h, tccir_stack

---

### 9. tccir_opt.c/h (Optimizations)

**Current Lines**: ~4,000 (lines 4054-8094)

**Contains**:
- `tcc_ir_dead_code_elimination()`
- `tcc_ir_dead_store_elimination()`
- `tcc_ir_cse_boolean()`
- `tcc_ir_idempotent_boolean_simplify()`
- `tcc_ir_return_value_opt()`
- `tcc_ir_boolean_simplify()`
- `tcc_ir_constant_propagation()`
- `tcc_ir_constant_propagation_tmp()`
- `tcc_ir_copy_propagation()`
- `tcc_ir_store_load_forwarding()`
- `tcc_ir_redundant_store_elimination()`
- `tcc_ir_arithmetic_cse()`

**Public API**:
```c
int tcc_ir_dead_code_elimination(TCCIRState *ir);
int tcc_ir_dead_store_elimination(TCCIRState *ir);
int tcc_ir_cse_boolean(TCCIRState *ir);
int tcc_ir_constant_propagation(TCCIRState *ir);
int tcc_ir_copy_propagation(TCCIRState *ir);
int tcc_ir_store_load_forwarding(TCCIRState *ir);
int tcc_ir_redundant_store_elimination(TCCIRState *ir);
int tcc_ir_arithmetic_cse(TCCIRState *ir);
```

**Dependencies**: tccir_core, tccir_vreg

---

### 10. tccir_codegen.c/h (Codegen Helpers)

**Current Lines**: ~200 (lines 4062+, scattered)

**Contains**:
- `tcc_ir_codegen_get_operand()`
- `tcc_ir_fill_registers()` / `tcc_ir_fill_registers_ir()`
- Register allocation integration

**Public API**:
```c
int tcc_ir_codegen_get_operand(TCCIRState *ir, const IRQuadCompact *q, int slot, SValue *out);
void tcc_ir_fill_registers(TCCIRState *ir, SValue *sv);
void tcc_ir_fill_registers_ir(TCCIRState *ir, IROperand *op);
void tcc_ir_register_allocation_params(TCCIRState *ir);
```

**Dependencies**: tccir_materialize

---

## Makefile Updates

```makefile
# Current:
CORE_FILES = tccir.c tccir_operand.c tccls.c ...

# Proposed:
TCCIR_FILES = tccir_core.c tccir_debug.c tccir_type.c tccir_pool.c \
              tccir_vreg.c tccir_live.c tccir_stack.c tccir_materialize.c \
              tccir_opt.c tccir_codegen.c
TCCIR_HEADERS = tccir.h tccir_core.h tccir_debug.h tccir_type.h tccir_pool.h \
                tccir_vreg.h tccir_live.h tccir_stack.h tccir_materialize.h \
                tccir_opt.h tccir_codegen.h

CORE_FILES = $(TCCIR_FILES) tccir_operand.c tccls.c ...
CORE_FILES += tcc.h config.h libtcc.h tcctok.h tccir_operand.h tccld.h $(TCCIR_HEADERS)
```

---

## Migration Strategy

### Phase 1: Create New Files (1 week)

1. Create all new header files with proper include guards
2. Extract functions to new .c files WITHOUT removing from tccir.c
3. Use `#include` to bring them together
4. Test after each extraction

```c
/* tccir.c - during migration */
#include "tccir_debug.c"  /* Will become proper compilation unit */
#include "tccir_type.c"
/* ... etc ... */
```

### Phase 2: Update Build System (2 days)

1. Update Makefile to compile separate .o files
2. Update include paths
3. Test full build

### Phase 3: Clean Up (2 days)

1. Remove original functions from tccir.c
2. Keep tccir.c as thin orchestration layer OR remove entirely
3. Update all #include references

### Phase 4: Verify (2 days)

1. Run full test suite
2. Performance benchmarking
3. Code review

---

## Benefits

| Benefit | Description |
|---------|-------------|
| **Maintainability** | Each module has single responsibility |
| **Compile Time** | Incremental builds faster |
| **Testability** | Can test modules in isolation |
| **Code Review** | Smaller files easier to review |
| **Onboarding** | New devs understand codebase faster |
| **Parallel Development** | Multiple devs can work on different modules |

---

## Risks & Mitigation

| Risk | Mitigation |
|------|------------|
| Break existing code | Migrate incrementally, test after each step |
| Circular dependencies | Careful header design, forward declarations |
| Performance regression | Benchmark at each phase |
| Merge conflicts | Coordinate with other developers |

---

## File Size Comparison

| File | Current Lines | After Split |
|------|---------------|-------------|
| tccir.c | 8,277 | 0 (removed) |
| tccir_core.c | - | ~1,200 |
| tccir_debug.c | - | ~270 |
| tccir_type.c | - | ~70 |
| tccir_pool.c | - | ~150 |
| tccir_vreg.c | - | ~200 |
| tccir_live.c | - | ~600 |
| tccir_stack.c | - | ~350 |
| tccir_materialize.c | - | ~750 |
| tccir_opt.c | - | ~4,000 |
| tccir_codegen.c | - | ~200 |
| **Total** | **8,277** | **~7,790** |

(Slight reduction due to removed duplicate code/comments)

---

## Recommended Order of Extraction

1. **tccir_type.c** - Easiest, no dependencies
2. **tccir_debug.c** - Self-contained, uses types
3. **tccir_pool.c** - Core infrastructure
4. **tccir_vreg.c** - Depends on pool
5. **tccir_stack.c** - Depends on vreg
6. **tccir_live.c** - More complex, depends on vreg
7. **tccir_core.c** - Orchestration, depends on all above
8. **tccir_materialize.c** - Arch-dependent, use machine interface
9. **tccir_codegen.c** - Depends on materialize
10. **tccir_opt.c** - Largest, depends on core

---

## Open Questions

1. Should tccir_opt.c be further split into:
   - tccir_opt_dce.c
   - tccir_opt_cse.c
   - tccir_opt_propagation.c
   - etc.?

2. Should we keep tccir.c as a thin wrapper that includes all modules,
   or remove it entirely?

3. How to handle the inline ASM code (CONFIG_TCC_ASM)?
   - Keep in tccir_core.c?
   - Separate tccir_asm.c?

4. Should the architecture-dependent materialization code move entirely
   to the backend (arm-thumb-gen.c)?
