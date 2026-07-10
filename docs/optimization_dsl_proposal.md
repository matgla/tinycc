# Optimization DSL Proposal — Declarative Pass Expression

> tinycc · armv8-m fork · optimization DSL · 2026-07-09
>
> Status: **proposal** — not implemented, not merged.
> **Hard dependency:** this builds on the predicate & query framework
> ([plan_opt_predicate_framework.md](plan_opt_predicate_framework.md)), which is
> *also* an unimplemented proposal. Every `ir_op_any`, `IROP_P_*`,
> `ir_q_barrel_shifted`, `ir_q_operands`, `irop_is_direct_stack_slot`, and the
> `when()/and()/and_not()` guard DSL used below come from *that* document and do
> not exist in the tree today. The DSL cannot be built before the framework
> lands (at least L1/L2/L4/L5). See §13 for the dependency gate.
> Related: [optimizations/architecture.md](optimizations/architecture.md).

## §0 Executive summary

Current optimization passes are written as imperative C: scan loops, switch
bodies, hand-rolled guard predicates, private mutation helpers. The same
*shape* repeats across the generator tables — match a pattern, check
conditions, rewrite, signal change. This proposal adds a **declarative layer**
on top of the existing generator model so that a new optimization can be
expressed as a small **pattern → guard → rewrite** clause instead of a
hand-written dispatch function.

The DSL does **not** replace the C implementation and it is **not** a runtime
interpreter. It is a set of C *expansion macros*: `OPT_GEN(name) { ... }`
expands to a normal `ssa_gen_fn` / `ir_opt_gen_fn` dispatch function, and a
companion macro emits the `IRSSAOptGen` / `IROptGen` table entry that the
existing engine already dispatches. The generated function calls the same
shared helpers a hand-written pass would call — so there is no per-instruction
interpretation overhead beyond what the guards themselves cost. Pass authors
write the *what*; the macros emit the *how* as ordinary C.

```c
/* BEFORE — imperative C (real API), ~30 lines */
static int ir_gen_mul_to_shl(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];
  if (q->op != TCCIR_OP_MUL)
    return 0;
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);
  if (!irop_is_immediate(src2))
    return 0;
  int64_t m = irop_get_imm64_ex(ir, src2);
  if (m <= 0 || (m & (m - 1)) != 0)         /* not a power of two */
    return 0;
  /* + hand-rolled: single-use check, barrel-shift guard, etc. */
  q->op = TCCIR_OP_SHL;
  tcc_ir_set_src2(ir, i, irop_make_imm(ctz64(m)));
  return 1;
}

/* AFTER — declarative DSL (proposed), ~12 lines */
OPT_GEN_SSA(mul_to_shl, TCCIR_OP_MUL) {
  PATTERN(
    .constraints = { .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(is_power_of_two(imm(src2)))
    and(ssa_single_use(ctx, vreg(dest)))
    and_not(ir_q_barrel_shifted(ir, q)));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src2   = imm(ctz64(imm(src2))));         /* shift = log2(multiplier) */
}
```

The example is illustrative, not final syntax; §4 specifies the grammar and §5
shows exactly what the macros expand to.

## §1 Why a DSL?

Three problems the existing codebase has accumulated:

### 1. Pattern repetition costs attention

Every new generator re-derives the same boilerplate:
- Opcode dispatch already handled by the engine, but operand fan-out and
  bounds/NOP handling are not
- Operand unpacking (including the op4 subtleties: MLA accum, indexed scale,
  SELECT cond)
- Guard composition (pure, no side effects, barrel-shift, single-use,
  range-safe)
- Mutation (opcode/operand set through the real setters)
- Signalling change to the engine (`return 1`; the driver bumps the analysis
  generation and re-runs to fixpoint)

The predicate framework (its L1–L5) is designed to eliminate the *guards* and
*operands* problems. What remains is the **pattern-matching + rewrite**
scaffolding that each generator re-derives. The DSL makes that scaffolding a
single include — *once the framework it depends on exists*.

### 2. New passes are expensive to write correctly

The regression-test-first workflow
([debugging_fuzz_divergences.md](debugging_fuzz_divergences.md)) demands that
every fix starts with a failing test. Writing the fix itself requires:
- Knowing the exact operand layout (op4? direct-stack-slot rule? vreg type?)
- Knowing which guards apply (barrel shift? join target? range clobber?)
- Using the right setter (`tcc_ir_set_src1/src2/dest`, opcode assignment)
- Not desyncing engine analyses (return the right change signal so the driver
  re-derives DU / merge / loops on the next generation)

