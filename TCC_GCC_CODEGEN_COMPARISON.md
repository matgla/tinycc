# TCC vs GCC Code Generation Comparison & Optimization Plan

## Status Update (Fix Applied)

**Indexed Load Fusion Fix Applied!**

The indexed load optimization was not triggering because the pattern matcher assumed only ONE operand in an ADD has a vreg. When both operands have vregs (common: `ADD P0, T0` where P0 is a parameter and T0 is a temp), it would try P0 first and fail to find a SHL defining it.

**Fix:** Check both operands for SHL definition instead of stopping at the first vreg.

**Results After Fix:**
```
+-----------+-------+-------+-------+
| Compiler  | text  | data  |  bss  |
+-----------+-------+-------+-------+
| TCC -O0   |   362 |     0 |     0 |
| TCC -O1   |   262 |     0 |     0 |  ← was 282
| GCC -O1   |   188 |     0 |     0 |
+-----------+-------+-------+-------+

Ratio: TCC -O1 / GCC -O1 = 1.39x  ← was 1.50x
```

**Per-Function Improvements:**
| Function         | Before | After | GCC  | Improvement |
|------------------|--------|-------|------|-------------|
| load_element     | 10     | **6** | 6    | **-40%**    |
| bubble_sort      | 124    | **108**| 76  | **-13%**    |

---

## Executive Summary

| Function         | TCC -O1 | GCC -O1 | TCC/GCC | Potential Savings |
|------------------|---------|---------|---------|-------------------|
| bubble_sort      | 124     | 76      | 1.63x   | 48 bytes (39%)    |
| copy_sum         | 60      | 36      | 1.66x   | 24 bytes (40%)    |
| dot_product      | 52      | 40      | 1.30x   | 12 bytes (23%)    |
| load_element     | 10      | 6       | 1.66x   | 4 bytes (40%)     |
| sum_array        | 36      | 30      | 1.20x   | 6 bytes (17%)     |
| **TOTAL**        | **282** | **188** | **1.50x** | **94 bytes (33%)**|

---

## Detailed Function-by-Function Analysis

### 1. `load_element` (TCC: 10 bytes, GCC: 6 bytes) — Easiest Win

**TCC -O1 Disassembly (10 bytes = 3 instructions):**
```armasm
00000110 <load_element>:
 110:   ea4f 0281       mov.w   r2, r1, lsl #2      ; 4 bytes - shift index
 114:   1883            adds    r3, r0, r2          ; 2 bytes - add to base
 116:   6818            ldr     r0, [r3, #0]        ; 2 bytes - load
 118:   4770            bx      lr                  ; 2 bytes - return
```

**GCC -O1 Disassembly (6 bytes = 2 instructions):**
```armasm
000000b6 <load_element>:
  b6:   f850 0021       ldr.w   r0, [r0, r1, lsl #2]  ; 4 bytes - indexed load with shift!
  ba:   4770            bx      lr                     ; 2 bytes - return
```

**Root Cause:** TCC's indexed load optimization IS generating the pattern in IR:
```
0000: T0 <-- P1 SHL #2
0001: T1 <-- P0 ADD T0
0002: T2 <-- T1***DEREF*** [LOAD]
```
But the optimized IR still shows:
```
0000: R2(T0) <-- R1(P1) SHL #2
0001: R3(T1) <-- R0(P0) ADD R2(T0)
0002: R0(T2) <-- R3(T1)***DEREF*** [LOAD]
```

**Issue:** The indexed memory fusion optimization (`opt_indexed_memory`) is NOT being triggered!

**Fix Required:**
1. Check why `ir_opt_indexed_memory_fusion()` doesn't fire on this pattern
2. Verify the pattern matcher handles SHL+ADD+LOAD with these operand forms
3. Ensure `TCCIR_OP_LOAD_INDEXED` is generated when pattern matches

---

### 2. `sum_array` (TCC: 36 bytes, GCC: 30 bytes)

**TCC -O1 Disassembly (36 bytes):**
```armasm
000000ec <sum_array>:
  ec:   b430            push    {r4, r5}              ; 2 bytes
  ee:   2200            movs    r2, #0                ; 2 bytes - sum = 0
  f0:   460b            mov     r3, r1                ; 2 bytes - save n
  f2:   f101 34ff       add.w   r4, r1, #-1           ; 4 bytes - n-1
  f6:   4621            mov     r1, r4                ; 2 bytes - update n
  f8:   2b00            cmp     r3, #0                ; 2 bytes - check old n
  fa:   dd06            ble.n   10a                   ; 2 bytes
  fc:   4604            mov     r4, r0                ; 2 bytes - save ptr
  fe:   1d05            adds    r5, r0, #4            ; 2 bytes - ptr+4
 100:   4628            mov     r0, r5                ; 2 bytes - update ptr
 102:   f8d4 c000       ldr.w   ip, [r4]              ; 4 bytes - load *oldptr
 106:   4462            add     r2, ip                ; 2 bytes - sum += val
 108:   e7f2            b.n     f0                    ; 2 bytes
 10a:   4610            mov     r0, r2                ; 2 bytes
 10c:   bc30            pop     {r4, r5}              ; 2 bytes
 10e:   4770            bx      lr                    ; 2 bytes
```

