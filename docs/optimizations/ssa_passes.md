# SSA-Phase Optimizations

After flat-IR loop transforms and CFG/SSA construction, the SSA optimization
engine runs on the SSA-form IR with use-def chains.

## Pipeline Position

```
Flat IR loop transforms → CFG + dominators → SSA construction → SSA rename
→ SSA optimization passes → Phi resolution → Register allocation
```

The SSA engine is driven by `tcc_ir_ssa_opt_run()` in `source/opt/ssa/engine/driver.c`.
It iterates up to 5 times until convergence.

## Pass Order ( SSA_RUN macro)

There are two SSA optimization drivers, selected per function by whether any VAR
was promoted to SSA/phi form:
- **Promoted path** (`had_promotable != 0`) → `tcc_ir_ssa_opt_run` (source/opt/ssa/engine/driver.c),
  the order below. Runs the broad `ssa:var_to_param_forward`.
- **Fallback path** (nothing promotable) → the `RUN_SSA` loop in ir/regalloc.c, a
  similar order but with `ssa:var_forward` (narrow) in place of
  `ssa:var_to_param_forward`. `ssa_opt_vinfo`/rename are not populated here.

```c
ssa:var_const_fold
ssa:sccp
ssa:cprop
ssa:var_to_param_forward   (promoted path; fallback runs ssa:var_forward instead)
ssa:fold
ssa:cprop              (second pass)
ssa:var_imm_prop
ssa:load_cse
ssa:branch
ssa:cmp_eq_prop
ssa:reassoc
ssa:strength
ssa:narrow
ssa:gvn
ssa:phi_simplify
ssa:dead_loop          (O2 only)
ssa:dce
[target-specific gens]
```

Plus post-loop:
```
ssa:guard_collapse     (fixpoint: load_cse → cprop → fold → branch → DCE)
```

## Pass Descriptions

### ssa:var_const_fold
Fold VAR reads that are statically known to hold a constant. Resolves
LEA/ASSIGN chains to stack offsets and folds constant values.

### ssa:sccp
Sparse Conditional Constant Propagation. Propagates constants through
conditional branches using a lattice (TOP → constant values → BOTTOM).

### ssa:cprop
Copy propagation. Propagates single-def copies through the IR, replacing
uses of the destination with the source. Runs twice to handle transitive
copies.

### ssa:var_forward  (fallback path)
`ssa_opt_var_forward` (source/opt/ssa/scalar/cprop.c). Forwards a single-def,
non-address-taken VAR into its reload-copy uses only — `Ty <- Vn [ASSIGN|LOAD]`
becomes `Ty <- stored_val [ASSIGN]`, per-use, dominance + call-barrier guarded,
leaving the dead def for DCE. Does NOT reach direct arithmetic/compare/param
operand consumers (that residual is what the flat `var_tmp_fwd` still covers on
fresh pre-SSA IR; see plan_legacy_flat_ir_ssa_retire.md).

### ssa:var_to_param_forward  (promoted path)
`ssa_opt_var_to_param_forward` (same file). The broad forwarder: a single-def,
non-address-taken VAR whose def stores a safe value is forwarded into ALL its
dominated value uses (any operand position, via `ssa_op_reads_vreg`), then the
def is NOPed. All-or-nothing per VAR (bails if any use is undominated, crosses a
call barrier, or carries a pinned barrel-shift). Name is historical — it
generalized well beyond FUNCPARAMVAL sites.

### ssa:fold
Arithmetic/logic folding. Simplifies expressions like `x & 0xFF`,
`x | 0xFF`, `x ^ x`, constant arithmetic, etc.

### ssa:var_imm_prop
Variable immediate propagation. Propagates immediate values through
variables that are defined by constant assignments.

### ssa:load_cse
Load common subexpression elimination. Tracks memory reads through
stack addresses and folds duplicate loads. Uses `ssa_opt_resolve_lea_stackloc`
and `ssa_opt_resolve_temp_to_base_off` to canonicalize addresses.

### ssa:branch
Branch folding. Folds dead branches, simplifies conditional jumps,
and removes unreachable code paths.

### ssa:cmp_eq_prop
CMP equality-fact propagation. Walks the dominance tree pushing equality
facts from CMP+JEQ/JNE pairs and folds redundant compares whose result
is already known on the dominated path.

### ssa:reassoc
Addition reassociation. Reorders additions to enable further folding
(e.g. `(a + b) + (-b) → a`).

### ssa:strength
Strength reduction. Replaces expensive operations with cheaper ones
(e.g. multiplication by power-of-2 → shift).

### ssa:narrow
Type narrowing. Converts wider operations to narrower ones when the
result fits in a smaller type (e.g. 64-bit → 32-bit when value fits).

### ssa:gvn
Global Value Numbering. Identifies and eliminates global common
subexpressions across basic blocks.

### ssa:phi_simplify
Phi node simplification. The local rule eliminates phis whose defined,
non-self operands are one value. A structural Tarjan pass also collapses
mutually recursive phi components when they have exactly one external defined
value and compatible types. Group replacement is preflighted atomically, so a
protected use on any member preserves the whole component. All-self,
all-undefined, and multi-value components remain for later passes. A congruent
pass then merges phis in the same block that carry identical
`(pred_block -> vreg)` operand maps and type, keeping the first as
representative; operand matching is by predecessor edge, not slot order, and
uses exact vreg equality only. The three transforms iterate to a joint fixed
point, since each can expose new opportunities for the others.

