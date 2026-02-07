# TCC Optimization Plan V2 - Disassembly Analysis

## Current Status (Feb 2026)
- **TCC -O1**: 346 bytes (1.84x GCC -O1)
- **GCC -O1**: 188 bytes

---

## DETAILED IMPLEMENTATION: Fix LR Push/Pop in Loops

### Problem Analysis - UPDATED FINDINGS

**Status: PARTIALLY WORKING**

Testing reveals two cases:

#### Case 1: Non-leaf functions ✅ WORKING
```asm
; Prologue saves LR
stmdb   sp!, {r4, r5, r6, r8, ip, lr}
...
; Loop uses LR freely without push/pop
ldr.w   ip, [r6]
ldr.w   lr, [r5]      ; LR used as scratch - NO PUSH!
mul.w   r8, ip, lr
...
; Epilogue restores via PC
ldmia.w sp!, {..., pc}
```

#### Case 2: Leaf functions ❌ STILL PUSHING/POPPING
```asm
; Prologue does NOT save LR (leaf function)
stmdb   sp!, {r4, r5, r6, r8}
...
; Loop must push/pop LR to preserve return address
ldr.w   ip, [r6]
push    {lr}          ; Save return address
ldr.w   lr, [r5]      ; Use LR as scratch
mul.w   r8, ip, lr
pop     {lr}          ; Restore return address
```

### Root Cause (Leaf Functions)

In leaf functions:
1. LR contains the return address and is NOT saved at prologue
2. When register pressure is high, `get_scratch_reg_with_save()` picks LR
3. It correctly pushes LR to preserve the return address
4. But this adds 4 bytes per push/pop pair **inside the loop**

### Solution for Leaf Functions

**Option A: Promote to "pseudo non-leaf" when LR needed as scratch**

If we detect that we'll need LR as scratch, save it at prologue instead of inside the loop.

```c
// In prologue generation (arm-thumb-gen.c around line 4751):
// Check if any instruction in the function will need LR as scratch
if (leaffunc && will_need_lr_as_scratch(ir)) {
    registers_to_push |= (1 << R_LR);  // Save LR at prologue
    ir->leaffunc = 0;  // Treat as non-leaf for scratch allocation
}
```

**Option B: Avoid LR in leaf functions entirely**

Modify `get_scratch_reg_with_save()` to never pick LR for leaf functions, forcing it to use callee-saved registers (R4-R11) with prologue saves instead.

### Updated TODO List

- [x] **1. Verify non-leaf functions work** ✅ CONFIRMED WORKING
- [ ] **2. Fix leaf function case**
  - [ ] 2.1 Add pre-scan to detect if LR will be needed as scratch
  - [ ] 2.2 If yes, save LR at prologue and mark as non-leaf for scratch purposes
  - [ ] 2.3 Alternative: avoid LR in leaf functions, prefer R4-R11

---

### Original Analysis (kept for reference)

In functions like `copy_sum` and `dot_product`, TCC generates:
```asm
push    {lr}          ; Save LR to use as scratch
ldr.w   lr, [r8]      ; Use LR as scratch register
...
pop     {lr}          ; Restore LR
```

This happens because:
1. `tcc_ls_find_free_scratch_reg()` returns LR (R14) as a "free" register
2. `get_scratch_reg_with_save()` then PUSHES LR to save its value before using it
3. After using LR, it POPS to restore

But this is wasteful because:
- In non-leaf functions, LR is already saved at function prologue
- The value in LR mid-function is **garbage** (or the saved return address copy)
- We're saving garbage and restoring garbage

### Root Cause

In `tccls.c:tcc_ls_find_free_scratch_reg()`:
```c
/* Finally try LR if not a leaf function */
if (!is_leaf && !(live_regs & (1u << 14)))
  return 14;
```