**GCC -O1 Disassembly (30 bytes):**
```armasm
00000098 <sum_array>:
  98:   4602            mov     r2, r0                ; 2 bytes - ptr to r2
  9a:   1e4b            subs    r3, r1, #1            ; 2 bytes - n-1
  9c:   2900            cmp     r1, #0                ; 2 bytes - check n
  9e:   dd08            ble.n   b2                    ; 2 bytes
  a0:   2000            movs    r0, #0                ; 2 bytes - sum = 0
  a2:   f852 1b04       ldr.w   r1, [r2], #4          ; 4 bytes - POST-INCREMENT load!
  a6:   4408            add     r0, r1                ; 2 bytes - sum += val
  a8:   3b01            subs    r3, #1                ; 2 bytes - decrement counter
  aa:   f1b3 3fff       cmp.w   r3, #-1               ; 4 bytes
  ae:   d1f8            bne.n   a2                    ; 2 bytes
  b0:   4770            bx      lr                    ; 2 bytes
  b2:   2000            movs    r0, #0                ; 2 bytes
  b4:   4770            bx      lr                    ; 2 bytes
```

**Key Differences:**
1. **Post-increment load** - GCC uses `ldr.w r1, [r2], #4` (4 bytes) vs TCC's 3 instructions (8 bytes)
2. **Register pressure** - GCC doesn't need to save r4/r5
3. **No redundant MOVs** - TCC has `mov r4, r0; adds r5, r0, #4; mov r0, r5`

**TCC IR shows the pattern:**
```
0006: R4(T2) <-- R0(P0) [ASSIGN]     ; ptr copy
0007: R5(T3) <-- R0(P0) ADD #4       ; ptr + 4
0008: R0(P0) <-- R5(T3) [STORE]      ; update ptr
0009: R2(V0) <-- R2(V0) ADD R4(T2)***DEREF***  ; sum += *oldptr
```

**Fixes Required:**
1. Post-increment fusion should match `ptr_old = ptr; ptr = ptr + 4; *ptr_old`
2. The pattern exists in `ir_opt_postinc_fusion()` but isn't firing
3. Need to check if `opt_postinc_fusion` is enabled and pattern matcher works

---

### 3. `copy_sum` (TCC: 60 bytes, GCC: 36 bytes) — Biggest Relative Gap

**TCC -O1 (60 bytes) Loop Body:**
```armasm
  c0:   4605            mov     r5, r0                ; save dst
  c2:   1d06            adds    r6, r0, #4            ; dst+4
  c4:   4630            mov     r0, r6                ; update dst
  c6:   460e            mov     r6, r1                ; save src1
  c8:   f101 0804       add.w   r8, r1, #4            ; src1+4
  cc:   4641            mov     r1, r8                ; update src1
  ce:   4690            mov     r8, r2                ; save src2
  d0:   f102 0904       add.w   r9, r2, #4            ; src2+4
  d4:   464a            mov     r2, r9                ; update src2
  d6:   f8d6 c000       ldr.w   ip, [r6]              ; load *old_src1
  da:   f8d8 e000       ldr.w   lr, [r8]              ; load *old_src2
  de:   eb0c 090e       add.w   r9, ip, lr            ; sum
  e2:   f8c5 9000       str.w   r9, [r5]              ; store to *old_dst
```

**GCC -O1 (36 bytes) Loop Body:**
```armasm
  80:   f851 3b04       ldr.w   r3, [r1], #4          ; POST-INC load src1
  84:   f852 4b04       ldr.w   r4, [r2], #4          ; POST-INC load src2
  88:   4423            add     r3, r4                ; sum
  8a:   f840 3b04       str.w   r3, [r0], #4          ; POST-INC store dst
```

**Analysis:**
- TCC loop: 18 instructions (44 bytes in loop)
- GCC loop: 4 instructions (16 bytes in loop)
- **3× improvement possible with post-increment addressing!**

**Fixes Required:**
1. Post-increment load fusion for ALL THREE pointers
2. Post-increment store fusion
3. Dead code elimination for the old pointer values

