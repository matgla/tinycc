/* Guard: an inlined body with TWO struct returns must not retarget the
 * inline return slot twice.
 *
 * tccgen.c's inline expansion optimizes `return <simple stack local>;` for a
 * struct-returning callee by RETARGETING inline_return_loc at that local
 * instead of copying it into the return slot (saves a memmove).  With two
 * returns
 *
 *     SRA get(unsigned live) {
 *       SRA res;
 *       if (try_get(live, &res)) return res;   // retargets loc -> res
 *       return fallback();                     // retargeted it AGAIN
 *     }
 *
 * the second retarget moved the caller's read away from the slot the first
 * return had left its value in — and the first return emits no copy, so its
 * path read an UNINITIALIZED slot.  DSE then legitimately deleted the now
 * unread store chain, which made the damage look like a DSE bug; the IR was
 * already wrong before any optimization pass.
 *
 * On the device this produced reg = -1 out of tcc's own scratch allocator:
 * the rodata-anchor sequence then used one register for both the address and
 * the temp (push{r0}; ldr r0,[r9,#24]; add r0,r0; pop{r0}), discarding the
 * base add, so string pointers stayed raw .rodata offsets and faulted.
 *
 * The struct MUST be a bitfield struct returned by value, and the first
 * return's local must have had its address escape into a callee out-param.
 */

#include <stdio.h>

typedef struct SRA { int reg : 29; unsigned saved : 2; unsigned would : 1; } SRA;

static int try_get(unsigned live, SRA *out)
{
  unsigned avail = (~live) & 0xFu;
  if (avail) {
    SRA res = {0};
    res.reg = 31 - __builtin_clz(avail);
    *out = res;
    return 1;
  }
  return 0;
}

static SRA fallback(void) { SRA r = {0}; r.reg = 12; r.saved = 1; return r; }

static SRA get_scratch(unsigned live)
{
  SRA res;
  if (try_get(live, &res))
    return res;
  return fallback();
}

/* Separate caller so get_scratch is inlined (the bug needs the expansion). */
int use_reg(unsigned live) { SRA a = get_scratch(live); return a.reg; }
int use_saved(unsigned live) { SRA a = get_scratch(live); return (int)a.saved; }

volatile unsigned vlive;

int main(void)
{
  vlive = 0u;          /* r0..r3 free  -> highest free low reg = 3 */
  printf("free  reg=%d saved=%d\n", use_reg(vlive), use_saved(vlive));
  vlive = 0x7u;        /* only r3 free */
  printf("one   reg=%d saved=%d\n", use_reg(vlive), use_saved(vlive));
  vlive = 0xFu;        /* none free -> fallback() path */
  printf("none  reg=%d saved=%d\n", use_reg(vlive), use_saved(vlive));
  return 0;
}
