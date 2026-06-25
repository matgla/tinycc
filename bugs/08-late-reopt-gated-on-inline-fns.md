# 08 — `gen_late_reopt_functions` only iterated `inline_fns`, locking out non-auto-inline functions

**Status:** FIXED in this branch ([tccgen.c:29381-29453](../tccgen.c#L29381-L29453))
**Severity:** Medium — entire end-of-TU dead-static-store mechanism silently skipped most candidate functions.

## Symptom

`gen_late_reopt_functions` walks `tcc_state->inline_fns` and re-compiles
entries with `func_late_reopt` set:

```c
for (i = 0; i < s->nb_inline_fns; ++i) {
  fn = s->inline_fns[i];
  sym = fn->sym;
  if (!sym->type.ref->f.func_late_reopt) continue;
  ... begin_macro(compile_ts, 1); next(); gen_function(sym); ...
}
```

It requires `fn->func_str` (the saved token stream) to replay-compile.
Tokens are saved only when the function takes one of the inline-related
paths in `decl()` — specifically when `sym->type.t & VT_INLINE` is set
or `auto_inline_sig_ok(sym)` returns 1.

`auto_inline_sig_ok` rejects:
- `double` / `long double` parameters or return type (via `auto_inline_type_ok` enum)
- struct *parameters* in non-static functions
- `_Complex` types
- unnamed parameters
- VLA parameters
- vector types
- structs > 16 bytes

Any function matching one of these signatures fell through to the plain
`else { gen_function(sym); }` branch with **no token preservation**.
At end-of-TU, those functions could not be re-compiled even when
`tcc_ir_tu_analyze_dead_statics` marked their writes as dead.

## Repro

`tests/gcctestsuite/.../gcc.c-torture/compile/pr25483.c`:

```c
static int mdct_win[8];
int decode_init(double d) {           /* double param → auto_inline_sig_ok = 0 */
  int j;
  for (j = 4; j; j--) { d *= 0.5; mdct_win[j] = (d * 3); }
}
```

`mdct_win` has no readers in the TU — TU analysis correctly flagged it
`tu_no_readers` and `decode_init` as `func_late_reopt`. But
`decode_init` was never in `inline_fns`, so `gen_late_reopt_functions`
silently skipped it. Output: 30 instructions vs GCC's 1.

## Fix

In `decl()`'s "regular function definition" `else` branch, when
`opt_dead_store` is enabled, take the same save+replay path that the
auto-inline TOO-LARGE branch uses:

```c
if (tcc_state->opt_dead_store) {
  struct InlineFunc *fn = tcc_malloc(...);
  fn->sym = sym;
  skip_or_save_block(&fn->func_str);
  int body_len = fn->func_str->len;
  if (body_len <= 512) {
    dynarray_add(&tcc_state->inline_fns, &tcc_state->nb_inline_fns, fn);
    /* replay-compile */
    begin_macro(compile_ts, 1); next(); gen_function(sym); end_macro();
    if (!sym->type.ref->f.tu_static_writer) {
      /* not a writer — drop tokens, detach so gen_inline_functions skips */
      fn->sym = NULL; tok_str_free(fn->func_str);
    }
  } else {
    /* body too large to retain — still need to replay-compile from the
     * saved stream because skip_or_save_block consumed the tokens */
    begin_macro(fn->func_str, 1); next(); gen_function(sym); end_macro();
  }
}
```

For `tu_static_writer` entries that weren't flagged for late_reopt
(their statics turned out to have readers), the *existing*
`gen_inline_functions` walk re-emits the body anyway — overwriting
only the symbol's `st_value` and leaving the first emission's bytes
as orphan in `.text`. That re-emission is desirable: it produces a
more optimized body once all auto-inline candidates have had their
flags finalized. Do *not* attempt to detach those entries from
`inline_fns` to suppress the re-emit — doing so leaves you with the
sub-optimized first emission (regression observed on
`tests/tests2/55_lshift_type.c`, main grew 532 → 1459 instructions).

Also gate the "function might return no value" warning on
`!ir_late_reopt_phase` so the second compile doesn't double-emit it.

## Why it matters (cascade)

Pairs with [[06]] (summary collector now records the write) and [[07]]
(late_cleanup pass can now NOP the unfused TEMP-DEREF STORE). The three
together close pr25483's gap from 30 instructions to 16. Further wins
beyond that need a pure-loop elimination pass (the remaining
`__aeabi_dmul` calls into `d`, but `d`'s final value is never observed
— GCC reaches `bx lr` by recognizing the whole loop is dead).

## Related

- [[06]] — write summary collector fix.
- [[07]] — DSE pass fix to match the unfused store form.
