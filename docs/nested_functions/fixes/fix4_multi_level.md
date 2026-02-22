# Fix 4: `nested_multi_level.c` — Multi-Level Nesting (Chain-of-Chains)

**Test**: `tests/ir_tests/nested_multi_level.c`
**Error**: `'a' undeclared` — `level2` can't access grandparent variable `a` from `main`
**Root Cause**: Two independent problems:
  1. `prescan_captured_vars()` only searches immediate parent's `local_stack`
  2. ARM codegen only does single-hop chain dereference (R10 as direct base)
**Complexity**: High — touches parser prescan, IR metadata, and 4+ codegen paths

---

## Problem

```c
int main(void) {       // "grandparent"
  int a = 1;
  int level1(int x) {  // "parent"    — captures a (prescan sees it in token stream)
    int b = 20;
    int level2(int y) { // "child"     — needs a, b, x
      return a + b + x + y;   // ERROR: 'a' undeclared
    }
    return level2(300);
  }
  printf("%d\n", level1(10));  // expected: 1+20+10+300 = 331
  a = 100;
  printf("%d\n", level1(10));  // expected: 100+20+10+300 = 430
}
```

`level2` accesses:
| Var | Origin      | Chain depth | Access pattern                          |
|-----|-------------|-------------|-----------------------------------------|
| `b` | level1      | 1           | `[R10 + offset_b]` (direct)            |
| `x` | level1      | 1           | `[R10 + offset_x]` (direct)            |
| `a` | main        | 2           | `[[R10 + CHAIN_SLOT] + offset_a]`       |

### Why level1 already captures `a`

`prescan_captured_vars(nf_for_level1, main_local_stack)` runs during main's
parsing (`tccgen.c:11978`).  It does a **flat token scan** of level1's entire
body — including the tokens inside level2's definition.  The token `a` appears
in level2's `return a + b + x + y;`, and `a` IS in main's `local_stack`.
So level1 already captures `a` with depth 1.  **This is correct and works today.**

### Why level2 fails to capture `a`

When `compile_nested_functions()` compiles level1 (`tccgen.c:11111`), level1's
`block(0)` discovers level2 and calls
`prescan_captured_vars(nf_for_level2, level1_local_stack)` (`tccgen.c:11978`).

- `b` found in level1's local_stack → captured ✓
- `x` found in level1's params → captured ✓
- `a` **NOT** in level1's local_stack → **not captured** ✗

The prescan never checks `tcc_state->current_nested_func` (level1's captured
vars).  Later, when level2's parser hits `a` at `tok_identifier` (`tccgen.c:7374`),
it searches `nf_for_level2->captured_tokens` — empty for `a` — and falls
through to `tcc_error("'a' undeclared")`.

---

## Design: Fixed Chain Slot Convention

R10 is already pushed as a callee-saved register in the function prologue, but
its position in the PUSH frame varies depending on which other registers are
pushed.  Computing the push-frame offset is possible but fragile and couples
codegen tightly to the register allocator.

**Chosen approach**: every function with `has_static_chain` explicitly stores
R10 at a **fixed, known offset** from FP immediately after the frame pointer
setup.  This is the **chain slot**.

```
CHAIN_SLOT_OFFSET = -4   (first slot below FP, i.e. FP - 4)
```

Multi-hop access is then uniform — each hop loads `[current_fp + CHAIN_SLOT_OFFSET]`:

```asm
; depth 1 (parent var): direct
LDR  Rd, [R10, #var_offset]

; depth 2 (grandparent var):
LDR  temp, [R10, #-4]          ; temp = saved chain = grandparent's FP
LDR  Rd,   [temp, #var_offset]

; depth 3 (great-grandparent var):
LDR  temp, [R10, #-4]          ; temp → grandparent's FP
LDR  temp, [temp, #-4]         ; temp → great-grandparent's FP
LDR  Rd,   [temp, #var_offset]
```

**Cost**: 4 bytes of stack + 1 STR instruction per nested function that
receives a static chain.  Acceptable for correctness.

---

## Changes (7 steps)

### Step 1 — Add `captured_chain_depth[]` to `NestedFunc`  (`tcc.h:~733`)

```c
typedef struct NestedFunc
{
  /* ... existing fields ... */
  int captured_offsets[MAX_CAPTURED_VARS];
  int captured_tokens[MAX_CAPTURED_VARS];
  int captured_vregs[MAX_CAPTURED_VARS];
  CType captured_types[MAX_CAPTURED_VARS];
+ int captured_chain_depth[MAX_CAPTURED_VARS];  /* 1 = parent, 2 = grandparent, ... */
  int nb_captured;
  /* ... */
} NestedFunc;
```