---

### 4. `dot_product` (TCC: 52 bytes, GCC: 40 bytes)

**GCC Loop:**
```armasm
  5c:   f853 2f04       ldr.w   r2, [r3, #4]!         ; PRE-INCREMENT load a[i]
  60:   f851 4f04       ldr.w   r4, [r1, #4]!         ; PRE-INCREMENT load b[i]
  64:   fb04 0002       mla     r0, r4, r2, r0        ; multiply-accumulate!
  68:   4563            cmp     r3, ip
  6a:   d1f7            bne.n   5c
```

**Key Optimizations Missing:**
1. **Pre-increment addressing** (`[r3, #4]!`) - different from post-increment
2. **MLA instruction** - GCC uses `mla r0, r4, r2, r0` for `sum += a[i] * b[i]`
3. **Pointer-based loop termination** - compare against end pointer, not counter

**TCC IR already has MLA support:**
```
TCCIR_OP_MLA,       /* Multiply-Accumulate: dest = src1 * src2 + accum */
```
But it's not being generated for this pattern!

**Fixes Required:**
1. Add pre-increment addressing mode (new IR op: `TCCIR_OP_LOAD_PREINC`)
2. Generate MLA for `sum += expr1 * expr2` pattern
3. Consider strength reduction: counter → pointer comparison

---

### 5. `bubble_sort` (TCC: 124 bytes, GCC: 76 bytes) — Most Complex

**Key GCC Optimizations:**
```armasm
  1a:   681a            ldr     r2, [r3, #0]          ; load arr[j]
  1c:   f853 1f04       ldr.w   r1, [r3, #4]!         ; PRE-INC load arr[j+1], update j ptr
  20:   428a            cmp     r2, r1                ; compare values
  22:   bfc4            itt     gt                    ; IT block for conditional
  24:   f843 1c04       strgt.w r1, [r3, #-4]         ; conditional store arr[j] = arr[j+1]
  28:   601a            strgt   r2, [r3, #0]          ; conditional store arr[j+1] = tmp
```

**GCC Tricks TCC Doesn't Use:**
1. **IT blocks (If-Then)** - 2 conditional stores without branch
2. **Pre-increment load** - `ldr.w r1, [r3, #4]!`
3. **Negative offset store** - `str.w r1, [r3, #-4]`
4. **Pointer-based loop** - compare against end pointer

---

## Implementation Priority Matrix

| Optimization | Impact | Complexity | Priority | Functions Affected |
|-------------|--------|------------|----------|-------------------|
| Fix indexed load fusion | HIGH | LOW | **P0** | load_element |
| Fix post-increment fusion | HIGH | MEDIUM | **P0** | sum_array, copy_sum |
| Add pre-increment addressing | HIGH | MEDIUM | **P1** | dot_product, bubble_sort |
| Generate MLA for mul+acc | MEDIUM | LOW | **P1** | dot_product |
| Pointer loop termination | MEDIUM | HIGH | **P2** | all loops |
| IT blocks for conditionals | LOW | HIGH | **P3** | bubble_sort |

---

## Detailed Implementation Plan

### Phase 1: Fix Indexed Load Fusion (P0) — Estimated: 4-8 hours

**Current State:**
- `TCCIR_OP_LOAD_INDEXED` exists in IR
- `tcc_gen_machine_load_indexed_op()` generates correct code
- Pattern matcher in `ir_opt_indexed_memory_fusion()` exists

**Debug Steps:**
1. Add debug output to `ir_opt_indexed_memory_fusion()` to see why pattern doesn't match
2. Verify pattern matches: `SHL scale; ADD base, shifted; LOAD addr`
3. Check operand type constraints (vreg vs temp, etc.)

**Expected Fix Location:** [ir/opt.c](ir/opt.c) around line 3171

**Test Case:**
```c
int load_element(int *arr, int idx) { return arr[idx]; }
// Should generate: ldr.w r0, [r0, r1, lsl #2]
```

### Phase 2: Fix Post-Increment Fusion (P0) — Estimated: 8-16 hours

**Pattern Analysis for `sum_array`:**
```
BEFORE (TCC IR):
  R4(T2) <-- R0(P0) [ASSIGN]        ; ptr_old = ptr
  R5(T3) <-- R0(P0) ADD #4          ; tmp = ptr + 4
  R0(P0) <-- R5(T3) [STORE]         ; ptr = tmp
  ... ADD R4(T2)***DEREF***         ; use *ptr_old

SHOULD BECOME:
  R4(T2), R0(P0) <-- LOAD_POSTINC R0(P0), #4
```