One missing guard is the entire class of bugs this fork keeps fixing (the
memory index lists dozens). The DSL lets you specify *what* you match and
composes the guard vocabulary from the pattern's operand types and op
properties, so a class of guard, once named in the framework, is hard to
forget.

### 3. Target-specific generators are verbose

ARM-specific generators (MLA fusion, indexed memory, disp fusion — see
`opt_gens_fusion.c`) follow the same shape as generic generators but with extra
operand unpacking and ARM-specific guards. The DSL lets them be expressed as
pattern extensions with target-specific guard clauses. The LOC reduction is a
projection, not a measurement (§9).

## §2 Design principles

| Principle | Meaning |
|-----------|---------|
| **C-native** | No code generation step, no external tools, no parser. Pure C expansion macros + `static const` tables. |
| **Composable** | Builds on the predicate framework (L1–L5). A DSL pass *is* a regular `IROptGen` / `IRSSAOptGen` table entry. |
| **Generative, not interpreted** | Macros expand to ordinary imperative C at compile time. There is **no** runtime table-walking match engine — "the match logic is data" describes the *source*, not the *runtime*. This is the design choice that keeps overhead near zero and keeps the output greppable/debuggable. |
| **Observable** | Every match can fire through a `TCC_TRACE_OPT` hook, analogous to the framework's `TCC_TRACE_GUARDS`. |
| **Incremental** | A generator is 100% DSL or 0% DSL. The DSL is opt-in per generator; imperative and DSL generators coexist in the same table. |
| **Self-hostable** | The real language ceiling is *this fork's tcc front-end, not the host gcc*: `ir/opt/` is in the `test-selfhost` bootstrap set (part of `make test`), so the ARMv8-M tcc must parse every construct used here. That pins the DSL to C11 — variadic macros, designated initializers, compound literals; no nested functions, statement expressions, or attributes tcc doesn't parse. C23 is *not* freely available (§10). The good news: the whole design, including the hook alternative (§11.1), needs nothing beyond C11 — `opt_gens_fusion.c` already proves that subset self-hosts. |

## §3 The three layers

```
┌─────────────────────────────────────────────────┐
│  L3 — Pass descriptor                            │  ← opt_pipeline / ssa_opt driver
│  requires · invalidates · fixpoint (existing)    │
├─────────────────────────────────────────────────┤
│  L2 — Generator table entry                      │  ← DSL-emitted IROptGen / IRSSAOptGen
│  {op, dispatch_fn, name, needs_du}               │
├─────────────────────────────────────────────────┤
│  L1 — Pattern grammar (macros)                   │  ← OPT_GEN / PATTERN / GUARD / REWRITE
│  expand to a dispatch function + table entry     │
└─────────────────────────────────────────────────┘
         │              │              │
         ▼              ▼              ▼
   predicate fw    predicate fw   predicate fw
   L2 operands     L4 guards      L5 mutation funnel
   (proposed)      (proposed)     (proposed)
```

All three of the boxes the arrows point to are **proposed, not built**. This
DSL is one layer above an unbuilt floor.

### L1 — Pattern grammar

The DSL vocabulary. `OPT_GEN_SSA` (SSA engine) and `OPT_GEN_FLAT` (pre-SSA
engine) are the two entry macros — the two engines have *different* contexts
(`IRSSAOptCtx` with use-def chains vs. `IROptCtx` with the lazy DU/merge/loops
cache) and different operand-access surfaces, so the target engine must be
named, not inferred (see §10). The macro takes the generator name and the
trigger opcode (matching the `.op` field of the table entry) and opens a
function body:

```c
OPT_GEN_SSA(my_pass, TCCIR_OP_MUL) {
  PATTERN(
    .constraints = { .src1 = IR_CONSTRAINT_IMM,
                     .src2 = IR_CONSTRAINT_VREG });
  GUARD(
    when(ir_op_any(q->op, IROP_P_ALU))
    and(ssa_single_use(ctx, vreg(dest)))
    and_not(ir_q_barrel_shifted(ir, q)));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src1   = vreg(src2),
    .src2   = imm(shift_amount(src1)));
}
```

