# Optimization DSL — worked example: MLA fusion

> tinycc · armv8-m fork · DSL worked example · 2026-07-09
>
> Status: **review artifact** — not a work item.
> Companion to [../optimization_dsl_proposal.md](../optimization_dsl_proposal.md)
> and its foundation [../plan_opt_predicate_framework.md](../plan_opt_predicate_framework.md).
> Every `ir_op_any`, `IROP_*`, `ir_q_*`, `when()/and()`, and the whole `OPT_GEN_*`
> family used below is **proposed, not implemented**.

## §0 Why this example

The main proposal claims the DSL's headline benefit with MLA fusion: *"the DSL
cannot express an MLA rewrite that leaves op4 invisible to later use/def scans,
because the accessor advertises it by construction."* That is a strong claim
about exactly the pass that produced the most fuzzer regressions in this fork
(tests 257, 267, 285, 5053, 9494). So the fair test of the DSL is not a toy
peephole — it is **this** pass, `ir_gen_mla_fusion`
([ir/opt_gens_fusion.c:169](../../ir/opt_gens_fusion.c#L169)).

This document translates that real pass into the proposed DSL, then accounts
honestly for what the DSL captures, what it can only *call out to*, and what it
cannot express at all. The verdict (§5) is the point of the exercise.

## §1 The pass today

`MUL(a,b); ADD(mul_result, c) → MLA(a, b, c)` — fold a multiply feeding an add
into one multiply-accumulate. The real function is ~170 lines. Its structure,
by role:

| Lines | Role | Shape |
|---|---|---|
| 174–175 | feature-flag gate (`-fmla-fusion`) | scalar guard |
| 179–210 | **match**: ADD whose src1 *or* src2 is defined by a MUL-class op | DU back-query, either operand |
| 212–216 | guard: no SYMREF operand; accum STACKOFF must be lval | operand-tag guards |
| 218–248 | guard: MUL operands not lval-non-local / not immediate (short form); MUL result single-use; **not a duplicated MUL** | includes an O(n) whole-function scan (228–243) |
| 250–251 | guard: MUL and ADD in same block | range/structural |
| 259–262 | guard: no operand derived from memory (stale-value class) | recursive DU walk |
| 264–269 | guard: accumulator not redefined between MUL and ADD | DU back-query |
| 271–277 | guard: memory-read accumulator can't skip stores (seed 5053) | range-preserves-memory |
| 279–306 | **variant match**: 64-bit MLA needs a store-back-to-accumulator fold — a *third* instruction | look-ahead + STORE match |
| 308–316 | rewrite prep: sign flag; alias MUL srcs onto accum for SMLAL in-place | conditional operand rewrite |
| 318–333 | **rewrite**: MUL→MLA, and *move the operand block* to a fresh 4-slot pool region | op4 growth |
| 335–337 | **rewrite**: NOP the ADD (and the store, for 64-bit) | delete |

## §2 The pass in the DSL

What a faithful DSL author would write. The trigger opcode is `ADD` (the pass
scans ADDs and looks back for the MUL), so the match is a `.pair` whose *second*
is the trigger and whose *first* is found by DU:

```c
OPT_GEN_FLAT(mla_fusion, TCCIR_OP_ADD) {           /* pre-SSA / IROptCtx engine */
  PATTERN(
    .enabled_by = FLAG(opt_mla_fusion),            /* 174–175 */
    .pair = {
      .first = { .op_class = IROP_P_MLA_MUL,        /* MUL / SMULL / UMULL / ... */
                 .link     = IR_PAIR_DEF_OF_EITHER_SRC },   /* 179–210 */
      .same_block = 1 },                            /* 250–251 */
    .constraints = {
      .src1 = IR_CONSTRAINT_NOT_SYMREF,
      .src2 = IR_CONSTRAINT_NOT_SYMREF,
      .dest = IR_CONSTRAINT_NOT_SYMREF });          /* 212–214 */

  GUARD(
    when(accum_is_valid_lval_or_reg())              /* 215–216 */
    and(mul_operands_fusible(first))                /* 224, 245–246 */
    and(ir_opt_du_uses(du, vreg(first_dest)) == 1)  /* 247 single-use */
    and_not(mul_is_duplicated(ctx, first))          /* 218–243 — see §3 */
    and_not(operand_derived_from_memory(ctx, mul_src1(first), mul_idx)
            || operand_derived_from_memory(ctx, mul_src2(first), mul_idx)
            || operand_derived_from_memory(ctx, accum, i))         /* 259–262 */
    and(accum_not_redefined_between(ctx, mul_idx, i))              /* 264–269 */
    and(accum_memory_read_safe(ctx, mul_idx, i)));                 /* 271–277 */

  REWRITE(
    .on = first,                                    /* rewrite the MUL, not the ADD */
    .new_op = TCCIR_OP_MLA,
    .op4    = accum,                                /* accumulator advertised as op4 */
    .delete_second = 1);                            /* NOP the ADD — 335 */
}
```

That is the honest best case. Now the accounting.

## §3 What the DSL captures cleanly

Roughly the top third of the pass maps to DSL constructs with no loss:

- **Trigger + pair discovery (179–210).** `.pair.first.link = IR_PAIR_DEF_OF_EITHER_SRC`
  encodes "either ADD source is defined by a MUL-class op." The either-operand
  fan-out — the part that is easy to get subtly wrong by hand — becomes one
  declared link relation.
- **op-class match (`IROP_P_MLA_MUL`).** The framework's L1 mask replaces the
  private `ir_gen_is_mla_mul_op` / `ir_gen_is_long_mla_mul_op` helpers. One
  named mask, greppable, shared.
- **The op4 rewrite (318–333).** This is the claim that motivated the whole
  proposal, and it holds: `.op4 = accum` routes through the framework's L5
  mutation funnel, which owns the "grow to a 4-slot pool region" invariant
  (real code lines 325–333) *and* makes op4 visible to every subsequent use/def
  scan. A DSL author cannot forget the pool move because they never write it —
  and cannot leave op4 invisible because the accessor advertises it. This is a
  genuine, real reduction of the exact bug class (257/267/285/9494).
- **Same-block + single-use + delete (250, 247, 335).** Standard declared guards
  and a `.delete_second`.

If the pass were *only* these parts, the DSL would be an unambiguous win: ~40
lines of error-prone fan-out and pool bookkeeping collapse to a declaration.

## §4 What the DSL can only call out to — and what it can't express

The bottom two-thirds do not fit the pattern→guard→rewrite shape. Being honest
about this is the whole reason for the example.

### 4.1 Guards that must stay as named C predicates (escape hatch)

Every `and(...)`/`and_not(...)` clause in §2 that names a function
(`mul_is_duplicated`, `operand_derived_from_memory`, `accum_not_redefined_between`,
`accum_memory_read_safe`, `mul_operands_fusible`) is **not** DSL — it is a
hand-written C predicate the DSL merely *invokes*. The DSL contributes the
composition (`&&` folding, tracing) but none of the logic. Concretely:

- **`mul_is_duplicated` (218–243)** is an **O(n) whole-function scan** for a
  second MUL with the same operands. A peephole DSL has no vocabulary for
  "search the entire function for a twin instruction." This stays a C function,
  full stop. It is also the kind of scan the framework's L3 range engine is
  *not* designed for (it is global, not range-scoped).
- **`operand_derived_from_memory` (259–262, real helper 135–168)** is a
  **recursive DU walk** with a depth bound. No declarative construct.
- **`accum_memory_read_safe` (271–277)** combines `is_jump_target` with
  `ir_xform_range_preserves_memory` — expressible as a framework L3 range
  predicate, but only because that predicate already exists; the DSL adds
  nothing.

So of the ~9 guard conditions, **1–2 are declarative** (single-use, same-block,
tag checks) and **5–6 are C predicates the DSL only chains.** The GUARD block
looks declarative but is mostly a call list.

### 4.2 The 64-bit variant is a *triple*, not a pair (279–306)

For `SMULL`/`UMULL`, fusion is legal only when the 64-bit result is stored
straight back to the accumulator's own slot — the pass looks ahead past NOPs for
a `STORE(add_dest → accum_slot)` and folds *that* store into the rewrite too
(`store_idx`, deleted at 337). This is a three-instruction pattern with a
data-flow-conditional third member. The `.pair` grammar in the proposal cannot
express it; it would need a `.triple` with a conditional third leg *and* a
predicate tying the store's dest vreg to the accumulator vreg. The proposal
lists `.triple` as "on demand" — this example is the demand, and it is not
trivial.

### 4.3 Rewrites the DSL model has no slot for (308–316)

Before emitting the MLA, the pass **mutates the MUL's own source operands**:
if a MUL source aliases the accumulator low word, it rewrites that source to the
accumulator operand (`tcc_ir_set_src1/src2(mul_idx, accum_op)`), so SMLAL/UMLAL
can accumulate in place. This is a *pre-rewrite of a different instruction's
operands, conditional on an alias analysis*. The `REWRITE` block has `.new_op`,
`.op4`, `.delete_second` — there is no way to say "and also, conditionally,
overwrite src1 of the matched-first instruction with a computed operand." This
is genuinely imperative and stays C.

### 4.4 Flag gate and sign flag

`.enabled_by = FLAG(opt_mla_fusion)` (174) and the sign-flag assignment
`final_dest.is_unsigned = (old_mul_op == TCCIR_OP_UMULL)` (308–309) are minor,
but the second is another "compute a field on a synthesized operand" step with
no declarative slot.

## §5 Verdict

Line accounting for *this* pass:

| Category | Lines | DSL disposition |
|---|---:|---|
| Match fan-out + op-class + pair | ~40 | **declarative** (real win) |
| op4 rewrite + pool move | ~15 | **declarative** (the headline win) |
| Simple guards (single-use, same-block, tags) | ~10 | **declarative** |
| Complex guards (dup scan, mem-derived, range) | ~45 | **C predicate, DSL only chains** |
| 64-bit store-back triple | ~28 | **not expressible** (needs `.triple` + conditional leg) |
| Conditional MUL-operand rewrite | ~10 | **not expressible** (imperative) |
| Flag/sign bookkeeping | ~5 | partial |

So for the pass the proposal picks as its flagship, roughly **half is
declarative and half stays imperative** — and the imperative half is precisely
the fuzzer-bug-prone part (the dup scan, the memory-derived guard, the seed-5053
range guard, the alias rewrite). The DSL's real, defensible claim is narrower
than the proposal states:

> **The DSL makes op4 impossible to forget and collapses the operand fan-out.
> It does *not* make the guards declarative — the hard guards remain
> hand-written C predicates that the DSL merely composes and traces.**

That is still worth something: op4 invisibility caused four named regressions,
and the fan-out/pool-move is exactly the mechanical part that is easy to
mis-hand-roll. But this example does **not** support the proposal's stronger
framing that new passes become cheap to write correctly — the correctness of
MLA fusion lives almost entirely in the C predicates the DSL can't author.

## §6 Recommendation for the main proposal

1. **Re-scope the pitch.** Sell the DSL as "op4-safe fan-out + rewrite sugar for
   *simple* peepholes," not as a guard-eliminating pass-authoring framework.
   The `bool_idempotent` / `mul_to_shl` class of pass is the true target; MLA
   fusion is at the edge of, or past, the DSL's reach.
2. **Pick a different flagship.** Using MLA fusion as the motivating example
   over-promises. A pass whose guards are *all* framework predicates (e.g. a
   fold rule from `ssa_opt_fold.c`) would show the DSL at its actual strength.
3. **Specify `.triple` and conditional legs before claiming pair/triple
   support**, or explicitly exclude multi-instruction store-back folds — this
   example shows they are not a minor add-on.
4. **Make "drop to C" the normal path, not an escape hatch.** Since real passes
   are ~half named C predicates, "drop to a C guard" is where authors actually
   live. The cleanest way to make that first-class is **not** the macro DSL but a
   **function-pointer manipulator library + hooks** (main proposal §11.1): a
   generic `ir_gen_fuse_pair(ctx, i, &spec)` owning the shared op4/pool rewrite,
   with a `guard` hook for the varying logic. §7 below shows that version of this
   pass head-to-head. It captures the same declarative core, keeps the hard
   guards as breakpointable C, and — unlike the macros — is C11-only and passes
   the `test-selfhost` gate today (see §8).

The one-line takeaway for review: *the DSL earns its keep on the mechanical
op4/fan-out layer of this pass, but not on its correctness-critical guard layer
— so the proposal should be re-scoped to the passes where the guard layer is
already framework predicates, and the hook shape (§7) is the better fit for
everything else.*

## §7 The hook version, head-to-head

The same pass expressed as a `FusePairSpec` + hooks instead of macros:

```c
static int mla_guard(IROptCtx *ctx, const FusePairState *st)
{
  /* every hard guard from §4.1 lives here, as plain C — breakpointable */
  return mul_operands_fusible(st->first)
      && ir_opt_du_uses(&ctx->du, st->first_dest_vr) == 1
      && !mul_is_duplicated(ctx, st->first)              /* the O(n) scan */
      && !operand_derived_from_memory(ctx, st->mul_src1, st->mul_idx)
      && !operand_derived_from_memory(ctx, st->mul_src2, st->mul_idx)
      && !operand_derived_from_memory(ctx, st->accum, st->add_idx)
      && accum_not_redefined_between(ctx, st->mul_idx, st->add_idx)
      && accum_memory_read_safe(ctx, st->mul_idx, st->add_idx);
}

static const FusePairSpec mla_spec = {
  .trigger_op     = TCCIR_OP_ADD,
  .first_op_class = IROP_P_MLA_MUL,
  .link           = IR_PAIR_DEF_OF_EITHER_SRC,
  .guard          = mla_guard,
  .rewrite        = fuse_pair_to_op4,   /* shared: MUL→MLA, op4=accum, pool move, NOP add */
};
/* engine row:  {TCCIR_OP_ADD, ir_gen_fuse_pair_dispatch(&mla_spec), "mla_fusion"} */
```

**What changed vs. the macro version (§2):**

- The op4/pool rewrite (§3, the headline win) is now a *shared function*
  `fuse_pair_to_op4`, reused by every pair fusion — same safety, written once.
- The hard guards (§4.1) sit in `mla_guard` as ordinary C. No pretence that they
  are declarative; they are exactly the fuzzer-bug-prone predicates, now in one
  named, testable, breakpointable function.
- No `when()/and()` guard-DSL macros needed, so this does **not** wait on that
  framework layer.

**What did *not* change** — the ceiling is identical:

- The 64-bit store-back triple (§4.2) still doesn't fit `FusePairSpec`; it needs
  either a `FuseTripleSpec` or a bespoke pass.
- The conditional MUL-operand alias rewrite (§4.3) still lives in `.rewrite` as
  imperative code — the hook gives it a home, but doesn't make it declarative.

So the hook shape is *strictly better packaging* of the same reality the macro
version exposed: the mechanical layer is shared and safe, the correctness layer
is honest C. It does not raise the expressiveness ceiling.

## §8 Language ceiling (why C11, not C23)

Both shapes above must be compilable by **this fork's tcc**, not just the host
gcc: `ir/opt/opt_gens_fusion.c` — where this pass lives — is in the
`tests/selfhost/test_selfhost_compile.py` bootstrap set, run by `make test` via
the `test-selfhost` gate. The gate cross-compiles the optimizer through the
ARMv8-M tcc and would fail on any construct the fork's front-end can't parse.

Consequence: **C23 is not freely available here.** Every C23 feature used in
`ir/opt/` is a front-end feature that must first be implemented in `tccgen.c`,
plus a self-host regression risk — the cost is doubled, not "clang accepts it."
The hook design needs none of it: it is plain C11 (struct-of-function-pointers,
designated initializers, compound literals), and `opt_gens_fusion.c` already
ships that exact style through the gate today. If any C23 sugar is ever worth
pulling in, `[[nodiscard]]` on the change-signal return is the only candidate
with real payoff, and it should be a deliberate front-end feature with its own
self-host test — never a DSL convenience.
