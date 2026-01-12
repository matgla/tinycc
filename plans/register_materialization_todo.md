# Register Materialization Consolidation - TODO

## Completed

- [x] **Phase 1**: Add `tcc_machine_load_constant()` API to tcc.h and implement in arm-thumb-gen.c
- [x] **Phase 2**: Added `tcc_ir_materialize_const_to_reg()` as explicit helper (instead of automatic materialization in `tcc_ir_materialize_value()`)
- [x] **Phase 3**: Add `tcc_machine_load_cmp_result()` and `tcc_machine_load_jmp_result()` APIs
- [x] **Phase 4**: Machine APIs implemented for VT_CMP and VT_JMP handling
- [x] **Phase 5 (partial)**: Added `need_src1_in_reg`/`need_src2_in_reg` flags in IR code generation loop
  - MUL, DIV, UDIV, IMOD, UMOD, UMULL now use IR-level `tcc_ir_materialize_const_to_reg()`
  - Backend `thumb_materialize_binop32_sources()` still exists but is now redundant for constants
- [x] **Tests**: All tests passing

## Remaining

- [x] **Phase 5**: Removed `thumb_materialize_binop32_sources()`
  - Simplified `thumb_emit_regonly_binop32()` - now only handles VT_LVAL fallback
  - Simplified `thumb_emit_mod32()` similarly
  - Files: [arm-thumb-gen.c](../arm-thumb-gen.c)

- [ ] **Phase 6**: Simplify `load_to_dest()` by removing VT_CONST/VT_CMP/VT_JMP handling
  - **Status**: Deferred - requires changing all callers to use IR-level materialization first
  - Current `load_to_dest()` still handles these cases as fallback
  - The new `tcc_machine_load_constant/cmp_result/jmp_result` functions provide the same functionality at IR level
  - Files: [arm-thumb-gen.c](../arm-thumb-gen.c) lines 2756-2891

## Design Notes

### Why Explicit Materialization?

During implementation we discovered that **automatic materialization of all constants breaks operations that expect immediates**:
- Shift counts must remain as VT_CONST for 64-bit shifts
- Many ARM instructions have flexible second operand (immediate or register)
- Blindly materializing all constants wastes registers

**Solution**: `tcc_ir_materialize_const_to_reg()` is an **explicit** helper called only when an operation specifically needs a constant in a register.

### New APIs Added

**tcc.h / arm-thumb-gen.c:**
```c
void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit);
void tcc_machine_load_cmp_result(int dest_reg, int condition_code);
void tcc_machine_load_jmp_result(int dest_reg, int jmp_addr, int invert);
```

**tccir.h / tccir.c:**
```c
void tcc_ir_materialize_const_to_reg(TCCIRState *ir, SValue *sv, TCCMaterializedValue *result);
```

## Related Files

- [cozy-wishing-kazoo.md](cozy-wishing-kazoo.md) - Original plan
- [load_spill_refactor_plan.md](load_spill_refactor_plan.md) - Background context
- [../docs/IR_MACHINE_CONTRACT.md](../docs/IR_MACHINE_CONTRACT.md) - IR/backend contract