`PATTERN`, `GUARD`, and `REWRITE` are variadic macros. Variadic capture is
required, not cosmetic: designated-initializer lists contain commas
(`{ .src1 = X, .src2 = Y }`) and the C preprocessor does **not** protect commas
inside braces — only inside parentheses. `#define PATTERN(...)` captures the
whole list as `__VA_ARGS__` and splices it into a compound literal, which is the
only way this parses. §4 and §5 make the expansion explicit.

### L2 — Generator table entry

Each `OPT_GEN_SSA` emits one `IRSSAOptGen` entry, registered with a companion
macro so the table stays a plain `static const` array (the real struct is
`{int op; ssa_gen_fn fn; const char *name;}`; the flat `IROptGen` adds a
`uint8_t needs_du`):

```c
/* Emitted by OPT_GEN_SSA(mul_to_shl, TCCIR_OP_MUL): */
static int opt_dsl_dispatch_mul_to_shl(IRSSAOptCtx *ctx, int i);

const IRSSAOptGen strength_ssa_gens[] = {
  OPT_GEN_ENTRY(mul_to_shl, TCCIR_OP_MUL),   /* → {TCCIR_OP_MUL, opt_dsl_dispatch_mul_to_shl, "mul_to_shl"} */
  /* ... more entries ... */
};
```

The dispatch function is what the macro body expands to. It:
1. Re-checks the trigger opcode (defensive; the engine already dispatches by op)
2. Unpacks operands per the pattern constraints
3. Evaluates the guard expression
4. Applies the rewrite via the real setters
5. Returns 1 (changed) or 0 (no match); the *driver* — not the gen — advances
   the analysis generation and decides re-run

### L3 — Pass descriptor

No new mechanism here — this is the *existing* pipeline. A DSL table plugs into
the current `IROptPass` (pre-SSA) or `IRSSAOptPass` (SSA) exactly as a
hand-written table does. The pre-SSA engine already ships a generic adapter,
`tcc_ir_opt_gen_pass_adapter(IROptCtx*, const IROptGenPassData*)`, that runs an
`IROptGen` table as a pass; DSL tables reuse it unchanged. See §7.

## §4 Pattern grammar — specification

### 4.1 PATTERN

```
PATTERN(
  .constraints = {                         /* optional: per-slot constraints */
    .dest  = IR_CONSTRAINT_*,              /* VREG, IMM, STACKOFF, LVAL, ANY */
    .src1  = IR_CONSTRAINT_*,
    .src2  = IR_CONSTRAINT_*,
    .op4   = IR_CONSTRAINT_*,              /* MLA accum, indexed scale, SELECT cond */
  },
  .range = {                               /* optional: range-scoped pattern */
    .lo = IR_RANGE_PREV_INSTR,
    .hi = IR_RANGE_NEXT_INSTR,
    .stop_ops = IROP_M_*,                  /* stop set (framework L1 masks) */
    .flags = IR_RANGE_*,
  },
  .pair = {                                /* optional: two-instruction pattern */
    .second = { .op = TCCIR_OP_*, .link = IR_PAIR_DEST_TO_SRC1 },
    .adjacent = 1,                         /* adjacent, no join target between */
    .no_clobber = IROP_M_*,               /* no clobbering ops between */
  },
);
```

The trigger opcode is the `OPT_GEN_*(name, OP)` argument, so `.op` does not
appear inside `PATTERN`. The `.pair` link is an *explicit relation*
(`dest_of_first == src1_of_second`), not a raw slot index — slot indices were
ambiguous in the earlier draft.

Operand access is by the framework's L2 iterator, not a fictional
`irop_get_operand(ir, q, k)`: the real primitives are `tcc_ir_op_get_src1/src2`,
`tcc_ir_op_get_dest`, `irop_get_vreg`, `irop_get_imm64_ex`, and (proposed)
`ir_q_operands()` which advertises op4 by construction. The `.constraints`
fields compile to checks over those primitives:

- `IR_CONSTRAINT_IMM` → `irop_is_immediate(op)`
- `IR_CONSTRAINT_VREG` → `irop_get_vreg(op) >= 0`
- `IR_CONSTRAINT_STACKOFF` → `irop_is_direct_stack_slot(op)` *(framework L2)*
- `IR_CONSTRAINT_ANY` → no check

There are **no** pre-declared "operand layout" constants. The earlier draft's
`IR_LAYOUT_MUL`/`IR_LAYOUT_SHL` were redundant with the constraint list and the
framework's op-property table, which already knows each opcode's arity and slot
roles. Drop them.

