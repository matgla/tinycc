# IR Optimization TODOs

## High Priority - Phase 1: Constant Propagation

### 1.1 Analyze Constants
**File**: `tccir.c`
```c
typedef struct VarConstInfo {
    int is_constant;      // 1 if var holds known constant
    int64_t value;        // the constant value
    int def_count;        // number of definitions
} VarConstInfo;

// Function to implement:
void tcc_ir_analyze_constants(TCCIRState *ir, VarConstInfo *var_info, int var_count);
```

**Implementation steps**:
1. Allocate `VarConstInfo` array for all VAR vregs
2. Scan all instructions, for each VAR vreg:
   - Count definitions (ASSIGN to that VAR)
   - If single definition and source is VT_CONST, mark as constant
   - Store the constant value
3. Return the analysis results

### 1.2 Propagate Constants
**File**: `tccir.c`
```c
// Function to implement:
int tcc_ir_propagate_constants(TCCIRState *ir, VarConstInfo *var_info, int var_count);
```

**Implementation steps**:
1. Scan all instructions
2. For each source operand (src1, src2):
   - If operand is a VAR vreg
   - Check if var_info[vreg].is_constant == 1
   - Replace VAR vreg with immediate: `operand->r = VT_CONST; operand->c.i = value; operand->vr = -1;`
3. Return count of replacements made

### 1.3 Algebraic Simplification
**File**: `tccir.c`
```c
// Function to implement:
int tcc_ir_algebraic_simplify(TCCIRState *ir);
```

**Implementation steps**:
1. Create simplification rule table:
   ```c
   struct AlgRule {
       TccIrOp op;
       int check_src1_const;  // 1 if rule applies to constant src1
       int check_src2_const;  // 1 if rule applies to constant src2
       int64_t const_value;   // the constant to match
       TccIrOp result_op;     // TCCIR_OP_ASSIGN for identity, or new op
       int64_t result_const;  // for ASSIGN, the constant result
   };
   ```

2. Rules to implement:
   - `X + 0 → X` (ASSIGN src1)
   - `0 + X → X` (ASSIGN src2)
   - `X - 0 → X` (ASSIGN src1)
   - `X * 1 → X` (ASSIGN src1)
   - `1 * X → X` (ASSIGN src2)
   - `X * 0 → 0` (ASSIGN constant 0)
   - `0 * X → 0` (ASSIGN constant 0)
   - `X << 0 → X` (ASSIGN src1)
   - `0 << X → 0` (ASSIGN constant 0)
   - `X >> 0 → X` (ASSIGN src1)
   - `X & 0 → 0` (ASSIGN constant 0)
   - `X & -1 → X` (ASSIGN src1)
   - `X | 0 → X` (ASSIGN src1)
   - `X | -1 → -1` (ASSIGN constant -1)

3. For each arithmetic instruction:
   - Check if src1 or src2 is VT_CONST
   - Apply matching rule
   - Convert operation to ASSIGN if identity found

### 1.4 Constant Folding
**File**: `tccir.c`
```c
// Function to implement:
int tcc_ir_constant_fold(TCCIRState *ir);
```

**Implementation steps**:
1. Scan all instructions
2. If both src1 and src2 are VT_CONST:
   - Evaluate operation at compile time
   - Replace instruction with ASSIGN of result
   - Handle: ADD, SUB, MUL, DIV, MOD, AND, OR, XOR, SHL, SHR, SAR

3. Example:
   ```c
   case TCCIR_OP_ADD:
       result = src1_const + src2_const;
       break;
   case TCCIR_OP_SHL:
       result = src1_const << src2_const;
       break;
   ```

### 1.5 Integration
**File**: `tccgen.c` (or wherever optimization pipeline is)

Add to `gfunc_epilog()` or similar:
```c
if (ir) {
    // Existing dead store elimination
    int removed = tcc_ir_dead_store_elimination(ir);

    // NEW: Phase 1 optimizations
    VarConstInfo *var_info = tcc_mallocz(ir->next_local_variable * sizeof(VarConstInfo));
    tcc_ir_analyze_constants(ir, var_info, ir->next_local_variable);
    int propagated = tcc_ir_propagate_constants(ir, var_info, ir->next_local_variable);
    int simplified = tcc_ir_algebraic_simplify(ir);
    int folded = tcc_ir_constant_fold(ir);
    tcc_free(var_info);

    // Re-run dead store elimination after constant propagation
    if (propagated + simplified + folded > 0) {
        removed += tcc_ir_dead_store_elimination(ir);
    }
}
```

---

## Medium Priority - Phase 2: Copy Propagation

### 2.1 Copy Propagation Implementation
**File**: `tccir.c`
```c
// Function to implement:
int tcc_ir_copy_propagation(TCCIRState *ir);
```

