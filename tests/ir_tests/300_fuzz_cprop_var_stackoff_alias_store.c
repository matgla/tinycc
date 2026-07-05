/* combo fuzz seed 74935 (O2): SSA copy-propagation forwarded an
 * address-taken local across an aliasing store through a pointer to it.
 *
 * The source computes `u9 = u10 ^ 0` (the `^0` term folds from a
 * `3501411854 * -(s8/1621620329)` == 0 idiom), which const-folding lowers
 * to the copy `T101 <-- u10 [ASSIGN]`.  Right after, `*p11 = 44492376`
 * stores through `p11 == &u10`, overwriting u10's stack slot, and only then
 * is u9 (== the captured pre-store u10) consumed by csmix.
 *
 * ssa_opt_cprop's ssa_gen_cprop_copy_var_stackoff forwarded the STACKOFF
 * source u10 into T101's uses and NOP'd the copy.  Its safety scan only
 * bailed on a *direct* redef of u10's vreg (dest == src_vr), so it missed
 * the deref store `*p11 = ...` (whose dest vreg is the pointer, not u10).
 * The forwarded use then re-read u10's slot *after* it was clobbered
 * (44492376) instead of the captured value (2586559119) -> wrong checksum.
 *
 * Fix: when the STACKOFF source is address-taken its slot is aliasable, so
 * bail on any intervening memory store (deref/indexed) between the copy and
 * the use.  The sibling ssa_gen_cprop_copy_param carries the same guard.
 * O0/O1/Os were already correct; only O2 diverged.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
static struct SB5 sbh3(struct SB1 p, unsigned x)
{
  struct SB5 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu,
                   (unsigned)(((unsigned)(x) % ((unsigned)(2979732810u) | 1u))) & 0xffu };
  return r;
}
static struct SB8 sbh4(struct SB8 p, unsigned x)
{
  struct SB8 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu, (unsigned)(p.b) & 0xffffffffu };
  return r;
}
static struct SB8 sbh5(struct SB4 p, unsigned x)
{
  struct SB8 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu,
                   (unsigned)(((unsigned)(((unsigned)(3989621058u) / ((unsigned)(p.a) | 1u))) *
                              (unsigned)(((unsigned)(((unsigned)(2982319486u) & (unsigned)(3589206433u))) /
                                         ((unsigned)(((unsigned)(p.a) & (unsigned)(576429016u))) | 1u))))) & 0xffffffffu };
  return r;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  int s8 = (int)(1083247308u & 0xffffffff);
  unsigned u9 = 3648012684u;
  unsigned u10 = 2586559119u;
  unsigned *p11 = &u10;
  { struct SB8 sba14 = { (unsigned)((unsigned)(s8)) & 0xffffffffu, (unsigned)(442403819u) & 0xffffffffu }; (void)sba14; }
  { struct SB8 sba16 = { (unsigned)(((unsigned)(((unsigned)(430131030u) + (unsigned)((*p11)))) ^ (unsigned)(((unsigned)(u9) << ((unsigned)(3130469443u) & 31u))))) & 0xffffffffu, (unsigned)(((unsigned)(u10) >= ((unsigned)(((unsigned)(u10) / ((unsigned)(694489988u) | 1u))) ^ cs))) & 0xffffffffu }; (void)sba16; }
  u9 = (unsigned)(((unsigned)(u10) ^ (unsigned)(((unsigned)(3501411854u) * (unsigned)((-((unsigned)(((unsigned)((unsigned)(s8)) / ((unsigned)(1621620329u) | 1u))) | 0u))))))) & 0xffffffffu;
  *p11 = (unsigned)(44492376u);
  cs = csmix(cs, *p11);
  cs = csmix(cs, u9);
  { struct SB1 sba18 = { 1u }; struct SB5 sbt19 = sbh3(sba18, cs); cs = csmix(cs, sbt19.b); }
  { struct SB8 sba20 = { 19088744u, 19088745u }; struct SB8 sbt21 = sbh4(sba20, cs); cs = csmix(cs, sbt21.b); }
  { struct SB4 sba22 = { 38177487u }; struct SB8 sbt23 = sbh5(sba22, cs); cs = csmix(cs, sbt23.b); }
  printf("checksum=%08x\n", cs);
  return 0;
}