### 4.2 GUARD

Composes with the framework's guard DSL (its L4). `when(...)` opens the chain
and yields a single boolean the dispatch function tests; `and`/`and_not` fold
`&&`/`&& !` onto it:

```c
GUARD(
  when(ir_op_any(q->op, IROP_P_ALU))
  and(ssa_single_use(ctx, vreg(dest)))
  and_not(ir_q_barrel_shifted(ir, q))
  and(ir_range_ok_simple(ir, def_idx, use_idx, IROP_M_CLOBBERS_MEM,
                         IR_RANGE_NO_JUMP_TARGET)));
```

Constraint checks from `PATTERN.constraints` are folded into the guard
automatically by the expansion — there is no separate `GUARD_AUTO`. Inside the
guard body, `dest`, `src1`, `src2`, `op4` are the unpacked `IROperand`s from the
pattern, and `vreg(x)` / `imm(x)` are convenience accessors over them.

### 4.3 REWRITE

```c
REWRITE(
  .new_op = TCCIR_OP_SHL,                    /* new opcode (omit to keep same) */
  .dest   = vreg(dest),                      /* forward matched dest */
  .src1   = vreg(src2),                      /* swap src1 ← matched src2 */
  .src2   = imm(shift_amount(src1)),         /* computed immediate */
  .delete_second = 1,                        /* for .pair: NOP-out the second instr */
);
```

Three rewrite modes, each expanding to the real mutation API:
- **Replace** — assign `q->op` and call `tcc_ir_set_src1/src2/dest` (most common)
- **Delete** — set the instruction (or the paired one) to `TCCIR_OP_NOP`; the
  engine compacts NOPs later
- **Expand** — insert extra instructions through the framework's L5 mutation
  funnel (the funnel is what remaps jump *and* switch-table targets — see
  regression test 268; a raw insert helper is exactly the bug that funnel
  prevents)

Bindings (`dest`, `src1`, `src2`, `op4`) are the fixed names of the unpacked
operands; there are no user-chosen binding identifiers (the earlier draft's
`.shift_amount` / `.add_src` invented names with no defined resolution rule).
Computed values use plain C expressions over the bindings.

## §5 What the macros expand to

`OPT_GEN_SSA(mul_to_shl, TCCIR_OP_MUL) { PATTERN(...); GUARD(...); REWRITE(...); }`
expands to a single dispatch function. This is ordinary C — inspectable with
`gcc -E`, steppable in gdb, greppable by name:

```c
static int opt_dsl_dispatch_mul_to_shl(IRSSAOptCtx *ctx, int i)
{
  TCCIRState *ir = ctx->ir;
  IRQuadCompact *q = &ir->compact_instructions[i];

  /* trigger opcode (defensive; engine already dispatches on it) */
  if (q->op != TCCIR_OP_MUL)
    return 0;

  /* operand unpack (PATTERN → framework L2 accessors) */
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  IROperand src1 = tcc_ir_op_get_src1(ir, q);
  IROperand src2 = tcc_ir_op_get_src2(ir, q);

  /* constraints (PATTERN.constraints) folded into the guard */
  if (!(irop_get_vreg(src1) >= 0))     return 0;   /* .src1 = VREG */
  if (!(irop_is_immediate(src2)))      return 0;   /* .src2 = IMM  */

  /* guard (GUARD → framework L4) */
  if (!( ir_op_any(q->op, IROP_P_ALU)
         && ssa_single_use(ctx, irop_get_vreg(dest))
         && !ir_q_barrel_shifted(ir, q) ))
    return 0;

  /* rewrite (REWRITE → real setters) */
  q->op = TCCIR_OP_SHL;
  tcc_ir_set_src1(ir, i, irop_get_vreg(src2));
  tcc_ir_set_src2(ir, i, irop_make_imm(ctz64(irop_get_imm64_ex(ir, src2))));
  return 1;   /* driver advances the analysis generation and re-runs */
}
```

The key point: **the match code is expanded from the pattern, not walked from a
table at runtime.** There is no generic interpreter to slow down; the DSL is a
source-level convenience that emits the same code a careful author would write.
The "match engine" is the set of shared framework helpers the expansion calls,
not a per-instruction dispatcher.

### 5.1 Cost — projected, not measured

