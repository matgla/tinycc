# Codegen dry-run optimisation plan

Two complementary optimisations to reduce compilation time on memory-constrained
hardware (4–6 MB for TCC).

---

## Option A — Skip dry-run for scratch-conflict-free functions

### Rationale

The dry-run serves three purposes:

1. Scratch tracking — fills `dry_insn_scratch[]` / `dry_insn_saves[]`, feeds Phase-3 fixup.
2. LR-in-prologue detection — `tcc_gen_machine_dry_run_get_lr_push_count()`.
3. Branch offset analysis — `branch_opt_analyze()` selects 16-bit vs 32-bit encodings.

If scratch pushes are provably impossible, purposes 1 and 2 are no-ops and the
dry-run can be skipped entirely. Purpose 3 falls back to conservative 32-bit
encodings (already the default fallback), costing 2 bytes per branch — acceptable.

### Condition

ARM has r0–r12 = 13 allocatable integer registers; scratch needs at most 2
simultaneously. If there are always ≥2 free integer registers and ≥2 free VFP
registers at every program point, no push/pop can occur.

```c
int can_skip_dry_run =
    __builtin_popcountll(ir->ls.dirty_registers)       <= 11 &&
    __builtin_popcountll(ir->ls.dirty_float_registers) <= 14; // 16 s-regs available
```

Evaluated once, just before the two-pass loop in `tcc_ir_codegen_generate`.

### What changes when skipping

| Concern | Effect |
|---|---|
| `dry_insn_scratch[]` / `dry_insn_saves[]` | Stay zero (`tcc_mallocz`) — correct |
| Phase-3 fixup loop | Sees all-zero saves — no-op, safe to run or skip |
| LR in prologue | No scratch push → no LR push; `leaffunc` already set correctly |
| Branch optimizer | `branch_opt_analyze` not called → 32-bit fallback for all branches |
| Prologue emission | Uses `ir->ls.dirty_registers` + `stack_size` directly — both available |

### Loop structure change

```c
// still call branch_opt_init so get_encoding returns the 32-bit fallback cleanly
tcc_gen_machine_branch_opt_init();

int pass_start = can_skip_dry_run ? 1 : 0;
for (int pass = pass_start; pass < 2; pass++)
{
  ...
}
```

When `pass_start == 1`, emit the prologue at the point where it was previously
emitted inside the dry-run finalisation block (just before the real-run starts).

---

## Modified Option B — Cache decoded operands, reuse in real-run

Only active when Option A did **not** fire.

### Rationale

Every instruction goes through `decode_mop_args` → `machine_op_from_ir` (interval
table lookups, register resolution) **twice** — once in the dry-run, once in the
real-run. Caching the dry-run results eliminates the second decode pass.

Only `dest`, `src1`, `src2` are cached (3 slots × 24 bytes = 72 bytes/instruction).
`scale` and `accum` operands (indexed memory ops, MLA) are rare and re-decoded in
the real-run.

### Memory cost

`3 × sizeof(MachineOperand) × N` on a 32-bit host:

| Instructions | Memory |
|---|---|
| 50  | 3.6 KB |
| 100 | 7.2 KB |
| 500 | 36 KB  |

### Allocation

```c
// allocated before the two-pass loop, only when !can_skip_dry_run
MachineOperand *mop_cache = tcc_malloc(3 * ir->next_instruction_index * sizeof(MachineOperand));
// layout: [3*i+0] = dest, [3*i+1] = src1, [3*i+2] = src2
```

### Dry-run: fill cache

After every `DECODE(...)` call in the dry-run instruction loop:

```c
mop_cache[3*i+0] = a.dest;
mop_cache[3*i+1] = a.src1;
mop_cache[3*i+2] = a.src2;
```

### After dry-run: decide whether cache is valid

Phase-3 fixup mutates the interval table when `any_fixup != 0`.

```c
int use_mop_cache = !any_fixup;
if (!use_mop_cache) {
    tcc_free(mop_cache);
    mop_cache = NULL;
}
```

### Real-run: use cache via wrapper macro

```c
#define DECODE(...) (use_mop_cache                                                \
    ? cached_mop_args(mop_cache, i, (MopSpec){__VA_ARGS__},                      \
                      ir, cq, &src1_ir, &src2_ir, &dest_ir, has_incoming_jump)   \
    : decode_mop_args(ir, cq, &src1_ir, &src2_ir, &dest_ir, i,                  \
                      has_incoming_jump, (MopSpec){__VA_ARGS__}))
```

`cached_mop_args` reads dest/src1/src2 from the cache and re-calls
`machine_op_from_ir` only for `scale` and `accum` when the spec requests them.

### Teardown

```c
tcc_free(mop_cache);   // after real-run ends; safe when NULL (tcc_free checks)
```

---

## Combined control flow

```
can_skip_dry_run == 1
    Option A fires: single pass (pass=1 only), no cache, 32-bit branches,
    prologue emitted immediately before real-run.

can_skip_dry_run == 0
    Option B active: two passes, mop_cache allocated.
        any_fixup == 0  →  cache reused in real-run
        any_fixup != 0  →  cache freed, normal decode in real-run
```

---

## Files to modify

| File | Change |
|---|---|
| `ir/codegen.c` | Condition check, `pass_start`, prologue placement, cache alloc/fill/use/free |
| `arm-thumb-gen.c` | Ensure `branch_opt_init` is safe to call without a subsequent `branch_opt_analyze` |
