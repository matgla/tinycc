# Phase 2: Static Chain — Captured Variable Access

**Effort**: 3-5 days
**Files**: `tccgen.c`, `tcc.h`, `tccir.h`, `ir/core.c`, `ir/core.h`, `tccls.c`, `arch/armv8m.c`, `arm-thumb-defs.h`

## Overview

Enable nested functions to read/write variables from the parent's stack frame via a static chain pointer passed in R10 (following GCC's ARM convention). Includes a token pre-scan to mark captured variables as address-taken before the parent's IR is generated.

## TODO

- [x] Define `REG_STATIC_CHAIN 10` in `arm-thumb-defs.h`
- [x] Add `static_chain_reg` field to `ArchitectureConfig` in `tcc.h`
- [x] Set `.static_chain_reg = 10` in `arch/armv8m.c`
- [x] Add `has_static_chain`, `static_chain_vreg` fields to `TCCIRState`
- [x] Add `captured_offsets[]`, `captured_vregs[]`, `captured_tokens[]`, `nb_captured` fields to `NestedFunc` struct
- [x] Implement `prescan_captured_vars()` — token scan for parent variable references
- [x] Call `prescan_captured_vars()` in `decl(VT_LOCAL)` right after `skip_or_save_block()`
- [x] Mark captured parent symbols with `addrtaken` + `tcc_ir_set_addrtaken()` to force stack spill
- [x] Store captured variable FP offsets in `NestedFunc.captured_offsets[]`
- [x] Resolve captured variable offsets post-register-allocation (lookup vreg → `allocation.offset`)
- [x] In nested `gen_function()`: detect `has_static_chain`, allocate chain vreg
- [x] Emit chain vreg initialization: `chain_vreg = R10` at function entry
- [x] Modify variable resolution in nested function: detect parent-scope variables (`tok_identifier`)
- [x] Generate chain-relative LOAD/STORE IR for captured variable access (base=R10, offset=parent FP offset)
- [x] In register allocator (`tccls.c`): exclude R10 from allocatable set when `has_static_chain`
- [x] Pre-assign chain vreg interval to R10 (like parameter incoming_reg)
- [x] In parent's call to nested function: emit `SET_CHAIN` (MOV R10, R7) before call
- [x] Detect nested function at call site via `vtop->sym->a.nested_func` (not `vtop->type.ref`)
- [x] Add `SET_CHAIN` to real codegen pass in `ir/codegen.c` (not just dry-run)
- [x] Add `SET_CHAIN` to `tcc_ir_get_op_name()` in `ir/dump.c`
- [x] Name mangling: GCC convention `funcname.N` via `asm_label` + `tok_alloc`
- [x] `VT_STATIC` for nested function symbols (STB_LOCAL binding)
- [x] Save/restore `cur_text_section` + `ind` after each nested `gen_function()` (safety resets)
- [x] Save/restore debug state (`debug_info`, `debug_info_root`) via `tcc_debug_save_state()`/`tcc_debug_restore_state()`
- [x] Nested function code emitted BEFORE parent code in `.text` (layout: nested funcs → parent)
- [x] Parent ELF symbol updated post-nested-compilation (`func_ind = ind; put_extern_sym(...)`)
- [x] Test with `nested_capture_read.c` — **PASS** ✓
- [x] Test with `nested_capture_write.c` — **PASS** ✓
- [x] Test with `nested_capture_multiple.c` — **PASS** ✓
- [x] Test with `nested_multiple.c` — **PASS** ✓
- [x] Test with `nested_basic.c`, `nested_basic_args.c`, `nested_basic_simple.c` — **PASS** ✓
- [x] Test with `nested_direct_call_args.c` — **PASS** ✓
- [x] Test with `nested_shadowing.c` — **PASS** ✓

### Known Limitations (out of scope for Phase 2)

- [ ] `nested_capture_array.c` — array capture fails ("pointer expected")
- [ ] `nested_multi_level.c` — multi-level nesting fails ("undeclared" — prescan only sees immediate parent)
- [ ] `nested_recursive_parent.c` — captured var in recursive parent fails ("undeclared")
- [ ] `nested_struct_return.c` — struct return from nested function fails (type mismatch)
- [ ] `nested_funcptr.c`, `nested_funcptr_call_twice.c`, `nested_funcptr_indirect.c` — function pointer / trampoline support (Phase 3)

