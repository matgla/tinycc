# Plan: Hybrid Base Instruction + Runtime Suffix Parsing

## Overview

Replace 10,000+ pre-registered ARM assembly tokens with ~200 base instruction tokens plus runtime parsing of condition codes (eq/ne/cs/...) and width qualifiers (.w/.n). This reduces memory from ~800KB to ~45KB while maintaining full instruction coverage.

### Memory Impact

| Component | Before | After | Savings |
|-----------|--------|-------|---------|
| TokenSym count | ~10,500 | ~200 | 98% |
| table_ident | 82 KB | ~2 KB | 98% |
| TokenSym structs | ~500 KB | ~10 KB | 98% |
| **Total** | **~580 KB** | **~12 KB** | **~570 KB** |

### Root Cause Analysis

In `thumb-tok.h`, the `DEF_ASM_CONDED_WITH_QUALIFIER(x)` macro expands each instruction to **64 variants**:
- 16 condition codes (eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, base, rsvd)
- 4 width qualifiers (base, .w, .n, ._)

With 153 instructions using this macro: **153 × 64 = 9,792 tokens**

---

## Implementation Plan

### Phase 1: Core Infrastructure (arm-thumb-defs.h)

#### 1.1 Define Condition Code Enum and Tables

**Location**: [arm-thumb-defs.h](arm-thumb-defs.h)

```c
/* Condition code enumeration */
typedef enum thumb_condition_code {
    COND_EQ = 0,  /* Equal */
    COND_NE = 1,  /* Not equal */
    COND_CS = 2,  /* Carry set (unsigned >=) */
    COND_CC = 3,  /* Carry clear (unsigned <) */
    COND_MI = 4,  /* Minus (negative) */
    COND_PL = 5,  /* Plus (positive or zero) */
    COND_VS = 6,  /* Overflow set */
    COND_VC = 7,  /* Overflow clear */
    COND_HI = 8,  /* Higher (unsigned >) */
    COND_LS = 9,  /* Lower or same (unsigned <=) */
    COND_GE = 10, /* Greater or equal (signed >=) */
    COND_LT = 11, /* Less than (signed <) */
    COND_GT = 12, /* Greater than (signed >) */
    COND_LE = 13, /* Less or equal (signed <=) */
    COND_AL = 14, /* Always (unconditional) */
    COND_RSVD = 15, /* Reserved */
} thumb_condition_code;

/* Width qualifier enumeration */
typedef enum thumb_width_qualifier {
    WIDTH_NONE = 0,   /* No qualifier */
    WIDTH_WIDE = 1,   /* .w */
    WIDTH_NARROW = 2, /* .n */
    WIDTH_RESERVED = 3, /* ._ */
} thumb_width_qualifier;

/* Suffix parsing result */
typedef struct thumb_asm_suffix {
    thumb_condition_code condition;
    thumb_width_qualifier width;
    uint8_t has_suffix; /* 1 if any suffix was present */
} thumb_asm_suffix;
```

#### 1.2 Condition Code Name Lookup Table

**Location**: [arm-thumb-defs.h](arm-thumb-defs.h)

```c
/* Condition code name to enum mapping */
static const struct {
    const char *name;
    thumb_condition_code code;
} cond_names[] = {
    {"eq", COND_EQ},
    {"ne", COND_NE},
    {"cs", COND_CS},
    {"hs", COND_CS}, /* Alias */
    {"cc", COND_CC},
    {"lo", COND_CC}, /* Alias */
    {"mi", COND_MI},
    {"pl", COND_PL},
    {"vs", COND_VS},
    {"vc", COND_VC},
    {"hi", COND_HI},
    {"ls", COND_LS},
    {"ge", COND_GE},
    {"lt", COND_LT},
    {"gt", COND_GT},
    {"le", COND_LE},
    {"al", COND_AL},
    {NULL, COND_AL}, /* Default/unconditional */
};

#define COND_NAMES_COUNT (sizeof(cond_names) / sizeof(cond_names[0]) - 1)
```

#### 1.3 Suffix Parsing Function

**Location**: [arm-thumb-asm.c](arm-thumb-asm.c)

