# Plan: SSA const-string / strlen / strcpy folding pass

**Status:** planned · **Branch:** `legacyOptRemoval` · Part of the `const_prop`→SSA
migration ([`plan_legacy_flat_ir_ssa_retire.md`](plan_legacy_flat_ir_ssa_retire.md)).

## Why

The only `strlen`/`strcmp`/`memcmp`… constant-folder today is the **flat**
`string_calls` pass (`tcc_ir_opt_const_string_calls`, [`ir/opt_constfold.c`](../ir/opt_constfold.c)),
gated on `FLAG(opt_const_prop)` and run **before** the SSA phase. It can only fold a
call whose string-address argument is *already* a constant `GlobalSym+addend` at that
early point — which, for index-computed addresses like `strlen(&a[i][j] ± k)`, only
happens when `const_prop` first folds the local index chain. When `const_prop` is
disabled, the flat pass sees a runtime address, irreversibly redirects
`strlen → __tcc_strlen`, and the later SSA folding of the address is wasted (no
consumer). This is the mechanism behind the `gcc-execute/strlen-4:test_array_ptr`
regression (585→897 instrs at O2; see the analysis that motivated this plan).

This pass is the **SSA-phase consumer**: it re-runs const-string folding *after*
`ssa:cprop`/`ssa:fold`/`ssa:gvn`/`ssa:reassoc` have reduced the address to a constant
symref. `resolve_str_builtin_id` already maps both `"strlen"` and `"__tcc_strlen"` to
`STRBI_STRLEN`, so it folds calls the flat pass already redirected — no need to defer
the flat redirect.

**Dependency:** by itself this closes only calls whose address is constant *at SSA
time*. Full strlen-4 recovery also needs the reach fix (SSA constant-propagation of
single-def local scalar VARs into arithmetic operands — "Gap A" in the analysis). This
pass is what makes Gap A pay off, and lands first because it is self-contained.

## Non-goals for v1

- No new arithmetic/address folding — we consume what the existing SSA passes produce.
- Not deleting flat `string_calls` yet; it stays until `const_prop` retires. Both
  folding to the same immediate is safe (idempotent result).

## Architecture

### New location: `source/opt/ssa/`

A new directory (mirrors `source/opt/framework/`, `source/backend/generators/`), so the
in-flight source restructure ([[source-restructure-plan]]) has a home for SSA passes as
they are relocated out of `ir/opt/`.

```
source/opt/ssa/
  Makefile              # included by top Makefile; adds -Isource/opt/ssa, lists objs
  const_string_fold.c   # driver + registry (g_handlers table + by-id index)
  const_string_fold.h   # public entry: tcc_ir_ssa_opt_const_string_fold(ctx)
  str_handlers.h        # StrFoldCtx/StrFoldHandler interface + extern handler decls
  str_strlen.c          # tcc_strfold_strlen
  str_strcpy.c          # tcc_strfold_strcpy
  str_strcmp.c          # tcc_strfold_strcmp / strncmp / memcmp
```

**Decision (file layout): one TU per builtin from the start** — each handler is its own
compilation unit contributing one `extern const StrFoldHandler` to the central registry.
`const_string_fold.c` holds only the driver + table.

Build wiring (matches `source/opt/Makefile`):
- Top `Makefile`: add `include source/opt/ssa/Makefile` next to the existing
  `include source/opt/Makefile` (line ~325), and append the `.c` to `CORE_FILES`
  (or a `SSA_OPT_FILES` var the sub-Makefile exports).
- The generic `$(X)%.o : %.c` rule (Makefile:370) already builds any nested path, and
  `DEFINES` already carries `-I. -I./ir -I./ir/opt`. The sub-Makefile only adds
  `-Isource/opt/ssa` and an explicit object dependency block (header list) like
  `source/opt/Makefile` does, so header edits force a rebuild.

### Registry interface (no string-compare if/else chain)

Callee → handler is resolved once via the existing `resolve_str_builtin_id` (returns a
compact `StrBuiltinId` enum). Handlers are looked up by that id through an O(1) index
table, not repeated `strcmp`.

