/* agg_deep fuzz seed 86393 (O1): tcc_ir_opt_ptr_load_cse (Phase 6) CSE'd a
 * pointer deref across an aliasing store to the pointed-to local.
 *
 * `u4` is address-taken (`pa211 = &u4`, `ppa212 = &pa211`), so `**ppa212`
 * reads u4's stack slot.  The body reads `(**ppa212) & 31` twice with a write
 * to u4 in between:
 *
 *     u4 = ... (**ppa212) ... (**ppa212) & 31 ...;   // reads OLD u4
 *     u4 = helper1(..., ((u3 - u4) >> ((**ppa212) & 31)) | ...);  // reads NEW u4
 *
 * After ptr_load_cse collapses the invariant `*ppa212` (== pa211) loads, both
 * `(**ppa212) & 31` sub-expressions share one deref temp T8 (== *pa211 == u4).
 * The pass caches that deref and forwards the pre-store value to the second
 * read — but only flushed its cache on a *register-form* write to an
 * address-taken VAR (`!dest.is_lval`).  The first `u4 = ...` store is emitted
 * as an ASSIGN with an is_lval dest (u4 must be materialized to memory because
 * a pointer reads it afterwards); that ASSIGN is neither a STORE op nor caught
 * by the `!dest.is_lval` addrtaken guard, so the deref cache was never
 * invalidated and the second read used u4's stale pre-store value.  The shift
 * amount was thus computed from the old u4 (==5) instead of the new one (==0),
 * giving the wrong checksum.
 *
 * Fix: flush the ptr_load_cse deref cache on ANY write to an address-taken
 * VAR, is_lval or not.  The sibling local ALU-CSE pass (opt_copyprop.c) got the
 * same treatment for cached lval-src (deref) entries.  O0/O2/Os were correct;
 * only O1 diverged.  Reduced from gen_c.py --profile agg_deep --seed 86393;
 * gcc -O2 agrees with the O0 value (9bacf0f6).
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(2469754763u) / ((unsigned)(1716139108u) | 1u))) * (unsigned)(pb))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

struct N { unsigned a; unsigned b; };

struct N2 { struct N n; unsigned t; };

int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1212512847u & 0xff);
  unsigned u3 = 4108210878u;
  unsigned u4 = 2337491333u;
  unsigned arr5[8] = { 4263232862u, 2469551958u, 330471067u, 3390030084u, 3256619108u, 3758530003u, 1599103632u, 3469930344u };
  struct S st7 = { 1888984117u, 2342254955u, 3964779347u };
  struct S st8 = { 464194355u, 3510345096u, 4168587668u };
  struct N2 n29 = { { 2647381555u, 4272818725u }, 2829903446u };
  unsigned m210[4][4] = { { 3617684221u, 522328531u, 823220576u, 2590639754u }, { 3144012322u, 2556748891u, 2541271350u, 43299522u }, { 2859953544u, 1674708478u, 133856189u, 2157479804u }, { 476889490u, 797388126u, 3011714214u, 868902747u } };
  unsigned *pa211 = &u4;
  unsigned **ppa212 = &pa211;

  u4 = (unsigned)((((unsigned)(n29.t) & 1u) ? (unsigned)(m210[((unsigned)(462907338u) & 3u)][((unsigned)(2181561350u) & 3u)]) : (unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) + (unsigned)(1695121179u))) & (unsigned)((**ppa212)))) & (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) / ((unsigned)(arr5[((unsigned)(2108316921u) & 7u)]) | 1u))) << ((unsigned)(((unsigned)(m210[((unsigned)(3556061051u) & 3u)][((unsigned)(1108928950u) & 3u)]) >> ((unsigned)((**ppa212)) & 31u))) & 31u))))))) & 0xffffffffu;
  u4 = (unsigned)(helper1(((unsigned)(st8.f2) % ((unsigned)(u3) | 1u)), ((unsigned)(((unsigned)(((unsigned)(u3) - (unsigned)(u4))) >> ((unsigned)((**ppa212)) & 31u))) | (unsigned)(st7.f0)))) & 0xffffffffu;

  cs = csmix(cs, *pa211);
  printf("checksum=%08x\n", cs);
  return 0;
}
