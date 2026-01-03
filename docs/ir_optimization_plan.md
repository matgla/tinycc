# IR Optimization Plan

## Overview

This plan addresses the unoptimized IR generated for code like:

```c
int Move(int *source, int *dest) {
  int i = 0, j = 0;
  while (j < 4 && dest[j] == 0)
    j++;
  dest[j - 1] = source[i];
  return dest[j - 1];
}
```

Current issues:
- Variable `i` is always 0 but not propagated
- `j-1` computed twice after the loop
- Address calculations repeated
- Store followed by load from same address
- Unnecessary spills due to excessive temporaries

## Phase 1: Constant Propagation with Algebraic Simplification

**Goal**: Eliminate constant variables and simplify arithmetic with constants.

### 1.1 Track Constant Variables

For each VAR vreg, determine:
- Is it assigned exactly once (single definition)?
- Is the assignment a constant value?
- Is it never modified after initial assignment?

Data structure:
```c
typedef struct VarConstInfo {
    int is_constant;      // 1 if var holds known constant
    int64_t value;        // the constant value
    int def_count;        // number of definitions
} VarConstInfo;
```

### 1.2 Propagate Constants

Replace uses of constant VARs with immediate values:
```
Before: VReg TMP:11 <-- VReg VAR:0 SHL #2   (where VAR:0 = 0)
After:  VReg TMP:11 <-- #0 SHL #2
```

### 1.3 Algebraic Simplification (Constant Folding)

Apply these transformations when one operand is constant:

| Pattern | Result |
|---------|--------|
| `X ADD #0` | `X` (ASSIGN) |
| `X SUB #0` | `X` (ASSIGN) |
| `X SHL #0` | `X` (ASSIGN) |
| `X SHR #0` | `X` (ASSIGN) |
| `X SAR #0` | `X` (ASSIGN) |
| `X MUL #1` | `X` (ASSIGN) |
| `X MUL #0` | `#0` (ASSIGN) |
| `X AND #0` | `#0` (ASSIGN) |
| `X AND #-1` | `X` (ASSIGN) |
| `X OR #0` | `X` (ASSIGN) |
| `X OR #-1` | `#-1` (ASSIGN) |
| `#0 SHL X` | `#0` (ASSIGN) |
| `#0 ADD X` | `X` (ASSIGN) |
| `#C1 OP #C2` | `#result` (full constant fold) |

### 1.4 Expected Result for Example

```
Before:
  0018: VReg TMP:11 <-- VReg VAR:0 SHL #2
  0019: VReg TMP:12 <-- VReg PAR:0 ADD VReg TMP:11
  0020: VReg TMP:13 <-- VReg TMP:12***DEREF***

After constant propagation (VAR:0 = 0):
  0018: VReg TMP:11 <-- #0 SHL #2
  0019: VReg TMP:12 <-- VReg PAR:0 ADD VReg TMP:11
  0020: VReg TMP:13 <-- VReg TMP:12***DEREF***

After algebraic simplification (#0 SHL #2 = #0):
  0018: VReg TMP:11 <-- #0
  0019: VReg TMP:12 <-- VReg PAR:0 ADD VReg TMP:11
  0020: VReg TMP:13 <-- VReg TMP:12***DEREF***

After X ADD #0 = X:
  0018: (eliminated by DCE)
  0019: VReg TMP:12 <-- VReg PAR:0
  0020: VReg TMP:13 <-- VReg TMP:12***DEREF***

Final (after copy prop):
  0020: VReg TMP:13 <-- VReg PAR:0***DEREF***
```

---

## Phase 2: Copy Propagation

**Goal**: Eliminate redundant copy temporaries.

### 2.1 Identify Copy Chains

Pattern to detect:
```
TMP:X <-- VAR:Y        (copy instruction)
VAR:Y <-- TMP:X OP Z   (TMP:X used immediately)
```

If TMP:X is used only once and immediately after its definition, replace:
```
VAR:Y <-- VAR:Y OP Z
```

### 2.2 General Copy Propagation

For any `TMP:X <-- SRC` (ASSIGN), replace subsequent uses of TMP:X with SRC (within the same basic block, before SRC is redefined).

### 2.3 Expected Result for Example

```
Before:
  0012: VReg TMP:6 <-- VReg VAR:1
  0013: VReg VAR:1 <-- VReg TMP:6 ADD #1

After:
  0013: VReg VAR:1 <-- VReg VAR:1 ADD #1
```

---

## Phase 3: Common Subexpression Elimination (CSE)

**Goal**: Reuse computed values instead of recomputing.

### 3.1 Basic Block CSE

Within a basic block (no jumps), track computed expressions:

```c
typedef struct CSEEntry {
    TccIrOp op;
    int src1_vr;
    int src2_vr;       // or constant value
    int src2_is_const;
    int64_t src2_const;
    int result_vr;
    int instruction_idx;
} CSEEntry;
```

For each arithmetic instruction, check if same computation exists. If so, replace with ASSIGN from previous result.