## Key Design: Token Pre-scan

The pre-scan runs at parse time (during `decl(VT_LOCAL)` right after `skip_or_save_block`) — before the parent's `block(0)` generates IR for variables that might be captured. This ensures captured variables are marked `addrtaken` early enough.

```
function prescan_captured_vars(nf, parent_local_stack):
    // Walk the saved TokenString looking for identifiers
    // that match parent local variable names.

    tokens = tok_str_buf(nf->func_str)
    pos = 0
    while tokens[pos] != TOK_EOF:
        t = tokens[pos]
        if t >= TOK_IDENT:
            sym = lookup in parent_local_stack for token t
            if sym != NULL && sym->r & VT_LOCAL:
                sym->type.t |= VT_ADDRTAKEN  // force to stack
                nf->captured_offsets[nf->nb_captured++] = sym->c
        pos = advance past token + associated data

    // NOTE: This is a shallow scan. If the nested function declares
    // a local with the same name as a parent variable, we over-mark.
    // Conservative over-marking is safe (extra stack spills) but suboptimal.
```

## Key Design: Captured Variable Resolution

During nested function compilation, variable lookups that find parent-scope symbols must produce chain-relative addressing instead of FP-relative:

```
// Before compiling nested function:
parent_local_stack_top = local_stack

// Inside nested gen_function, in variable resolution:
function resolve_variable_access(tok_id):
    sym = sym_find(tok_id)
    if sym == NULL: return NULL

    if sym->r & VT_LOCAL:
        if sym was pushed before parent_local_stack_top:
            // Captured variable — access via chain register
            return svalue_chain_relative(sym->c)  // offset from parent FP
        else:
            // Nested function's own local — normal FP access
            return svalue_fp_relative(sym->c)

    return sym  // global/external — unchanged

function svalue_chain_relative(parent_offset):
    // Use existing LOAD/STORE with chain_vreg as base (no new SValue kind)
    // Option B from plan: check ir->has_static_chain + sym_scope
    sv.r = VT_LOCAL | VT_LVAL
    sv.c.i = parent_offset
    // Tag this SValue so IR emitter uses chain_vreg instead of FP
    // Implementation: check if sym_scope < nested function scope
    return sv
```

## Key Design: Chain Vreg Setup

```
function gen_function_nested_setup(ir):
    if not ir->has_static_chain: return

    // Allocate a vreg for the chain — behaves like a parameter in R10
    chain_vreg = tcc_ir_alloc_local_vreg(ir)
    ir->static_chain_vreg = chain_vreg

    // The register allocator will:
    // 1. Exclude R10 from general allocation
    // 2. Pre-assign chain_vreg to R10
    // 3. Mark its live range as the entire function (conservative)
```

## Key Design: Register Allocation

```
function tcc_ls_allocate_registers(ls, params, float_params, spill_base):
    ...existing setup...

    if current function has_static_chain:
        // Remove R10 from allocatable set
        ls->registers_map &= ~(1ULL << 10)

        // Pre-assign chain vreg to R10
        chain_interval = find_interval(ls, ir->static_chain_vreg)
        chain_interval->r0 = 10
```

## Key Design: Direct Call Chain Setup

```
// In parent's gfunc_call path, when calling nested function:
function gen_call(func_sym, args):
    if func_sym is a nested function:
        // Emit: MOV R10, R7 (pass parent FP as chain)
        emit TCCIR_OP_SET_CHAIN  // implicit: R10 <- FP
    emit TCCIR_OP_FUNCCALLVAL func_sym, args...
```

## Test Cases (Phase 2)

See [tests/nested_capture_read.c](tests/nested_capture_read.c), [tests/nested_capture_write.c](tests/nested_capture_write.c), [tests/nested_capture_multiple.c](tests/nested_capture_multiple.c), [tests/nested_capture_array.c](tests/nested_capture_array.c), [tests/nested_direct_call_args.c](tests/nested_direct_call_args.c), [tests/nested_shadowing.c](tests/nested_shadowing.c).