**Implementation steps**:
1. Track basic block boundaries (JUMP, JUMPIF, function calls)
2. For each basic block:
   - Track ASSIGN instructions: `TMP:X <-- SRC`
   - Count uses of TMP:X in subsequent instructions
   - If TMP:X used only once within block AND SRC not redefined:
     - Replace use of TMP:X with SRC
     - Mark ASSIGN for removal
3. Clear tracking at basic block boundaries

### 2.2 Integration
Add after Phase 1 in optimization pipeline:
```c
int copied = tcc_ir_copy_propagation(ir);
if (copied > 0) {
    removed += tcc_ir_dead_store_elimination(ir);
}
```

---

## Medium Priority - Phase 3: Common Subexpression Elimination

### 3.1 CSE Data Structure
**File**: `tccir.c`
```c
typedef struct CSEEntry {
    TccIrOp op;
    int src1_vr;
    int src1_is_const;
    int64_t src1_const;
    int src2_vr;
    int src2_is_const;
    int64_t src2_const;
    int result_vr;
    int instruction_idx;
    int valid;
} CSEEntry;

#define CSE_TABLE_SIZE 64
```

### 3.2 CSE Implementation
**File**: `tccir.c`
```c
// Function to implement:
int tcc_ir_arithmetic_cse(TCCIRState *ir);
```

**Implementation steps**:
1. Create CSE table per basic block
2. For each arithmetic instruction:
   - Compute hash of (op, src1, src2)
   - For commutative ops (ADD, MUL, AND, OR, XOR), normalize operand order
   - Check if expression already in table
   - If found: replace with ASSIGN from previous result
   - If not: add to table
3. Invalidate entries when operand is redefined
4. Clear table at basic block boundaries

### 3.3 Expression Matching
```c
static int expressions_match(TACQuadruple *q1, TACQuadruple *q2, int commutative) {
    if (q1->op != q2->op) return 0;

    // Direct match
    if (operands_equal(&q1->src1, &q2->src1) &&
        operands_equal(&q1->src2, &q2->src2))
        return 1;

    // Commutative match
    if (commutative &&
        operands_equal(&q1->src1, &q2->src2) &&
        operands_equal(&q1->src2, &q2->src1))
        return 1;

    return 0;
}
```

### 3.4 Integration
Add after Phase 2 in optimization pipeline:
```c
int cse_removed = tcc_ir_arithmetic_cse(ir);
if (cse_removed > 0) {
    removed += tcc_ir_dead_store_elimination(ir);
}
```

---

## Testing Checklist

### Phase 1 Tests
- [ ] VAR:0 constant propagation in Move() function
- [ ] `X + 0` simplification
- [ ] `X * 0` simplification
- [ ] `0 << N` simplification
- [ ] Full constant folding `#5 + #3`
- [ ] Run existing test suite

### Phase 2 Tests
- [ ] TMP:6/VAR:1 copy elimination in Move()
- [ ] Single-use temporary elimination
- [ ] Basic block boundary handling
- [ ] Run existing test suite

### Phase 3 Tests
- [ ] Duplicate j-1 calculation elimination
- [ ] Commutative operation matching (X+Y == Y+X)
- [ ] Expression invalidation on redefinition
- [ ] Run existing test suite

### Performance Tests
- [ ] Measure instruction count before/after
- [ ] Measure compilation time impact
- [ ] Measure code size reduction
- [ ] Verify no correctness regressions

---

## Expected Results on Move() Function

### Current: 27 instructions
```
0000: VAR:0 <-- #0
0001: VAR:1 <-- #0
...
0018: TMP:11 <-- #0          (VAR:0 not propagated!)
0019: TMP:12 <-- PAR:0 ADD TMP:11
...
0022: TMP:14 <-- TMP:8       (duplicate of line 15!)
0023: TMP:15 <-- TMP:14 SHL #2   (duplicate of line 16!)
0024: TMP:16 <-- PAR:1 ADD TMP:15 (duplicate of line 17!)
```

### After Phase 1: ~24 instructions
```
0000: VAR:1 <-- #0           (VAR:0 eliminated)
...
0018: TMP:13 <-- PAR:0***DEREF***  (simplified from ADD #0)
```

### After Phase 2: ~23 instructions
```
0011: VAR:1 <-- VAR:1 ADD #1   (TMP:6 eliminated)
```

### After Phase 3: ~19 instructions
```
0015: TMP:8 <-- VAR:1 SUB #1
0016: TMP:9 <-- TMP:8 SHL #2
0017: TMP:10 <-- PAR:1 ADD TMP:9
...
0022: (reuse TMP:8, TMP:9, TMP:10 instead of recomputing)
```

### Summary
- **Current**: 27 instructions
- **Target**: 19 instructions
- **Reduction**: 30%
- **Benefits**: Fewer spills, better register allocation, smaller code size
