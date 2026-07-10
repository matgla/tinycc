# Post-Register-Allocation Optimizations

These passes run after the linear scan register allocator has assigned
physical registers to virtual registers.

## Pipeline Position

```
SSA opt → Phi resolution → Live interval construction → Graph coalescing
→ Linear scan → Write results → Live regs bitmap → Post-RA coalescing
→ Codegen peepholes → Branch optimization → Machine code emission
```

## Passes

### tcc_ir_move_coalescing (`ir/regalloc.c`)

Eliminates register-to-register copies (ASSIGN dest = src) by making
both sides share the same physical register.

**Requirements:**
- Source must die at the ASSIGN instruction
- Destination's live range must not conflict with source's register
- Call-crossing safety must be preserved
- Both endpoints must be in REAL registers (not PREG_NONE or PREG_SPILLED)
- Neither endpoint can be a coalesced member (would split the class)

**Forward direction**: Reassign dest to use src's register. Source dies
at the ASSIGN.

**Reverse direction**: Reassign src to use dest's register. Dest dies
at the ASSIGN.

**Special handling:**
- LOAD from in-register VAR treated as ASSIGN for coalescing
- Sub-word btypes skipped (would lose narrowing)
- Two-address relaxation: at dst.end, if the instruction reads dst and
  writes a result in src_reg, coalescing is safe (ARM two-operand form)

### tcc_ir_opt_post_ra_forward_diamond (`ir/opt_promote.c`)

Forwards values through diamond control flow after register allocation.
Pattern: `if (x) a = ...; else b = ...; c = ...` where c uses either a
or b — if a == b on both paths, the diamond collapses to a single value.

### Codegen Peepholes (`arm-thumb-gen.c`)

Post-emission peephole optimizations in the thumb-2 code generator:

| Peephole | Description |
|----------|-------------|
| Branch range optimization | Select optimal branch encoding (short/long) based on offset |
| Literal pool optimization | Minimize literal pool size, share literals |
| Stack slot optimization | Minimize stack frame size, share spill slots |
| Narrow instruction selection | Select narrowest valid encoding for operations |
| Two-address relaxation | Use two-address forms (e.g. `ADD Rd, Rn, Rm` instead of `ADD Rd, Rn, Rm, #0`) |

### Branch Optimization (`arm-thumb-gen.c`)

Branch range analysis and encoding selection:
- Tracks all branch instructions during dry-run
- Analyzes branch offsets to select optimal encodings (short vs long)
- Handles literal pool patching for branches that cross pool boundaries

## Optimization Levels Impact

Post-RA passes run at all optimization levels. The linear scan allocator
itself is level-independent; only the IR fed into it changes.

## Interaction with Pre-RA Optimizations

Post-RA passes are conservative — they cannot change the IR structure,
only rearrange register assignments. This makes them safe to run
regardless of optimization level.

The pre-RA passes (SSA opt, loop transforms) have a larger impact on
register pressure by reducing live ranges and eliminating dead code.
Post-RA passes then optimize the final register assignment.
