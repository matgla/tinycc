# TCC Optimization Plan: Catching Up to GCC -O1

## Executive Summary

Based on disassembly analysis of `bubble_sort` benchmark, TCC -O1 generates code that is **2.5x larger** and significantly slower than GCC -O1. This plan outlines specific optimizations to close this gap.

**Current State:**
| Metric | TCC -O1 | GCC -O1 | Gap |
|--------|---------|---------|-----|
| Code Size | ~188 bytes | ~76 bytes | 2.5x |
| Instructions | ~47 | 22 | 2.1x |
| Register Pressure | 6 saved (r4-r9) | 1 saved (lr) | 6x |

**Target:** Achieve within 1.5x code size and comparable performance to GCC -O1 for loop-heavy code.

---

## Phase 1: Common Subexpression Elimination (CSE) - HIGH PRIORITY

### Problem
TCC recalculates the same expressions repeatedly within loops. In `bubble_sort`, the array base address calculation `f5a7 7c80` (sub.w ip, r7, #256) appears **10+ times** in the inner loop.

### Current TCC Output
```asm
; Inner loop - array base calculated TWICE per iteration!
20004516:  f5a7 7c80   sub.w   ip, r7, #256      ; &arr[0] - FIRST time
2000451a:  eb0c 0504   add.w   r5, ip, r4        ; &arr[j]
...
20004524:  f5a7 7c80   sub.w   ip, r7, #256      ; &arr[0] - SECOND time (redundant!)
20004528:  eb0c 0406   add.w   r4, ip, r6        ; &arr[j+1]
```

### Solution
Implement CSE at the IR level (`tccir.c`) or during instruction selection (`arm-thumb-gen.c`).

**Implementation Steps:**
1. **IR-level CSE** (preferred):
   - Add hash table for available expressions in `tccir.c`
   - Track `IR_OP_SUB` with frame pointer and constant
   - Reuse previous result instead of generating new instruction
   
2. **Code-gen level CSE** (alternative):
   - Track recently computed values in `arm-thumb-gen.c`
   - Cache frame pointer offsets and reuse register

**Expected Impact:** 
- 30-40% reduction in inner loop instructions
- Eliminates redundant stack frame calculations

**Files to Modify:**
- `tccir.c` - Add CSE pass after IR generation
- `arm-thumb-gen.c` - Or add value caching during code gen

---

## Phase 2: Constant Propagation and Folding - HIGH PRIORITY

### Problem
TCC fails to propagate constants and fold expressions at compile time.

### Example: Initialization Loop

**TCC -O1 (multiplies every iteration):**
```asm
200044cc:  f04f 0c3f   mov.w   ip, #63           ; ip = 63
200044d0:  ebac 0002   sub.w   r0, ip, r2        ; r0 = 63 - i
200044d4:  f04f 0c07   mov.w   ip, #7            ; ip = 7
200044d8:  fb00 f40c   mul.w   r4, r0, ip        ; r4 = (63-i)*7
200044dc:  f104 0064   add.w   r0, r4, #100      ; r0 = (63-i)*7+100
```

**GCC -O1 (pre-computed, no multiply!):**
```asm
20003d3e:  f240 231d   movw    r3, #541          ; r3 = 541 (start: 63*7+100)
20003d42:  f842 3f04   str.w   r3, [r2, #4]!     ; Store, post-increment
20003d46:  3b07        subs    r3, #7            ; r3 -= 7 (no multiply!)
```

### Solution
Enhance constant propagation in the IR optimizer:

**Implementation Steps:**
1. Add constant propagation pass that tracks constant values through:
   - Assignment statements
   - Arithmetic with known constants
   - Loop induction variables
   
2. Recognize linear induction patterns:
   ```
   for (i = 0; i < 64; i++)
       arr[i] = (63 - i) * 7 + 100;
   ```
   Transform to:
   ```
   val = 541;  // 63*7+100
   for (i = 0; i < 64; i++) {
       arr[i] = val;
       val -= 7;
   }
   ```

**Expected Impact:**
- Eliminate expensive multiplications in initialization
- 20-30% speedup for array initialization patterns

**Files to Modify:**
- `tccir.c` - Add strength reduction pass
- Constant folding already exists, need to extend to induction variables

---

## Phase 3: Better Instruction Selection - MEDIUM PRIORITY

### Problem 1: No MLA (Multiply-Accumulate) Usage

**TCC:**
```asm
200045ac:  fb0c f200   mul.w   r2, ip, r0        ; r2 = arr[i] * i
200045b0:  1889        adds    r1, r1, r2        ; checksum += r2
```

**GCC:**
```asm
20003d70:  fb02 0003   mla     r0, r2, r3, r0    ; r0 += r2 * r3 (single instr!)
```

### Problem 2: No Post-Increment Addressing

**TCC (3 instructions per load):**
```asm
2000459c:  ea4f 0280   mov.w   r2, r0, lsl #2    ; r2 = i * 4
200045a0:  f5a7 7c80   sub.w   ip, r7, #256      ; ip = &arr[0]
200045a4:  eb0c 0302   add.w   r3, ip, r2        ; r3 = &arr[i]
200045a8:  f8d3 c000   ldr.w   ip, [r3]          ; load arr[i]
```

**GCC (1 instruction):**
```asm
20003d6c:  f85c 2f04   ldr.w   r2, [ip, #4]!     ; r2 = arr[i], ip += 4
```

### Solution
Add pattern matching for compound instructions in `arm-thumb-gen.c`:

**Implementation Steps:**
1. Add pattern for `MUL` followed by `ADD` with same destination → `MLA`
2. Add pattern for array access in loops → post-increment addressing
3. Recognize loop idioms:
   - `for (i=0; i<n; i++) arr[i]` → use post-increment pointer
   
**Expected Impact:**
- MLA: 1 instruction instead of 2 (50% reduction)
- Post-increment: 3-4 instructions → 1 instruction (75% reduction)

**Files to Modify:**
- `arm-thumb-gen.c` - Add instruction selection patterns
- `arm-thumb-opcodes.c` - Ensure MLA opcode exists

---

## Phase 4: Loop Structure Optimization - MEDIUM PRIORITY

### Problem
TCC generates naive nested loops with separate counters. GCC fuses loops and uses pointer arithmetic.

**TCC (nested loop structure):**
```asm
; Outer loop counter
200044e6:  2000        movs    r0, #0            ; i = 0
200044e8:  283f        cmp     r0, #63           ; i < 63?
...
; Inner loop counter  
200044f8:  2200        movs    r2, #0            ; j = 0
200044fa:  f04f 0c3f   mov.w   ip, #63
200044fe:  ebac 0300   sub.w   r3, ip, r0        ; r3 = 63 - i
20004502:  429a        cmp     r2, r3            ; j < 63-i?
```

**GCC (fused loop with end pointer):**
```asm
20003d4c:  4673        mov     r3, lr            ; r3 = array base
...
20003d5e:  4283        cmp     r3, r0            ; At end?
20003d60:  d1f5        bne.n   20003d4e          ; Continue inner
20003d62:  3804        subs    r0, #4            ; Reduce end pointer
20003d64:  4570        cmp     r0, lr            ; Done?
20003d66:  d1f1        bne.n   20003d4c          ; Continue outer
```

### Solution
GCC recognizes the bubble sort pattern and transforms it. This is advanced, so focus on:

**Implementation Steps:**
1. **Pointer-based iteration**:
   - Transform `arr[i]` loops to use running pointer
   - Increment pointer instead of recalculating offset
   
2. **Loop-invariant code motion**:
   - Move `63 - i` calculation outside inner loop
   - Hoist array base address calculation

**Expected Impact:**
- Simpler loop control
- Fewer instructions per iteration
- Better cache behavior

**Files to Modify:**
- `tccir.c` - Loop transformation pass
- `arm-thumb-gen.c` - Generate pointer-based loops

---

## Phase 5: LDR/STR with Offset Addressing - HIGH PRIORITY

### Problem
TCC generates explicit address calculations for array accesses instead of using ARM's offset addressing modes. This results in 3-4 instructions per array access when 1 would suffice.

### Example: Loading arr[j] and arr[j+1]

**TCC (4 instructions per load):**
```asm
; Load arr[j]
20004512:  ea4f 0482   mov.w   r4, r2, lsl #2    ; r4 = j * 4
20004516:  f5a7 7c80   sub.w   ip, r7, #256      ; ip = &arr[0] (CSE failed!)
2000451a:  eb0c 0504   add.w   r5, ip, r4        ; r5 = &arr[j]
2000452c:  f8d5 c000   ldr.w   ip, [r5]          ; ip = arr[j]

; Load arr[j+1] - recalculates everything!
2000451e:  1c54        adds    r4, r2, #1        ; r4 = j + 1
20004520:  ea4f 0684   mov.w   r6, r4, lsl #2    ; r6 = (j+1)*4
20004524:  f5a7 7c80   sub.w   ip, r7, #256      ; ip = &arr[0] (AGAIN!)
20004528:  eb0c 0406   add.w   r4, ip, r6        ; r4 = &arr[j+1]
20004532:  f8d4 e000   ldr.w   lr, [r4]          ; lr = arr[j+1]
```

**GCC (1-2 instructions with offset addressing):**
```asm
; r3 = array pointer, r0 = end pointer
20003d4e:  681a        ldr     r2, [r3, #0]      ; r2 = arr[j] (offset 0)
20003d50:  f853 1f04   ldr.w   r1, [r3, #4]!     ; r1 = arr[j+1], r3 += 4 (post-increment)
```

### ARM Addressing Modes Available

ARM Thumb-2 provides several efficient addressing modes TCC should use:

1. **`[Rn, #offset]`** - Load with immediate offset (best for struct/array access)
   ```asm
   ldr r0, [r1, #8]      ; r0 = *(r1 + 8)
   ```

2. **`[Rn, #offset]!`** - Load with pre-increment (update pointer before)
   ```asm
   ldr r0, [r1, #4]!     ; r1 += 4; r0 = *r1
   ```

3. **`[Rn], #offset`** - Load with post-increment (update pointer after)
   ```asm
   ldr r0, [r1], #4      ; r0 = *r1; r1 += 4
   ```

4. **`[Rn, Rm, LSL #n]`** - Load with register offset + shift
   ```asm
   ldr r0, [r1, r2, LSL #2]  ; r0 = *(r1 + (r2 << 2))
   ```

### Solution
Add pattern matching in instruction selector to use offset addressing:

**Implementation Steps:**

1. **Detect array access patterns:**
   ```c
   // Pattern: arr[const_index] → use [base, #offset]
   *(base + const) → ldr rd, [base, #const]
   
   // Pattern: arr[i] where i is loop variable → use [base, i, LSL #2]
   *(base + (i << 2)) → ldr rd, [base, i, LSL #2]
   
   // Pattern: sequential access → use post-increment
   for (i=0; i<n; i++) arr[i] → pointer with post-increment
   ```

2. **Add addressing mode to IR:**
   - Extend IR to represent memory operands with offset
   - Track base register + offset instead of computing full address

3. **Code generation for offset modes:**
   ```c
   // In arm-thumb-gen.c
   // Instead of:
   emit("mov r4, r2, lsl #2");      // offset = i*4
   emit("add r5, base, r4");        // addr = base + offset
   emit("ldr ip, [r5]");            // load
   
   // Generate:
   emit("ldr ip, [base, r2, LSL #2]");  // single instruction!
   ```

4. **Sequential access optimization:**
   - Detect `arr[i]` followed by `arr[i+1]`
   - First load: `[ptr, #0]`
   - Second load: `[ptr, #4]!` or load then `add ptr, #4`

### Concrete Example Transformations

**Current TCC (10 instructions for swap):**
```asm
mov  r4, r2, lsl #2       ; offset j
sub  ip, r7, #256         ; base
add  r5, ip, r4           ; &arr[j]
ldr  r6, [r5]             ; temp = arr[j]
adds r5, r2, #1           ; j+1
mov  r8, r5, lsl #2       ; offset j+1
sub  ip, r7, #256         ; base (again!)
add  r9, ip, r8           ; &arr[j+1]
ldr  r8, [r9]             ; arr[j+1]
str  r8, [r5]             ; arr[j] = arr[j+1]
...
```

**Optimized (3 instructions):**
```asm
ldr  r6, [r3, r2, LSL #2]     ; temp = arr[j]
ldr  r8, [r3, r4, LSL #2]     ; arr[j+1] (assuming r4=j+1)
str  r8, [r3, r2, LSL #2]     ; arr[j] = arr[j+1]
str  r6, [r3, r4, LSL #2]     ; arr[j+1] = temp
```

Or with pointer iteration (even better):
```asm
ldr  r2, [r3, #0]         ; r2 = arr[j]
ldr  r1, [r3, #4]!        ; r1 = arr[j+1], r3 += 4
str  r1, [r3, #-4]        ; arr[j] = r1 (was arr[j+1])
str  r2, [r3]             ; arr[j+1] = r2 (was arr[j])
```

**Expected Impact:**
- **70-80% reduction** in array access instruction count
- Eliminates explicit address calculations
- Better instruction scheduling (fewer dependencies)
- Smaller code size

**Files to Modify:**
- `arm-thumb-gen.c` - Add offset addressing pattern matching
- `tccir.c` - May need IR changes to represent offset operands
- `arm-thumb-opcodes.c` - Ensure offset variants of LDR/STR exist

---

## Phase 6: IT (If-Then) Block Generation - MEDIUM PRIORITY

### Problem
TCC uses branches for conditional stores, GCC uses IT blocks.

**TCC (6 instructions + branch):**
```asm
20004536:  45f4        cmp     ip, lr            ; Compare
20004538:  e8bd 4000   ldmia.w sp!, {lr}         ; Restore lr (spill!)
2000453c:  f340 8020   ble.w   20004580          ; Branch if not greater
; ... swap code (10+ instructions)
20004580:  f7ff bfc4   b.w     2000450c          ; Continue
```

**GCC (3 instructions, no branch!):**
```asm
20003d54:  428a        cmp     r2, r1            ; Compare
20003d56:  bfc4        itt     gt                ; If-Then block (2 instr)
20003d58:  f843 1c04   strgt.w r1, [r3, #-4]     ; Conditional store
20003d5c:  601a        strgt   r2, [r3, #0]      ; Conditional store
```

### Solution
Add IT block generation for short conditional sequences.

**Implementation Steps:**
1. Detect simple if-then patterns (1-4 conditional instructions)
2. Replace branch-around with IT block
3. Handle register pressure (no spills in IT block!)

**Constraints:**
- IT block can conditionally execute 1-4 instructions
- No branches inside IT block
- No register spills (can't use push/pop)

**Expected Impact:**
- Eliminate branch misprediction penalty
- Reduce code size significantly
- Better for short conditional sequences

**Files to Modify:**
- `arm-thumb-gen.c` - Add IT block generation
- `arm-thumb-opcodes.c` - Add IT instruction encoding

---

## Phase 7: Register Allocation Improvements - LOW PRIORITY

### Problem
TCC saves 6 registers (r4-r9) vs GCC saving only lr. This indicates:
- Poor register allocation
- Excessive register pressure
- Unnecessary spills

### Specific Issue: Spilling lr Around Comparison
```asm
20004530:  b500        push    {lr}              ; Spill lr (why?!)
20004532:  f8d4 e000   ldr.w   lr, [r4]          ; Use lr as temp
20004536:  45f4        cmp     ip, lr            ; Compare
20004538:  e8bd 4000   ldmia.w sp!, {lr}         ; Restore lr
```

GCC avoids this by using the free registers it has (only saves lr).

### Solution
Improve register allocator in `tccls.c`:

**Implementation Steps:**
1. Better live range analysis
2. Allocate registers more efficiently for short-lived temporaries
3. Avoid using callee-saved registers for short-lived values
4. Don't spill around simple operations

**Expected Impact:**
- Fewer registers to save/restore
- Smaller prologue/epilogue
- Faster function entry/exit

**Files to Modify:**
- `tccls.c` - Liveness analysis and register allocation

---

## Phase 8: Branch Optimization - LOW PRIORITY

### Problem
TCC generates unnecessary long branches to nearby code.

**TCC (unconditional branch):**
```asm
200044b2:  f280 8018   bge.w   200044e6          ; Branch to loop exit
200044b6:  f000 b803   b.w     200044c0          ; Unconditional to loop body
```

These are only a few bytes away but use 32-bit branch encoding.

### Solution
1. Use short conditional branches (bge.n) when target is within -252 to +258 bytes
2. Eliminate unnecessary jumps by reordering basic blocks
3. Fall-through to common path

**Expected Impact:**
- Smaller code size
- Slightly better icache utilization

**Files to Modify:**
- `arm-thumb-gen.c` - Instruction selection
- `arm-thumb-opcodes.c` - Ensure short branch variants exist

---

## Implementation Priority Matrix

| Phase | Optimization | Effort | Impact | Priority |
|-------|--------------|--------|--------|----------|
| 1 | Common Subexpression Elimination | Medium | Very High | **P0** |
| 2 | Constant Propagation/Strength Reduction | Medium | Very High | **P0** |
| 3 | Instruction Selection (MLA, post-inc) | Low | High | **P1** |
| 4 | LDR/STR with Offset Addressing | Low | Very High | **P0** |
| 5 | Loop Structure Optimization | High | High | **P1** |
| 6 | IT Block Generation | Medium | Medium | **P2** |
| 7 | Register Allocation | High | Medium | **P2** |
| 8 | Branch Optimization | Low | Low | **P3** |

---

## Success Metrics

After implementing all phases, measure improvement on `bubble_sort`:

| Metric | Current TCC | Target TCC | GCC -O1 |
|--------|-------------|------------|---------|
| Code Size | ~188 B | ~100 B | ~76 B |
| Instructions | ~47 | ~25 | ~22 |
| Registers Saved | 6 | 1-2 | 1 |
| Multiplications | 64+ | 0 | 0 |
| Array Access Instructions | 8 per element | 1-2 | 1 |
| Loads/Stores (inner loop) | 10+ | 2-3 | 2 |

**Stretch Goal:** Within 1.3x of GCC -O1 code size for this benchmark.

---

## Testing Strategy

1. **Unit Tests:**
   - Add IR-level tests for CSE (`tests/ir_tests/`)
   - Add assembly tests for new patterns (`tests/thumb/armv8m/`)

2. **Benchmark Verification:**
   - Run `bubble_sort` benchmark before/after
   - Verify correctness (checksum should match)
   - Measure cycle count improvement

3. **Regression Testing:**
   - Run full test suite: `make test`
   - Ensure no breakage in existing code

---

## Quick Wins (Start Here)

For immediate impact with manageable effort:

1. **Cache frame pointer offset** in `arm-thumb-gen.c` (Phase 1 lite)
   - Track last `sp - constant` calculation
   - Reuse register if same offset needed

2. **Add MLA pattern** (Phase 3)
   - Detect `MUL` + `ADD` sequence in instruction selector
   - Replace with single `MLA` instruction

3. **Use LDR with offset addressing** (Phase 4)
   - Detect `*(base + (index << 2))` pattern
   - Replace 3-4 instructions with single `ldr rd, [base, index, LSL #2]`
   - 70-80% reduction in array access code size

4. **Use post-increment for array loops** (Phase 3/4)
   - Detect `for(i=0; i<n; i++) arr[i]` pattern
   - Generate pointer + post-increment

These four changes alone could reduce code size by 50-60%.
