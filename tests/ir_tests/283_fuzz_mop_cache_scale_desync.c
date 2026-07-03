/*
 * ptr fuzz seed 30436 reduction (O1-only): the two-pass codegen's MopArgs
 * cache skipped instructions with scale/accum specs (LOAD_INDEXED /
 * STORE_INDEXED / MLA), so those re-decoded in the real run.  The decode-time
 * ASSIGN-coalesce peephole (ir_codegen_before_ret_peephole) PATCHES interval
 * allocations, and dry-run patches persist into the real run: here the
 * LOAD_INDEXED's following ASSIGN dest (T89) was still spilled when the
 * dry-run decoded the load (peephole declined), but the dry-run's later
 * ASSIGN-decode retargeted T89 to R8 — so the real-run re-decode of the load
 * fired the peephole after all, retargeting T59 into R8, while the ASSIGN's
 * cached dry-run operands still read T59's pre-patch register R12: emitted
 * `ldr r8, [...]` immediately clobbered by `mov r8, ip`, so arr8[i16&7]
 * was replaced by the TEST_ZERO scratch (0).
 * Fixed by caching scale/accum-spec decodes too, so the real run replays the
 * dry run's decode decisions (ir_decode_cached in ir/codegen.c).
 * Ground truth (tcc -O0 == gcc -O2): checksum=bb867f8a.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(405807398u) % ((unsigned)(3002424721u) | 1u))) << ((unsigned)(pb) & 31u))) % ((unsigned)(((unsigned)(((unsigned)(pa) ^ (unsigned)(lr))) << ((unsigned)(3612512006u) & 31u))) | 1u)));
  return (unsigned)(pa) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(45477325u & 0xffffffff);
  unsigned u4 = 275187542u;
  unsigned u5 = 1727806255u;
  unsigned u6 = 268753008u;
  unsigned u7 = 1422246273u;
  unsigned arr8[8] = { 1612021655u, 347327211u, 1138037545u, 1500924845u, 2783605838u, 836610805u, 1673471923u, 3614254362u };
  unsigned arr9[8] = { 2544569449u, 1765602049u, 2652642385u, 1448374864u, 329509033u, 991004681u, 2792221231u, 1746559747u };
  unsigned *p10 = &arr8[((unsigned)(u7) & 7u)];
  unsigned *p11 = &u5;
  unsigned *p12 = &u5;
  struct S st13 = { 3504837804u, 3012195217u, 3534867558u };

  u7 = (unsigned)(st13.f0) & 0xffffffffu;
  { unsigned g15 = 0u;
    while (g15 < 1u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((-((unsigned)(u6) | 0u))) | (unsigned)(((unsigned)(i14) >> ((unsigned)(((unsigned)(u5) % ((unsigned)(678615488u) | 1u))) & 31u))))) * (unsigned)((((unsigned)(helper1(((unsigned)(3333299853u) | (unsigned)(3547538991u)), ((unsigned)(u4) + (unsigned)(3362965387u)))) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(2972237243u))))));
      { unsigned g17 = 0u;
        while (g17 < 6u) {
          unsigned i16 = g17;
          cs = csmix(cs, i16);
          cs = csmix(cs, *p11);
          st13.f2 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(359064789u) % ((unsigned)((*p10)) | 1u))) | 0u))) > ((unsigned)(arr9[((unsigned)(u6) & 7u)]) ^ cs))) & (unsigned)((((unsigned)((-((unsigned)(((unsigned)(u6) < ((unsigned)((*p11)) ^ cs))) | 0u))) & 1u) ? (unsigned)(i14) : (unsigned)((-((unsigned)((-((unsigned)(arr8[((unsigned)(i16) & 7u)]) | 0u))) | 0u)))))));
          cs = csmix(cs, (unsigned)((unsigned)(s2)));
          g17++;
        }
      }
      g15++;
    }
  }

  cs = csmix(cs, st13.f2);
  cs = csmix(cs, *p12);
  printf("checksum=%08x\n", cs);
  return 0;
}
