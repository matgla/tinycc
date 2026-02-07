# Lazy Loading of Assembly Tokens

## Problem

The ARM/Thumb compiler registers **~10,000 assembly instruction tokens** at startup, even for pure C code that never uses inline assembly. This consumes:

- **~80KB** for `table_ident` pointer array
- **~500KB** for TokenSym structures (48 bytes + string each)
- Significant initialization time

### Root Cause

In `thumb-tok.h`, the `DEF_ASM_CONDED_WITH_QUALIFIER(x)` macro expands each instruction to **64 variants**:
- 16 condition codes (eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, base, rsvd)
- 4 width qualifiers (base, .w, .n, ._)

With 153 instructions using this macro: **153 × 64 = 9,792 tokens**

## Solution: Lazy Loading

Register assembly tokens only when inline assembly is first encountered.

---

## Implementation Plan

### 1. Add State Tracking (tcc.h)

```c
/* In TCCState struct */
unsigned char asm_tokens_loaded; /* 1 = assembly tokens have been registered */
```

### 2. Split Token Registration (tccpp.c)

#### 2.1 Modify `tccpp_new()` to skip assembly tokens

Current code registers ALL tokens from `tcc_keywords`:
```c
tok_ident = TOK_IDENT;
p = tcc_keywords;
while (*p) {
    tok_alloc(p, r - p - 1);
    p = r;
}
```

Change to register only C keywords (tokens before first DEF_ASM):

```c
tok_ident = TOK_IDENT;
p = tcc_keywords;
while (*p) {
    r = p;
    while (*r) r++;
    r++;
    /* Stop at first assembly token (TOK_ASM_xxx) */
    if (tok_ident >= TOK_ASM_FIRST)
        break;
    tok_alloc(p, r - p - 1);
    p = r;
}
s->asm_tokens_loaded = 0;
```

#### 2.2 Add `tcc_load_asm_tokens()` function

```c
/* Load assembly instruction tokens on demand */
ST_FUNC void tcc_load_asm_tokens(TCCState *s)
{
    const char *p, *r;

    if (s->asm_tokens_loaded)
        return;

    s->asm_tokens_loaded = 1;

    /* Skip to assembly tokens in tcc_keywords */
    p = tcc_keywords;
    while (*p && tok_ident < TOK_ASM_FIRST) {
        while (*p) p++;
        p++;
    }

    /* Register remaining tokens (assembly instructions) */
    while (*p) {
        r = p;
        while (*r) r++;
        r++;
        tok_alloc(p, r - p - 1);
        p = r;
    }
}
```

### 3. Define Token Boundary (tcctok.h)

Add marker before first assembly token:

```c
/* ... C keywords and operators ... */

/* === Assembly tokens start here === */
DEF(TOK_ASM_FIRST, "__asm_first__")  /* marker - never actually used */

/* Assembly directives */
DEF_ASMDIR(byte)
DEF_ASMDIR(word)
...
```

### 4. Trigger Loading on Inline Assembly (tccgen.c)

In the inline assembly parser, ensure tokens are loaded:

```c
static void asm_instr(void)
{
    /* Load assembly tokens on first use */
    if (!tcc_state->asm_tokens_loaded)
        tcc_load_asm_tokens(tcc_state);

    /* ... existing asm parsing code ... */
}
```

Also in `tccasm.c` for standalone assembly files:

```c
ST_FUNC void tcc_assemble(TCCState *s1, int do_preprocess)
{
    if (!s1->asm_tokens_loaded)
        tcc_load_asm_tokens(s1);

    /* ... existing code ... */
}
```

### 5. Handle Token Lookup (tccpp.c)

In `tok_alloc()`, if looking up an assembly-like identifier and tokens not loaded, load them first:

```c
ST_FUNC TokenSym *tok_alloc(const char *str, int len)
{
    /* ... existing hash lookup ... */

    /* If not found and could be an asm instruction, try loading asm tokens */
    if (!ts && !tcc_state->asm_tokens_loaded &&
        (parse_flags & PARSE_FLAG_ASM_FILE)) {
        tcc_load_asm_tokens(tcc_state);
        /* Retry lookup */
        return tok_alloc(str, len);
    }

    return tok_alloc_new(pts, str, len);
}
```

---

## Memory Savings

| Component | Before | After (no asm) | Savings |
|-----------|--------|----------------|---------|
| TokenSym count | ~10,500 | ~500 | 95% |
| table_ident | 82 KB | 4 KB | 95% |
| TokenSym structs | ~500 KB | ~25 KB | 95% |
| **Total** | **~580 KB** | **~30 KB** | **~550 KB** |

