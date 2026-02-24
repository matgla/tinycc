# Fix: Value Tracking Ignores Address-Taken Variables Across Calls

**Test case**: `gcc.c-torture/execute/20000313-1.c`
**Symptom**: Exit code 1 (abort) with `-O1 -g`, passes without optimization.

## Test Case

```c
unsigned int buggy(unsigned int *param)
{
  unsigned int accu, zero = 0, borrow;
  accu    = - *param;        // accu = 0xFFFFFFFF (negate 1)
  borrow  = - (accu > zero); // borrow = 0xFFFFFFFF
  *param += accu;            // *param = 1 + 0xFFFFFFFF = 0
  return borrow;
}

int main(void)
{
  unsigned int param  = 1;
  unsigned int borrow = buggy(&param);
  if (param != 0) abort();      // Should NOT abort
  if (borrow + 1 != 0) abort(); // Should NOT abort
  return 0;
}
```

Expected: `param == 0` after call (modified through pointer), `borrow == 0xFFFFFFFF`.

## Root Cause

The `tcc_ir_opt_value_tracking` pass in `ir/opt.c` (line ~919) incorrectly
constant-folds a comparison on a variable whose address was taken and passed to
a function call.

### IR for `main` before optimization:

```
0000: V0 <-- #1 [ASSIGN]            ; param = 1
0001: T0 <-- &V0                    ; take address of param
0002: PARAM0[call_0] T0             ; pass &param to buggy
0003: CALL GlobalSym(buggy) --> V1  ; call buggy(&param)
0004: CMP V0,#0                     ; check if param == 0
0005: JMP to 8  if "=="             ; skip abort if true
0006: FUNCPARAMVOID #65536
0007: CALL abort
```

### IR for `main` after optimization (BUGGY):

```
0000: V0 <-- #1 [ASSIGN]
0001: R4(T0) <-- &V0
0002: PARAM0[call_0] R4(T0)
0003: CALL GlobalSym(buggy) --> R5(V1)
0004: NOP                           ; ← BUG: CMP was removed
0005: NOP                           ; ← BUG: JMP was removed
0006: FUNCPARAMVOID #65536
0007: CALL abort                    ; ← always reached → crash
```

The value tracking pass sees `V0 = 1` at instruction 0000 and propagates this
constant through to instruction 0004 (`CMP V0, #0`). Since `1 != 0`, it
concludes the branch at 0005 is never taken and eliminates both the CMP and JMP
as NOPs. This causes the unconditional fall-through to `abort()`.

**The pass ignores that V0's address was taken (`&V0`) and passed to `buggy()`,
which modifies `*param` (i.e., V0) through the pointer.** After the CALL,
V0's value is no longer known to be 1.

## Disassembly Comparison

### Without optimization (correct):

```arm
; main:
10001198:  movs r0, #1            ; param = 1
1000119a:  str.w r0, [r7, #-4]    ; store to stack
1000119e:  subs r4, r7, #4        ; r4 = &param
100011a0:  mov r0, r4
100011a2:  bl buggy
100011a6:  mov r5, r0             ; save borrow
100011a8:  ldr.w r0, [r7, #-4]    ; RELOAD param from stack
100011ac:  cmp r0, #0             ; check param == 0
100011ae:  beq.w skip_abort1
100011b2:  bl abort
```

### With -O1 -g (broken):

```arm
; main:
10001190:  movs r0, #1            ; param = 1
10001192:  str.w r0, [r7, #-4]
10001196:  subs r4, r7, #4        ; r4 = &param
10001198:  mov r0, r4
1000119a:  bl buggy
1000119e:  mov r5, r0             ; save borrow
100011a0:  bl abort               ; ALWAYS calls abort! CMP/branch gone
```

## Bug Location

**File**: `ir/opt.c`, function `tcc_ir_opt_value_tracking` (line ~919)

Two missing safety checks:

### 1. Pattern 1 (line ~1019): Missing addrtaken guard on constant assignment