**Nothing here is implemented, so nothing has been benchmarked.** The earlier
draft's table of "+3 cycles/instr" figures and per-pass DSL LOC was fabricated;
it has been removed. What can be reasoned about:

- **Runtime:** because the macros expand to the same accessor/guard/setter calls
  a hand-written generator uses, the *expected* per-instruction cost is
  within noise of the imperative version. This is a claim to *verify*, not a
  result: the gate (§13) requires a `TCC_PASS_TIMING` corpus run showing no
  regression before any migration proceeds.
- **Source size:** a generator like `ir_gen_bool_idempotent` (~30 lines
  including operand unpack and logging) would shrink to roughly a `PATTERN` +
  `GUARD` + `REWRITE` triple. The realized ratio must be measured on the first
  two conversions, not asserted.

## §6 Target-specific extensions

ARM generators express the same shape with target-specific guards. Using the
real fusion pattern (MUL feeding an ADD → MLA), the `.pair` link is the
dest→src relation, and the delete is a NOP-out of the ADD:

```c
OPT_GEN_SSA(arm_fuse_mul_add_to_mla, TCCIR_OP_MUL) {
  PATTERN(
    .constraints = { .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_VREG },
    .pair = { .second = { .op = TCCIR_OP_ADD,
                          .link = IR_PAIR_DEST_TO_SRC1 },
              .adjacent = 1 });
  GUARD(
    when(ir_op_any(q->op, IROP_P_ALU))
    and(ir_op_any(second->op, IROP_P_ALU))
    and(ssa_single_use(ctx, vreg(dest))));      /* MUL result feeds only the ADD */
  REWRITE(
    .new_op = TCCIR_OP_MLA,
    .op4    = vreg_second(src2),                 /* accumulator = ADD's other src */
    .delete_second = 1);
}
```

Because op4 (the MLA accumulator) is a documented source of missing-guard bugs
(regression tests 257, 267, 285, 9494), routing it through the framework's op4
accessor is the whole point: the DSL cannot express an MLA rewrite that leaves
op4 invisible to later use/def scans, because the accessor advertises it by
construction.

Target-specific *guards* are plain framework predicates named in the `GUARD`
clause. The earlier draft's `opt_dsl_target_can_fuse` / `_arm` override hooks
are dropped — there is no dispatch mechanism to select an override at compile
time without either a build-time target macro (which the file already has) or a
runtime indirection (which contradicts §2's zero-overhead claim). Since this
fork targets exactly one architecture, a `#if` on the target macro, or simply
putting ARM generators in ARM files, is sufficient.

## §7 Pipeline integration

DSL tables are ordinary generator tables; they need **no new pipeline
mechanism**.

### 7.1 Pre-SSA passes

Reuse the existing adapter:

```c
static const IROptGenPassData strength_reduction_data = {
  .gens  = strength_gens,
  .count = ARRAY_LEN(strength_gens),
};
/* run via tcc_ir_opt_gen_pass_adapter(ctx, &strength_reduction_data) */
```

Register it in the `IROptPass` pipeline the same way the existing gen-table
passes (fusion, bool, branch) are registered.

### 7.2 SSA passes

An `IRSSAOptPass` carries a `gens` table directly:

```c
static const IRSSAOptPass ssa_pass_strength = {
  .name      = "ssa:strength",
  .run       = NULL,               /* NULL → driver runs the gens table */
  .gens      = strength_ssa_gens,
  .gen_count = ARRAY_LEN(strength_ssa_gens),
};
```

This matches the real `IRSSAOptPass` struct (`{name, run, gens, gen_count}`).

### 7.3 Fixpoint / cascade

Fixpoint is a property of the *driver*, not of the generator. The SSA driver
already re-runs `gens` tables to convergence and the pre-SSA cascade wrappers
already iterate. DSL tables inherit this for free — there is no new `.fixpoint`
field to invent on the generator, and the earlier draft's `OPT_CASCADE` /
`OPT_PASS` structs are unnecessary: the existing pipeline data structures
already carry `requires`/`invalidates` and cascade sequencing.

## §8 Migration path

Additive and reversible. **Gated behind the predicate framework** (§13) — the
DSL has nothing to expand onto until L1/L2/L4/L5 exist.

| Step | Action | Risk |
|------|--------|------|
| 0 | Land framework L1/L2/L4/L5 (separate effort) | — (prerequisite) |
| 1 | Add `ir/opt_dsl.h` (macros only; no runtime engine) | Low — header-only |
| 2 | Convert 1–2 trivial generators (`bool_idempotent`, one fold rule) | Low — proves the expansion |
| 3 | Measure: `gcc -E` output, `make test -j16`, `TCC_PASS_TIMING` corpus | Low — this is the go/no-go for the LOC and cost claims |
| 4 | Convert 5–10 medium generators (branch, fold, cprop rules) | Medium — validates on harder cases |
| 5 | Convert ARM fusion generators | Medium — validates op4 / pair handling |
| 6 | Remaining generators opt-in, each independently reversible | Low |

**Never convert all generators at once.** DSL and imperative generators coexist
in the same table. The gate for each step: `make test -j16` green + the fuzz
profiles for the touched op classes swept clean (per
[the memory-index convention](../CLAUDE.md); the user runs the sweeps).

## §9 What this deletes (and adds) — estimates

These are **projections from the framework's own counts** (~300 `tcc_ir_opt_*`
functions + 15 SSA passes; ~82 invalidation sites; op4 at 110 sites), not
measurements. They should be replaced with real deltas after step 3.