```c
/* Parse ARM assembly instruction suffix
 * Input:  token_str - full token string (e.g., "addeq.w")
 * Output: suffix - parsed condition and width qualifier
 * Returns: Length of suffix portion (0 if no suffix)
 */
static int parse_asm_suffix(const char *token_str, thumb_asm_suffix *suffix)
{
    const char *dot = NULL;
    const char *p = token_str;
    int suffix_len = 0;

    suffix->condition = COND_AL; /* Default: always */
    suffix->width = WIDTH_NONE;
    suffix->has_suffix = 0;

    /* Skip base instruction name (it's all letters until we hit something else) */
    while (*p && isalpha(*p))
        p++;

    /* Check for condition code suffix */
    if (*p == '\0') {
        /* No suffix at all */
        return 0;
    }

    /* Try to match condition code */
    for (size_t i = 0; i < COND_NAMES_COUNT; i++) {
        size_t cond_len = strlen(cond_names[i].name);
        if (strncmp(p, cond_names[i].name, cond_len) == 0) {
            suffix->condition = cond_names[i].code;
            suffix->has_suffix = 1;
            p += cond_len;
            suffix_len += cond_len;
            break;
        }
    }

    /* Check for width qualifier (.w, .n, ._) */
    if (*p == '.') {
        suffix->has_suffix = 1;
        p++; /* Skip dot */
        suffix_len++;

        if (strncmp(p, "w", 1) == 0 || strncmp(p, "W", 1) == 0) {
            suffix->width = WIDTH_WIDE;
            p++;
            suffix_len++;
        } else if (strncmp(p, "n", 1) == 0 || strncmp(p, "N", 1) == 0) {
            suffix->width = WIDTH_NARROW;
            p++;
            suffix_len++;
        } else if (*p == '_') {
            suffix->width = WIDTH_RESERVED;
            p++;
            suffix_len++;
        }
    }

    return suffix_len;
}

/* Extract base instruction name from token
 * Input:  token_str - full token string (e.g., "addeq.w")
 * Output: base_buf - buffer to store base name
 *         base_buf_size - size of base_buf
 * Returns: Length of base name
 */
static int get_base_instruction_name(const char *token_str, char *base_buf, int base_buf_size)
{
    const char *p = token_str;
    int len = 0;

    /* Copy base instruction name */
    while (*p && isalpha(*p) && len < base_buf_size - 1) {
        base_buf[len++] = *p++;
    }
    base_buf[len] = '\0';

    return len;
}
```

#### 1.4 Global State for Parsed Suffix

**Location**: [arm-thumb-asm.c](arm-thumb-asm.c)

```c
/* Global state for current assembly instruction suffix */
static thumb_asm_suffix current_asm_suffix = {
    .condition = COND_AL,
    .width = WIDTH_NONE,
    .has_suffix = 0,
};

/* Helper macros to maintain compatibility during transition */
#define THUMB_GET_CONDITION_FROM_STATE() (current_asm_suffix.condition)
#define THUMB_HAS_WIDE_QUALIFIER_FROM_STATE() (current_asm_suffix.width == WIDTH_WIDE)
#define THUMB_HAS_NARROW_QUALIFIER_FROM_STATE() (current_asm_suffix.width == WIDTH_NARROW)
```

---

### Phase 2: Token Definition Changes (thumb-tok.h)

#### 2.1 Replace Macro Definitions

**Location**: [thumb-tok.h](thumb-tok.h)

**Current code (lines 137-203)**:
```c
#define DEF_ASM_CONDED(x) \
  DEF(TOK_ASM_##x##eq, #x "eq") \
  DEF(TOK_ASM_##x##ne, #x "ne") \
  ... /* 16 variants */

#define DEF_ASM_CONDED_WITH_QUALIFIER(x) \
  DEF_ASM_CONDED(x) \
  DEF_ASM_CONDED_WITH_SUFFIX(x, w) \
  DEF_ASM_CONDED_WITH_SUFFIX(x, n) \
  DEF_ASM_CONDED_WITH_SUFFIX(x, _)
```