---

## Testing

1. **C-only compilation**: Verify no assembly tokens loaded
   ```bash
   echo 'int main() { return 0; }' | ./tcc -c - -o /dev/null
   # Should show ~500 TokenSyms instead of ~10,500
   ```

2. **Inline assembly**: Verify tokens loaded on demand
   ```bash
   echo 'int main() { __asm__("nop"); return 0; }' | ./tcc -c - -o /dev/null
   # Should show ~10,500 TokenSyms
   ```

3. **Assembly files**: Verify tokens loaded for .S files
   ```bash
   ./tcc -c test.S -o test.o
   ```

4. **Run full test suite**: Ensure no regressions

---

## Implementation Order

### Phase 1: Core Lazy Loading Infrastructure

1. [ ] **Add `TOK_ASM_FIRST` marker to tcctok.h**
   - Location: In `tcctok.h`, before the first `DEF_ASMDIR` or `DEF_ASM` token
   - Action: Add `DEF(TOK_ASM_FIRST, "__asm_first__")` as a boundary marker
   - Files modified: `tcctok.h`

2. [ ] **Add `asm_tokens_loaded` flag to TCCState**
   - Location: In `tcc.h`, within the `TCCState` struct definition
   - Action: Add `unsigned char asm_tokens_loaded;` field
   - Files modified: `tcc.h`
   - Implementation detail:
     ```c
     struct TCCState {
         ...
         unsigned char asm_tokens_loaded; /* 1 = assembly tokens have been registered */
         ...
     };
     ```

3. [ ] **Modify `tccpp_new()` to stop at TOK_ASM_FIRST**
   - Location: In `tccpp.c`, within the `tccpp_new()` function
   - Action: Add condition to skip assembly tokens during initial registration
   - Files modified: `tccpp.c`
   - Implementation:
     - Find the token registration loop (iterates over `tcc_keywords`)
     - Add `if (tok_ident >= TOK_ASM_FIRST) break;` before `tok_alloc()`
     - Initialize `s->asm_tokens_loaded = 0;` after the loop

4. [ ] **Implement `tcc_load_asm_tokens()` function**
   - Location: In `tccpp.c`
   - Action: Create new function to load assembly tokens on demand
   - Files modified: `tccpp.c`
   - Implementation:
     ```c
     ST_FUNC void tcc_load_asm_tokens(TCCState *s)
     {
         const char *p, *r;

         if (s->asm_tokens_loaded)
             return;

         s->asm_tokens_loaded = 1;

         /* Skip to assembly tokens in tcc_keywords */
         p = tcc_keywords;
         while (*p && tok_ident < TOK_ASM_FIRST) {
             while (*p) p++;
             p++;
         }

         /* Register remaining tokens (assembly instructions) */
         while (*p) {
             r = p;
             while (*r) r++;
             r++;
             tok_alloc(p, r - p - 1);
             p = r;
         }
     }
     ```

5. [ ] **Add forward declaration for `tcc_load_asm_tokens()` in tccpp.c**
   - Action: Ensure the function is properly declared before use
   - Files modified: `tccpp.c` (at top of file with other ST_FUNC declarations)

### Phase 2: Trigger Points

6. [ ] **Add trigger in `asm_instr()` (tccasm.c)**
   - Location: In `tccasm.c`, at the beginning of `asm_instr()` function
   - Action: Add lazy load check before parsing inline assembly
   - Files modified: `tccasm.c`
   - Implementation:
     ```c
     ST_FUNC void asm_instr(void)
     {
         /* Load assembly tokens on first use of inline asm */
         if (!tcc_state->asm_tokens_loaded)
             tcc_load_asm_tokens(tcc_state);

         /* ... existing asm parsing code ... */
     }
     ```

7. [ ] **Add trigger in `asm_global_instr()` (tccasm.c)**
   - Location: In `tccasm.c`, at the beginning of `asm_global_instr()` function
   - Action: Add lazy load check for global assembly statements
   - Files modified: `tccasm.c`
   - Implementation:
     ```c
     ST_FUNC void asm_global_instr(void)
     {
         if (!tcc_state->asm_tokens_loaded)
             tcc_load_asm_tokens(tcc_state);

         /* ... existing code ... */
     }
     ```

