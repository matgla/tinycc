# SSA Conversion Plan

## Goal

Insert a mandatory SSA (Static Single Assignment) construction pass between IR generation and optimization. The current `ir/opt.c` will be rewritten against SSA form. This document covers only the SSA infrastructure — no new optimizations yet.

## Current IR Summary

- Flat array of `IRQuadCompact` instructions
- Three vreg namespaces: VAR (locals), TEMP (compiler-generated), PARAM (function args)
- VARs can be assigned multiple times (not SSA)
- TEMPs are mostly single-def but not enforced
- Basic block boundaries are implicit: instructions following a JUMP/JUMPIF target (`is_jump_target` flag) start a new block
- No explicit CFG data structure — passes scan linearly and track jump targets
- Operands stored in a pool indexed by `operand_base`

## Design

### Phase 1: CFG Construction

Build an explicit control flow graph from the flat instruction stream.

**Data structures:**

```c
typedef struct IRBasicBlock {
  int start_idx;          /* first instruction index (inclusive) */
  int end_idx;            /* last instruction index (inclusive) */
  int id;                 /* block index */

  int *preds;             /* predecessor block IDs */
  int nb_preds;
  int *succs;             /* successor block IDs */
  int nb_succs;

  int idom;              /* immediate dominator block ID */
  int *dom_frontier;     /* dominance frontier set */
  int nb_dom_frontier;
  int *dom_children;     /* children in dominator tree */
  int nb_dom_children;
} IRBasicBlock;
```

**Algorithm:**
1. Scan instruction array; every `is_jump_target` or instruction following a JUMP/JUMPIF/RETURNVALUE/RETURNVOID starts a new block
2. Build successor edges: JUMP → target block, JUMPIF → target + fallthrough, RETURN → (none), IJUMP → all possible targets
3. Build predecessor edges (reverse of successors)

**File:** `ir/cfg.c`

### Phase 2: Dominator Tree

Compute immediate dominators using the Cooper-Harvey-Kennedy algorithm (simple iterative, efficient for reducible CFGs which TCC always produces).

**Algorithm:** "A Simple, Fast Dominance Algorithm" (Keith D. Cooper, Timothy J. Harvey, Ken Kennedy, 2001)

1. Initialize idom[entry] = entry, all others undefined
2. Iterate in reverse postorder until fixed point:
   - For each block b (except entry), idom[b] = intersect(idom of all preds)
3. Compute dominance frontier from idom tree

**File:** `ir/cfg.c` (same file, closely coupled with CFG)

### Phase 3: SSA Construction

Convert VARs and TEMPs into SSA form using the standard algorithm:

1. **Phi placement** (iterated dominance frontier):
   - For each variable v, find all blocks that define v
   - Place phi nodes at the dominance frontier of those blocks
   - Iterate until no new phis are added

2. **Renaming** (dominator tree walk):
   - Walk dominator tree in preorder
   - Maintain a rename stack per variable
   - At each use: replace vreg with current SSA name from stack
   - At each def: push new SSA name onto stack
   - At each phi in successor: fill the phi operand for this edge

**Phi node representation:**

```c
typedef struct IRPhiNode {
  int32_t dest_vreg;       /* SSA vreg being defined */
  int nb_operands;
  struct {
    int32_t vreg;          /* SSA vreg from this predecessor */
    int pred_block_id;     /* which predecessor edge */
  } *operands;
} IRPhiNode;
```

Phi nodes are stored per-block (array at the top of each `IRBasicBlock`), not as regular instructions. This avoids disturbing the compact instruction array.

**What gets SSA-renamed:**
- VAR vregs (locals) — these are the primary multi-def case
- TEMP vregs — already mostly single-def, but SSA enforces it
- PARAM vregs — treated as a single def at function entry

**What does NOT get SSA-renamed:**
- StackLoc stores/loads (memory operations through pointers)
- Global symbol references
- Immediate constants

**File:** `ir/ssa.c`

### Phase 4: SSA Destruction (before regalloc)

Convert out of SSA form for the register allocator (`tccls.c`) which expects the current flat IR format.

