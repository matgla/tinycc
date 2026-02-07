# Loop-Invariant Code Motion (LICM) Implementation Plan

## Overview

LICM is an optimization that moves computations that produce the same result on every loop iteration outside the loop body. This is critical for the stack address problem where `Addr[StackLoc[-256]]` is computed 8+ times inside the bubble_sort inner loop.

## Current Problem

```
; Inner loop body (executed N² times):
0030: R4(T12) <-- R2(V3) SHL #2
0031: R5(T13) <-- Addr[StackLoc[-256]] ADD R4(T12)  ; <-- Invariant base!
...
0038: R5(T18) <-- Addr[StackLoc[-256]] ADD R4(T17)  ; <-- Same base!
```

Generated code:
```asm
; Each iteration:
sub.w   ip, r7, #256    ; REDUNDANT - same every iteration
add.w   r5, ip, r4
```

## Target Transformation

```
; Before loop:
0029: R10(Tbase) <-- Addr[StackLoc[-256]] [ASSIGN]  ; Hoisted!

; Inner loop body:
0030: R4(T12) <-- R2(V3) SHL #2
0031: R5(T13) <-- R10(Tbase) ADD R4(T12)  ; Use hoisted value
```

Generated code:
```asm
; Before loop (once):
sub.w   r10, r7, #256

; Each iteration:
add.w   r5, r10, r4     ; No redundant sub.w!
```

---

## Implementation Phases

### Phase 1: Loop Detection

**Goal**: Identify natural loops in the IR control flow graph.

#### 1.1 Build Control Flow Graph (CFG)

```c
typedef struct CFGBlock {
  int start_idx;           // First instruction index
  int end_idx;             // Last instruction index (inclusive)
  int *successors;         // Array of successor block indices
  int num_successors;
  int *predecessors;       // Array of predecessor block indices
  int num_predecessors;
} CFGBlock;

typedef struct CFG {
  CFGBlock *blocks;
  int num_blocks;
  int *block_of_instr;     // Maps instruction idx -> block idx
} CFG;
```

**Algorithm**:
1. Scan IR for basic block boundaries:
   - Leaders: instruction 0, jump targets, instructions after jumps
2. Create blocks between leaders
3. Add edges based on JUMP/JUMPIF/fall-through

#### 1.2 Compute Dominators

A block D dominates block B if every path from entry to B goes through D.

```c
typedef struct DominatorInfo {
  int *idom;               // Immediate dominator for each block
  uint8_t **dom_set;       // dom_set[b] = set of blocks dominated by b
} DominatorInfo;
```

**Algorithm**: Use iterative dataflow or Lengauer-Tarjan.

#### 1.3 Identify Natural Loops

A **back edge** is an edge B→H where H dominates B.
- H is the **loop header**
- The **loop body** is all blocks that can reach B without going through H

```c
typedef struct Loop {
  int header_block;        // Loop header block index
  int *body_blocks;        // Array of block indices in loop body
  int num_body_blocks;
  int preheader_block;     // Block to insert hoisted code (may need creation)
  struct Loop *parent;     // Enclosing loop (for nested loops)
  struct Loop *children;   // Nested loops
} Loop;
```

**Algorithm**:
1. Find all back edges (B→H where H dominates B)
2. For each back edge, compute loop body via reverse DFS from B, stopping at H

---

### Phase 2: Loop-Invariant Identification

**Goal**: Identify instructions whose operands don't change within the loop.

#### 2.1 Definition: Loop-Invariant

An instruction I is loop-invariant if ALL its operands are:
1. Constants (immediates, symbols)
2. Defined outside the loop
3. Defined by a loop-invariant instruction (recursive)

#### 2.2 Algorithm