**Replace with**:
```c
/* New simplified macro - single token per instruction */
#define DEF_ASM_BASE(x) DEF(TOK_ASM_##x, #x)

/* Keep old macros temporarily for transition, but they expand to single token */
#define DEF_ASM_CONDED(x) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_WITH_QUALIFIER(x) DEF_ASM_BASE(x)
#define DEF_ASM_CONDED_WITH_SUFFIX(x, y) DEF_ASM_BASE(x)
```

#### 2.2 Update All Instruction Definitions

**Action**: Replace all `DEF_ASM_CONDED_WITH_QUALIFIER(x)` with `DEF_ASM_BASE(x)`

**Files affected**: [thumb-tok.h](thumb-tok.h) lines 204-404

**Example**:
```c
// Before:
DEF_ASM_CONDED_WITH_QUALIFIER(adc)
DEF_ASM_CONDED_WITH_QUALIFIER(adcs)
DEF_ASM_CONDED_WITH_QUALIFIER(add)
DEF_ASM_CONDED_WITH_QUALIFIER(adds)

// After:
DEF_ASM_BASE(adc)
DEF_ASM_BASE(adcs)
DEF_ASM_BASE(add)
DEF_ASM_BASE(adds)
```

**Script to automate**:
```bash
# In thumb-tok.h, replace all instances
sed -i 's/DEF_ASM_CONDED_WITH_QUALIFIER/DEF_ASM_BASE/g' thumb-tok.h
sed -i 's/DEF_ASM_CONDED_VFP_F32_F64(\(.*\))/DEF_ASM_BASE(\1)/g' thumb-tok.h
```

---

### Phase 3: Token Lookup Modification (tccpp.c)

#### 3.1 Enhanced Token Allocation

**Location**: [tccpp.c](tccpp.c) - `tok_alloc()` function

**Implementation**:
```c
ST_FUNC TokenSym *tok_alloc(const char *str, int len)
{
    TokenSym *ts;
    int h;
    CString *cstr;

    /* ... existing hash lookup code ... */
    h = calc_hash(str, len) % TOK_HASH_SIZE;
    ts = tok_hash[h];

    while (ts) {
        if (ts->len == len && !memcmp(ts->str, str, len))
            return ts; /* Found existing token */
        ts = ts->hash_next;
    }

    /* Token not found - check for asm instruction with suffix */
    if (parse_flags & PARSE_FLAG_ASM_FILE) {
        /* Try to parse as base + suffix instruction */
        char base_buf[32];
        int base_len;

        /* Extract potential base name */
        base_len = get_base_instruction_name_from_str(str, len, base_buf, sizeof(base_buf));

        /* Look up base instruction */
        ts = tok_lookup(base_buf, base_len);
        if (ts && ts->tok >= TOK_ASM_nopeq && ts->tok <= TOK_ASM_iteee) {
            /* Found base instruction - create synthetic token with suffix info */
            /* Note: We store the original string for error messages */
            /* The suffix info will be parsed and stored in global state when used */
            return tok_alloc_new(&tok_hash[h], str, len);
        }
    }

    /* Not an asm instruction - create new token */
    return tok_alloc_new(&tok_hash[h], str, len);
}

/* Helper: Extract base name from string (must match version in arm-thumb-asm.c) */
static int get_base_instruction_name_from_str(const char *str, int len, char *base_buf, int base_buf_size)
{
    int i = 0;
    while (i < len && i < base_buf_size - 1 && isalpha(str[i])) {
        base_buf[i] = str[i];
        i++;
    }
    base_buf[i] = '\0';
    return i;
}
```

#### 3.2 Forward Declaration

**Location**: [tccpp.c](tccpp.c) - near top with other forward declarations

```c
/* Forward declarations for suffix parsing (implemented in arm-thumb-asm.c) */
#ifdef TCC_TARGET_ARM
struct thumb_asm_suffix;
int parse_asm_suffix(const char *token_str, struct thumb_asm_suffix *suffix);
int get_base_instruction_name(const char *token_str, char *base_buf, int base_buf_size);
#endif
```

---

### Phase 4: Opcode Dispatch Refactoring (arm-thumb-asm.c)

#### 4.1 Update `asm_opcode()` Function