```c
/* str_handlers.h */

typedef struct StrFoldCtx {
  IRSSAOptCtx *ssa;        /* SSA opt context (ir, cfg, vinfo) */
  TCCIRState  *ir;
  int          call_idx;   /* index of the FUNCCALL* instruction */
  int          builtin_id; /* StrBuiltinId resolved from callee */
  int          is_valued;  /* 1 = FUNCCALLVAL, 0 = FUNCCALLVOID */
} StrFoldCtx;

typedef struct StrFoldHandler {
  int         builtin_id;                    /* STRBI_* this handler serves */
  const char *name;                          /* logging / -dump-ir-passes */
  int       (*can_fold)(const StrFoldCtx *); /* pure predicate — NO mutation */
  int       (*fold)(StrFoldCtx *);           /* mutate IR, return #changes */
} StrFoldHandler;

/* Each handler is a plain object; relocating it to its own TU later is
 * cut-paste + one line in the registry + one Makefile object. */
extern const StrFoldHandler tcc_strfold_strlen;
extern const StrFoldHandler tcc_strfold_strcpy;
extern const StrFoldHandler tcc_strfold_strcmp;
/* … append new ones here … */
```

`can_fold` / `fold` are deliberately split: the driver skips cheaply, the predicate stays
side-effect-free (safe to call from a future cost model or a dry-run), and `fold` owns
all mutation. This is the `can_be_folded(input) { fold_processor(input) }` shape asked
for.

### Registry table + driver

```c
/* const_string_fold.c */

static const StrFoldHandler *const g_handlers[] = {
  &tcc_strfold_strlen,
  &tcc_strfold_strcpy,
  &tcc_strfold_strcmp,
  /* append; order irrelevant — dispatch is by builtin_id */
};

/* built once from g_handlers, indexed by StrBuiltinId for O(1) dispatch */
static const StrFoldHandler *g_by_id[STRBI__COUNT];   /* add STRBI__COUNT sentinel to enum */

int tcc_ir_ssa_opt_const_string_fold(IRSSAOptCtx *ctx)
{
  int changes = 0;
  ensure_index_built();                      /* fill g_by_id from g_handlers */
  for (int i = 0; i < ctx->ir->next_instruction_index; ++i) {
    IRQuadCompact *q = &ctx->ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    Sym *callee = irop_get_sym_ex(ctx->ir, tcc_ir_op_get_src1(ctx->ir, q));
    if (!callee) continue;
    int id = resolve_str_builtin_id(callee->v, get_tok_str(callee->v, NULL));
    const StrFoldHandler *h = (id > 0 && id < STRBI__COUNT) ? g_by_id[id] : NULL;
    if (!h) continue;
    StrFoldCtx c = { ctx, ctx->ir, i, id, q->op == TCCIR_OP_FUNCCALLVAL };
    if (h->can_fold(&c))
      changes += h->fold(&c);
  }
  return changes;
}
```

### Driver placement

Add to the `SSA_RUN` block in `tcc_ir_ssa_opt_run` ([`ir/opt/ssa_opt.c`](../ir/opt/ssa_opt.c),
~line 744), immediately **after** `ssa:load_cse` and **before** `ssa:branch` /
`ssa:cmp_eq_prop`:

```c
SSA_RUN("ssa:const_string_fold", tcc_ir_ssa_opt_const_string_fold(ctx));
```

Rationale: address folding (`ssa:fold`/`gvn`/`reassoc`) has run earlier in the same
iteration; folding `strlen(...) → imm` here lets `ssa:branch`/`ssa:cmp_eq_prop` collapse
`imm == N` and `ssa:dce` kill the dead `printf`/`abort` tail in the same driver pass. The
driver already re-iterates to fixpoint (`max_iterations`), so cascades converge.

## v1 handlers

Ported from the flat `string_calls` logic, reusing the existing shared evaluators in
[`ir/opt_utils.h`](../ir/opt_utils.h): `ir_opt_eval_const_string`,
`ir_opt_eval_const_u64`, `ir_opt_get_call_param_operand`.