**Algorithm:** naive phi elimination (sufficient for now, can optimize later with copy coalescing):

1. For each phi node `v_i = phi(v_a, v_b, ...)`:
   - Insert `ASSIGN v_i ← v_a` at end of predecessor block for edge a
   - Insert `ASSIGN v_i ← v_b` at end of predecessor block for edge b
2. Remove all phi nodes
3. Flatten CFG back to linear instruction array

Lost-copy and swap problems are rare in practice with linear scan; can add parallel-copy resolution later if needed.

**File:** `ir/ssa.c` (destruction is the inverse of construction)

## Integration Points

### Pipeline position

Current pipeline at -O1+ (SSA regalloc is default):
```
tccgen.c (IR emission)
  → ir/opt.c: pre-SSA optimizations (iterative loop)
  → ir/regalloc.c: SSA-based register allocation
      internally: build CFG → construct SSA → rename
                → ir/opt/: SSA optimization engine (cprop → dce → target generators)
                → build intervals → linear scan → phi resolution
  → ir/codegen.c + arm-thumb-gen.c: code generation
```

Fallback pipeline at -O0 (or `-fno-ssa-regalloc`):
```
tccgen.c (IR emission)
  → ir/cfg.c + ir/ssa.c: construct SSA → rename
  → ir/opt/: SSA optimization engine
  → ir/ssa.c: destroy SSA
  → ir/opt.c: pre-SSA optimizations
  → tccls.c: legacy liveness + linear scan
  → ir/codegen.c + arm-thumb-gen.c: code generation
```

Final pipeline (step 7 done — SSA is default, legacy removed):
```
tccgen.c (IR emission)
  → ir/opt.c: pre-SSA optimizations (iterative loop)
  → ir/regalloc.c: SSA-based register allocation
      internally: build CFG → construct SSA → rename
                → ir/opt/: SSA optimization engine (SCCP, GVN, DCE, target generators)
                → build intervals → linear scan → phi resolution
  → ir/codegen.c + arm-thumb-gen.c: code generation
```

### Interface to existing code

- `tccgen.c`: orchestrates SSA pipeline (build CFG → construct → rename → optimize → destroy)
- `ir/opt/`: SSA optimization engine — target-independent passes + registered target generators
- `arch/arm/ssa_opt_arm.c`: ARM target-specific generators, registered via `tcc_ir_ssa_opt_register_target()`
- `ir/opt.c`: pre-SSA optimization passes — run after SSA destruction on flat IR
- `tccls.c`: unchanged (receives flat IR after SSA destruction); replaced by `ir/regalloc.c` in step 5
- `ir/codegen.c`: unchanged — operates post-regalloc

### New API surface

```c
/* ir/cfg.c */
typedef struct IRCFG { ... } IRCFG;
IRCFG *tcc_ir_cfg_build(TCCIRState *ir);
void tcc_ir_cfg_free(IRCFG *cfg);

/* ir/ssa.c */
void tcc_ir_ssa_construct(TCCIRState *ir, IRCFG *cfg);
void tcc_ir_ssa_destroy(TCCIRState *ir, IRCFG *cfg);
```

### vreg numbering

SSA creates new vregs (each def gets a unique name). Options:

**Option A: Extend existing vreg encoding.**
Use TCCIR_VREG_TYPE_TEMP with new positions beyond the original max. Phi dests and renamed defs get fresh positions. Simple, no encoding changes.

**Option B: New TCCIR_VREG_TYPE_SSA.**
Add a 4th vreg type. Cleaner separation, easier to assert "is this SSA?" but uses one of the few remaining type bits.

Recommendation: **Option A** — reuse TEMP namespace. SSA vregs are just temps with the invariant that each position has exactly one def. No encoding changes needed.

## Implementation Order

### Done

1. **`ir/cfg.c`** — CFG + dominator tree + dominance frontier ✓
   - CFG build, RPO, CHK dominators, dominance frontier all working
   - Infinite-loop guard + bitset dedup optimization applied
   - All tests pass with SSA phi placement enabled at -O1+