```c
int is_loop_invariant(TCCIRState *ir, int instr_idx, Loop *loop, uint8_t *invariant_flags) {
  IRQuadCompact *q = &ir->compact_instructions[instr_idx];

  // Side-effecting instructions cannot be hoisted
  if (has_side_effects(q->op))
    return 0;

  // Check each source operand
  for (int i = 0; i < num_sources(q); i++) {
    IROperand src = get_source(ir, q, i);

    if (is_constant(src))
      continue;  // Constants are invariant

    if (is_vreg(src)) {
      int def_instr = find_definition(ir, src.vreg);
      if (def_instr < 0)
        return 0;  // No definition found (error?)

      if (!is_in_loop(def_instr, loop))
        continue;  // Defined outside loop - invariant

      if (invariant_flags[def_instr])
        continue;  // Defined by invariant instruction

      return 0;  // Defined in loop by non-invariant - NOT invariant
    }
  }

  return 1;
}
```

#### 2.3 Iterative Marking

```c
void mark_loop_invariants(TCCIRState *ir, Loop *loop, uint8_t *invariant_flags) {
  int changed;
  do {
    changed = 0;
    for (int i = 0; i < loop->num_body_blocks; i++) {
      CFGBlock *block = &cfg->blocks[loop->body_blocks[i]];
      for (int j = block->start_idx; j <= block->end_idx; j++) {
        if (!invariant_flags[j] && is_loop_invariant(ir, j, loop, invariant_flags)) {
          invariant_flags[j] = 1;
          changed = 1;
        }
      }
    }
  } while (changed);
}
```

---

### Phase 3: Safety Checks for Hoisting

Not all loop-invariant instructions can be safely hoisted.

#### 3.1 Must Execute Check

An instruction can only be hoisted if it's guaranteed to execute on every loop iteration. Otherwise, hoisting could introduce side effects that wouldn't happen in the original program.

For pure computations (no side effects), this is usually safe.

#### 3.2 No Clobbering

The hoisted instruction's destination must not be:
- Live at loop entry (would clobber existing value)
- Used before definition in the loop (same issue)

#### 3.3 Stack Address Special Case

For `Addr[StackLoc[offset]]`:
- Always safe to hoist (no side effects)
- Result is always the same (frame pointer is constant)
- Only concern: register allocation for the hoisted value

---

### Phase 4: Code Transformation

#### 4.1 Create/Find Preheader

A **preheader** is a block that:
- Has only the loop header as successor
- Is the only predecessor of the header from outside the loop

If no preheader exists, create one by inserting a new block.

#### 4.2 Hoist Instructions

```c
void hoist_instruction(TCCIRState *ir, int instr_idx, Loop *loop) {
  // 1. Find or create preheader
  int preheader = get_or_create_preheader(ir, loop);

  // 2. Move instruction to end of preheader (before the jump to header)
  move_instruction(ir, instr_idx, preheader);

  // 3. Update vreg definitions
  // The vreg defined by this instruction is now live across the loop
}
```

#### 4.3 IR Instruction Movement

This is the most complex part. Options:

**Option A: In-place Movement**
- Swap instruction slots
- Update all jump targets that point between old and new positions
- Complex and error-prone

**Option B: Mark-and-Regenerate**
- Mark instructions with their new positions
- Regenerate IR in correct order
- Simpler but requires IR rebuild

**Option C: Preheader as Separate IR Segment**
- Keep hoisted instructions in a separate list
- Emit preheader instructions before loop during codegen
- Minimal IR changes

---

### Phase 5: Register Allocation Integration

Hoisted loop-invariant values need registers that live across the entire loop.

#### 5.1 Current Challenge

The register allocator runs before LICM, so hoisted values don't have allocated registers.

#### 5.2 Options

**Option A: Run LICM Before Register Allocation**
- Modify IR before regalloc
- Hoisted instructions get natural register allocation
- Cleanest solution

**Option B: Reserve Registers for LICM**
- Before regalloc, identify potential LICM candidates
- Reserve callee-saved registers for them
- More complex coordination

