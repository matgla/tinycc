# Legacy `or_bool_diamond` → `ssa:or_bool_diamond`

Status: **COMPLETE** (2026-07-07). Legacy call sites removed; delta 0/4267 vs
the legacy reference; `make test-ir` green.

## The pattern

`acc |= (cond ? 1 : 0)` — most commonly a `fails |= check(...)` where the
inlined check returns 0/1 through an anonymous stack slot:

```
i_jmpif: JUMPIF cond → i_st_f          (skip the true arm)
         ... true arm (e.g. inlined printf) ...
i_st_t:  STORE slot, #1
i_jmp:   JUMP i_or
i_st_f:  STORE slot, #0
i_or:    dst = src OR slot             (merge OR)
```

The bool is materialized into a raw no-vreg `STACKOFF` slot, so neither SSA
promotion (VARs only) nor `ra_promote_multidef_temps_to_vars` (TEMPs only)
ever sees it — no generic SSA pass covers it. The transform computes the OR
directly on each arm and drops the slot:

```
i_st_t:  dst = src OR #1
i_st_f:  dst = src        (ASSIGN; same as src|0)
i_or:    NOP
```

Constraints: slot referenced only at `i_st_t`/`i_st_f`/`i_or` (vreg-carrying
VARs sharing the offset don't count — non-overlapping slot reuse), stored
values exactly {1, 0}, `i_jmp` is the unique jump into `i_or`, no other jump
or switch-table entry targets `i_st_f` or `i_or`.

## Migration

- Engine kept in place as `ssa_opt_or_bool_diamond` (`ir/opt_branch.c`),
  following the `ssa_opt_licm`-in-`licm.c` precedent.
- Driven from the flat region of `ra_linear_scan_ssa` (`ir/regalloc.c`)
  immediately after `ssa:cfg_cleanup`, whose `eliminate_fallthrough` creates
  the required `STORE slot,#0` / `OR` adjacency. Gate matches the legacy
  tccgen call (`opt_const_prop`, -O1+); knob
  `TCC_DISABLE_PASS=ssa:or_bool_diamond`.
- Removed: the late `gen_function` call (tccgen.c), the two
  `PASS_GATED("or_bool", ...)` pipeline entries (`ir/opt_pipeline.c`) — both
  proven inert: they ran before fallthrough elimination, so the intervening
  `JUMP` between the false-arm STORE and the OR always broke the match — and
  the `_ex` wrapper + `opt.h` decls.
- Unit coverage: `tests/unit/arm/armv8m/test_opt_branch_cascade.c`
  (`UT_COVERS("ssa:or_bool_diamond")`).

## Possible follow-up

The engine still pattern-matches flat quads. A real SSA formulation would
promote single-store-per-arm/single-load slots to phis and sink the OR through
the phi — subsuming `stack_bool_diamond` too. Not needed for parity.
