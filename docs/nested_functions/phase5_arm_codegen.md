# Phase 5: ARM Thumb-2 Code Generation

**Effort**: 3-5 days
**Files**: `arm-thumb-gen.c`, `arm-thumb-opcodes.c`, `arm-thumb-opcodes.h`, `ir/codegen.c`

## Overview

Lower chain-relative IR operations to Thumb-2 instructions. Modify prologue/epilogue to save/restore R10. Emit trampoline machine code and chain slots. Lower `SET_CHAIN` to `MOV R10, R7`.

## TODO

- [x] Modify `gen_func_prologue()` to push R10 when `ir->has_static_chain`
- [x] Verify R10 is already in the callee-saved register set in `arch/armv8m.c` (`static_chain_reg = 10`)
- [x] Modify `gen_func_epilogue()` to pop R10 (via existing push_mask — R10 included in `pushed_registers`)
- [x] Implement chain-relative `LDR.W Rd, [R10, #offset]` codegen path (via `base_reg = architecture_config.static_chain_reg`)
- [x] Implement chain-relative `STR.W Rd, [R10, #offset]` codegen path (via `base_reg = architecture_config.static_chain_reg`)
- [x] Handle large offsets (>4095) via scratch register + register-offset addressing (fallback in `load_word_from_base`/`store_word_to_base`)
- [x] Implement `tcc_gen_machine_set_chain()` — emit `MOV R10, R7` (Thumb-2)
- [x] Add `TCCIR_OP_SET_CHAIN` case in `ir/codegen.c` dispatch
- [x] Implement `emit_trampoline_for_nested_func()` in `tccgen.c`:
  - [x] `LDR.W R10, [PC, #offset]` — load chain slot address
  - [x] `LDR.W R10, [R10, #0]` — dereference chain slot
  - [x] `LDR.W PC, [PC, #offset]` — branch to nested function
  - [x] NOP for alignment if needed
  - [x] Emit data words (function addr, chain slot addr) with R_ARM_ABS32 relocations
- [x] Implement chain slot allocation — allocate 4 bytes in `.data` section (`setup_nested_func_trampoline()`)
- [x] Create chain slot ELF symbol (`__chain_<name>`, STB_LOCAL)
- [x] Create trampoline ELF symbol (`__tramp_<name>`, STB_LOCAL, +1 Thumb bit)
- [x] Wire trampoline emission into `compile_nested_functions()` flow (emit only if `trampoline_needed`)
- [x] Test trampoline disassembly matches expected Thumb-2 encoding (all tests pass)

## Register Conventions

| Register | Role | Notes |
|----------|------|-------|
| R0-R3 | Arguments / return | Caller-saved |
| R7 | Frame pointer | Thumb convention |
| R10 | Static chain | Callee-saved, loaded before nested call |
| R12 | IP (scratch) | Used by trampoline if needed |
| LR / R14 | Link register | Saved in prologue |
| PC / R15 | Program counter | Trampoline branch target |

## Prologue/Epilogue Pseudocode

```
function gen_func_prologue(ir):
    push_mask = compute_callee_saved_registers(ir)

    if ir->has_static_chain:
        push_mask |= (1 << 10)   // R10 callee-saved
        // R10 arrives with chain value — no extra setup needed

    emit PUSH {push_mask}
    if need_frame_pointer:
        emit MOV R7, SP
    emit SUB SP, SP, #frame_size

function gen_func_epilogue(ir):
    emit ADD SP, SP, #frame_size
    emit POP {push_mask | (1 << PC)}   // restores R10 and returns
```

## Chain-relative Load/Store Codegen

```
function codegen_load_via_chain(instruction):
    base_reg = get_physical_reg(instruction.src1)   // R10
    offset = instruction.offset
    dest_reg = get_physical_reg(instruction.dest)

    if 0 <= offset <= 4095:
        // Thumb-2 LDR.W Rd, [Rn, #imm12]
        emit_thumb32_ldr_imm12(dest_reg, base_reg, offset)
    else:
        // Large offset needs scratch register
        scratch = get_scratch_register()
        emit_thumb32_movw(scratch, offset & 0xFFFF)
        if offset > 0xFFFF:
            emit_thumb32_movt(scratch, (offset >> 16) & 0xFFFF)
        emit_thumb32_ldr_reg(dest_reg, base_reg, scratch)

function codegen_store_via_chain(instruction):
    base_reg = get_physical_reg(instruction.dest_addr)  // R10
    offset = instruction.offset
    src_reg = get_physical_reg(instruction.src1)

    if 0 <= offset <= 4095:
        emit_thumb32_str_imm12(src_reg, base_reg, offset)
    else:
        scratch = get_scratch_register()
        emit_thumb32_movw(scratch, offset & 0xFFFF)
        if offset > 0xFFFF:
            emit_thumb32_movt(scratch, (offset >> 16) & 0xFFFF)
        emit_thumb32_str_reg(src_reg, base_reg, scratch)
```