### 3.2 Handle Commutative Operations

For ADD, MUL, AND, OR, XOR: normalize operand order (smaller vreg first) before hashing.

### 3.3 Invalidation

Invalidate CSE entry for expression involving vreg X when X is redefined.

### 3.4 Expected Result for Example

```
Before (after loop, instructions 15-25):
  0015: VReg TMP:8 <-- VReg VAR:1 SUB #1
  0016: VReg TMP:9 <-- VReg TMP:8 SHL #2
  0017: VReg TMP:10 <-- VReg PAR:1 ADD VReg TMP:9
  ...
  0022: VReg TMP:14 <-- VReg VAR:1 SUB #1      <- same as 0015!
  0023: VReg TMP:15 <-- VReg TMP:14 SHL #2     <- same pattern
  0024: VReg TMP:16 <-- VReg PAR:1 ADD VReg TMP:15  <- same pattern

After CSE:
  0015: VReg TMP:8 <-- VReg VAR:1 SUB #1
  0016: VReg TMP:9 <-- VReg TMP:8 SHL #2
  0017: VReg TMP:10 <-- VReg PAR:1 ADD VReg TMP:9
  ...
  0022: VReg TMP:14 <-- VReg TMP:8             <- reuse!
  0023: VReg TMP:15 <-- VReg TMP:9             <- reuse!
  0024: VReg TMP:16 <-- VReg TMP:10            <- reuse!
```

---

## Phase 4: Store-Load Forwarding (Future)

**Goal**: Avoid reloading values that were just stored.

### 4.1 Track Recent Stores

After a STORE instruction:
```
ADDR***DEREF*** <-- VALUE [STORE]
```

Track that memory at ADDR contains VALUE.

### 4.2 Forward to Loads

When encountering a LOAD from same address:
```
DEST <-- ADDR***DEREF***
```

Replace with:
```
DEST <-- VALUE
```

### 4.3 Alias Analysis (Simple)

Invalidate tracked stores when:
- Any store to a potentially aliasing address
- Function call (conservative: invalidate all)
- End of basic block

### 4.4 Expected Result for Example

```
Before:
  0021: VReg TMP:10***DEREF*** <-- VReg TMP:13 [STORE]
  0022-0025: (compute same address)
  0025: VReg TMP:17 <-- VReg TMP:16***DEREF***
  0026: RETURNVALUE VReg TMP:17

After (combined with CSE showing TMP:16 == TMP:10):
  0021: VReg TMP:10***DEREF*** <-- VReg TMP:13 [STORE]
  0026: RETURNVALUE VReg TMP:13
```

---

## Implementation Order

1. **Phase 1: Constant Propagation** (tccir.c)
   - Add `tcc_ir_constant_propagation()` function
   - Add to optimization pipeline in tccgen.c after dead code elimination
   - Run dead code elimination after to clean up

2. **Phase 2: Copy Propagation** (tccir.c)
   - Add `tcc_ir_copy_propagation()` function
   - Add to pipeline after constant propagation

3. **Phase 3: General CSE** (tccir.c)
   - Extend existing CSE infrastructure (currently bool-only)
   - Add `tcc_ir_arithmetic_cse()` function
   - Add to pipeline after copy propagation

4. **Phase 4: Store-Load Forwarding** (future)
   - More complex, requires alias analysis
   - Consider as future enhancement

---

## Testing

Test cases to verify:
1. Original `Move` function - verify reduced instruction count
2. Constant variable elimination
3. Loop with invariant computations
4. Chained arithmetic with constants
5. Ensure no correctness regressions in existing test suite

---

## Expected Final IR for Move()

After all optimizations:

```
0000: VReg VAR:1 <-- #0
0001: CMP VReg VAR:1,#4
0002: VReg TMP:0 <-- (cond=0x9c)1 if "<S"
0003: VReg TMP:1 <-- VReg VAR:1 SHL #2
0004: VReg TMP:2 <-- VReg PAR:1 ADD VReg TMP:1
0005: VReg TMP:3 <-- VReg TMP:2***DEREF***
0006: CMP VReg TMP:3,#0
0007: VReg TMP:4 <-- (cond=0x94)1 if "=="
0008: VReg TMP:5 <-- VReg TMP:0 AND VReg TMP:4
0009: TEST_ZERO VReg TMP:5
0010: JMP to 13 if "=="
0011: VReg VAR:1 <-- VReg VAR:1 ADD #1
0012: JMP to 1
0013: VReg TMP:8 <-- VReg VAR:1 SUB #1
0014: VReg TMP:9 <-- VReg TMP:8 SHL #2
0015: VReg TMP:10 <-- VReg PAR:1 ADD VReg TMP:9
0016: VReg TMP:13 <-- VReg PAR:0***DEREF***
0017: VReg TMP:10***DEREF*** <-- VReg TMP:13 [STORE]
0018: RETURNVALUE VReg TMP:13
```

Reduction: 27 instructions → 19 instructions (~30% reduction)
Fewer temporaries = fewer spills = better register allocation