2. **`ir/ssa.c` phi placement** ✓
   - Only VARs with multi-block defs (skips TEMPs/PARAMs)
   - Single-scan, bulk allocation, early-exit for trivial functions
   - Wired into pipeline at -O1+ (`-fssa` / `-fno-ssa`)

3. **SSA renaming** ✓
   - `tcc_ir_ssa_rename()` implemented and produces correct SSA form
   - Enabled in pipeline with SSA construct → rename → optimize → destroy flow
   - SSA destruction inserts phi-resolution copies at predecessor block ends

4. **SSA optimization engine** ✓ (initial passes implemented)
   - Modular engine in `ir/opt/` with generator-based dispatch (like `thop_*` instruction builders)
   - Target-independent passes in `ir/opt/`, target-specific generators in `arch/arm/`
   - Backend registers generators via `tcc_ir_ssa_opt_register_target()` — generic code knows nothing about the target
   - **Infrastructure (`ir/opt/ssa_opt.h` + `ir/opt/ssa_opt.c`):**
     - `IRSSAOptCtx` — shared context with use-def chains per TEMP vreg
     - `IRSSAOptGen` — per-opcode generator descriptor (opcode → rewrite function)
     - `IRSSAOptPass` — pass descriptor (custom function or generator table)
     - Use-def chain builder: scans instructions + phi nodes in one pass
     - Helpers: `ssa_opt_nop_instr()`, `ssa_opt_replace_all_uses()`, `ssa_opt_run_gens()`
   - **DCE (`ir/opt/ssa_opt_dce.c`):** worklist-based, use-count == 0 → NOP defining instruction → cascade
   - **Copy propagation (`ir/opt/ssa_opt_cprop.c`):** generators `ssa_gen_cprop_assign` (vreg→vreg) and `ssa_gen_cprop_imm` (vreg→immediate)
   - **ARM generators (`arch/arm/ssa_opt_arm.c`):** `ssa_gen_arm_fuse_mul_add_to_mla`, `ssa_gen_arm_fuse_shl_add_to_load_indexed`, `ssa_gen_arm_fuse_shl_add_to_store_indexed`, `ssa_gen_arm_reduce_mul_to_shift`

5. **SSA-based register allocator** ✓
   - `ir/regalloc.c` (1633 lines) — arch-independent SSA-aware linear scan
   - `arch/arm/arm_regalloc.c` — ARM register tables (AAPCS, VFP)
   - Consumes SSA-renamed IR + phi nodes directly (no SSA destruction step)
   - Algorithm: linear scan on SSA with precoloring, call-crossing, 64-bit pairs
   - Phi resolution: topological sort, cycle breaking, ASSIGN insertion
   - Enabled at -O1+ via `-fssa-regalloc` (default on)
   - SSA optimization engine now wired in: runs between SSA rename and interval building

### Next

6. **Port remaining opts to SSA**
   - Constant propagation → sparse conditional constant propagation (SCCP)
   - CSE → dominator-tree-based value numbering (GVN)
   - Dead store elimination → SSA + alias analysis
   - Dead pure call elimination → use-count on call result vreg

7. **SSA default + legacy cleanup**
   - Make SSA the mandatory path — remove `-fssa` / `-fno-ssa` toggle, SSA always runs
   - Remove SSA destruction (`tcc_ir_ssa_destroy`) — regalloc consumes SSA directly
   - Delete legacy allocator: `tccls.c`, `ir/live.c`, associated headers
   - Delete pre-SSA passes replaced by SSA equivalents from `ir/opt.c`:
     - `tcc_ir_opt_dce` (replaced by `ssa_opt_dce`)
     - `tcc_ir_opt_copy_prop` (replaced by `ssa_opt_cprop`)
     - `tcc_ir_opt_mla_fusion`, `tcc_ir_opt_indexed_memory_fusion` (replaced by ARM generators)
     - `tcc_ir_opt_const_prop`, `tcc_ir_opt_const_prop_tmp`, `tcc_ir_opt_value_tracking` (replaced by SCCP)
     - `tcc_ir_opt_cse_arith`, `tcc_ir_opt_cse_global_load` (replaced by GVN)
   - Remove `IROptDU` infrastructure in `ir/opt.c` (superseded by `IRSSAVregInfo` use-def chains)
   - Clean up `tccgen.c` pipeline: single path through SSA construct → optimize → regalloc → codegen
   - Remove `opt_ssa` / `opt_ssa_regalloc` flags from `TCCState`
   - Update Makefile: remove deleted files from `IR_FILES` / `CORE_FILES`

