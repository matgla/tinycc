# LICM Redesign: Proper Dominance-Based Algorithm

## Problem

The current `ir/licm.c` has 4 bugs, all from missing CFG/dominance infrastructure:

1. **Switch-case back-edges**: ANY backward jump creates a "loop" — switch `break` JMPs create fake loops
2. **vreg=-1 invariance**: Operands with `vreg=-1` (pinned regs, anonymous values) assumed invariant
3. **Conditional store aliasing**: Stack address hoisting ignores conditional stores
4. **No dominance**: Can't verify a definition actually reaches the loop header

## Data Structures

```c
typedef struct IRBasicBlock {
    int start_idx;        /* First instruction index (inclusive) */
    int end_idx;          /* Past-last instruction index (exclusive) */
    int succs[2];         /* Successor block indices (-1 = none) */
    int num_succs;
    int *preds;           /* Predecessor block indices (dynamic array) */
    int num_preds;
    int idom;             /* Immediate dominator block index (-1 for entry) */
    int rpo_number;       /* Reverse postorder number */
} IRBasicBlock;

typedef struct IRCFG {
    IRBasicBlock *blocks;
    int num_blocks;
    int *rpo_order;       /* Block indices in reverse postorder */
    int *instr_to_block;  /* instruction index → block index (O(1)) */
} IRCFG;

typedef struct IRNaturalLoop {
    int header_block;     /* Block index of loop header */
    int *body_blocks;     /* Block indices in this loop */
    int num_body_blocks;
    int *exit_blocks;     /* Body blocks with successors outside loop */
    int num_exit_blocks;
    int depth;            /* Nesting depth (1 = outermost) */
} IRNaturalLoop;
```

## Algorithm

### Step 1: Build Basic Blocks

New block starts at: instruction 0, every `is_jump_target`, instruction after every terminator (JUMP/JUMPIF/RETURN/SWITCH_TABLE).

```
for i in 0..n:
    if is_leader[i]:
        create new block starting at i
    build instr_to_block[] mapping
```

### Step 2: Build CFG Edges

```
for each block B:
    last = B.end_idx - 1
    if JUMP: add_edge(B, target_block)
    if JUMPIF: add_edge(B, target_block) + add_edge(B, B+1)
    if RETURN: no successors
    else: add_edge(B, B+1)  // fall-through
```

### Step 3: Reverse Postorder (DFS)

Iterative DFS from block 0, record blocks in postorder, then reverse.

### Step 4: Dominator Tree (Cooper-Harvey-Kennedy)

```
idom[entry] = entry
repeat until no changes:
    for each B in RPO (skip entry):
        new_idom = first processed predecessor
        for each other processed predecessor P:
            new_idom = intersect(new_idom, P)
        idom[B] = new_idom

intersect(b1, b2):
    while b1 != b2:
        while rpo[b1] > rpo[b2]: b1 = idom[b1]
        while rpo[b2] > rpo[b1]: b2 = idom[b2]
    return b1
```

Converges in 2-3 iterations for structured C code. O(num_blocks²) worst case.

### Step 5: Natural Loop Detection

**Key fix for Bug 1**: A back-edge is `(B → H)` where H **dominates** B.

```
for each edge (B → H):
    if dominates(H, B):    // ← THIS CHECK FIXES BUG 1
        collect loop body via reverse flood-fill from B to H
        record exit blocks (body blocks with outside successors)
```

Switch-break edges FAIL the dominance check because the while-header does NOT dominate individual case bodies (they're only reached via the switch dispatch).

### Step 6: Loop-Invariant Detection (Fixed-Point)

Pre-compute `loop_defs[vreg] → count` for all vregs defined in the loop body.

An instruction is invariant if:
- No side effects (not STORE, CALL, etc.)
- Dest vreg has exactly 1 definition in the loop (or no dest)
- ALL source operands are invariant

An operand is invariant if:
- Tag is IMM32/I64/F32/F64/SYMREF → **always invariant** (constants/addresses)
- Tag is STACKOFF and !is_lval → invariant (stack address, not value)
- `vreg >= 0` and NOT in `loop_defs` → defined outside loop → **invariant**
- `vreg >= 0` and `loop_defs[vreg] == 1` and the single def is in `invariant_set` → **transitively invariant**
- `vreg < 0` → **NOT invariant** (fixes Bug 2)

Iterate until no new invariants found.

### Step 7: Safe Hoisting Check

Can hoist instruction I in block B if:
1. B **dominates all exit blocks** of the loop — OR dest vreg is dead after loop
2. For LOADs: no STORE in the loop body writes to the same address (fixes Bug 3)
3. No side effects (already checked)
4. Single def in loop (already checked)

### Step 8: Hoist

Process inner loops first (bottom-up by depth).

For each hoistable instruction:
1. Clone instruction to preheader (using `insert_instruction_before`)
2. NOP the original
3. After all hoists for one loop, rebuild CFG before processing next loop

## Integration

- New file: `ir/cfg.c` + `ir/cfg.h` with reusable CFG/dominator infrastructure
- Rewrite `ir/licm.c` loop detection + invariant analysis using CFG
- Pipeline position: unchanged (Phase 5 in tccgen.c, after const_prop, before IV strength reduction)
- Refactor `ir/opt.c` global CSE (`gcse_build_blocks`) to share CFG infrastructure

## Bug Fix Summary

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| 1. Switch back-edges | No dominance check on back-edges | `dominates(H, B)` in loop detection |
| 2. vreg=-1 invariance | `vreg < 0 → return 1` | Check tag first; `vreg < 0 → return 0` |
| 3. Conditional stores | No alias analysis for hoisted loads | Check for same-offset STOREs in loop |
| 4. No dominance tree | Pattern-based approach | Cooper-Harvey-Kennedy algorithm |

## Estimated Impact

- **set_key**: 776 → ~550 (hoisting 4 AES table bases from each key-schedule loop)
- **encrypt/decrypt**: 2108 → ~1600 (hoisting 4 table bases from encryption rounds)
- Total AES: 5075 → ~3800 (~25% reduction)