**Option C: Post-Regalloc LICM with Spill**
- Run LICM after regalloc
- Use a spill slot for hoisted values if no register available
- Simple but may not help performance

---

## Recommended Implementation Order

### Step 1: CFG Construction (2-3 days)
- [ ] Implement `tcc_ir_build_cfg()`
- [ ] Add basic block identification
- [ ] Add successor/predecessor computation
- [ ] Unit tests for CFG

### Step 2: Loop Detection (2-3 days)
- [ ] Implement dominator computation
- [ ] Implement back-edge detection
- [ ] Implement natural loop identification
- [ ] Unit tests for loop detection

### Step 3: Simple LICM for Stack Addresses (1-2 days)
- [ ] Identify `Addr[StackLoc[*]]` operands in loops
- [ ] Hoist to preheader (insert ASSIGN instruction)
- [ ] Replace uses in loop body
- [ ] Focus only on stack addresses initially

### Step 4: Register Allocation Integration (2-3 days)
- [ ] Move LICM pass before register allocation
- [ ] Ensure hoisted values get proper vregs
- [ ] Handle callee-saved register pressure

### Step 5: General LICM (optional, 3-5 days)
- [ ] Extend to other loop-invariant expressions
- [ ] Add safety checks
- [ ] Handle nested loops

---

## File Structure

```
ir/
├── cfg.c          # CFG construction
├── cfg.h          # CFG data structures
├── dom.c          # Dominator computation
├── dom.h          # Dominator structures
├── loop.c         # Loop detection
├── loop.h         # Loop structures
├── licm.c         # LICM transformation
├── licm.h         # LICM interface
```

---

## API Design

```c
// Main entry point
int tcc_ir_opt_licm(TCCIRState *ir);

// Internal APIs
CFG *tcc_ir_build_cfg(TCCIRState *ir);
void tcc_ir_free_cfg(CFG *cfg);

DominatorInfo *tcc_ir_compute_dominators(CFG *cfg);
void tcc_ir_free_dominators(DominatorInfo *dom);

Loop *tcc_ir_detect_loops(CFG *cfg, DominatorInfo *dom, int *num_loops);
void tcc_ir_free_loops(Loop *loops, int num_loops);

int tcc_ir_hoist_invariants(TCCIRState *ir, Loop *loop);
```

---

## Testing Strategy

### Unit Tests
1. CFG construction for various control flow patterns
2. Dominator computation correctness
3. Loop detection for simple/nested/irreducible loops
4. LICM transformation verification

### Integration Tests
1. bubble_sort benchmark - verify stack address hoisting
2. Matrix multiplication - verify array base hoisting
3. Nested loop cases
4. Edge cases: single-iteration loops, break/continue

### Performance Tests
1. Measure code size reduction
2. Count eliminated instructions
3. Compare against GCC -O1 output

---

## Expected Results for bubble_sort

**Before LICM:**
- 8x `sub.w ip, r7, #256` in inner loop body
- ~280 bytes for bench_bubble_sort

**After LICM:**
- 1x `sub.w r10, r7, #256` in loop preheader
- ~200 bytes estimated (saving ~80 bytes)
- Still not as good as GCC (~76 bytes) due to other optimizations

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| CFG complexity for irreducible graphs | Medium | Focus on natural loops only |
| Register pressure increase | High | Limit hoisting if register pressure is high |
| Incorrect code motion | Critical | Extensive testing, safety checks |
| Integration with existing passes | Medium | Clear pass ordering documentation |

---

## Timeline Estimate

| Phase | Effort | Dependencies |
|-------|--------|--------------|
| CFG Construction | 2-3 days | None |
| Loop Detection | 2-3 days | CFG |
| Simple Stack Addr LICM | 1-2 days | Loop Detection |
| Regalloc Integration | 2-3 days | Simple LICM |
| General LICM | 3-5 days | All above |

**Total: 10-16 days for full implementation**

**Quick win (Stack Addr only): 5-8 days**