## Complexity Estimates

| Component | Lines (est.) | Algorithm complexity | Status |
|-----------|-------------|---------------------|--------|
| CFG build | ~150 | O(n) — single scan | ✓ |
| Dominator tree (CHK) | ~120 | O(n * d) — fast for structured code | ✓ |
| Dominance frontier | ~80 | O(n_blocks^2) worst case, O(n) typical | ✓ |
| Phi placement | ~100 | O(vars * blocks) | ✓ |
| SSA renaming | ~150 | O(instructions) | ✓ |
| SSA destruction | ~120 | O(phi_nodes) — interim until SSA regalloc | ✓ |
| SSA opt engine | ~400 | O(n * passes) — iterative convergence | ✓ |
| SSA opt DCE | ~80 | O(n) — worklist-based | ✓ |
| SSA opt copy prop | ~120 | O(n) — generator-based | ✓ |
| ARM generators | ~400 | O(n) — per-instruction pattern match | ✓ |
| SSA linear scan regalloc | ~400 | O(n) — single pass over live intervals | |
| SCCP | ~300 | O(n) — lattice-based worklist | |
| GVN | ~400 | O(n) — dominator-tree value numbering | |
| Legacy cleanup | negative | deletion of tccls.c, live.c, redundant opt.c passes | |
| **Total** | **~2820** | | |

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| IJUMP (computed goto) makes CFG imprecise | Already handled: functions with IJUMP skip advanced opts. For SSA, treat IJUMP as jumping to all known label targets (same as today). |
| Address-taken locals can't be SSA-renamed | Don't rename them. If a VAR has its address taken (LEA of that VAR), keep it as a memory operation. Only promote non-address-taken scalars to SSA vregs. |
| Critical edges (pred has multiple succs, succ has multiple preds) | Insert empty split blocks during phi elimination. Simple, adds at most O(edges) blocks. |
| Compile-time regression | All algorithms are near-linear. CHK dominators is O(n^2) worst case on irreducible CFGs, but TCC always generates reducible CFGs (no `goto` into loops from outside). |

## Current Status (2026-05-04)

All IR tests (`make test -j16`) and GCC torture tests pass.

**What is live in the pipeline at -O1+:**
- CFG construction + dominator tree + dominance frontier (`ir/cfg.c`)
- SSA phi placement + renaming for multi-block VAR defs (`ir/ssa.c`)
- SSA optimization engine (`ir/opt/`): copy propagation, DCE, ARM target generators
- SSA destruction with phi-resolution copies (`ir/ssa.c`)
- Pre-SSA optimizations including `opt_cse` / `cse_arith` (`ir/opt.c`)
- Existing liveness + linear scan register allocator (`tccls.c` + `ir/live.c`)

**SSA optimization engine architecture:**
- Target-independent infrastructure in `ir/opt/` — use-def chains, generator dispatch, pass table
- Target-specific generators in `arch/arm/` — registered via `tcc_ir_ssa_opt_register_target()`
- Generic code has no knowledge of the underlying hardware
- Each generator is an explicit named function (like `thop_*` instruction builders)

**Next steps:**
- Port remaining optimizations to SSA: SCCP, GVN (step 6)
- Legacy cleanup: make SSA default, remove tccls.c + ir/live.c + redundant opt.c passes (step 7)

## Non-Goals (explicitly out of scope for current phase)

- Mem2Reg / SROA (needed eventually, not for current phase)
- Pruned SSA (full SSA is simpler to implement, prune later)
- Incremental SSA updates (rebuild from scratch each time is fine)
- Spill weight heuristics (use simple "most uses = least spill priority" initially)