## SET_CHAIN Lowering

```
function codegen_set_chain(instruction):
    // Parent is about to call a nested function.
    // Copy FP to static chain register: MOV R10, R7
    // Thumb-2: 0x4637 would be MOV R7, R6 — wrong
    // High register MOV: 0x46BA = MOV R10, R7  (01000110 10 111 010)
    emit_thumb16(0x46BA)   // MOV R10, R7
```

## Trampoline Machine Code Layout (24 bytes)

```
Offset  Encoding         Instruction              Comment
------  --------         -----------              -------
+0      F8DF A008        LDR.W R10, [PC, #8]     R10 = &chain_slot (from +16)
+4      F8DA A000        LDR.W R10, [R10, #0]    R10 = *chain_slot (FP value)
+8      F8DF F004        LDR.W PC, [PC, #4]      PC = func_addr (from +16)
+12     BF00             NOP                      alignment padding
+14     BF00             NOP                      alignment padding
+16     [4 bytes]        .word chain_slot_addr    R_ARM_ABS32 relocation
+20     [4 bytes]        .word func_addr | 1      R_ARM_ABS32 relocation (+1 Thumb)
```

Total: 24 bytes per trampoline.

### Trampoline Emission Pseudocode

```
function emit_trampoline_code(nested_sym, chain_slot_sym):
    tramp_name = mangle("__tramp_", nested_sym->name)
    tramp_start = ind

    // LDR.W R10, [PC, #8]  — PC+4+8 = tramp_start+12, but Thumb PC = inst+4
    // At offset +0: PC = tramp_start+4, want data at +16, offset = 16-4 = 12
    // Wait: recalculate for Thumb-2 LDR literal
    // PC reads as instruction_address + 4, word-aligned down
    // LDR.W Rt, [PC, #imm12] — PC is Align(PC,4)
    // Must compute exact offsets at emission time

    arm_thumb_ldr_pc_literal_w(REG_R10, chain_slot_ptr_offset)  // +0
    arm_thumb_ldr_imm_w(REG_R10, REG_R10, 0)                   // +4
    arm_thumb_ldr_pc_literal_w(REG_PC, func_ptr_offset)         // +8
    arm_thumb_nop16()                                            // +12
    arm_thumb_nop16()                                            // +14

    // Data words at +16 and +20
    chain_slot_data_offset = ind
    emit_word(0)
    add_reloc(cur_text_section, chain_slot_sym, chain_slot_data_offset, R_ARM_ABS32)

    func_addr_data_offset = ind
    emit_word(0)
    add_reloc(cur_text_section, nested_sym, func_addr_data_offset, R_ARM_ABS32)

    // Register trampoline symbol (address +1 for Thumb bit)
    put_extern_sym_2(tramp_sym, cur_text_section,
                     tramp_start | 1, ind - tramp_start, 0)
```

### Chain Slot Creation Pseudocode

```
function create_chain_slot(nested_sym):
    slot_name = mangle("__chain_", nested_sym->name)

    // Allocate in .data (not .bss — explicit zero init)
    data_sec = s1->data_section
    offset = section_add(data_sec, 4, 4)  // 4 bytes, 4-byte align
    write32le(data_sec->data + offset, 0)  // init to 0

    // Create local ELF symbol
    slot_sym = put_elf_sym(s1->symtab_section, offset, 4,
                           ELF32_ST_INFO(STB_LOCAL, STT_OBJECT),
                           0, data_sec->sh_num, slot_name)
    return slot_sym
```

## Parent Chain Slot Write

Before calling a nested function through a pointer, the parent must write its FP to the chain slot:

```
function gen_write_chain_slot(chain_slot_sym):
    // STR R7, [addr_of_chain_slot]
    // This is an absolute address store — needs full address materialization
    scratch = get_scratch_register()
    emit_movw_movt(scratch, chain_slot_sym)   // with R_ARM_ABS32 or MOVW/MOVT reloc pair
    emit_str(R7, scratch, 0)                  // STR R7, [scratch]
```

## Test Cases

| Test File | Validates |
|-----------|-----------|
| `nested_basic.c` | Prologue/epilogue R10 save, direct call SET_CHAIN |
| `nested_capture_read.c` | LDR.W via chain (R10+offset) |
| `nested_capture_write.c` | STR.W via chain (R10+offset) |
| `nested_funcptr.c` | Trampoline emission, chain slot, indirect call |
| `nested_funcptr_indirect.c` | Trampoline passed to external function |
| `nested_struct_return.c` | LDR/STR via chain with struct size > 4 |
