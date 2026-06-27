/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=671).
 * PENDING (unfixed) -O2 regression:  * jump_threading (ir/opt_pipeline.c jump_thread group) dropped a store that
 * follows an always-true (constant-folded) conditional inside a loop.  The
 * source pattern is:
 *     while (...) { cs = csmix(...); if (CONST & 1) { arr9[..] = s2; cs = csmix(...); } arr8[..] = arr9[..]; }
 * With the condition folded to "always taken", the post-conditional store
 * `arr8[0] = arr9[u5&7]` vanished from the loop body, so arr8[0] kept its
 * initializer instead of being refreshed each iteration.  tcc -O0/-O1 were
 * correct; the bug appeared only at -O2 (where jump-threading runs).  Expected
 * checksum is gcc -m32 -funsigned-char (ARM ABI: unsigned char, 32-bit long).
 */

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
  if ((unsigned)(((unsigned)(492505571u) != ((unsigned)(pb) ^ lr))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) >> ((unsigned)(pa) & 31u))) ^ (unsigned)(((unsigned)(lr) + (unsigned)(pb)))));
  if ((unsigned)(((unsigned)(((unsigned)(163147573u) ^ (unsigned)(2338126641u))) > ((unsigned)(pb) ^ lr))) & 1u) lr += (unsigned)(pa);
  lr = (unsigned)((~((unsigned)(pa) | 0u)));
  lr = (unsigned)(((unsigned)(pb) % ((unsigned)(((unsigned)((((unsigned)(251037344u) & 1u) ? (unsigned)(1250232281u) : (unsigned)(1184067378u))) | (unsigned)(((unsigned)(pb) << ((unsigned)(2950430654u) & 31u))))) | 1u)));
  lr = (unsigned)(((unsigned)(((unsigned)(1453394280u) / ((unsigned)(((unsigned)(pb) + (unsigned)(1395454846u))) | 1u))) & (unsigned)(4213132740u)));
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(pb) == ((unsigned)(((unsigned)(pb) ^ lr)) ^ lr))) % ((unsigned)(1875486736u) | 1u))) / ((unsigned)(pa) | 1u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s2 = (short)(304740015u & 0xffff);
  long s3 = (long)(528575106u & 0xffffffff);
  int s4 = (int)(367776779u & 0xffffffff);
  unsigned u5 = 1973465635u;
  unsigned u6 = 3843492378u;
  unsigned u7 = 679725822u;
  unsigned arr8[8] = { 935241509u, 1649463831u, 3995577116u, 3004995134u, 79024171u, 1539135757u, 3255382896u, 4071951243u };
  unsigned arr9[8] = { 223783464u, 3212060194u, 1167023432u, 1652065559u, 2696814833u, 3807205455u, 704495684u, 2377494374u };

  for (unsigned g11 = 0u; g11 < 3u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    cs = csmix(cs, (unsigned)(((unsigned)(u5) << ((unsigned)((unsigned)(s3)) & 31u))));
  }
  cs = csmix(cs, (unsigned)(u5));
  { unsigned g13 = 0u;
    while (g13 < 3u) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      if ((unsigned)(u5) & 1u) {
        arr9[((unsigned)(u6) & 7u)] = (unsigned)((unsigned)(s2));
        arr9[((unsigned)(u7) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(746689762u) % ((unsigned)(2351168295u) | 1u))) | (unsigned)((~((unsigned)(u5) | 0u))))) + (unsigned)(i12))) / ((unsigned)(((unsigned)(((unsigned)(u5) == ((unsigned)(((unsigned)(779533584u) * (unsigned)(u7))) ^ cs))) ^ (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) != ((unsigned)((unsigned)(s3)) ^ cs))) ^ (unsigned)((-((unsigned)(u5) | 0u))))))) | 1u)));
        cs = csmix(cs, (unsigned)((unsigned)(s3)));
        arr9[((unsigned)(2761161164u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(1148838478u) & 31u))) / ((unsigned)(((unsigned)(arr8[((unsigned)(123731187u) & 7u)]) % ((unsigned)(713694012u) | 1u))) | 1u))) ^ (unsigned)(u7))) >> ((unsigned)(((unsigned)(4061861357u) | (unsigned)(3350298946u))) & 31u)));
      }
      i12 = (unsigned)(u5) & 0xffffffffu;
      arr8[((unsigned)(3209751160u) & 7u)] = (unsigned)(arr9[((unsigned)(u5) & 7u)]);
      g13++;
    }
  }
  arr9[((unsigned)(u5) & 7u)] = (unsigned)((-((unsigned)(((unsigned)(((unsigned)(((unsigned)(2444416804u) & (unsigned)(u5))) << ((unsigned)(arr8[((unsigned)(4017832967u) & 7u)]) & 31u))) * (unsigned)(((unsigned)(((unsigned)(2834849003u) >> ((unsigned)(arr9[((unsigned)(u6) & 7u)]) & 31u))) >> ((unsigned)(2596328393u) & 31u))))) | 0u)));
  cs = csmix(cs, (unsigned)(((unsigned)(u5) / ((unsigned)((-((unsigned)(973874076u) | 0u))) | 1u))));

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
