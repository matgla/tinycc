/* Regression test (reduced differential-fuzz repro, gen_c.py float seeds).
 * tcc_ir_opt_memmove_to_indexed_stores folded `memcpy(&u, &f, 4)` after the
 * small-function inliner expanded `T f(T x){ T u; memcpy(&u,&x,sizeof u); return u; }`:
 * it relocated the source store and NOPed the memcpy, but the contributing
 * store's dest carried the source's named-VAR vreg identity — rewriting only
 * its stack offset left the store writing the *source* local, so the memcpy
 * destination `u` was never written and `return u` read 0 (fix: ir/opt.c —
 * bail when a contributing store's dest is a named VAR/PARAM local).
 * tcc -O0 was always correct; the bug appeared at -O1/-O2.  Expected checksum
 * is gcc -m32 -funsigned-char (here ABI-independent: raw float bits).
 */
#include <stdio.h>
#include <string.h>

/* Reinterpret a float's bits as unsigned via a memcpy through a local — the
 * exact fbits_f() shape the fuzzer's float programs are built from. */
static unsigned fbits_f(float f)
{
  unsigned u;
  memcpy(&u, &f, sizeof u);
  return u;
}

int main(void)
{
  float f12 = -0x1.8b76280000000p+17f;
  printf("checksum=%08x\n", fbits_f(f12));
  return 0;
}
