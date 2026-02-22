# Phase 3: Trampoline Generation (Address-of Nested Function)

**Effort**: 5-7 days
**Files**: `tccgen.c`, `arm-thumb-gen.c`, `arm-thumb-opcodes.c`, `tccelf.c`

## Overview

When a nested function's address is taken (e.g., passed as a function pointer), generate a static trampoline in `.text` that sets up the static chain (R10) before jumping to the actual function. A writable chain slot in `.data` holds the parent's FP value.

## TODO

- [x] Add `trampoline_needed` flag to `NestedFunc` struct
- [x] Add `trampoline_sym` and `chain_slot_sym` fields to `NestedFunc` or nested `Sym`
- [x] Detect address-of-nested-function in expression evaluation (`tccgen.c`)
- [x] Differentiate direct call vs address-taken contexts for nested function symbols
- [x] Implement `create_chain_slot()` — allocate 4 bytes in `.data` section
- [x] Implement `emit_trampoline_code()` — emit Thumb-2 trampoline in `.text`
- [x] Trampoline instruction sequence: LDR R10 chain_ptr → LDR R10 [R10] → LDR PC func_addr
- [x] Add `R_ARM_ABS32` relocations for function address and chain slot address data words
- [x] At address-of site: emit IR to write current FP into chain slot (`STR R7, [chain_slot_addr]`)
- [x] At address-of site: push trampoline address as the "function pointer" value
- [x] Call `emit_trampoline_code()` during/after nested function's `gen_function()`
- [x] Create `STB_LOCAL` ELF symbols for trampoline and chain slot
- [x] Handle Thumb bit (+1) on trampoline symbol address
- [x] Document re-entrancy limitation (recursive parent corrupts chain slot)
- [x] Test with `nested_funcptr.c`, `nested_funcptr_indirect.c`
- [x] Test with `20000822-1.c` (the original GCC torture test)

## Implementation Status

**Completed:**
- Core trampoline mechanism in `tccgen.c`:
  - Detection of address-of-nested-function in `unary()` at `&` operator
  - Implicit function-to-pointer decay for nested functions (when not directly called)
  - Chain slot allocation in `.data` section via `setup_nested_func_trampoline()`
  - Trampoline code emission (20 bytes: 3×LDR + literal pool) in `emit_trampoline_for_nested_func()`
  - Relocations for function and chain slot addresses (`R_ARM_ABS32`)
- New `TCCIR_OP_INIT_CHAIN_SLOT` IR opcode to store parent FP into chain slot at address-of site
- `tcc_gen_machine_init_chain_slot()` in `arm-thumb-gen.c`: emits LDR chain_addr + STR R7 sequence
- Proper `Sym *` tracking: `trampoline_tcc_sym` and `chain_slot_tcc_sym` in `NestedFunc`
- Trampoline emission inside `compile_nested_functions()` (before clearing nested func list)
- Section buffer management via `section_prealloc()` for trampoline bytes
- All tests passing:
  - `nested_funcptr.c` → 50, 15 ✓
  - `nested_funcptr_indirect.c` → 105, 205 ✓
  - `nested_funcptr_call_twice.c` → 20, 102 ✓
  - GCC torture `20000822-1.c` → exit 0 ✓
  - Full IR test suite: 3106 passed, 0 failures ✓

## Why Not Executable Stack Trampolines?

GCC generates small code snippets on the stack. This is **ruled out for ARMv8-M**: the stack is non-executable when MPU is enabled. We must keep trampoline code in `.text`.

## Chosen Approach: Static Trampoline in `.text` + Chain Slot in `.data`

### Trampoline Layout (20 bytes total)

```asm
; In .text — trampoline for f1.f2:
__tramp_f1__f2:
    LDR   r10, [pc, #8]         ; +0: r10 = chain slot address (from +12)
    LDR   r10, [r10]            ; +4: r10 = *chain_slot = parent FP value
    LDR   pc, [pc, #4]          ; +8: pc = function address (from +16), tail call
.Ldata_chain_ptr:
    .word __chain_slot_f1__f2   ; +12: R_ARM_ABS32 → writable slot in .data
.Ldata_func:
    .word f1__f2                 ; +16: R_ARM_ABS32 → nested function

; In .data:
__chain_slot_f1__f2:
    .word 0                      ; parent writes FP here at runtime
```