```c
/* Pattern 1: Direct constant assignment: Vx <- #const */
if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
{
  if (dest_pos >= 0 && dest_pos <= max_vreg)
  {
    // BUG: No check for addrtaken!
    state[dest_pos].is_constant = 1;
    state[dest_pos].value = irop_get_imm64_ex(ir, src1);
  }
  continue;
}
```

The sibling pass `tcc_ir_opt_const_prop` (line ~340) correctly guards:

```c
IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
if (interval && interval->addrtaken)
{
  var_info[pos].def_count++;
  var_info[pos].is_constant = 0;
  continue;
}
```

### 2. Missing CALL invalidation (after line ~1108)

The catch-all invalidation at line ~1108 only fires for instructions that
**define** a VAR vreg:

```c
/* Any other instruction that defines a VAR vreg invalidates the constant */
if (dest_pos >= 0 && dest_pos <= max_vreg && irop_config[q->op].has_dest)
{
  state[dest_pos].is_constant = 0;
}
```

But `FUNCCALLVOID` and `FUNCCALLVAL` do not define V0 — they define V1 (the
return value). V0 is modified **indirectly** through the pointer. The pass
never invalidates V0 across the call.

## Proposed Fix

Two changes in `tcc_ir_opt_value_tracking`:

### Fix A: Never mark address-taken variables as constant

At Pattern 1 (line ~1019), add the addrtaken guard before marking constant:

```c
/* Pattern 1: Direct constant assignment: Vx <- #const */
if (q->op == TCCIR_OP_ASSIGN && irop_is_immediate(src1))
{
  if (dest_pos >= 0 && dest_pos <= max_vreg)
  {
    /* If address is taken, the variable can be modified through aliases;
     * do not track it as constant. */
    IRLiveInterval *interval = tcc_ir_get_live_interval(ir, dest_vr);
    if (interval && interval->addrtaken)
    {
      state[dest_pos].is_constant = 0;
    }
    else
    {
      state[dest_pos].is_constant = 1;
      state[dest_pos].value = irop_get_imm64_ex(ir, src1);
    }
  }
  continue;
}
```

This is the **minimal and safest fix**. If a variable's address is taken, we
simply never consider it constant, period. This matches the conservative
approach used by `tcc_ir_opt_const_prop`.

### Fix B (belt-and-suspenders): Invalidate address-taken vars at CALLs

After the catch-all at line ~1108, add explicit CALL handling:

```c
/* Function calls can modify any address-taken variable through pointers */
if (q->op == TCCIR_OP_FUNCCALLVOID || q->op == TCCIR_OP_FUNCCALLVAL)
{
  for (int v = 0; v <= max_vreg; v++)
  {
    if (state[v].is_constant)
    {
      int32_t vr = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, v);
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, vr);
      if (interval && interval->addrtaken)
        state[v].is_constant = 0;
    }
  }
}
```

**Fix A alone is sufficient**, since it prevents addrtaken vars from ever
entering the constant state. Fix B is an extra safety net.

### Also apply to Pattern 2 (line ~1023)

The same addrtaken guard should be added to Pattern 2 (arithmetic with constant
operand) for completeness, since `Vx <- Vy + #const` could also propagate a
stale constant for an addrtaken variable.

## Testing

1. Verify the test passes with both `-O0` and `-O1 -g`:
   ```bash
   cd tests/ir_tests
   python run.py -c ../gcctestsuite/.../20000313-1.c
   python run.py -c ../gcctestsuite/.../20000313-1.c --cflags="-O1 -g"
   ```

2. Run the full test suite to check for regressions:
   ```bash
   make test -j16
   make test-all
   ```

## Risk Assessment

**Low risk.** Fix A is purely conservative — it reduces the set of variables
eligible for constant folding. Any variable whose address is taken will simply
not be optimized by this pass. This matches the behavior already used by the
sibling `tcc_ir_opt_const_prop` pass and cannot introduce new miscompilations.