| Consolidation | Basis | ≈ LOC out | DSL LOC in |
|---|---|---:|---:|
| Per-generator operand unpack + bounds/NOP | generators × ~8 lines | −est. | +macros |
| Per-generator guard composition | generators × ~5 lines | −est. | +0 (reuses framework L4) |
| Per-generator mutation + change signal | generators × ~5 lines | −est. | +macros |
| ARM generator verbosity | ~8 fusion gens | −est. | +0 (reuses framework) |
| **Net** | | **projected negative** | **~1 header** |

> **Honest framing.** The DSL removes *repetitive* generator scaffolding. It
> does nothing for the passes that are not generator-shaped: complex fixpoint
> loops and stateful passes (`opt_memory.c`'s phase-structured entry-store
> machine, the loop passes) stay as C. The DSL is a convenience layer for the
> common case, not a rewrite of the optimizer. Its entire value depends on the
> predicate framework landing first; on its own it is a thin sugar over an API
> that does not yet exist.

## §10 Risks & open questions

| Risk / question | Position |
|---|---|
| **Unbuilt foundation.** Every guard/operand helper is from an unimplemented framework proposal. | This is the top risk. The DSL is *gated* on the framework (§8 step 0, §13). Do not start the DSL first. |
| **Two engines, two contexts.** Flat `IROptCtx` (lazy DU/merge/loops cache) and SSA `IRSSAOptCtx` (use-def chains) expose *different* operand and analysis APIs. | Resolved by having two entry macros (`OPT_GEN_FLAT`, `OPT_GEN_SSA`). A single `OPT_GEN` cannot compile to both, because the guard vocabulary available differs (e.g. `ssa_single_use` exists only in the SSA context). The earlier draft's "compiles to either depending on context" was hand-waving. |
| **Generative vs interpreted confusion.** The earlier draft claimed both "data-driven table" and "generated dispatch function". | Committed to generative (§2, §5): macros expand to C. No runtime interpreter. This is what keeps overhead near zero and output debuggable. |
| **Macro comma pitfall.** Designated-initializer lists as macro args split on internal commas. | `PATTERN`/`GUARD`/`REWRITE` are variadic (`__VA_ARGS__`) and splice into compound literals. Verified feasible; §3/§4 note it. |
| **Debuggability.** | Expanded code is plain C: `gcc -E` shows it, gdb steps it, the dispatch fn has a real name. Optional `TCC_TRACE_OPT` mirrors the framework's guard trace. |
| **Performance.** | *Claim, unverified.* Expected within noise since expansion emits the same calls. Gated on a `TCC_PASS_TIMING` corpus run (§8 step 3). |
| **Expressiveness ceiling.** Multi-instruction, stateful, or non-local rewrites don't fit. | Explicitly out of scope. Those stay imperative. The DSL covers single- and paired-instruction peepholes only. The worked example ([plan/opt_dsl_worked_example_mla_fusion.md](plan/opt_dsl_worked_example_mla_fusion.md)) shows MLA fusion is already past this line. |
| **C23 won't rescue the macros.** Could `constexpr` / `[[nodiscard]]` / `typeof` / `nullptr` simplify the DSL? | Mostly no, and not for free. The self-host gate compiles `ir/opt/` with the fork's own tcc, so every C23 feature is a *front-end feature you must first implement in tccgen* + a self-host regression risk — the cost is doubled, not "clang accepts it." Only `[[nodiscard]]` on the change-signal return has real payoff, and it should be its own deliberate front-end feature, not a DSL convenience. The design needs no C23. |
| **Macros vs. hooks.** Is a generative macro layer even the right shape? | Open, and leaning *no* for guard-heavy passes. See §11.1: a function-pointer "manipulator library + hooks" delivers the same op4-safety win, stays debuggable, needs no guard-DSL macros, and matches house style (`IROptGen` is already a vtable). Macros may be worth it only for truly trivial one-liner peepholes. |

