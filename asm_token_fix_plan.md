# ASM Token Refactoring Fix Plan

## Problem Summary

After changing ASM tokens to use `DEF_ASM_BASE` (single token per instruction), all tests are failing because boot.S is incorrectly transformed to machine code.

### Root Cause

The old token layout used 64 token slots per instruction:
- Slots 0-14: Conditional variants (eq, ne, cs, etc.)
- Slots 15-30: Wide variants (.w)
- Slot 0x40: Set-flags variant (s)

The new layout with `DEF_ASM_BASE` assigns just ONE token per instruction. Condition codes and width qualifiers are now parsed at runtime and stored in `current_asm_suffix`.

**The problem**: Several macros and code patterns still assume the OLD token layout:

1. `THUMB_INSTRUCTION_GROUP(token)` - Extracts base instruction by masking off lower 6 bits
2. `THUMB_GET_CONDITION(token)` - Extracts condition code from token offset
3. `THUMB_HAS_WIDE_QUALIFIER(token)` - Checks if token is in wide variant range
4. `THUMB_HAS_NARROW_QUALIFIER(token)` - Checks if token is in narrow variant range

---

## Files Affected

### Primary: [arm-thumb-asm.c](arm-thumb-asm.c)

Contains 34 occurrences of deprecated macros that need updating.

### Secondary: [thumb-tok.h](thumb-tok.h)

Contains macro definitions that are now obsolete (but kept for reference).

---

## Fix Strategy

### Step 1: Use Runtime State Instead of Token-Based Macros

Replace all occurrences of:

| Old Macro | New Replacement |
|-----------|-----------------|
| `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_xxx` | `token == TOK_ASM_xxx` |
| `THUMB_GET_CONDITION(token)` | `THUMB_GET_CONDITION_FROM_STATE()` |
| `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| `THUMB_HAS_NARROW_QUALIFIER(token)` | `THUMB_HAS_NARROW_QUALIFIER_FROM_STATE()` |

### Step 2: Locations to Fix in arm-thumb-asm.c

#### Group 1: THUMB_INSTRUCTION_GROUP replacements

| Line | Current Code | Fix |
|------|--------------|-----|
| 1297 | `THUMB_INSTRUCTION_GROUP(token) == token_svariant` | `token == token_svariant` |
| 1336 | `THUMB_INSTRUCTION_GROUP(token) == data.regular_variant_token` | `token == data.regular_variant_token` |
| 1499 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addw` | `token == TOK_ASM_addw` |
| 1505 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_addw` | `token == TOK_ASM_addw` |
| 1510 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_add` | `token == TOK_ASM_add` |
| 1514 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_adds` | `token == TOK_ASM_adds` |
| 1616 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_movw` | `token == TOK_ASM_movw` |
| 1678 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subw` | `token == TOK_ASM_subw` |
| 1684 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subw` | `token == TOK_ASM_subw` |
| 1689 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_sub` | `token == TOK_ASM_sub` |
| 1693 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_subs` | `token == TOK_ASM_subs` |
| 1751 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldrd` | `token == TOK_ASM_ldrd` |
| 1900-1903 | Multiple `THUMB_INSTRUCTION_GROUP` checks | Direct token comparisons |
| 1915 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldr` | `token == TOK_ASM_ldr` |
| 2002 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_ldrd` | `token == TOK_ASM_ldrd` |
| 2927 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_add` | `token == TOK_ASM_add` |
| 3119 | `THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbz` | `token == TOK_ASM_cbz` |
| 3124-3125 | Multiple `THUMB_INSTRUCTION_GROUP` checks | Direct token comparisons |
| 3139-3140 | Multiple `THUMB_INSTRUCTION_GROUP` checks | Direct token comparisons |

#### Group 2: THUMB_HAS_WIDE_QUALIFIER replacements

| Line | Current Code | Fix |
|------|--------------|-----|
| 1337 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 1406 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 1440 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 1568 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 1735 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 1886 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 2255 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 2878 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 2934 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 2979 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 3083 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |
| 3114 | `THUMB_HAS_WIDE_QUALIFIER(token)` | `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()` |

#### Group 3: THUMB_GET_CONDITION replacement

| Line | Current Code | Fix |
|------|--------------|-----|
| 3158 | `condition = THUMB_GET_CONDITION(token);` | `condition = THUMB_GET_CONDITION_FROM_STATE();` |

### Step 3: Fix thumb_branch Function (Critical)

The `thumb_branch` function at line 3104 has multiple issues:

```c
// Line 3114 - Replace:
if (THUMB_HAS_WIDE_QUALIFIER(token))
// With:
if (THUMB_HAS_WIDE_QUALIFIER_FROM_STATE())

// Line 3119 - Replace:
if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbz || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnz)
// With:
if (token == TOK_ASM_cbz || token == TOK_ASM_cbnz)

// Line 3124-3125 - Replace:
if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_b || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_bl ||
    THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbz || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnz)
// With:
if (token == TOK_ASM_b || token == TOK_ASM_bl ||
    token == TOK_ASM_cbz || token == TOK_ASM_cbnz)

// Line 3139-3140 - Replace:
if (THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbz || THUMB_INSTRUCTION_GROUP(token) == TOK_ASM_cbnz)
// With:
if (token == TOK_ASM_cbz || token == TOK_ASM_cbnz)

// Line 3158 - Replace:
condition = THUMB_GET_CONDITION(token);
// With:
condition = THUMB_GET_CONDITION_FROM_STATE();
```

---

## Verification

After making all changes:

1. Rebuild the compiler:
   ```bash
   make clean && make
   ```

2. Test boot.S compilation:
   ```bash
   ./armv8m-tcc -c tests/ir_tests/qemu/mps2-an505/boot.S -o /tmp/boot.o
   arm-none-eabi-objdump -d /tmp/boot.o
   ```

3. Run the test suite:
   ```bash
   make test
   ```

---

## Expected Behavior After Fix

1. `bhs` should generate condition code 2 (CS/HS - carry set)
2. `blo` should generate condition code 3 (CC/LO - carry clear)
3. Unconditional `b` should generate condition code 14 (AL - always)
4. `.thumb_func` symbols should have bit 0 set in their address
5. All IR tests should pass

---

## Notes

- The macros in `thumb-tok.h` are marked as "DEPRECATED" but kept for reference
- The runtime state `current_asm_suffix` is already being populated correctly by `thumb_parse_token_suffix()`
- The helper macros `THUMB_GET_CONDITION_FROM_STATE()`, `THUMB_HAS_WIDE_QUALIFIER_FROM_STATE()`, and `THUMB_HAS_NARROW_QUALIFIER_FROM_STATE()` are already defined at lines 946-948