**Location**: [arm-thumb-asm.c:3000](arm-thumb-asm.c#L3000)

**Current implementation** uses `THUMB_INSTRUCTION_GROUP(token)` to extract base instruction.

**New implementation**:
```c
ST_FUNC void asm_opcode(TCCState *s1, int token)
{
    while (token == TOK_LINEFEED) {
        next();
        token = tok;
    }
    if (token == TOK_EOF)
        return;

    /* Parse suffix and store in global state */
    const char *token_str = get_tok_str(token, NULL);
    parse_asm_suffix(token_str, &current_asm_suffix);

    /* Get base token ID (same as token since we only have base tokens now) */
    int base_token = token;

    /* GAS-compatible aliases for conditional branches */
    {
        const char *alias = get_tok_str(token, NULL);
        if (alias) {
            if (strcmp(alias, "bhs") == 0)
                base_token = TOK_ASM_b;
            else if (strcmp(alias, "blo") == 0)
                base_token = TOK_ASM_b;
            /* ... other aliases ... */
        }
    }

    /* IT block handling */
    if (base_token >= TOK_ASM_it && base_token <= TOK_ASM_iteee) {
        thumb_conditional_opcode(s1, base_token);
        return;
    }

    if (thumb_conditional_scope > 0)
        --thumb_conditional_scope;

    /* VFP instruction dispatch (check before general dispatch) */
    if (strncmp(token_str, "vmov", 4) == 0) {
        thumb_emit_opcode(thumb_vmov_opcode(s1, base_token));
        return;
    }
    if (strncmp(token_str, "vadd", 4) == 0 || strncmp(token_str, "vsub", 4) == 0 ||
        strncmp(token_str, "vmul", 4) == 0 || strncmp(token_str, "vdiv", 4) == 0 ||
        strncmp(token_str, "vneg", 4) == 0) {
        thumb_emit_opcode(thumb_vfp_arith_opcode(s1, base_token));
        return;
    }
    if (strncmp(token_str, "vcmp", 4) == 0) {
        thumb_emit_opcode(thumb_vcmp_opcode(s1, base_token));
        return;
    }
    if (strncmp(token_str, "vmrs", 4) == 0) {
        thumb_emit_opcode(thumb_vmrs_opcode(s1, base_token));
        return;
    }
    if (strncmp(token_str, "vcvt", 4) == 0) {
        thumb_emit_opcode(thumb_vcvt_opcode(s1, base_token));
        return;
    }

    /* General instruction dispatch */
    switch (base_token) {
    case TOK_ASM_bx:
    case TOK_ASM_bl:
    case TOK_ASM_blx:
        return thumb_branch(s1, base_token);
    case TOK_ASM_adc:
    case TOK_ASM_adcs:
    case TOK_ASM_add:
    case TOK_ASM_adds:
    case TOK_ASM_addw:
    case TOK_ASM_and:
    case TOK_ASM_andseq:
    case TOK_ASM_orr:
    /* ... all data processing instructions ... */
        return thumb_data_processing_opcode(s1, base_token);

    case TOK_ASM_adr:
        return thumb_adr_opcode(s1, base_token);

    /* ... remaining instruction categories ... */
    }
}
```

#### 4.2 Update Instruction Handler Signatures

**Location**: [arm-thumb-asm.c](arm-thumb-asm.c) - All handler functions

**Changes required**:
1. Remove `THUMB_GET_CONDITION(token)` calls
2. Remove `THUMB_HAS_WIDE_QUALIFIER(token)` calls
3. Remove `THUMB_INSTRUCTION_GROUP(token)` calls
4. Use `current_asm_suffix.condition` and `current_asm_suffix.width` instead

**Example - `thumb_adr_opcode()` (lines 1161-1197)**:

**Current code**:
```c
static void thumb_adr_opcode(TCCState *s1, int token)
{
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
    if (THUMB_HAS_WIDE_QUALIFIER(token)) {
        encoding = ENFORCE_ENCODING_32BIT;
    }
    /* ... rest of function ... */
}
```

**New code**:
```c
static void thumb_adr_opcode(TCCState *s1, int token)
{
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
    if (current_asm_suffix.width == WIDTH_WIDE) {
        encoding = ENFORCE_ENCODING_32BIT;
    }
    /* ... rest of function ... */
}
```

**Example - `thumb_branch()` function**:

**Current code** (line 2929):
```c
condition = THUMB_GET_CONDITION(token);
switch (THUMB_INSTRUCTION_GROUP(token)) {
    case TOK_ASM_beq:
        /* ... */
}
```

**New code**:
```c
condition = current_asm_suffix.condition;
switch (token) {
    case TOK_ASM_b:
        /* ... */
}
```

#### 4.3 Update `thumb_generate_opcode_for_data_processing()`

**Location**: [arm-thumb-asm.c:1199](arm-thumb-asm.c#L1199)

**Current code** uses `THUMB_INSTRUCTION_GROUP(token)` for switch cases.

**Changes**:
```c
thumb_opcode thumb_generate_opcode_for_data_processing(int token, thumb_shift shift, Operand *ops)
{
    thumb_enforce_encoding encoding = ENFORCE_ENCODING_NONE;
    if (current_asm_suffix.width == WIDTH_WIDE) {
        encoding = ENFORCE_ENCODING_32BIT;
    }

    switch (token) {
    case TOK_ASM_adc:
    case TOK_ASM_adcs:
        return thumb_process_generic_data_op(
            (th_generic_op_data){
                .generate_imm_opcode = th_adc_imm,
                .generate_reg_opcode = th_adc_reg,
                .has_flags_variant = (token == TOK_ASM_adcs),
            },
            current_asm_suffix.condition, shift, ops);

    case TOK_ASM_and:
    case TOK_ASM_ands:
        return thumb_process_generic_data_op(
            (th_generic_op_data){
                .generate_imm_opcode = th_and_imm,
                .generate_reg_opcode = th_and_reg,
                .has_flags_variant = (token == TOK_ASM_ands),
            },
            current_asm_suffix.condition, shift, ops);
    /* ... remaining cases ... */
    }
}
```

#### 4.4 Update `thumb_process_generic_data_op()`

**Location**: [arm-thumb-asm.c](arm-thumb-asm.c) - Search for function definition

**Current signature**:
```c
static thumb_opcode thumb_process_generic_data_op(th_generic_op_data op, int token, thumb_shift shift, Operand *ops)
```

**New signature**:
```c
static thumb_opcode thumb_process_generic_data_op(th_generic_op_data op, thumb_condition_code cond, thumb_shift shift, Operand *ops)
```

**Changes inside function**:
- Replace `THUMB_GET_CONDITION(token)` with `cond` parameter
- Replace `THUMB_INSTRUCTION_GROUP(token)` comparisons with direct token checks

---

### Phase 5: Code Generator Updates (arm-thumb-gen.c)

#### 5.1 Update Opcode Emission Functions

**Location**: [arm-thumb-gen.c](arm-thumb-gen.c)

Many functions in this file currently take condition codes extracted from token IDs.
These need to be updated to accept explicit condition code parameters.

**Example function updates**:

```c
// Before:
thumb_opcode th_add_t3(uint32_t rd, uint32_t rn, uint32_t imm, int token)

// After:
thumb_opcode th_add_t3(uint32_t rd, uint32_t rn, uint32_t imm, thumb_condition_code cond)
```

**Search and replace pattern**:
```bash
# Find all functions that use THUMB_GET_CONDITION
grep -n "THUMB_GET_CONDITION" arm-thumb-gen.c

# Update each function signature and implementation
```

#### 5.2 Remove Token ID Math Macros

**Location**: [thumb-tok.h:121-134](thumb-tok.h#L121)

**Mark as deprecated**:
```c
/* DEPRECATED: These macros are obsolete after token refactoring */
/* Kept temporarily for reference during transition */
#define THUMB_INSTRUCTION_GROUP(tok) ((((tok) - TOK_ASM_nopeq) & 0xFFFFFFC0) + TOK_ASM_nopeq)
#define THUMB_HAS_WIDE_QUALIFIER(tok) \
  ((tok - THUMB_INSTRUCTION_GROUP(tok)) > 0x0f && (tok - THUMB_INSTRUCTION_GROUP(tok)) <= 0x1f)
/* ... etc ... */

/* New replacements */
#define THUMB_CURRENT_CONDITION() (current_asm_suffix.condition)
#define THUMB_CURRENT_WIDTH() (current_asm_suffix.width)
#define THUMB_IS_WIDE() (current_asm_suffix.width == WIDTH_WIDE)
#define THUMB_IS_NARROW() (current_asm_suffix.width == WIDTH_NARROW)
```

---

### Phase 6: VFP Instruction Handling

#### 6.1 VFP Suffix Strategy

**Decision**: Keep VFP type suffixes (.f32, .f64) as part of the base instruction name.

**Rationale**:
- Only ~30 VFP instructions with type suffixes
- Avoids complex type suffix parsing
- VFP instructions already have separate handling in `asm_opcode()`

#### 6.2 Update VFP Token Definitions

**Location**: [thumb-tok.h:392-403](thumb-tok.h#L392)

**Current code**:
```c
DEF_ASM_CONDED_VFP_F32_F64(vadd)
DEF_ASM_CONDED_VFP_F32_F64(vsub)
```

**New approach**: Keep type suffix in token name, remove condition codes:
```c
/* VFP instructions - keep type suffix as part of base name */
DEF_ASM_BASE(vadd_f32)
DEF_ASM_BASE(vadd_f64)
DEF_ASM_BASE(vsub_f32)
DEF_ASM_BASE(vsub_f64)
/* ... etc ... */
```

#### 6.3 Update VFP Parsing Functions

**Location**: [arm-thumb-asm.c](arm-thumb-asm.c) - Lines 2092-2253

**Update `thumb_vfp_arith_opcode()`**:
```c
static thumb_opcode thumb_vfp_arith_opcode(TCCState *s1, int token)
{
    /* No longer need to skip suffix tokens - type is in token name */

    Operand ops[3] = {};
    const char *tokstr = get_tok_str(token, NULL);
    const int nb_ops = process_operands(s1, sizeof(ops) / sizeof(ops[0]), ops);

    /* Determine size from token name */
    uint32_t sz;
    if (strstr(tokstr, ".f64") || strstr(tokstr, "f64"))
        sz = 64;
    else
        sz = 32;

    const bool is_unary = strncmp(tokstr, "vneg", 4) == 0;
    const int needed = is_unary ? 2 : 3;

    if (nb_ops != needed) {
        expect(is_unary ? "two operands" : "three operands");
    }

    /* ... rest of function unchanged ... */
}
```

---

### Phase 7: Clean Up and Testing

#### 7.1 Remove Deprecated Code

**Files to clean**:
1. [tccpp.c](tccpp.c) - Remove lazy loading functions if they exist:
   - `tcc_load_asm_tokens()`
   - `tccpp_new_lean()` (if created)
   - Reserved slot management code

2. [thumb-tok.h](thumb-tok.h) - Remove deprecated macros:
   - `THUMB_INSTRUCTION_GROUP()`
   - `THUMB_HAS_WIDE_QUALIFIER()`
   - `THUMB_HAS_NARROW_QUALIFIER()`
   - `THUMB_IS_CONDITIONAL()`
   - `THUMB_GET_CONDITION()`

3. [tcc.h](tcc.h) - Remove `asm_tokens_loaded` flag if added

#### 7.2 Testing Strategy

1. **Create comprehensive test suite**:

   **File: `tests/asm_suffix_test.c`**
   ```c
   /* Test all condition codes */
   void test_conditions(void) {
       __asm__("addeq r0, r1, r2");
       __asm__("addne r0, r1, r2");
       __asm__("addcs r0, r1, r2");
       /* ... all 16 conditions ... */
   }

   /* Test width qualifiers */
   void test_widths(void) {
       __asm__("add.w r0, r1, r2");
       __asm__("add.n r0, r1, r2");
   }

   /* Test combined suffixes */
   void test_combined(void) {
       __asm__("addeq.w r0, r1, r2");
       __asm__("addne.n r0, r1, r2");
   }

   /* Test VFP instructions */
   void test_vfp(void) {
       __asm__("vadd.f32 s0, s1, s2");
       __asm__("vadd.f64 d0, d1, d2");
   }
   ```

2. **Assembly file test**:

   **File: `tests/asm_suffix_test.S`**
   ```assembly
       .syntax unified
       .thumb

       /* Test condition codes */
       addeq r0, r1, r2
       addne r0, r1, r2
       addcs r0, r1, r2

       /* Test width qualifiers */
       add.w r0, r1, r2
       add.n r0, r1, r2

       /* Test combined */
       addeq.w r0, r1, r2

       /* Test VFP */
       vadd.f32 s0, s1, s2
       vadd.f64 d0, d1, d2
   ```

3. **Memory verification**:

   ```bash
   # Compile C-only code and check token count
   echo 'int main() { return 0; }' | ./tcc -c - -o /dev/null -vvv 2>&1 | grep TokenSym

   # Should show ~200 TokenSyms instead of ~10,500
   ```

---

## Implementation Todo List

### Phase 1: Core Infrastructure
- [ ] Add `thumb_condition_code` enum to [arm-thumb-defs.h](arm-thumb-defs.h)
- [ ] Add `thumb_width_qualifier` enum to [arm-thumb-defs.h](arm-thumb-defs.h)
- [ ] Add `thumb_asm_suffix` struct to [arm-thumb-defs.h](arm-thumb-defs.h)
- [ ] Add `cond_names[]` lookup table to [arm-thumb-defs.h](arm-thumb-defs.h)
- [ ] Implement `parse_asm_suffix()` in [arm-thumb-asm.c](arm-thumb-asm.c)
- [ ] Implement `get_base_instruction_name()` in [arm-thumb-asm.c](arm-thumb-asm.c)
- [ ] Add `current_asm_suffix` global variable to [arm-thumb-asm.c](arm-thumb-asm.c)
- [ ] Add helper macros `THUMB_CURRENT_CONDITION()`, etc. to [arm-thumb-asm.c](arm-thumb-asm.c)

### Phase 2: Token Definition Changes
- [ ] Define `DEF_ASM_BASE(x)` macro in [thumb-tok.h](thumb-tok.h)
- [ ] Update `DEF_ASM_CONDED()` to use `DEF_ASM_BASE()`
- [ ] Update `DEF_ASM_CONDED_WITH_QUALIFIER()` to use `DEF_ASM_BASE()`
- [ ] Update `DEF_ASM_CONDED_WITH_SUFFIX()` to use `DEF_ASM_BASE()`
- [ ] Update `DEF_ASM_CONDED_VFP_F32_F64()` to use `DEF_ASM_BASE()`
- [ ] Replace all instruction definitions in [thumb-tok.h:204-404](thumb-tok.h#L204)
- [ ] Replace all VFP instruction definitions in [thumb-tok.h:392-403](thumb-tok.h#L392)

### Phase 3: Token Lookup
- [ ] Add forward declarations to [tccpp.c](tccpp.c)
- [ ] Implement `get_base_instruction_name_from_str()` in [tccpp.c](tccpp.c)
- [ ] Update `tok_alloc()` in [tccpp.c](tccpp.c) to handle suffixed asm tokens
- [ ] Test token lookup with suffixed instructions

### Phase 4: Opcode Dispatch
- [ ] Update `asm_opcode()` to call `parse_asm_suffix()`
- [ ] Update all `switch(THUMB_INSTRUCTION_GROUP(token))` to `switch(token)`
- [ ] Update `thumb_branch()` function
- [ ] Update `thumb_data_processing_opcode()` function
- [ ] Update `thumb_adr_opcode()` function
- [ ] Update `thumb_generate_opcode_for_data_processing()` function
- [ ] Update `thumb_process_generic_data_op()` signature
- [ ] Update all instruction handler calls to pass condition explicitly
- [ ] Remove all `THUMB_GET_CONDITION()` calls
- [ ] Remove all `THUMB_HAS_WIDE_QUALIFIER()` calls
- [ ] Remove all `THUMB_INSTRUCTION_GROUP()` calls

### Phase 5: Code Generator
- [ ] Update opcode emission functions in [arm-thumb-gen.c](arm-thumb-gen.c)
- [ ] Replace condition code token parameters with `thumb_condition_code` enum
- [ ] Update all functions using `THUMB_GET_CONDITION()`
- [ ] Mark old macros as deprecated in [thumb-tok.h](thumb-tok.h)

### Phase 6: VFP Instructions
- [ ] Update VFP token definitions to include type suffix
- [ ] Update `thumb_vfp_arith_opcode()` function
- [ ] Update `thumb_vmov_opcode()` function
- [ ] Update `thumb_vcmp_opcode()` function
- [ ] Update `thumb_vmrs_opcode()` function
- [ ] Update `thumb_vcvt_opcode()` function
- [ ] Test all VFP instruction variants

### Phase 7: Testing and Cleanup
- [ ] Create `tests/asm_suffix_test.c`
- [ ] Create `tests/asm_suffix_test.S`
- [ ] Run test suite and verify no regressions
- [ ] Test memory usage with C-only code
- [ ] Test memory usage with inline asm
- [ ] Test all 16 condition codes
- [ ] Test all width qualifiers
- [ ] Test combined suffixes
- [ ] Test GAS-compatible aliases (bhs, blo, etc.)
- [ ] Remove any remaining lazy loading code
- [ ] Remove deprecated macros
- [ ] Final code cleanup and formatting

### Documentation
- [ ] Update inline comments
- [ ] Document new suffix parsing approach
- [ ] Add examples of valid instruction syntax
- [ ] Update any relevant design documents

---

## Further Considerations

### 1. Suffix Attachment Mechanism

**Decision**: Use global state (`current_asm_suffix`)

**Pros**:
- Matches existing `tok` global pattern
- Simple to implement
- No changes to function signatures needed

**Cons**:
- Global state (but already used extensively in TCC)
- Must ensure state is cleared properly

### 2. VFP Instruction Suffixes

**Decision**: Keep VFP type suffixes (.f32/.f64) as part of base token name

**Pros**:
- Only ~30 extra tokens
- Simpler parsing
- Type is known at token lookup time

**Cons**:
- Slightly more tokens than pure base approach
- Still need to handle .f32 vs .f64

### 3. Backward Compatibility

**Decision**: Yes, maintain full compatibility

**Implementation**:
- Runtime suffix decomposition handles all variants
- `__asm__("addeq.w")` works identically to before
- Assembly files with suffixed instructions work unchanged

### 4. Performance Impact

**Expected**: Negligible

**Reasoning**:
- Suffix parsing is O(n) where n is suffix length (typically 2-4 chars)
- String comparison happens anyway during token lookup
- No additional hash table lookups needed
- One-time parse per instruction

### 5. IT Block (If-Then) Handling

**Note**: IT blocks are special Thumb-2 instructions that specify condition for following instructions.

**Current handling**: Uses `thumb_conditional_scope` counter

**No changes needed**: IT block handling is independent of token structure

---

## Risk Analysis

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Token ID conflicts during transition | Medium | High | Incremental migration; compatibility macros |
| Performance regression | Low | Medium | Benchmark critical paths; optimize suffix parsing |
| Missed instruction handlers | Medium | High | Comprehensive grep for macro usage; thorough testing |
| VFP instruction breakage | Low | Medium | Separate VFP testing phase |
| Inline asm compatibility | Low | High | Test all common inline asm patterns |

---

## Migration Strategy

### Incremental Approach

1. **Week 1**: Phase 1 (Core Infrastructure)
   - Define new types and functions
   - No changes to existing code
   - Unit test suffix parsing

2. **Week 2**: Phase 2 (Token Definitions)
   - Create new `DEF_ASM_BASE()` macro
   - Keep old macros as aliases
   - Verify compilation

3. **Week 3**: Phase 3-4 (Lookup and Dispatch)
   - Update token allocation
   - Update opcode dispatch
   - Test basic instructions

4. **Week 4**: Phase 5-6 (Handlers and VFP)
   - Update all instruction handlers
   - Update VFP handling
   - Comprehensive testing

5. **Week 5**: Phase 7 (Cleanup and Testing)
   - Remove deprecated code
   - Final testing
   - Documentation