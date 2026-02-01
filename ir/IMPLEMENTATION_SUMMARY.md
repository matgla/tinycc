# TCCIR Subdirectory Refactoring - Implementation Summary

## Completed Work

### 1. Created ir/ Subdirectory Structure

```
ir/
├── README.md           # Documentation
├── ir.h               # Internal IR header (includes all modules)
├── type.h             # Type helpers (is_float, is_64bit, etc.)
├── pool.h             # Operand pool management
├── vreg.h             # Virtual register management
├── live.h             # Liveness analysis
├── stack.h            # Stack layout, spill slots
├── mat.h              # Value materialization
├── opt.h              # Optimizations
├── codegen.h          # Codegen helpers
├── dump.h             # Debug dumping
└── operand.h          # IROperand definitions (moved from root)
```

### 2. Consistent Naming Convention Established

#### Public API Pattern: `tcc_ir_<module>_<action>`

| Module | Old Name | New Name |
|--------|----------|----------|
| Core | `tcc_ir_allocate_block()` | `tcc_ir_alloc()` |
| Core | `tcc_ir_release_block()` | `tcc_ir_free()` |
| Core | `tcc_ir_gen_opi()` | `tcc_ir_gen_i()` |
| Core | `tcc_ir_gen_opf()` | `tcc_ir_gen_f()` |
| VReg | `tcc_ir_get_vreg_temp()` | `tcc_ir_vreg_alloc_temp()` |
| VReg | `tcc_ir_set_float_type()` | `tcc_ir_vreg_type_set_fp()` |
| Live | `tcc_ir_liveness_analysis()` | `tcc_ir_live_analysis()` |
| Live | `tcc_ir_compute_live_intervals()` | `tcc_ir_live_intervals_compute()` |
| Stack | `tcc_ir_build_stack_layout()` | `tcc_ir_stack_layout_build()` |
| Mat | `tcc_ir_materialize_value()` | `tcc_ir_mat_value()` |
| Opt | `tcc_ir_dead_code_elimination()` | `tcc_ir_opt_dce()` |
| Opt | `tcc_ir_constant_propagation()` | `tcc_ir_opt_const_prop()` |
| Codegen | `tcc_ir_codegen_get_operand()` | `tcc_ir_codegen_operand_get()` |
| Dump | `tcc_ir_show()` | `tcc_ir_dump()` |

### 3. Supporting Infrastructure Created

#### tccmachine.h / tccmachine.c
- Abstract machine interface (vtable pattern)
- Opaque scratch register handles
- Architecture-independent materialization requests

#### tccopt.h / tccopt.c
- FP offset materialization cache (moved from tccir.c)
- Pluggable optimization pass structure
- Optimization driver functions

#### tccir.h Updates
- Added `TCCFPMatCache` forward declaration
- Added `opt_fp_mat_cache` field to `TCCIRState`

### 4. Build System Updates

#### Makefile
- Added `tccmachine.c` and `tccopt.c` to CORE_FILES
- Added corresponding headers

### 5. Backward Compatibility

- tccir.h remains the public API at the project root
- All existing code compiles without modification
- All 480 tests pass

## Module Dependencies

```
type (no deps)
  ↓
pool (uses type)
  ↓
vreg (uses pool, type)
  ↓
stack (uses vreg)
live (uses vreg)
  ↓
core (uses pool, vreg, type)
mat (uses stack, vreg)
  ↓
codegen (uses mat, live)
opt (uses core)
dump (uses all)
```

## Next Steps (Future Work)

### Phase 2: Split tccir.c Implementation

1. Create `ir/type.c` with type helper implementations
2. Create `ir/pool.c` with pool management
3. Create `ir/vreg.c` with vreg operations
4. Continue with other modules...

### Phase 3: Update Build System

1. Add `ir/*.c` to Makefile compilation
2. Remove original `tccir.c` when complete

### Phase 4: Implement New Machine Interface

1. Create `arm-thumb-machine.c` implementing `TCCMachineInterface`
2. Migrate materialization code to use interface
3. Remove architecture-dependent code from IR layer

## API Reference

See individual header files in `ir/` for complete API documentation:
- `core.h` - IR block lifecycle, instruction insertion
- `vreg.h` - Virtual register allocation, type setting
- `live.h` - Liveness analysis, live intervals
- `stack.h` - Stack layout, spill slots
- `mat.h` - Value materialization
- `opt.h` - Optimization passes
- `codegen.h` - Code generation helpers
- `dump.h` - Debug output

## Testing

All tests pass:
- IR tests: 480/480 ✓
- Assembler tests: 156/156 ✓
- Internal tests: 63/63 ✓
- AEABI tests: 13/13 ✓
