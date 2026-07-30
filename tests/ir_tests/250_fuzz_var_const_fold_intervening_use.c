/* Regression: ssa_opt_var_const_fold (source/opt/ssa/scalar/cprop.c) deleted the
 * prior constant def of a VAR when folding a later self-update of that VAR,
 * without checking for an intervening USE of the def between the two.
 *
 * From signed fuzz seed 2016 (verbatim).  tcc -O0/-Os agreed with the
 * gcc -m32 -funsigned-char oracle (checksum=c58b312a); both tcc -O1 (e5edd2e1)
 * and tcc -O2 (55c0f024) diverged -- an optimizer miscompile.  Culprit pass
 * (SSA-pipeline, not -fno gated): ssa:var_const_fold.
 *
 * Root cause: the pass matches `Vx <- #c` ... `Vx <- Vx OP #imm` in one block
 * and folds the self-update to `Vx <- #(c OP imm)`, then NOPs the prior
 * `Vx <- #c` def.  In this seed main has:
 *     si11 = -2992;              // V2 <- #-2992   (prior def)
 *     si12 = si11 - si10;        // V3 <- V2 SUB V1 (INTERVENING use of V2)
 *     si11 = si11 & 0x7fff;      // V2 <- V2 AND #32767 (self-update; folds ok)
 * The self-update folds correctly to `V2 <- #29776`, but NOPing `V2 <- #-2992`
 * left the SUB reading an undefined V2, so si12 (-16928) was computed from
 * garbage.  Fix: only drop the prior def when Vx is not read anywhere between
 * the prior def and the self-update.
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(((unsigned)(4191645691u) | (unsigned)(pa))) * (unsigned)(1382152088u))) & 1u) lr += (unsigned)(((unsigned)(lr) >> ((unsigned)(((unsigned)(lr) >> ((unsigned)(3780959679u) & 31u))) & 31u)));
  lr = (unsigned)((-((unsigned)(pa) | 0u)));
  lr = (unsigned)(((unsigned)(((unsigned)((((unsigned)(1092341599u) & 1u) ? (unsigned)(1533921333u) : (unsigned)(lr))) << ((unsigned)((((unsigned)(634025655u) & 1u) ? (unsigned)(pa) : (unsigned)(lr))) & 31u))) - (unsigned)(lr)));
  if ((unsigned)(839791358u) & 1u) lr += (unsigned)(((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(pb) : (unsigned)(2696147663u))) << ((unsigned)(((unsigned)(pa) | (unsigned)(4101313884u))) & 31u)));
  return (unsigned)(100350884u) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(1866054840u) ^ (unsigned)(((unsigned)(1397676746u) / ((unsigned)(4173780879u) | 1u))))) | (unsigned)(lr)));
  lr = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(695778348u) / ((unsigned)(pb) | 1u))) | 0u))) ^ (unsigned)(((unsigned)(helper1(lr, 1539833207u)) & (unsigned)(((unsigned)(pa) >> ((unsigned)(pb) & 31u)))))));
  if ((unsigned)(((unsigned)(lr) * (unsigned)(((unsigned)(lr) ^ lr)))) & 1u) lr += (unsigned)(pa);
  if ((unsigned)(pb) & 1u) lr += (unsigned)(2429533309u);
  return (unsigned)(lr) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(1162040265u) % ((unsigned)(lr) | 1u))) | 0u))) - (unsigned)(helper2(lr, ((unsigned)(1842950984u) | (unsigned)(pb))))));
  lr = (unsigned)(2017636953u);
  lr = (unsigned)(((unsigned)(helper2(((unsigned)(311833464u) * (unsigned)(4023743703u)), (-((unsigned)(lr) | 0u)))) >> ((unsigned)(((unsigned)(((unsigned)(pb) % ((unsigned)(lr) | 1u))) + (unsigned)(lr))) & 31u)));
  return (unsigned)(((unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)(2441821325u) / ((unsigned)(3913278487u) | 1u))) & 31u))) / ((unsigned)(758679443u) | 1u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s4 = (short)(1091201129u & 0xffff);
  unsigned u5 = 2437169093u;
  unsigned u6 = 2705093685u;
  unsigned u7 = 2328140809u;
  unsigned arr8[8] = { 1946401979u, 1115985497u, 2202364714u, 181477407u, 803679709u, 70214932u, 3331565711u, 2104958214u };
  unsigned arr9[8] = { 2775751825u, 4040747615u, 4010324371u, 2501652786u, 3257894977u, 3027198863u, 2505478787u, 364931055u };
  int si10 = 13936;
  int si11 = -2992;
  int si12 = -25140;
  int si13 = 20235;

  si10 = (int)(signed char)(si10);
  si12 = (si11) - (si10);
  u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(arr9[((unsigned)(u7) & 7u)]) : (unsigned)(2805350291u))) - (unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(3250720452u))))) << ((unsigned)(((unsigned)(((unsigned)(2285350769u) / ((unsigned)(arr8[((unsigned)(u6) & 7u)]) | 1u))) % ((unsigned)(((unsigned)(u6) | (unsigned)(1945173572u))) | 1u))) & 31u))) >> ((unsigned)((-((unsigned)(((unsigned)(((unsigned)(u6) << ((unsigned)(u5) & 31u))) * (unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(u5) | 1u))))) | 0u))) & 31u))) & 0xffffffffu;
  si13 = (int)(signed char)(((int)(short)(u5)));
  si11 = ((si11) & 0x7fff) << ((unsigned)(((unsigned)(1120607571u) << ((unsigned)(1938046790u) & 31u))) & 15u);

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)(si10));
  cs = csmix(cs, (unsigned)(si11));
  cs = csmix(cs, (unsigned)(si12));
  cs = csmix(cs, (unsigned)(si13));
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