## §11 Comparison with alternatives

| Approach | Pros | Cons |
|----------|------|------|
| **Current: imperative C** | Full power, zero abstraction | Repetitive; guards re-derived per generator; easy to forget one |
| **Predicate framework alone (L1–L5)** | Ends the guard/operand/mutation folklore; each fix lands centrally | Doesn't reduce the pattern-match/rewrite scaffolding |
| **This DSL (macro layer, on top of the framework)** | Declarative, composable, generative (no overhead), observable | Thin, but stacks on an unbuilt floor; only helps generator-shaped passes; can't author the hard guards, only chain them |
| **Manipulator library + hooks** (§11.1) | Same op4/rewrite safety; debuggable; C11-only, self-host-clean today; no guard-DSL dependency | Runtime indirection per hook; still can't express the genuinely stateful passes |
| **External DSL (Bison/custom parser)** | Clean separation | Not self-hostable; build dependency; breaks the C-native ethos |
| **X-macro tables** | Data-driven, no codegen | Can't express guard composition; limited |
| **C++ templates/constexpr** | Compile-time evaluation | Breaks C11; not self-hostable by tcc |

### 11.1 The leading alternative: manipulator library + hooks

The worked example ([plan/opt_dsl_worked_example_mla_fusion.md](plan/opt_dsl_worked_example_mla_fusion.md))
established that the macro DSL captures only the *mechanical* layer of a real
pass (operand fan-out, the op4/pool rewrite) and can merely *chain* the
correctness-critical guards, which stay hand-written C. That finding points at a
different shape that may be preferable for everything except trivial peepholes.

**Key realization: the engine is already a vtable.** `IROptGen` /
`IRSSAOptGen` are `{trigger_op, method_fn, name}` — a flat function-pointer
table dispatched by a driver, i.e. "simple OOP in C," the same pattern the
`thop_*` builders and codegen `_mop` handlers already use. So "add function
pointers for manipulators" is not a new mechanism; the question is only *what to
parameterize*. Instead of expanding a monolith from macros, provide:

1. **A library of composable manipulator functions** — reusable match / guard /
   rewrite building blocks. This is exactly the predicate framework's L2–L5
   helpers; nothing new to invent.
2. **A few generic parameterized passes** that carry the shared invariants (op4
   growth, pool move, invalidation) and take function-pointer hooks for the part
   that varies:

```c
typedef struct {
  int       trigger_op;        /* e.g. TCCIR_OP_ADD */
  uint32_t  first_op_class;    /* IROP_P_MLA_MUL */
  uint8_t   link;              /* IR_PAIR_DEF_OF_EITHER_SRC */
  int  (*guard)(IROptCtx *, const FusePairState *);   /* NULL = always fuse */
  void (*rewrite)(IROptCtx *, const FusePairState *); /* shared op4/pool move */
} FusePairSpec;

int ir_gen_fuse_pair(IROptCtx *ctx, int i, const FusePairSpec *spec);
```

Adding a fusion becomes: fill a spec, write a `guard` if the default doesn't
cover you. The op4-safe rewrite lives *once*. Every hook is breakpointable —
strictly more debuggable than macro expansion.

**Why this may win over the macro layer:**

- Delivers the same op4-safety benefit (the shared `rewrite` owns the pool move).
- Needs **no** guard-DSL macros, so it does not depend on the `when()/and()`
  layer of the framework — it can start as soon as L2/L5 helpers exist.
- C11-only and self-host-clean *today* — `opt_gens_fusion.c` already ships this
  exact style through the `test-selfhost` gate.
- Matches house style; no `gcc -E` archaeology.

**Trade-offs, honestly:**

