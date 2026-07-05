/* Regression for differential-fuzz seed 2874: wrong-code at -O1/-O2.
 *
 * Root cause: tcc_ir_opt_store_redundant (ir/opt_memory.c) treats a
 * constant-index LOAD_INDEXED's read incompletely.  The generic per-operand
 * eviction (RSE_EVICT_FOR_SRC) only evicts the tracked store at the array's
 * BASE offset (element 0); a read of a non-zero element `arr[2]` via a
 * constant-index LOAD_INDEXED therefore failed to keep arr[2]'s producing
 * store alive, so a later store to the same slot wrongly killed it (the
 * dropped store fed an intermediate csmix, corrupting the checksum).  The
 * dedicated LOAD_INDEXED handler only covered the runtime-index case.
 *
 * (-fno-const-prop "fixes" it only by suppressing the const-prop that turns the
 * index constant; const_var_prop / sl_forward are sound enablers, not the bug.)
 *
 * Fix: the LOAD_INDEXED handler now also evicts the exact slot (base +
 * index<<scale) for a constant index.  Ground truth (gcc -m32 -funsigned-char):
 * checksum=75fc991c (== tcc -O0).  Buggy -O1/-O2 produced 42067ca9.
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
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) | (unsigned)(1403421279u))) << ((unsigned)(((unsigned)(lr) ^ (unsigned)(241228254u))) & 31u))) << ((unsigned)(pb) & 31u)));
  lr = (unsigned)(pb);
  lr = (unsigned)(pb);
  lr = (unsigned)(((unsigned)(((unsigned)(1368216116u) >> ((unsigned)(pa) & 31u))) * (unsigned)(53156875u)));
  return (unsigned)((~((unsigned)(2292359555u) | 0u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(241958825u & 0xffffffff);
  long s3 = (long)(1074641298u & 0xffffffff);
  unsigned u4 = 1305380894u;
  unsigned u5 = 3313434544u;
  unsigned u6 = 437555215u;
  unsigned u7 = 171060831u;
  unsigned arr8[8] = { 3619199989u, 587561886u, 1001515859u, 3918323290u, 970791251u, 3814497020u, 4285419517u, 3276498906u };
  unsigned arr9[8] = { 1433064782u, 3510485189u, 1799778821u, 303835480u, 3160467839u, 788780523u, 2521852464u, 2298611642u };
  struct S st10 = { 4272563366u, 2860391280u, 1545682751u };

  u7 = (unsigned)(((unsigned)((unsigned)(s3)) % ((unsigned)(arr8[((unsigned)(3964667507u) & 7u)]) | 1u))) & 0xffffffffu;
  arr8[((unsigned)(u7) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)(((unsigned)(st10.f2) ^ (unsigned)(arr8[((unsigned)(u7) & 7u)]))))) % ((unsigned)(helper1(((unsigned)((unsigned)(s2)) % ((unsigned)(st10.f2) | 1u)), 736360827u)) | 1u))) - (unsigned)(((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(arr8[((unsigned)(u4) & 7u)]) : (unsigned)(u6))) / ((unsigned)(((unsigned)(arr8[((unsigned)(u4) & 7u)]) ^ (unsigned)(((unsigned)(u6) << ((unsigned)(2244765377u) & 31u))))) | 1u)))));
  u6 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) % ((unsigned)(1581967986u) | 1u))) ^ (unsigned)(((unsigned)(u7) * (unsigned)(3549335271u))))) * (unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(u4) & 7u)]) > ((unsigned)(u7) ^ cs))) ^ (unsigned)(((unsigned)(u6) * (unsigned)(arr8[((unsigned)(u6) & 7u)]))))))) | (unsigned)(((unsigned)(st10.f1) ^ (unsigned)(((unsigned)((-((unsigned)(arr9[((unsigned)(3427199912u) & 7u)]) | 0u))) * (unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(1879608938u))))))))) & 0xffffffffu;
  if ((unsigned)(501675761u) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) * (unsigned)((unsigned)(s3)))) | (unsigned)(arr9[((unsigned)(u6) & 7u)]))));
    st10.f2 = (unsigned)(3319389946u);
    st10.f1 = (unsigned)(420478806u);
  } else {
    for (unsigned g12 = 0u; g12 < 1u; g12++) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      arr8[((unsigned)(u7) & 7u)] = (unsigned)(((unsigned)(2397888763u) << ((unsigned)(((unsigned)(73142872u) | (unsigned)(u5))) & 31u)));
    }
  }
  if ((unsigned)(((unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)((unsigned)(s2)) < ((unsigned)(((unsigned)(709260419u) + (unsigned)(u6))) ^ cs))) | 1u))) & (unsigned)(2329118710u))) & 1u) {
    u4 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(483797555u) | 0u))) - (unsigned)(((unsigned)(((unsigned)(3564909082u) / ((unsigned)(478001813u) | 1u))) - (unsigned)((~((unsigned)(3807591944u) | 0u))))))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(u7))) <= ((unsigned)(((unsigned)(u6) / ((unsigned)(arr9[((unsigned)(u7) & 7u)]) | 1u))) ^ cs))) << ((unsigned)((~((unsigned)(((unsigned)(127387323u) ^ (unsigned)(1726801684u))) | 0u))) & 31u))) & 31u))) & 0xffffffffu;
    arr9[((unsigned)(u7) & 7u)] = (unsigned)(((unsigned)((-((unsigned)(st10.f2) | 0u))) - (unsigned)(((unsigned)(st10.f0) - (unsigned)((~((unsigned)(827142306u) | 0u)))))));
    { unsigned g14 = 0u;
      while (g14 < 3u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        arr9[((unsigned)(4264154433u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(1044595121u))) << ((unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(arr9[((unsigned)(110793644u) & 7u)]))) & 31u))) - (unsigned)((unsigned)(s2)))) * (unsigned)(helper1(arr8[((unsigned)(u4) & 7u)], ((unsigned)(((unsigned)(3576629892u) / ((unsigned)(u6) | 1u))) - (unsigned)(((unsigned)(3585847080u) % ((unsigned)(u6) | 1u))))))));
        cs = csmix(cs, (unsigned)(arr9[((unsigned)(3172437083u) & 7u)]));
        arr8[((unsigned)(3190289822u) & 7u)] = (unsigned)(((unsigned)((unsigned)(s3)) & (unsigned)((((unsigned)(((unsigned)(2162988735u) - (unsigned)((unsigned)(s3)))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u4) / ((unsigned)(i13) | 1u))) % ((unsigned)(((unsigned)((unsigned)(s3)) >> ((unsigned)(u5) & 31u))) | 1u))) : (unsigned)(((unsigned)(((unsigned)(st10.f1) << ((unsigned)(i13) & 31u))) * (unsigned)(u6)))))));
        u5 = (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(u4) & 31u))) ^ (unsigned)(st10.f0))) & 0xffffffffu;
        st10.f0 = (unsigned)((unsigned)(s3));
        g14++;
      }
    }
  }
  u5 = (unsigned)(u7) & 0xffffffffu;

  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