8. [ ] **Add trigger in `tcc_assemble()` (tccasm.c)**
   - Location: In `tccasm.c`, at the beginning of `tcc_assemble()` function
   - Action: Add lazy load check for standalone .S assembly files
   - Files modified: `tccasm.c`
   - Implementation:
     ```c
     ST_FUNC void tcc_assemble(TCCState *s1, int do_preprocess)
     {
         if (!s1->asm_tokens_loaded)
             tcc_load_asm_tokens(s1);

         /* ... existing code ... */
     }
     ```

9. [ ] **Handle token lookup in `tok_alloc()` (tccpp.c)**
   - Location: In `tccpp.c`, within `tok_alloc()` function
   - Action: Add lazy load when looking up asm-like identifiers in .S files
   - Files modified: `tccpp.c`
   - Implementation:
     ```c
     ST_FUNC TokenSym *tok_alloc(const char *str, int len)
     {
         /* ... existing hash lookup ... */

         /* If not found and could be an asm instruction, try loading asm tokens */
         if (!ts && !tcc_state->asm_tokens_loaded &&
             (parse_flags & PARSE_FLAG_ASM_FILE)) {
             tcc_load_asm_tokens(tcc_state);
             /* Retry lookup */
             return tok_alloc(str, len);
         }

         return tok_alloc_new(pts, str, len);
     }
     ```

### Phase 3: Testing

10. [ ] **Test C-only compilation**
    - Verify no assembly tokens loaded
    - Command: `echo 'int main() { return 0; }' | ./tcc -c - -o /dev/null`
    - Expected: ~500 TokenSyms instead of ~10,500
    - Add debug print in `tccpp_new()` to count tokens

11. [ ] **Test inline assembly**
    - Verify tokens loaded on demand
    - Command: `echo 'int main() { __asm__("nop"); return 0; }' | ./tcc -c - -o /dev/null`
    - Expected: ~10,500 TokenSyms

12. [ ] **Test assembly files**
    - Verify tokens loaded for .S files
    - Command: `./tcc -c test.S -o test.o`
    - Create test.S with basic assembly instructions

13. [ ] **Run full test suite**
    - Ensure no regressions in existing functionality
    - Pay special attention to assembly-related tests

14. [ ] **Test VFP instructions specifically**
    - Create test file with VFP instructions: `vadd.f32`, `vmov`, `vcmp`, etc.
    - Verify VFP tokens are loaded correctly
    - Test both inline asm and .S file paths

15. [ ] **Remove debug prints**
    - Clean up any temporary debug output from tccpp.c

---

## VFP (Vector Floating Point) Parsing Section

### Overview

VFP instructions are ARM/Thumb floating-point instructions that operate on:
- **Single-precision registers**: `s0`-`s31` (32-bit)
- **Double-precision registers**: `d0`-`d15` (64-bit)
- **VFP status registers**: `fpsid`, `fpscr`, `fpexc`

### VFP Token Expansion

Like other ARM instructions, VFP tokens are expanded with:
- 16 condition codes (eq, ne, cs, cc, mi, pl, vs, vc, hi, ls, ge, lt, gt, le, base, rsvd)
- Type suffixes (`.f32`, `.f64`)

For example, `vadd` expands to:
- `vaddeq.f32`, `vaddne.f32`, ... (16 × 2 = 32 tokens for f32)
- `vaddeq.f64`, `vaddne.f64`, ... (16 × 2 = 32 tokens for f64)

Total for each VFP instruction: **~64 tokens**

### VFP Instructions Defined in thumb-tok.h

| Category | Instructions | Tokens per Instruction | Total Tokens |
|----------|--------------|------------------------|--------------|
| Arithmetic | vadd, vsub, vmul, vdiv, vneg | 32 | 160 |
| Comparison | vcmp | 32 | 32 |
| Data Transfer | vmov, vpush, vpop | 32-64 | ~128 |
| Status Register | vmrs | 16 | 16 |
| **Total** | **~10 instructions** | **~32** | **~336 tokens** |

### VFP Parsing in arm-thumb-asm.c

VFP parsing is handled by specialized functions:

#### 1. `thumb_vfp_arith_opcode()` - Arithmetic Operations
**Location**: [arm-thumb-asm.c:2092](arm-thumb-asm.c#L2092)

Handles: `vadd`, `vsub`, `vmul`, `vdiv`, `vneg`

**Syntax**:
```
vadd.f32 s0, s1, s2    @ s0 = s1 + s2 (single-precision)
vadd.f64 d0, d1, d2    @ d0 = d1 + d2 (double-precision)
vneg.f32 s0, s1        @ s0 = -s1 (unary)
```

**Implementation steps**:
1. Skip suffix tokens if present (e.g., `.f32`)
2. Parse operands using `process_operands()`
3. Determine operand size from token (`thumb_vfp_size_from_token()`)
4. Validate operand types match expected size
5. Emit appropriate VFP opcode

#### 2. `thumb_vmov_opcode()` - Data Transfer
**Location**: [arm-thumb-asm.c:2135](arm-thumb-asm.c#L2135)

Handles various `vmov` variants:
- VFP register to VFP register: `vmov s0, s1`
- GP to single-precision: `vmov r0, s0`
- Two GP to double-precision: `vmov d0, r0, r1`

**Syntax**:
```
vmov s0, s1           @ VFP to VFP (single)
vmov d0, d1           @ VFP to VFP (double)
vmov r0, s0           @ VFP to ARM register
vmov s0, r0           @ ARM register to VFP
vmov d0, r0, r1       @ Two ARM registers to VFP double
vmov r0, r1, d0       @ VFP double to two ARM registers
```

#### 3. `thumb_vcmp_opcode()` - Comparison
**Location**: [arm-thumb-asm.c:2193](arm-thumb-asm.c#L2193)

**Syntax**:
```
vcmp.f32 s0, s1       @ Compare s0 and s1, set FPSCR flags
vcmp.f64 d0, d1       @ Compare d0 and d1
```

#### 4. `thumb_vmrs_opcode()` - Status Register Access
**Location**: [arm-thumb-asm.c:2217](arm-thumb-asm.c#L2217)

**Syntax**:
```
vmrs r0, fpscr        @ Move FP status to ARM register
```

### VFP Register Parsing

VFP registers are registered as assembly tokens in [thumb-tok.h:31-93](thumb-tok.h#L31):

```c
/* Single-precision VFP registers s0-s31 */
DEF_ASM(s0) ... DEF_ASM(s31)

/* Double-precision VFP registers d0-d15 */
DEF_ASM(d0) ... DEF_ASM(d15)

/* VFP status registers */
DEF_ASM(fpsid)
DEF_ASM(fpscr)
DEF_ASM(fpexc)

/* VFP magical ARM register */
DEF_ASM(apsr_nzcv)
```

### VFP Suffix Handling

VFP instructions with type suffixes (`.f32`, `.f64`) are split during tokenization:

**Tokenization of `vadd.f32 s0, s1, s2`**:
1. `vadd` - base instruction token
2. `.` - separator token
3. `f32` - type suffix token
4. `s0` - destination register
5. `,` - comma
6. `s1` - source register 1
7. `,` - comma
8. `s2` - source register 2

The parser handles this in `thumb_vfp_arith_opcode()`:
```c
// Skip suffix tokens if present
if (tok == '.') {
    next(); // skip the dot
    next(); // skip the suffix (f32 or f64)
}
```

### VFP Instruction Dispatch

VFP instructions are dispatched in `asm_opcode()` at [arm-thumb-asm.c:3038-3064](arm-thumb-asm.c#L3038):

```c
const char *token_str = get_tok_str(token, NULL);
if (strncmp(token_str, "vmov", 4) == 0) {
    thumb_emit_opcode(thumb_vmov_opcode(s1, token));
    return;
}
if (strncmp(token_str, "vadd", 4) == 0 || strncmp(token_str, "vsub", 4) == 0 ||
    strncmp(token_str, "vmul", 4) == 0 || strncmp(token_str, "vdiv", 4) == 0 ||
    strncmp(token_str, "vneg", 4) == 0) {
    thumb_emit_opcode(thumb_vfp_arith_opcode(s1, token));
    return;
}
if (strncmp(token_str, "vcmp", 4) == 0) {
    thumb_emit_opcode(thumb_vcmp_opcode(s1, token));
    return;
}
if (strncmp(token_str, "vmrs", 4) == 0) {
    thumb_emit_opcode(thumb_vmrs_opcode(s1, token));
    return;
}
```

### VFP Lazy Loading Impact

With lazy loading:
- **Before**: VFP tokens (~336) always loaded at startup
- **After**: VFP tokens only loaded when assembly is used
- **Savings**: ~16KB for C-only programs

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Token ID gaps if asm tokens loaded late | Use reserved range for asm tokens |
| Performance impact of lazy check | Single boolean check, negligible |
| Missed trigger points | Comprehensive testing of asm paths |
| Two-phase mode complexity | Load in both phases if needed |
| VFP token lookup failures | Test all VFP instruction variants |
| Suffix token parsing issues | Test `.f32` and `.f64` suffixes explicitly |