All existing captures get depth 1 (set in prescan, Step 3).

### Step 2 — Add `captured_chain_depths[]` to `TCCIRState`  (`tccir.h:~379`)

Parallel array to `captured_offsets_list[]`:

```c
  int32_t captured_offsets_list[32];
+ int32_t captured_chain_depths[32]; /* 1 = direct R10, 2+ = multi-hop */
  int32_t captured_count;
```

Initialize to 0 in `tcc_ir_alloc()` (already zeroed by `tcc_mallocz`).

### Step 3 — Extend `prescan_captured_vars()` to walk ancestor captures  (`tccgen.c:11196`)

Current code (simplified):
```c
Sym *s = sym_find2(parent_local_stack, t);
if (s && ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM)))
{
  /* ... existing capture logic — mark addrtaken, record offset, etc. ... */
  nf->nb_captured++;
}
```

Extend with an `else` branch after the existing capture block:
```c
    /* ... existing capture block (now also sets chain_depth = 1) ... */
    nf->captured_chain_depth[nf->nb_captured] = 1;
    nf->nb_captured++;
  }
+ /* Not found in parent locals — search parent's own captured vars.
+  * When compiling level1, current_nested_func == nf_for_level1.
+  * level1 captured 'a' from main with depth 1, so level2 inherits
+  * it with depth 2. */
+ else if (tcc_state->current_nested_func)
+ {
+   NestedFunc *parent_nf = tcc_state->current_nested_func;
+   for (int j = 0; j < parent_nf->nb_captured; j++)
+   {
+     if (parent_nf->captured_tokens[j] == t)
+     {
+       /* Guard: check not already captured (e.g. token appears twice) */
+       int dup = 0;
+       for (int k = 0; k < nf->nb_captured; k++)
+         if (nf->captured_tokens[k] == t) { dup = 1; break; }
+       if (dup) break;
+
+       nf->captured_offsets[nf->nb_captured]     = parent_nf->captured_offsets[j];
+       nf->captured_tokens[nf->nb_captured]      = t;
+       nf->captured_types[nf->nb_captured]       = parent_nf->captured_types[j];
+       nf->captured_chain_depth[nf->nb_captured] = parent_nf->captured_chain_depth[j] + 1;
+       nf->nb_captured++;
+       break;
+     }
+   }
+ }
```