**Pattern for post-increment (canonical form):**
```
ptr_old = ptr        ; ASSIGN
ptr = ptr + stride   ; ADD + STORE
use = *ptr_old       ; LOAD (deref of ptr_old)
```

**Implementation Steps:**
1. Review `ir_opt_postinc_fusion()` in [ir/opt.c](ir/opt.c#L3461)
2. Pattern must handle: `old = ptr; ptr = ptr + N; *old`
3. Verify stride matches element size (4 for int, 1 for char, etc.)
4. Update liveness analysis to handle modified operand

**Files to Modify:**
- [ir/opt.c](ir/opt.c) - pattern matcher
- [arm-thumb-gen.c](arm-thumb-gen.c#L5333) - already has `tcc_gen_machine_load_postinc_op()`

### Phase 3: Add Pre-Increment Addressing (P1) — Estimated: 16-24 hours

**New IR Operation:**
```c
TCCIR_OP_LOAD_PREINC,   /* ptr += offset; dest = *ptr - ARM LDR rd,[rn,#imm]! */
TCCIR_OP_STORE_PREINC,  /* ptr += offset; *ptr = src - ARM STR rd,[rn,#imm]! */
```

**Pattern for pre-increment:**
```
ptr = ptr + stride   ; ADD + STORE (update FIRST)
val = *ptr           ; LOAD from NEW ptr value
```

**Key difference from post-increment:**
- Post-inc: load from OLD pointer, then increment
- Pre-inc: increment first, then load from NEW pointer

**ARM Encoding:**
- `ldr.w rt, [rn, #imm]!` — puw=5 (p=1, u=1, w=1) for positive offset
- `ldr.w rt, [rn, #-imm]!` — puw=5 (p=1, u=0, w=1) for negative offset

**Implementation Files:**
- [tccir.h](tccir.h) - add new opcodes
- [ir/opt.c](ir/opt.c) - add pattern matcher
- [ir/dump.c](ir/dump.c) - add IR dump support
- [arm-thumb-gen.c](arm-thumb-gen.c) - add code generation

### Phase 4: Generate MLA for Multiply-Accumulate (P1) — Estimated: 4-8 hours

**Pattern:**
```c
sum += a[i] * b[i];

IR BEFORE:
  T6 <-- T3***DEREF*** MUL T5***DEREF***
  V0 <-- V0 ADD T6

IR AFTER:
  V0 <-- MLA(V0, T3***DEREF***, T5***DEREF***)
```

**MLA already exists in IR:** `TCCIR_OP_MLA`

**Pattern matcher needed:**
```
if (op == TCCIR_OP_ADD &&
    prev_op == TCCIR_OP_MUL &&
    add.src2 == mul.dest &&
    add.dest == add.src1) {
  // Convert to MLA
}
```

**ARM Encoding:**
- `mla rd, rn, rm, ra` — rd = rn * rm + ra

---

## Verification Plan

### Instruction Count Accuracy

ARM Thumb-2 uses mixed 16-bit and 32-bit encodings. To accurately count:

```bash
# Count bytes (most reliable)
arm-none-eabi-nm -S file.o | grep " T func_name"

# Count instructions (must account for encoding)
arm-none-eabi-objdump -d file.o | grep -E "^\s+[0-9a-f]+:" | \
  awk '{
    addr = strtonum("0x" $1)
    if (prev_addr != "") {
      size = addr - prev_addr
      if (size == 2) count16++
      else if (size == 4) count32++
    }
    prev_addr = addr
  }
  END { print "16-bit: " count16 ", 32-bit: " count32 }'
```

### Test Commands

```bash
# Compare specific function
arm-none-eabi-objdump -d /tmp/tcc_O1.o | grep -A30 "<load_element>:"
arm-none-eabi-objdump -d /tmp/gcc_O1.o | grep -A10 "<load_element>:"

# Run comparison script
./scripts/compare_codegen.sh

# Dump IR for debugging
./armv8m-tcc -O1 -dump-ir -c test.c -o /dev/null
```

---

## Success Metrics

After implementing all P0+P1 optimizations:

| Function         | Current | Target  | Improvement |
|------------------|---------|---------|-------------|
| load_element     | 10      | 6       | -40%        |
| sum_array        | 36      | 28-30   | -17-22%     |
| copy_sum         | 60      | 40-44   | -27-33%     |
| dot_product      | 52      | 40-44   | -15-23%     |
| bubble_sort      | 124     | 90-100  | -19-27%     |
| **TOTAL**        | **282** | **~210** | **~25%**   |

**Ultimate Goal:** TCC -O1 within 1.2× of GCC -O1 (currently 1.5×)