This returns LR as available, but then in `arm-thumb-gen.c:get_scratch_reg_with_save()`:
```c
if (reg found by tcc_ls_find_free_scratch_reg)
  return { .reg = reg, .saved = 0 };  // NO PUSH needed
else
  // Fall through to push/pop logic
```

Wait - if `tcc_ls_find_free_scratch_reg` returns LR, it should NOT need push/pop. Let me re-examine...

Actually the issue is that `tcc_ls_find_free_scratch_reg` returns `PREG_NONE` when all regs are live, and then the fallback in `get_scratch_reg_with_save` picks LR and pushes it.

### The Real Flow

1. All R0-R12 are live (used by register allocator)
2. `tcc_ls_find_free_scratch_reg` returns `PREG_NONE`
3. `get_scratch_reg_with_save` falls back to `no_free_reg` label
4. It picks R_IP (R12) first, but R12 is excluded (already in use)
5. It only picks R_LR if `ir->leaffunc` (but we're in non-leaf!)
6. It picks R0-R3, but they're excluded too
7. Falls through to R4-R11, picks one and PUSHES it

So the real issue is: **we need more scratch registers available**.

### Solution Options

#### Option A: Make LR available as scratch in non-leaf functions (preferred)
Since LR is already saved at prologue, we can use it freely without push/pop.

**Change in `get_scratch_reg_with_save`:**
```c
// After no_free_reg label, BEFORE trying R_IP:
if (ir && !ir->leaffunc && !(exclude_regs & (1 << R_LR)))
{
  // Non-leaf function: LR is saved at prologue, we can use it freely
  reg_to_save = R_LR;
  result.reg = R_LR;
  result.saved = 0;  // DON'T PUSH - already saved at prologue!
  scratch_global_exclude |= (1u << R_LR);
  return result;
}
```

**Risk:** None - LR is always saved at prologue in non-leaf functions.

#### Option B: Improve register allocation to reduce pressure
More complex, requires changes to tccls.c.

#### Option C: Use more callee-saved registers (R4-R11)
Would require ensuring they're saved at prologue if used.

### Matching Algorithm

```
WHEN: get_scratch_reg_with_save() needs a register and none are free

IF is_non_leaf_function AND LR_not_excluded:
    RETURN LR without push/pop (it's already saved at prologue)
ELSE IF R_IP_not_excluded:
    PUSH R_IP, use it, POP later
ELSE IF is_leaf AND LR_not_excluded:
    PUSH LR, use it, POP later
ELSE:
    Try R0-R3, R4-R11 with push/pop
```

### Draft Implementation

```c
// In arm-thumb-gen.c, modify get_scratch_reg_with_save()

static ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs)
{
  ScratchRegAlloc result = {0};
  TCCIRState *ir = tcc_state->ir;

  exclude_regs |= scratch_global_exclude;

  // 1. First try to find a truly free register via liveness analysis
  if (ir) {
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx,
                                            exclude_regs, ir->leaffunc);
    if (reg != PREG_NONE && reg < 16) {
      result.reg = reg;
      result.saved = 0;
      scratch_global_exclude |= (1u << reg);
      return result;
    }
  }

no_free_reg:
  // 2. NEW: In non-leaf functions, LR is saved at prologue - use it freely!
  if (ir && !ir->leaffunc && !(exclude_regs & (1 << R_LR)))
  {
    result.reg = R_LR;
    result.saved = 0;  // No push needed - already saved at prologue
    scratch_global_exclude |= (1u << R_LR);
    return result;
  }

  // 3. Fall back to push/pop for IP
  if (!(exclude_regs & (1 << R_IP)))
  {
    reg_to_save = R_IP;
  }
  // ... rest of existing code
}
```

### Files to Modify

1. **arm-thumb-gen.c** - `get_scratch_reg_with_save()` function
2. **tccls.c** - Optionally adjust `tcc_ls_find_free_scratch_reg()` priorities

### Testing Strategy

1. Compile `copy_sum` and verify no push/pop of LR in loop
2. Run full test suite to check for regressions
3. Compare code size before/after

### Expected Impact

- `copy_sum`: Remove 2 push + 2 pop = **8 bytes** saved per function
- `dot_product`: Remove 2 push + 2 pop = **8 bytes** saved
- `bubble_sort`: Similar savings
- **Total estimated**: 15-25 bytes across test functions

---

## TODO List for LR Fix

- [ ] **1. Analyze current flow**
  - [ ] 1.1 Add debug prints to `get_scratch_reg_with_save` to confirm when LR push/pop happens
  - [ ] 1.2 Verify LR is in `registers_to_push` at prologue for non-leaf functions
  - [ ] 1.3 Document which functions trigger the issue

- [ ] **2. Implement Option A**
  - [ ] 2.1 Modify `get_scratch_reg_with_save()` to use LR without push in non-leaf
  - [ ] 2.2 Add `!ir->leaffunc` check before `no_free_reg` push logic
  - [ ] 2.3 Ensure `scratch_global_exclude` tracks LR usage properly

- [ ] **3. Verify correctness**
  - [ ] 3.1 Check that LR is always pushed at prologue (line 4751 in arm-thumb-gen.c)
  - [ ] 3.2 Check that functions using LR as scratch still return correctly
  - [ ] 3.3 Verify no nested scratch allocations clobber LR unexpectedly

- [ ] **4. Test**
  - [ ] 4.1 Run `make test -j16`
  - [ ] 4.2 Compile `copy_sum`, `dot_product` and verify no LR push/pop in loops
  - [ ] 4.3 Run `compare_codegen.sh` and verify size reduction

- [ ] **5. Edge cases**
  - [ ] 5.1 What if LR is used AND we need another scratch? (nested allocation)
  - [ ] 5.2 What about functions that call other functions? (LR is clobbered by BL)
  - [ ] 5.3 Ensure `restore_scratch_reg` handles the `saved=0` case correctly

---

## Per-Function Analysis

### 1. `load_element` - TCC: 10 bytes, GCC: 6 bytes (1.66x)

**TCC -O1:**
```asm
mov.w   r2, r1, lsl #2      ; 4 bytes - shift index
adds    r3, r0, r2          ; 2 bytes - add base
ldr     r0, [r3, #0]        ; 2 bytes - load
bx      lr                  ; 2 bytes
```

**GCC -O1:**
```asm
ldr.w   r0, [r0, r1, lsl #2] ; 4 bytes - indexed load with shift!
bx      lr                   ; 2 bytes
```

**Missing Optimization: Indexed Load with Shift**
- GCC uses `ldr.w r0, [r0, r1, lsl #2]` - single instruction for `arr[idx]`
- TCC generates 3 instructions: shift, add, load
- **Priority: HIGH** - Very common pattern
- **Complexity: MEDIUM** - Need to recognize `base + (index << shift)` in code generator

---

### 2. `sum_array` - TCC: 40 bytes, GCC: 30 bytes (1.33x)

**TCC -O1 loop:**
```asm
12c:   mov     r3, r1              ; counter copy
12e:   add.w   r4, r1, #-1         ; decrement
132:   mov     r1, r4              ; move back
134:   cmp     r3, #0              ; compare old value
136:   ble.w   14a                 ; exit if <= 0
13a:   mov     r4, r0              ; ptr copy
13c:   adds    r5, r0, #4          ; ptr + 4
13e:   mov     r0, r5              ; update ptr
140:   ldr.w   ip, [r4]            ; load from old ptr
144:   add     r2, ip              ; accumulate
146:   b.w     12c                 ; loop
```

**GCC -O1 loop:**
```asm
a2:   ldr.w   r1, [r2], #4        ; POST-INCREMENT LOAD!
a6:   add     r0, r1              ; accumulate
a8:   subs    r3, #1              ; decrement counter
aa:   cmp.w   r3, #-1             ; compare
ae:   bne.n   a2                  ; loop
```

**Missing Optimizations:**
1. **Post-increment load** - `ldr.w r1, [r2], #4` vs 3 instructions
   - **Priority: HIGH** - Pattern exists but not matching correctly
   - Need to fix pattern: ADD comes BEFORE the LOAD in IR

2. **Redundant MOV elimination** - Many unnecessary register copies
   - **Priority: HIGH** - `mov r3, r1; mov r1, r4` is wasteful

3. **Narrow branch instructions** - TCC uses `b.w` (4 bytes), GCC uses `bne.n` (2 bytes)
   - **Priority: MEDIUM** - Check if branch target is in range for 16-bit encoding

---

### 3. `copy_sum` - TCC: 76 bytes, GCC: 36 bytes (2.11x) ⚠️ WORST

**TCC -O1 loop (simplified):**
```asm
f2:   mov     r5, r0              ; dst copy (for store later)
f4:   adds    r6, r0, #4          ; dst + 4
f6:   mov     r0, r6              ; update dst
f8:   mov     r6, r1              ; src1 copy
fa:   add.w   r8, r1, #4          ; src1 + 4
fe:   mov     r1, r8              ; update src1
100:  mov     r8, r2              ; src2 copy
102:  add.w   r9, r2, #4          ; src2 + 4
106:  mov     r2, r9              ; update src2
108:  ldr.w   ip, [r6]            ; load src1
10c:  push    {lr}                ; UNNECESSARY!
10e:  ldr.w   lr, [r8]            ; load src2
112:  add.w   r9, ip, lr          ; add
116:  pop     {lr}                ; UNNECESSARY!
11a:  str.w   r9, [r5]            ; store dst
11e:  b.w     ec                  ; loop
```

**GCC -O1 loop:**
```asm
80:   ldr.w   r3, [r1], #4        ; load src1 with post-inc
84:   ldr.w   r4, [r2], #4        ; load src2 with post-inc
88:   add     r3, r4              ; add
8a:   str.w   r3, [r0], #4        ; store dst with post-inc
8e:   add.w   ip, ip, #1          ; increment counter
92:   cmp     lr, ip              ; compare
94:   bne.n   80                  ; loop
```

**Missing Optimizations:**
1. **Post-increment load/store** - 3x usage in one loop!
   - TCC: 6 instructions per pointer (copy + add + mov)
   - GCC: 1 instruction per pointer

2. **Unnecessary push/pop of LR** - TCC is saving LR mid-loop!
   - **Priority: CRITICAL** - This is a register allocator bug
   - LR should not be used as a general-purpose register if we're going to push/pop it

3. **Excessive register copies** - 6 MOV instructions in loop body
   - **Priority: HIGH** - Coalescing needed

---

### 4. `dot_product` - TCC: 68 bytes, GCC: 40 bytes (1.70x)

**TCC -O1 loop:**
```asm
b0:   mov.w   r5, r4, lsl #2      ; i * 4
b4:   adds    r6, r0, r5          ; &a[i]
b6:   mov     r8, r5              ; copy offset
b8:   add.w   r5, r1, r8          ; &b[i]
bc:   ldr.w   ip, [r6]            ; load a[i]
c0:   push    {lr}                ; UNNECESSARY!
c2:   ldr.w   lr, [r5]            ; load b[i]
c6:   mul.w   r8, ip, lr          ; multiply
ca:   pop     {lr}                ; UNNECESSARY!
ce:   add     r3, r8              ; sum +=
d0:   b.w     aa                  ; loop
```

**GCC -O1 loop:**
```asm
5c:   ldr.w   r2, [r3, #4]!       ; PRE-INCREMENT load a[i]
60:   ldr.w   r4, [r1, #4]!       ; PRE-INCREMENT load b[i]
64:   mla     r0, r4, r2, r0      ; MLA! multiply-accumulate
68:   cmp     r3, ip              ; end check
6a:   bne.n   5c                  ; loop
```

**Missing Optimizations:**
1. **Pre-increment addressing** - `ldr.w r2, [r3, #4]!`
   - Different from post-increment: pointer updated BEFORE use
   - TCC doesn't have this pattern at all

2. **MLA (Multiply-Accumulate)** - `mla r0, r4, r2, r0`
   - `sum += a * b` in one instruction!
   - **Priority: HIGH** - Common DSP pattern

3. **Unnecessary push/pop of LR** - Same bug as copy_sum

4. **Loop counter optimization** - GCC uses end-pointer comparison
   - Instead of `i < n`, compare `ptr != end_ptr`
   - Eliminates index variable entirely

---

### 5. `bubble_sort` - TCC: 152 bytes, GCC: 76 bytes (2.00x)

**Key differences:**
1. **Conditional execution (IT blocks)** - GCC uses:
   ```asm
   itt     gt
   strgt.w r1, [r3, #-4]
   strgt   r2, [r3, #0]
   ```
   TCC uses branches instead of conditional execution

2. **Pre-increment addressing** - `ldr.w r1, [r3, #4]!`

3. **Register usage** - TCC saves 8 registers, GCC saves only LR

4. **Branch optimization** - GCC uses 16-bit branches, TCC uses 32-bit

---

## Optimization Priority List

### Critical (blocking multiple functions)
1. **Fix LR push/pop in loops** - Register allocator using LR then saving it
   - Impact: copy_sum, dot_product, bubble_sort
   - Fix: Don't allocate LR for values, or don't save it unnecessarily

### High Priority
2. **Indexed Load with Shift** - `ldr.w r0, [r0, r1, lsl #2]`
   - Impact: load_element, all array indexing
   - Complexity: Medium - IR pattern matching

3. **Post-increment Load/Store** - Fix pattern matching
   - Impact: sum_array, copy_sum
   - The IR pattern has ADD before LOAD, not after

4. **MLA (Multiply-Accumulate)** fusion
   - Impact: dot_product, any `sum += a * b`
   - Complexity: Medium - similar to existing MLA fusion

5. **MOV elimination / Register Coalescing**
   - Impact: All functions
   - Many `mov rX, rY` that shouldn't exist

### Medium Priority
6. **Pre-increment addressing** - `ldr.w r2, [r3, #4]!`
   - Impact: dot_product, bubble_sort
   - Different from post-increment

7. **Narrow branch encoding** - Use 16-bit branches when possible
   - Impact: All functions with loops
   - Check if target in ±2KB range

8. **Conditional execution (IT blocks)**
   - Impact: bubble_sort, any if-then patterns
   - Complexity: High - need predication analysis

### Lower Priority
9. **Loop counter to pointer comparison**
   - Transform `for(i=0; i<n; i++) arr[i]` to pointer iteration
   - Usually done by loop strength reduction

---

## Implementation Order

### Phase 1: Quick Wins (est. 10-15% improvement)
1. Fix LR allocation/spilling bug
2. Indexed load with shift: `ldr.w rd, [rn, rm, lsl #imm]`
3. Narrow branch encoding

### Phase 2: Pattern Matching (est. 15-20% improvement)
4. Fix post-increment pattern (ADD before LOAD)
5. Add pre-increment addressing
6. MLA fusion for `sum += a * b`

### Phase 3: Register Optimization (est. 10-15% improvement)
7. MOV elimination / copy propagation to registers
8. Better register allocation to reduce spills

### Phase 4: Advanced (est. 5-10% improvement)
9. IT blocks for conditional execution
10. Loop optimizations (strength reduction, etc.)

---

## Target
- Current: 1.84x GCC -O1
- After Phase 1: ~1.6x
- After Phase 2: ~1.4x
- After Phase 3: ~1.25x
- Ultimate goal: ~1.1-1.2x GCC -O1