Build with `TCC_LOG_IR_GEN=1` to report phis and operands before each
simplification run, phis afterward, and trivial/SCC/congruent removals.

Build with `TCC_LOG_LS=1` for out-of-SSA and allocation statistics:

- phi operands reaching out-of-SSA conversion;
- parallel copies requested and emitted;
- cycle-breaking temporaries;
- spills among intervals participating in phi copies;
- copies coalesced and copies still requiring code generation.

The counters compile away with their logging scope disabled and produce no
unconditional output.

### ssa:dead_loop (O2 only)
Dead loop elimination. Collapses side-effect-free counting loops whose
result is a loop-invariant constant into a guarded constant expression.

### ssa:dce
Dead code elimination. Removes instructions whose results are unused.
Maintains a worklist of dead TEMPs.

### ssa:guard_collapse
Fixpoint pass that peels sequential constant-guard chains one folded
branch per round. Pattern: `STORE; CMP; JUMPIF; call abort; LOAD ...`.
Runs: load_cse → cprop → fold → branch → DCE in a loop.

## Loop Transforms (Flat IR, Before SSA)

These run on flat IR immediately before CFG/SSA construction in
`ir/regalloc.c`:

| Pass | Gate | Description |
|------|------|-------------|
| `ssa:cfg_cleanup` | `opt_jump_threading && -O1+` | Jump chain threading, fall-through elimination, orphan flag removal |
| `ssa:struct_copy_roundtrip` | `opt_redundant_store` | Drop memmove(A,B);memmove(B,A) pairs |
| `ssa:or_bool_diamond` | `opt_const_prop` | Fold `acc \|= (cond ? 1 : 0)` into per-arm ORs |
| `ssa:stack_addr_simplify` | `opt_const_prop` | Deref-of-known-stack-addr → direct StackLoc |
| `ssa:reroll` | `opt_reroll` | Re-roll N identical consecutive blocks into a counted loop |
| `ssa:licm` | `opt_licm` | Loop-invariant code motion |
| `ssa:loop_rotate` | `-O1+` | Convert top-tested loops to bottom-tested |
| `ssa:first_iter_exit` | `-O1+ && opt_const_prop` | Peel loops whose exit is provably true on first entry |
| `ssa:ptr_iv_exit_subst` | `-O1+ && opt_const_prop` | Substitute pointer IV exit values with closed-form expressions |
| `ssa:loop_const_sim` | `opt_loop_unroll` | Execute register-only loops on host, emit residual final values |
| `ssa:loop_unroll` | `opt_loop_unroll` | Fully unroll or closed-form-eliminate constant-trip loops |
| `ssa:iv_strength_reduction` | `opt_iv_strength_red` | Transform array-indexing recurrences into stride pointers |
| `ssa:decrement_to_zero` | `-O1+` | Rewrite count-up loops to count-down-to-zero for SUBS fusion |

## Target-Specific Generators (ARM)

Registered via `tcc_ir_ssa_opt_register_target()` from
`arch/arm/ssa_opt_arm.c`:

| Generator | Description |
|-----------|-------------|
| `arm_fuse_mul_add_to_mla` | `MUL(a,b); ADD(t1,c) → MLA(a,b,c)` (1 cycle vs 2) |
| `arm_fuse_shl_add_to_load_indexed` | `SHL; ADD; LOAD → LOAD_INDEXED` |
| `arm_fuse_shl_add_to_store_indexed` | `SHL; ADD; STORE → STORE_INDEXED` |
| `arm_reduce_mul_to_shift` | `MUL(a, 2^N) → SHL(a, N)` |
| `arm_fuse_load_through_add_imm` | `ADD(base, imm); LOAD → LOAD (base, #imm)` |
| `arm_fuse_store_through_add_imm` | `ADD(base, imm); STORE → STORE (base, #imm)` |
| `arm_fuse_mla_accum_through_add_imm` | `ADD(accum, imm); MLA → MLA (accum, #imm)` |
| `arm_fuse_store_src_through_add_imm` | `ADD(src, imm); STORE → STORE (src, #imm)` |

## SSA Engine Internals

### Use-Def Chains (`IRSSAVregInfo`)
Each TEMP vreg tracks:
- `def_instr`: defining instruction index (-1 for phi/entry)
- `def_phi_block`: phi block (-1 if not phi)
- `def_count`: number of definitions (>1 means non-SSA multi-def)
- `uses[]`: list of uses (instruction indices and phi operand slots)

### Address Resolution
- `ssa_opt_resolve_lea_stackloc()`: Resolve TEMP → Addr[StackLoc[N]]
- `ssa_opt_resolve_temp_to_base_off()`: Resolve TEMP → (base_vr, offset)
- `ssa_opt_indirect_stack_offset()`: Resolve STORE/LOAD effective stack offset

### Variable Promotion
SSA promotion decides which VARs become TEMP (promotable):
- Single-block CFG: all non-addrtaken VARs are promotable
- Multi-block CFG: VARs defined in ≥2 blocks, or single-def VARs whose
  def-block has a non-empty dominance frontier

### Phi Placement
Uses the classic dominator-frontier algorithm:
1. For each promotable VAR, find its definition blocks
2. Seed worklist with def blocks
3. For each block in worklist, compute dominance frontier
4. Place phi at each DF block (if not already there)
5. Add new DF blocks to worklist