**Why this works**: at prescan time for level2, `tcc_state->current_nested_func`
points to level1's `NestedFunc`.  level1's prescan (run during main's parsing)
already captured `a` with depth 1.  So the lookup finds `a` there and captures
it for level2 with depth 2.  This generalizes transitively to arbitrary depth.

### Step 4 — Propagate chain depths to IR  (`tccgen.c:~11293`)

In `gen_function()`, where `captured_offsets_list` is populated:

```c
  ir->captured_count = nf->nb_captured;
  for (int j = 0; j < nf->nb_captured && j < 32; j++)
+ {
    ir->captured_offsets_list[j] = nf->captured_offsets[j];
+   ir->captured_chain_depths[j] = nf->captured_chain_depth[j];
+ }
```

### Step 5 — Emit chain save in prologue  (`arm-thumb-gen.c`, prologue)

In `tcc_gen_machine_prologue()`, after the frame pointer setup (`MOV FP, SP`)
and stack allocation (`SUB SP, #stack_size`):

```c
+ /* Save incoming static chain (R10) at fixed chain slot [FP - 4].
+  * This allows child nested functions to follow the chain to
+  * grandparent frames via multi-hop LDR sequences. */
+ if (ir && ir->has_static_chain)
+ {
+   ot_check(th_str_imm(architecture_config.static_chain_reg, R_FP,
+                        4, /* abs offset for FP-4 encoding */
+                        6, ENFORCE_ENCODING_NONE));
+   /* Note: the stack allocator must reserve this slot — see Step 5b. */
+ }
```

**Step 5b — Reserve chain slot in stack layout**.  In `tccgen.c` (or `ir/core.c`),
when `has_static_chain` is set, bias `loc` by -4 before local variable
allocation begins, so that FP-4 is never assigned to a local var:

```c
  /* Reserve chain save slot at FP-4 */
  if (ir->has_static_chain)
    ir->loc -= 4;  /* or equivalent mechanism in the stack allocator */
```

If `loc` is not used directly (IR manages its own stack layout), add an
explicit 4-byte reserved region at the top of the local area in `ir/stack.c`.
The key invariant is: **no variable or spill slot may be placed at FP-4 when
`has_static_chain` is set**.

### Step 6 — ARM codegen: multi-hop chain dereference (4 sites)

The pattern is the same at all 4 sites.  Extract a helper function:

```c
/* Resolve the base register for a captured variable access.
 * For depth 1, returns R10 directly.
 * For depth > 1, emits LDR chain to follow ancestor frame pointers
 * and returns a scratch register holding the target ancestor's FP.
 * Caller must restore scratch via *out_scratch when done. */
static int resolve_chain_base(TCCIRState *ir, int ci,
                              uint32_t exclude_regs,
                              ScratchRegAlloc *out_scratch,
                              int *used_scratch)
{
  int depth = ir->captured_chain_depths[ci];
  if (depth <= 1)
  {
    *used_scratch = 0;
    return architecture_config.static_chain_reg;  /* R10 */
  }

  /* Multi-hop: follow chain through (depth - 1) intermediate frames.
   * Each frame saves its incoming R10 at [FP - 4] (CHAIN_SLOT_OFFSET). */
  *out_scratch = get_scratch_reg_with_save(exclude_regs);
  *used_scratch = 1;

  /* Start from R10 (points to immediate parent's FP) */
  thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
  ot_check(th_mov_reg(out_scratch->reg,
                       architecture_config.static_chain_reg,
                       FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                       no_shift, ENFORCE_ENCODING_NONE, false));

  for (int hop = 1; hop < depth; hop++)
  {
    /* LDR temp, [temp, #-4]  — follow chain link */
    load_from_base_ir(out_scratch->reg, PREG_REG_NONE,
                      IROP_BTYPE_INT32, 0,
                      4 /* abs */, 1 /* sign: negative */,
                      out_scratch->reg);
  }
  return out_scratch->reg;
}
```

Then update each of the 4 chain-access sites:

| # | File | Line | Context |
|---|------|------|---------|
| 1 | `arm-thumb-gen.c` | 2287 | LOAD path (`resolve_base_ir`) |
| 2 | `arm-thumb-gen.c` | 3215 | STORE path (`store_ex_ir`) |
| 3 | `arm-thumb-gen.c` | 4816 | LEA / ADD accumulator path |
| 4 | `arm-thumb-gen.c` | 6375 | Additional chain-relative access |

At each site, replace:
```c
base_reg = architecture_config.static_chain_reg;
```
with:
```c
ScratchRegAlloc chain_scratch;
int chain_used = 0;
base_reg = resolve_chain_base(ir, ci, exclude_regs, &chain_scratch, &chain_used);
/* ... existing access using base_reg ... */
if (chain_used) restore_scratch_reg(&chain_scratch);
```

### Step 7 — Remove xfail  (`tests/ir_tests/test_qemu.py:290`)

```python
NESTED_XFAIL_TEST_FILES = [
-   ("nested_multi_level.c", 0),
]
```

Move the test to the passing `NESTED_TEST_FILES` list.

---

## Compilation & Verification

```bash
# 1. Build
make cross -j16

# 2. Quick manual test
cd tests/ir_tests
python run.py -c nested_multi_level.c
# Expected output:
#   331
#   430

# 3. Dump IR to verify chain_depth metadata
python run.py -c nested_multi_level.c --dump-ir
# Look for captured var 'a' with chain_depth=2

# 4. Disassemble level2 to verify double-dereference
arm-none-eabi-objdump -d build/nested_multi_level.elf | grep -A 30 '<level1.0.level2'
# Should show:  LDR Rtemp, [R10, #-4]   then   LDR Rd, [Rtemp, #offset]

# 5. Full regression suite
cd ../.. && make test -j16
```

## Risks & Edge Cases

1. **Stack alignment**: Reserving 4 bytes at FP-4 may shift existing locals.
   Verify 8-byte AAPCS alignment is maintained after the bias.
2. **Offset encoding**: FP-4 is a small negative offset — verify `th_str_imm`
   and `load_from_base_ir` handle negative offsets for the chain slot correctly.
3. **Depth > 2**: The multi-hop loop generalizes, but add a test with 3 levels
   (f → g → h → i accessing f's var) to confirm.
4. **Mixed depths**: A single nested function may capture vars at different
   depths (depth 1 for parent vars, depth 2 for grandparent vars).  Each
   captured var uses its own `chain_depths[ci]` — no conflict.
5. **Address-of captured var**: `LEA` on a depth-2 variable must produce the
   correct address.  The chain hop gives the ancestor FP, and adding the offset
   gives the variable's address — same pattern, just no final LDR.
6. **Store to grandparent var**: `a = 100` in the test mutates `a` in main's
   frame via the chain.  The STORE path (site #2) must use the resolved base
   register.
