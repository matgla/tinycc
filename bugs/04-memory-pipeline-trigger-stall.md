# 04 — `memory_passes` group stalls when its trigger returns 0 mid-cascade

**Status:** WORKED AROUND via the `kb_cascade` compound pass in [ir/opt_pipeline.c](../ir/opt_pipeline.c)
**Severity:** Medium — limits how far a single pipeline run can drive a chain reaction.

## Symptom

`pipeline_run_group` ([ir/opt_pipeline.c:63-118](../ir/opt_pipeline.c#L63-L118)) iterates a pass
group until the *trigger* pass returns 0:

```c
if (group->trigger_idx >= 0) {
  int tch = trigger->run(ctx);
  ...
  if (tch <= 0) break;
}
```

The `memory_passes` group uses `sl_forward` as its trigger
([ir/opt_pipeline.c:220-232](../ir/opt_pipeline.c#L220-L232)). Once `sl_forward` exhausts the
*currently visible* forwarding opportunities, the group exits — even if
other passes in the group (or future iterations) would create new
opportunities for it.

## Repro

bitfld-1, iteration 1 of `memory_passes`:

1. `sl_forward` — forwards stored value into the *first* chain's
   re-read. Returns >0. Group continues.
2. `const_cascade`, `known_bits`, `branch_fold_2x`, `dce`,
   `elim_fallthru` — together they fold the first chain, kill its
   `abort()`, NOP the now-trivial JMP-to-next.

Iteration 2:

3. `sl_forward` re-runs on the cleaned-up IR. With the `abort()` call
   gone, it *could now* forward the stack store across to the **next**
   chain's read. But its analysis returns 0 because the changes from
   step 2 haven't been re-discovered as new forwarding sites in this
   iteration's pre-scan, **or** sl_forward's incremental check decides
   there's nothing new. Group exits. The other three chains never fold.

End state: only the first of four `abort()` chains is eliminated.

## Workaround

A compound pass `kb_cascade` ([ir/opt_pipeline.c:150-169](../ir/opt_pipeline.c#L150-L169)) loops the
relevant subset internally to a fixed point:

```c
for (int i = 0; i < 8; i++) {
  ch += tcc_ir_opt_known_bits(ir);
  ch += tcc_ir_opt_const_prop_tmp(ir);
  ch += tcc_ir_opt_branch_folding(ir);
  tcc_ir_opt_dce(ir);
  ch += tcc_ir_opt_eliminate_fallthrough(ir);
  tcc_ir_opt_compact_nops(ir);
  ch += tcc_ir_opt_sl_forward(ir);
  if (!ch) break;
}
```

It's added at the end of `memory_passes`. With this, all four bitfld-1
chains cascade in a single pipeline step.

## Better fix (deferred)

The trigger mechanism is a useful optimization (skip the group when
nothing's primed it), but it should be triggered by *any* pass returning
> 0, not specifically the indexed trigger. Two options:

1. Change `pipeline_run_group` to compute `round_changes` from the full
   group and re-iterate while `round_changes > 0`, falling back to the
   trigger only as a first-iteration gate.
2. Promote `sl_forward` out of the trigger slot, run the group based on
   `round_changes` like the trigger-less groups already do.

Either change affects every group, so it needs a wider sweep to verify
no group depends on the early-exit behavior. The narrow `kb_cascade`
workaround sidesteps that risk.

## Related

- [[02]] — the cascade only matters because `known_bits` *can* fold the chain heads; the trigger stall hid that we needed to.
- [[01]] — the chain head's IMOD fold is what creates the dead `abort()` whose removal lets `sl_forward` continue.
