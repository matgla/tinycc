# Complex Numbers Fix Plan

**Created:** 2026-02-26
**Goal:** Fix all complex float arithmetic (add/sub/mul/div) end-to-end

## Root Cause Analysis

The complex implementation has correct type system (Phase 1) and IR encoding (Phase 2),
but Phase 3 (code generation) has multiple bugs that cause infinite loops at runtime.

### Bug 1: Parameters/variables not marked as complex
- **Location:** `tccgen.c:800-834`
- **Problem:** `tcc_ir_vreg_type_set_complex()` is never called for parameter or variable
  vregs. The register allocator treats them as single-register floats (LS_REG_TYPE_INT)
  instead of register pairs (LS_REG_TYPE_COMPLEX_FLOAT).
- **Evidence:** Debug output shows `reg_type=0` for complex params instead of `reg_type=5`.

### Bug 2: Incoming register assignment ignores complex
- **Location:** `ir/codegen.c:365`
- **Problem:** `int is_64bit = interval && (interval->is_double || interval->is_llong);`
  does NOT check `interval->is_complex`. Complex function params get assigned single
  registers (r0, r1) instead of register pairs (r0:r1, r2:r3).
- **Evidence:** IR dump shows `src1: pr0=0 pr1=31` — pr1=31 is PREG_REG_NONE.

### Bug 3: Complex variable initialization doesn't zero imaginary part
- **Location:** `tccgen.c` (gen_cast_s) + `arm-thumb-gen.c` (store handler)
- **Problem:** `_Complex float a = 1.0f;` generates `V0 <-- #1065353216 [ASSIGN]` —
  a single scalar assignment. The imaginary part (second 4 bytes) is uninitialized.
- **Expected:** Should store {1.0f, 0.0f} = two 4-byte values.

### Bug 4: Stack corruption in thumb_process_complex_op
- **Location:** `arm-thumb-gen.c:~4665`
- **Problem:** After `th_pop(pop_mask)`, the code does
  `th_add_imm(R_SP, R_SP, 4, ...)` for single-register case. But pop already
  adjusts SP, so this corrupts the stack by 4 bytes.

### Bug 5: Complex mul/div IR generation missing
- **Location:** `ir/core.c:1168`
- **Problem:** `tcc_ir_gen_f()` only handles FADD/FSUB for complex, not FMUL/FDIV.
  Mul/div fall through to scalar FP path which treats complex as a single float.

### Bug 6: Complex mul codegen has clobbering issues
- **Location:** `arm-thumb-gen.c` (thumb_process_complex_mul)
- **Problem:** `gen_softfp_mul_call()` tries to save results in r2-r5, but each
  `__aeabi_fmul` call clobbers r0-r3. The function also has a broken pop sequence
  that stores r6 to stack[0] then pops r0-r3, expecting r0 to get the real result,
  but the imag result was already moved to r1 before the pop.

### Bug 7: Complex div codegen has register ordering issues
- **Location:** `arm-thumb-gen.c` (thumb_process_complex_div)
- **Problem:** When source registers overlap with r0-r3 (common case), the
  sequential mov instructions can clobber values before they're read.

### Bug 8: Debug fprintf in production code
- **Location:** Multiple files
- **Problem:** Many `fprintf(stderr, "DEBUG ...")` statements in hot paths.

---

## TODO List

- [ ] Fix 1: Mark param/var vregs as complex (`tccgen.c:800-834`)
- [ ] Fix 2: Fix incoming register assignment (`ir/codegen.c:365`)
- [ ] Fix 3: Handle real-to-complex initialization
- [ ] Fix 4: Fix stack corruption in `thumb_process_complex_op`
- [ ] Fix 5: Add FMUL/FDIV to complex IR generation (`ir/core.c`)
- [ ] Fix 6: Rewrite `thumb_process_complex_mul`
- [ ] Fix 7: Fix register ordering in `thumb_process_complex_div`
- [ ] Fix 8: Remove all debug fprintf statements
- [ ] Verify: `make cross` builds
- [ ] Verify: `50_complex_types.c` passes
- [ ] Verify: `51_complex_arith.c` passes (all 4 ops)
- [ ] Verify: `make test -j16` no regressions
- [ ] Update `IMPLEMENTATION_STATUS.md`

---

## Implementation Details

### Fix 1: Mark param/var vregs as complex

**File:** `tccgen.c` lines 800-834

After the existing `is_float(type->t)` blocks for both params and variables, add:

```c
/* Mark complex parameters - needs register pairs */
if (type->t & VT_COMPLEX)
  tcc_ir_vreg_type_set_complex(tcc_state->ir, vreg);
```

Two locations:
1. Line ~804: After param float marking (inside `if (r & VT_PARAM)`)
2. Line ~828: After variable float marking (inside else branch)

---

### Fix 2: Fix incoming register assignment

**File:** `ir/codegen.c` line 365

Change:
```c
int is_64bit = interval && (interval->is_double || interval->is_llong);
```
To:
```c
int is_64bit = interval && (interval->is_double || interval->is_llong || interval->is_complex);
```

This ensures complex params are assigned register pairs (r0:r1, r2:r3) in
`tcc_ir_set_incoming_arg_registers()`, and that `argno` advances by 2.

---

### Fix 3: Handle real-to-complex initialization

**File:** `arm-thumb-gen.c` — store handler for complex types

When storing a scalar value to a complex variable (VT_COMPLEX flag set), the store
handler must:
1. Store the scalar value as the real part (at offset +0)
2. Store zero (0x00000000) as the imaginary part (at offset +4 for float)

This can be detected when the destination is marked complex but the source is a
scalar constant or single-register value.

