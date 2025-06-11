/* Regression: a store to a struct member at offset 0 reached through the result
 * of an inner assignment -- `(vv = make())->flags = CONST;` -- dropped the
 * DEREF (store-through-pointer) marker on the store operand when the `base + 0`
 * address fold collapsed the zero offset.  The IR became
 *
 *     CALL make --> T3
 *     T3 <-- #CONST [STORE]      (BUG: T3, not T3***DEREF***)
 *     PARAM0 T3                  (use(vv) -- T3 already clobbered)
 *
 * so the backend emitted `mov T3,#CONST` (a register write) instead of
 * `str #CONST,[T3]`.  The memory store was LOST and the live pointer `vv` was
 * overwritten with CONST.  Only triggered when the call result stays a TEMP
 * (opaque/forward-declared callee) and the member offset is 0; a nonzero offset
 * or a non-chained `vv = make(); vv->flags = CONST;` were fine, and so was the
 * VREG-allocated path.
 *
 * Surfaced as a HardFault in toybox sh setvar_long() at
 *   if (!(was = vv = findvar(s, &ff))) (vv = addvar(s, ff))->flags = VAR_NOFREE;
 * where VAR_NOFREE == (1<<10) == 1024 became the variable-table pointer.
 */
#include <stdio.h>

struct S { long flags; char *str; };
static struct S slot;

/* Forward-declared so `victim` sees them as opaque (call result stays a TEMP). */
struct S *make(void);
long use(struct S *p);

/* Same shape as the miscompiled setvar_long line. */
long victim(void) { struct S *vv; (vv = make())->flags = 1024; return use(vv); }

struct S *make(void) { slot.flags = 7; slot.str = "x"; return &slot; }
long use(struct S *p) { return p->flags; }

int main(void)
{
  long r = victim();
  printf("r=%ld slot=%ld\n", r, slot.flags);
  return (r == 1024 && slot.flags == 1024) ? 0 : 1;
}