PC-relative offset calculation (Thumb: PC reads as current + 4):
- LDR at +0: PC=+4, offset=8 → loads from +12 (chain_slot address)
- LDR at +8: PC=+12, offset=4 → loads from +16 (function address)

### Execution Flow

1. Parent takes `&f2` → writes parent FP to chain slot, gets trampoline address
2. Caller invokes the "function pointer" (trampoline address)
3. Trampoline loads chain slot address, dereferences to get parent FP into R10
4. Trampoline jumps to actual nested function
5. Nested function uses R10 to access captured variables

## Pseudocode: Trampoline Emission

```
function emit_trampoline_code(nested_sym, chain_slot_sym):
    tramp_start = ind

    // LDR R10, [PC, #8] — load address of chain slot from literal pool
    arm_thumb_ldr_literal_w(R10, 8)       // Thumb-2: F8DF A008

    // LDR R10, [R10, #0] — dereference: r10 = *chain_slot = parent FP
    arm_thumb_ldr_imm_w(R10, R10, 0)      // Thumb-2: F8DA A000

    // LDR PC, [PC, #4] — tail jump to nested function
    arm_thumb_ldr_literal_w(PC, 4)        // Thumb-2: F8DF F004

    // NOP (alignment)
    arm_thumb_nop()                        // Thumb-2: BF00

    // Literal pool:
    emit_word(0)                           // function address placeholder
    add_relocation(R_ARM_ABS32, nested_sym, ind - 4)

    emit_word(0)                           // chain slot address placeholder
    add_relocation(R_ARM_ABS32, chain_slot_sym, ind - 4)

    // Register trampoline symbol
    put_extern_sym_2(tramp_sym, cur_text_section, tramp_start + 1, ind - tramp_start, 0)
    //                                               +1 for Thumb bit
```

## Pseudocode: Chain Slot Creation

```
function create_chain_slot(nested_sym):
    data_sec = tcc_state->data_section
    offset = section_add(data_sec, 4, 4)   // 4 bytes, 4-byte aligned

    chain_slot_name = concat("__chain_", nested_sym->name)
    chain_slot_sym = put_elf_sym(...)       // STB_LOCAL

    // Initialize to 0
    write32le(data_sec->data + offset, 0)

    return chain_slot_sym
```

## Pseudocode: Address-of Detection & IR Generation

```
// In expression evaluation (tccgen.c):
function handle_symbol_reference(sym):
    if sym is a nested function:
        if context is direct function call (immediately followed by '('):
            // Direct call — use SET_CHAIN (Phase 2) + BL
            gen_call_nested_direct(sym, args)
        else:
            // Address taken — need trampoline
            sym->nested_addr_taken = 1
            gen_addr_of_nested_func(sym)

function gen_addr_of_nested_func(nested_sym):
    // 1. Write current FP to chain slot
    emit IR: chain_addr <- SYMBOL(__chain_slot_f1__f2)
    emit IR: STORE [chain_addr], FP

    // 2. Push trampoline address as function pointer value
    emit IR: result <- SYMBOL(__tramp_f1__f2 + 1)  // +1 Thumb bit
    vpush(result)
```

## Re-entrancy Limitation

This approach is **NOT re-entrant**: if the parent function recurses, each invocation writes the same `.data` chain slot. The last writer wins, corrupting earlier invocations' nested function pointers.

**Acceptable for now**: most GCC torture tests don't combine recursion + nested function pointers.

**Future fix (deferred)**: Stack-allocated trampoline descriptors:
- Allocate `{func_addr, chain_value}` pair on parent stack
- Trampoline reads from descriptor address passed via R12 (IP)
- Requires `alloca`-like mechanism or static stack reservation

## Test Cases (Phase 3)

See [tests/nested_funcptr.c](tests/nested_funcptr.c), [tests/nested_funcptr_indirect.c](tests/nested_funcptr_indirect.c), [tests/nested_funcptr_call_twice.c](tests/nested_funcptr_call_twice.c), [tests/nested_recursive_parent.c](tests/nested_recursive_parent.c).

Final validation: `20000822-1.c` from GCC torture suite.