1. **`strlen`** (`STRBI_STRLEN`) — `can_fold`: arg0 evaluates to a const string (global
   symref, or stack-tracked via the existing `ir_opt_eval_stack_strlen` logic).
   `fold`: NOP params, rewrite op to `ASSIGN`, `src1 = imm(strlen(s))`. Primary win.
2. **`strcmp`/`strncmp`/`memcmp`** (`STRBI_STRCMP`…) — both args const strings ⇒ fold to
   the comparison immediate (reuse `ir_opt_fold_strcmp_result` etc.); else no-op (leave
   redirect to the flat pass for now).
3. **`strcpy`** (`STRBI_STRCPY`) — **implemented (`str_strcpy.c`), registered, and
   passing.** `can_fold`: dst (param0) is a bare stack address (`STACKOFF`), src (param1)
   is a const string of length L with `(L+1) % 4 == 0`, the source symref addend is
   word-aligned, and the call result is unused. `fold`: rewrite the `FUNCCALLVAL/VOID` in
   place to `BLOCK_COPY(dst_stackoff, src_symref, L+1)` (mirrors the memset→BLOCK_COPY
   construction in `ir/opt.c`; the backend lowers small copies to inline `ldm`/`stm`, large
   to memcpy). `(L+1)%4` must be a whole word — a partial word would clobber dst past the
   NUL. tccgen already lowers *safe* `_chk` variants to plain strcpy, so only proven-safe
   copies reach the pass.

   **Root-cause fix that unblocked it:** BLOCK_COPY reads its source with `LDM`, which
   **faults on an unaligned base**. String literals are byte-granular, so a literal source
   could land at e.g. `…a9` → QEMU UsageFault in `builtins/str{,n}cat-chk` (the existing
   struct-init BLOCK_COPY always had an aligned anonymous rodata block, so it never hit
   this). Fixed by **word-aligning string literals** in tccgen (`ad.a.aligned = 3` on the
   `TOK_STR`/`TOK_LSTR` path) plus a defensive `addend % 4 == 0` guard in the handler.
   Covered by the runtime IR test `352_ssa_const_string_fold.c` (aligned + odd-length
   cases) and UT11 negative-guard tests.

### Shared-helper promotion

`ir_opt_eval_const_string_operand` and `ir_opt_stack_addr_offset` are currently `static`
in `opt_constfold.c`. Promote the ones the handlers need to `ir/opt_utils.{h,c}`
(de-static, add prototypes) rather than duplicating. `ir_opt_nop_call_params` /
`change_callee_sym_keep_type` are likewise reused, not re-implemented.

## Validation

- **Correctness:** `make test -j16` (13.5k IR tests). Add a focused IR test under
  `tests/ir_tests/` exercising `strlen`/`strcmp`/`strcpy` of constant strings with
  index-computed addresses, plus a **negative** test: a `volatile`/stored-between global
  string must NOT fold (aliasing safety), and a runtime (non-const) address must be left
  alone. QEMU runtime check for the fold results.
- **Size:** A/B with `TCC_DISABLE_PASS=const_prop` + per-function
  `arm-none-eabi-objdump` on `strlen-4.c` (target: `test_array_ptr` OFF 897 → 585 once
  Gap A also lands; measure this pass's standalone contribution first). Gate on
  `python metrics/compare_worktree.py --baseline-commit HEAD --opt o2`.
- **Observability:** the pass name `ssa:const_string_fold` participates in
  `-dump-ir-passes=` and `TCC_DISABLE_PASS=` automatically via `SSA_RUN`.

## Follow-ups (append handlers, no driver change)

- strcpy/stpcpy inline byte-store fold; `strcat`/`strncat`; `strchr`/`strrchr` const
  fold; `memchr` const fold (flat has it — port the offset logic).
- Once every flat `string_calls` case has an SSA handler and `const_prop` retires,
  delete `tcc_ir_opt_const_string_calls` and its pipeline entry.
- Relocate each handler to its own TU in `source/opt/ssa/` (str_strlen.c, …) — the
  registry + Makefile were designed for exactly this split.