- **Indirection cost.** 2–3 indirect calls per candidate where a monolith
  inlines. Not a new cost class — the engine already pays one indirect call per
  gen per instruction — but the hot passes (`opt_dce`, `opt_memory`) should stay
  monolithic. This is a per-pass choice, which argues against forcing everything
  through *either* macros or hooks.
- **Keep the "OOP" flat.** A spec struct with a few `NULL`-able hooks is the
  sweet spot; no inheritance or base-class chains for ~300 leaf passes that
  share almost nothing.
- **Same ceiling.** MLA fusion still overflows — its 64-bit store-back triple
  and conditional MUL-operand rewrite thread a fat state struct the monolith
  gets for free. Hooks don't fix that; the genuinely hard passes stay
  hand-written under either design.

**Working position:** prefer the manipulator library + hooks over the macro DSL,
not alongside it. Reserve the macro layer (if built at all) for one-liner
peepholes where there is no guard to author. This makes "drop to a C guard" the
*normal* path rather than an escape hatch — which the worked example shows is
where real passes actually live.

## §12 Appendix: full example (proposed syntax)

Strength reduction, two rules, plugged into the SSA pipeline. Uses only the
constructs specified above:

```c
/* ir/opt_gens_strength.c — strength reduction (proposed) */
#include "ir.h"
#include "opt/ssa_opt.h"
#include "opt_dsl.h"

/* MUL(a, 2^N) → SHL(a, N) */
OPT_GEN_SSA(strength_mul_power2, TCCIR_OP_MUL) {
  PATTERN(
    .constraints = { .dest = IR_CONSTRAINT_VREG,
                     .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_IMM });
  GUARD(
    when(ir_op_any(q->op, IROP_P_ALU))
    and(is_power_of_two(imm(src2))));
  REWRITE(
    .new_op = TCCIR_OP_SHL,
    .src2   = imm(ctz64(imm(src2))));
}

/* ADD(x, y) where y is defined by SHL → fold the shift into the ADD */
OPT_GEN_SSA(strength_add_shift, TCCIR_OP_ADD) {
  PATTERN(
    .constraints = { .dest = IR_CONSTRAINT_VREG,
                     .src1 = IR_CONSTRAINT_VREG,
                     .src2 = IR_CONSTRAINT_VREG },
    .pair = { .second = { .op = TCCIR_OP_SHL,
                          .link = IR_PAIR_DEF_OF_SRC2 },
              .adjacent = 1 });
  GUARD(
    when(ir_op_any(q->op, IROP_P_ALU))
    and(ssa_single_use(ctx, vreg_second(dest))));
  REWRITE(
    .new_op        = TCCIR_OP_ADD_SHIFTED,   /* barrel-shifted ADD */
    .op4           = vreg_second(src2),
    .delete_second = 1);
}

const IRSSAOptGen strength_ssa_gens[] = {
  OPT_GEN_ENTRY(strength_mul_power2, TCCIR_OP_MUL),
  OPT_GEN_ENTRY(strength_add_shift,  TCCIR_OP_ADD),
};
const int strength_ssa_gens_count =
  sizeof(strength_ssa_gens) / sizeof(strength_ssa_gens[0]);
```

This expands (per §5) to two dispatch functions and a two-entry `IRSSAOptGen`
table — exactly the shape `bool_gens[]` in `ir/opt_gens_bool.c` has today. The
*intent* is declarative; the *output* is the same code the engine already runs.

## §13 Dependency gate (summary)

The DSL is buildable only after the predicate framework provides, at minimum:

- **L1** — the op-property table + named masks (`IROP_P_ALU`, `IROP_M_*`,
  `ir_op_any`)
- **L2** — the operand iterator advertising op4 (`ir_q_operands`), and the
  slot accessors (`irop_is_direct_stack_slot`, `ir_q_barrel_shifted`)
- **L4** — the guard DSL (`when`/`and`/`and_not`) and `ssa_single_use` /
  range predicates
- **L5** — the mutation funnel (for `Expand`-mode inserts that must remap
  jump and switch-table targets)

Until those exist in-tree, this document is a design target, not a work item.

---

*This document proposes a declarative, generative macro layer on top of the
existing generator model. It builds entirely on the (unimplemented) predicate
framework ([plan_opt_predicate_framework.md](plan_opt_predicate_framework.md))
for guard composition, operand access, and mutation safety. The DSL is opt-in,
incrementally adoptable, C11-compatible, and — critically — cannot be started
before its foundation lands.*