Alternatively, in `tccgen.c` `gen_cast_s()` around line 4005:
- Detect `(dbt & VT_COMPLEX) && !(sbt & VT_COMPLEX)`
- Just propagate VT_COMPLEX to vtop so the ASSIGN IR instruction carries the flag
- The codegen store for ASSIGN with complex dest and scalar src generates two stores

---

### Fix 4: Fix stack corruption in thumb_process_complex_op

**File:** `arm-thumb-gen.c` around line 4665

Delete this block:
```c
if (pop_count == 1)
  ot_check(th_add_imm(R_SP, R_SP, 4, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
```

`th_pop()` already adjusts SP by `4 * popcount(pop_mask)`. Adding 4 more corrupts
the stack frame.

---

### Fix 5: Add FMUL/FDIV to complex IR generation

**File:** `ir/core.c` in `tcc_ir_gen_f()` around line 1168

Change:
```c
if (is_complex_op && (ir_op == TCCIR_OP_FADD || ir_op == TCCIR_OP_FSUB))
```
To:
```c
if (is_complex_op && (ir_op == TCCIR_OP_FADD || ir_op == TCCIR_OP_FSUB ||
                      ir_op == TCCIR_OP_FMUL || ir_op == TCCIR_OP_FDIV))
```

The codegen already has `thumb_process_complex_mul` and `thumb_process_complex_div`
for FMUL/FDIV dispatch in `tcc_gen_machine_fp_op`. This fix ensures the IR
generation path creates the right instruction with complex-typed operands.

---

### Fix 6: Rewrite thumb_process_complex_mul

**File:** `arm-thumb-gen.c`

Current implementation has fundamental issues with register clobbering across
soft-float calls. Rewrite strategy:

```
(a+bi) * (c+di) = (ac-bd) + i(ad+bc)
```

Safe approach using stack for all intermediates:
1. Push all 4 input components (a, b, c, d) to stack
2. Compute ac: load a,c from stack -> call __aeabi_fmul -> push result
3. Compute bd: load b,d from stack -> call __aeabi_fmul -> push result
4. Compute ad: load a,d from stack -> call __aeabi_fmul -> push result
5. Compute bc: load b,c from stack -> call __aeabi_fmul -> push result
6. Real = ac - bd: load ac,bd from stack -> call __aeabi_fsub -> push result
7. Imag = ad + bc: load ad,bc from stack -> call __aeabi_fadd -> push result
8. Pop real,imag results -> move to dest registers
9. Clean up stack

Key fix: Do NOT try to keep intermediate results in r2-r6. Every __aeabi call
clobbers r0-r3, and saving/restoring callee-saved registers (r4-r6) adds
complexity. Use the stack for all intermediates — it's simpler and correct.

Stack layout for intermediates (growing down from current SP):
```
[sp+20] = d  (imag of src2)
[sp+16] = c  (real of src2)
[sp+12] = b  (imag of src1)
[sp+ 8] = a  (real of src1)
[sp+ 4] = intermediate results (reused)
[sp+ 0] = intermediate results (reused)
```

---

### Fix 7: Fix register ordering in thumb_process_complex_div

**File:** `arm-thumb-gen.c`

The `__divsc3(float a, float b, float c, float d)` call expects:
- r0 = a (real of numerator)
- r1 = b (imag of numerator)
- r2 = c (real of denominator)
- r3 = d (imag of denominator)

Problem: if src registers ARE r0-r3 (which they typically are since params arrive
in r0:r1 and r2:r3), the sequential mov instructions clobber values:
```c
if (s1_r != R0) mov R0, s1_r;  // might clobber s2_r if s2_r == R0
if (s1_i != R1) mov R1, s1_i;  // might clobber s2_i if s2_i == R1
```

Fix: Push all source values to stack first, then pop into r0-r3 in correct order.
Or use careful ordering analysis to determine safe mov sequence.

Simpler fix: Since complex params typically arrive in r0:r1 and r2:r3, which is
exactly the __divsc3 argument order, check if registers already match and skip
moves. For the general case, save to stack and reload.

---

### Fix 8: Remove debug fprintf

**Files to clean:**
- `arm-thumb-gen.c` — Remove fprintf in `thumb_process_complex_op`, `thumb_process_complex_mul`, `thumb_process_complex_div`, `tcc_gen_machine_fp_op`
- `ir/core.c` — Remove fprintf in `tcc_ir_put` (2 locations) and `tcc_ir_gen_f`
- `ir/live.c` — Remove fprintf in `tcc_ir_live_intervals_compute`
- `ir/pool.c` — Remove fprintf in `tcc_ir_pool_add`
- `ir/vreg.c` — Remove fprintf in `tcc_ir_vreg_type_set_complex` and `tcc_ir_vreg_type_get`
- `tccir_operand.c` — Remove fprintf in `svalue_to_iroperand`
- `tccgen.c` — Remove the large debug block before `tcc_ir_liveness_analysis` (~line 11900)
- `tccls.c` — Remove fprintf in `tcc_ls_add_live_interval`

---

## Verification Plan

```bash
# 1. Build
make clean && make cross

# 2. Type system test (should already pass)
cd tests/ir_tests && python run.py -c 50_complex_types.c

# 3. Arithmetic test (the main fix target)
cd tests/ir_tests && python run.py -c 51_complex_arith.c

# 4. Full regression suite
make test -j16
```

Expected 51_complex_arith.c output:
```
add: 4.0 + 0.0i
sub: -2.0 + 0.0i
mul: 3.0 + 0.0i
div: 3.0 + 0.0i
OK: All basic complex arithmetic tests passed!
```
