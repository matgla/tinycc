# Phase 6: Linker Support

**Effort**: 1-2 days
**Files**: `arm-link.c`, `tccelf.c`

## Overview

Enable relocations and symbol visibility for nested function artifacts: nested function symbols, trampoline symbols, and chain slot symbols. Almost entirely covered by existing `R_ARM_ABS32` relocation handling — the main work is ensuring correct symbol binding.

## TODO

- [x] Verify `R_ARM_ABS32` relocs emitted by trampoline resolve correctly in `relocate_section()` (`arm-link.c`)
- [x] Ensure nested function symbol `.text` address includes +1 Thumb bit in relocation value
- [x] Set nested function symbols to `STB_LOCAL` binding (not exported)
- [x] Set trampoline symbols (`__tramp_*`) to `STB_LOCAL` binding
- [x] Set chain slot symbols (`__chain_*`) to `STB_LOCAL` binding
- [x] Verify no duplicate symbol names when parent is called recursively (unique mangling)
- [x] Test ELF output with `arm-none-eabi-objdump -t` to verify symbol table
- [x] Test ELF output with `arm-none-eabi-objdump -r` to verify relocations

## Relocations

The trampoline uses two `R_ARM_ABS32` entries in `.text` (data words embedded after instructions):

| Data Word | Relocation Target | Value After Linking |
|-----------|--------------------|---------------------|
| `+16: .word 0` | `__chain_<name>` (`.data`) | Absolute address of chain slot |
| `+20: .word 0` | `<nested_func>` (`.text`) | Absolute address of nested function \| 1 (Thumb) |

The existing `arm-link.c` `relocate_section()` handles `R_ARM_ABS32`:

```c
case R_ARM_ABS32:
    *(uint32_t *)ptr += val;
    break;
```

This should work without modification. The Thumb bit (+1) is part of the symbol value, set when the symbol is created with `put_extern_sym_2()`.

## Symbol Visibility

All nested function artifacts are file-local:

```
function create_nested_func_symbol(mangled_name, text_section, offset, size):
    sym = put_elf_sym(s1->symtab_section, offset | 1,  // +1 Thumb
                      size,
                      ELF32_ST_INFO(STB_LOCAL, STT_FUNC),
                      0, text_section->sh_num,
                      mangled_name)
    return sym

function create_trampoline_symbol(tramp_name, text_section, offset, size):
    sym = put_elf_sym(s1->symtab_section, offset | 1,  // +1 Thumb
                      size,
                      ELF32_ST_INFO(STB_LOCAL, STT_FUNC),
                      0, text_section->sh_num,
                      tramp_name)
    return sym

function create_chain_slot_symbol(slot_name, data_section, offset):
    sym = put_elf_sym(s1->symtab_section, offset, 4,
                      ELF32_ST_INFO(STB_LOCAL, STT_OBJECT),
                      0, data_section->sh_num,
                      slot_name)
    return sym
```

## Name Mangling

Nested function names use GCC convention to ensure uniqueness:

| Artifact | Name Pattern | Example |
|----------|-------------|---------|
| Nested function | `<funcname>.<index>` | `multiply.0` |
| Trampoline | `__tramp_<funcname>.<index>` | `__tramp_multiply.0` |
| Chain slot | `__chain_<funcname>.<index>` | `__chain_multiply.0` |

The `.N` suffix is the nested function index within the parent (0, 1, 2, ...). This ensures unique symbol names even when the parent function is called recursively. The mangled name is stored in `sym->asm_label` (see `tccgen.c:11942-11944`).

## Potential Issues

1. **Section ordering**: Trampoline code is emitted in `.text` after the nested function. The linker must not reorder or coalesce these sections.

2. **Alignment**: Trampoline data words at `+16` and `+20` must be 4-byte aligned. The NOP padding at `+12`/`+14` ensures this (trampoline starts at a 2-byte aligned address in `.text`).

3. **PIC/PIE**: Not applicable for ARMv8-M embedded targets (absolute addressing only).

## Implementation Status

**Status**: ✅ COMPLETE

All linker support for nested functions has been implemented and verified. The existing `R_ARM_ABS32` relocation handling in `arm-link.c` works correctly for the trampoline data words.

### Symbol Creation Locations

| Symbol Type | Location | Binding |
|-------------|----------|---------|
| Nested function | `tccgen.c:11948` - `put_extern_sym()` | `STB_LOCAL` via `VT_STATIC` |
| Chain slot | `tccgen.c:10857` - `put_elf_sym()` | `STB_LOCAL` explicit |
| Trampoline | `tccgen.c:10881` - `put_elf_sym()` | `STB_LOCAL` explicit |

### Verification

Symbol table from `nested_funcptr.c`:

```
$ arm-none-eabi-readelf -s nested_funcptr.o

   Num:    Value  Size Type    Bind   Vis      Ndx Name
     2: 00000001    20 FUNC    LOCAL  DEFAULT    1 multiply.0
     3: 00000000     4 OBJECT  LOCAL  DEFAULT    2 __chain_multiply.0
     4: 00000015    20 FUNC    LOCAL  DEFAULT    1 __tramp_multiply.0
    11: 00000029    92 FUNC    GLOBAL DEFAULT    1 main
```

Relocations from `nested_funcptr.o`:

```
$ arm-none-eabi-readelf -r nested_funcptr.o

Relocation section '.rel.text':
 Offset     Type            Sym.Value  Sym. Name
00000020  R_ARM_ABS32       00000000   __chain_multiply.0
00000024  R_ARM_ABS32       00000001   multiply.0        # +1 Thumb bit
00000078  R_ARM_ABS32       00000015   __tramp_multiply.0
```

## Test Cases

| Test | Validates | Status |
|------|-----------|--------|
| `nested_funcptr.c` | R_ARM_ABS32 relocs resolve, trampoline branches to correct address | ✅ PASS |
| `nested_funcptr_indirect.c` | Chain slot address resolves, trampoline works across call boundary | ✅ PASS |
| `objdump -t` on any nested func ELF | STB_LOCAL symbols present with correct names | ✅ VERIFIED |
| `objdump -r` on relocatable output | R_ARM_ABS32 entries for trampoline data words | ✅ VERIFIED |
