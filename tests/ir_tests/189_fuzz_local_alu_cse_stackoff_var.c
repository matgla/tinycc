/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=202).
 * tcc_ir_opt_local_alu_cse reused a stale (pb XOR lr) after lr was reassigned: the redefinition kill ignored STACKOFF-lval VAR-read keys (fix: ir/opt_copyprop.c).
 * tcc -O0 was always correct; the bug appeared at -O1/-O2.  Expected checksum
 * is gcc -m32 -funsigned-char (ARM ABI: unsigned char, 32-bit long).
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
  lr = (unsigned)(((unsigned)(((unsigned)(1400125997u) - (unsigned)(3731634844u))) << ((unsigned)(((unsigned)(((unsigned)(3233404506u) - (unsigned)(pb))) | (unsigned)(pa))) & 31u)));
  lr = (unsigned)(((unsigned)(pa) & (unsigned)(535909986u)));
  lr = (unsigned)(((unsigned)(((unsigned)((((unsigned)(4187868031u) & 1u) ? (unsigned)(1283041921u) : (unsigned)(pb))) >> ((unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)(pb) ^ lr)) & 31u))) & 31u))) << ((unsigned)(((unsigned)((((unsigned)(2166308221u) & 1u) ? (unsigned)(pa) : (unsigned)(2522259232u))) >> ((unsigned)(((unsigned)(3167608334u) != ((unsigned)(lr) ^ lr))) & 31u))) & 31u)));
  lr = (unsigned)(((unsigned)(((unsigned)(1888545375u) | (unsigned)(((unsigned)(lr) ^ (unsigned)(pb))))) % ((unsigned)(((unsigned)(3457748423u) | (unsigned)(((unsigned)(2131611745u) - (unsigned)(1576556136u))))) | 1u)));
  if ((unsigned)(((unsigned)(pa) * (unsigned)(((unsigned)(pb) / ((unsigned)(2592405011u) | 1u))))) & 1u) lr += (unsigned)(pb);
  return (unsigned)(((unsigned)(84480709u) >> ((unsigned)(((unsigned)((-((unsigned)(lr) | 0u))) - (unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(pa) : (unsigned)(818078360u))))) & 31u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(((unsigned)(lr) >> ((unsigned)(((unsigned)(lr) ^ lr)) & 31u))) / ((unsigned)(((unsigned)(1997572364u) / ((unsigned)(2033586945u) | 1u))) | 1u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(3892112403u) << ((unsigned)(1295302507u) & 31u))) >> ((unsigned)(pa) & 31u)));
  if ((unsigned)(pb) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(pa) | (unsigned)(165476416u))) | (unsigned)((-((unsigned)(pa) | 0u)))));
  if ((unsigned)(1935933804u) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(pb) / ((unsigned)(2981356973u) | 1u))) | (unsigned)(1488662773u)));
  if ((unsigned)(((unsigned)(3711150725u) % ((unsigned)(((unsigned)(1479638094u) << ((unsigned)(3978762565u) & 31u))) | 1u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) << ((unsigned)(pa) & 31u))) >> ((unsigned)((-((unsigned)(pb) | 0u))) & 31u)));
  lr = (unsigned)(((unsigned)(1582096348u) << ((unsigned)(((unsigned)(((unsigned)(3205156985u) ^ (unsigned)(lr))) >> ((unsigned)(3601652833u) & 31u))) & 31u)));
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(3846456377u) % ((unsigned)(3129697379u) | 1u))) > ((unsigned)(((unsigned)(856472114u) >> ((unsigned)(lr) & 31u))) ^ lr))) - (unsigned)(((unsigned)(pa) | (unsigned)(((unsigned)(1569479078u) + (unsigned)(pb))))))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((((unsigned)((~((unsigned)(((unsigned)(lr) * (unsigned)(pa))) | 0u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(pa) - (unsigned)(pb))) << ((unsigned)(((unsigned)(1179291337u) >> ((unsigned)(pa) & 31u))) & 31u))) : (unsigned)(((unsigned)(((unsigned)(pa) * (unsigned)(465104030u))) / ((unsigned)((((unsigned)(lr) & 1u) ? (unsigned)(3592801430u) : (unsigned)(2347280377u))) | 1u)))));
  lr = (unsigned)((~((unsigned)(2551145396u) | 0u)));
  lr = (unsigned)((~((unsigned)(((unsigned)(((unsigned)(pb) >> ((unsigned)(101722552u) & 31u))) - (unsigned)(((unsigned)(2491393385u) & (unsigned)(pa))))) | 0u)));
  return (unsigned)(4013183934u) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  long s4 = (long)(1884814561u & 0xffffffff);
  int s5 = (int)(1882157229u & 0xffffffff);
  unsigned u6 = 4046928364u;
  unsigned u7 = 3170584092u;
  unsigned u8 = 210787422u;
  unsigned u9 = 2904710333u;
  unsigned u10 = 125621611u;
  unsigned u11 = 629705118u;
  unsigned arr12[8] = { 1286357478u, 2558054388u, 2348200908u, 2206775503u, 222410174u, 4014194726u, 738050136u, 1650029853u };
  struct S st13 = { 4063017281u, 68077474u, 795228074u };

  u11 = (unsigned)(((unsigned)(u7) * (unsigned)(st13.f1))) & 0xffffffffu;
  { unsigned g15 = 0u;
    while (g15 < 10u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)((-((unsigned)(2427360190u) | 0u))));
      cs = csmix(cs, (unsigned)(((unsigned)(i14) ^ (unsigned)(u9))));
      { unsigned g17 = 0u;
        while (g17 < 4u) {
          unsigned i16 = g17;
          cs = csmix(cs, i16);
          u10 = (unsigned)(st13.f2) & 0xffffffffu;
          arr12[((unsigned)(u10) & 7u)] = (unsigned)(arr12[((unsigned)(2972996477u) & 7u)]);
          st13.f2 = (unsigned)((~((unsigned)(((unsigned)(arr12[((unsigned)(3012478137u) & 7u)]) ^ (unsigned)(((unsigned)((~((unsigned)(4122937160u) | 0u))) & (unsigned)(((unsigned)((unsigned)(s5)) % ((unsigned)(arr12[((unsigned)(817370965u) & 7u)]) | 1u))))))) | 0u)));
          u9 = (unsigned)((~((unsigned)(((unsigned)(((unsigned)(((unsigned)(u11) * (unsigned)(st13.f1))) ^ (unsigned)(((unsigned)(1426931710u) & (unsigned)(3344514891u))))) * (unsigned)(st13.f2))) | 0u))) & 0xffffffffu;
          arr12[((unsigned)(u10) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(1330021002u) | 0u))) << ((unsigned)(st13.f2) & 31u))) ^ (unsigned)(((unsigned)(u10) - (unsigned)(((unsigned)((unsigned)(s5)) ^ (unsigned)(u11))))))) - (unsigned)(((unsigned)(((unsigned)(((unsigned)(u11) << ((unsigned)(st13.f2) & 31u))) % ((unsigned)(((unsigned)(509310997u) + (unsigned)(arr12[((unsigned)(2536192200u) & 7u)]))) | 1u))) % ((unsigned)(1194223657u) | 1u)))));
          cs = csmix(cs, (unsigned)(3113411926u));
          g17++;
        }
      }
      cs = csmix(cs, (unsigned)(((unsigned)(st13.f0) << ((unsigned)(((unsigned)(st13.f2) & (unsigned)(((unsigned)(arr12[((unsigned)(u8) & 7u)]) >> ((unsigned)(((unsigned)((unsigned)(s5)) & (unsigned)(st13.f1))) & 31u))))) & 31u))));
      cs = csmix(cs, (unsigned)((unsigned)(s5)));
      i14 = (unsigned)((((unsigned)(arr12[((unsigned)(u10) & 7u)]) & 1u) ? (unsigned)(4244586759u) : (unsigned)(((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(1002560295u) + (unsigned)(u8))))) % ((unsigned)(u9) | 1u))))) & 0xffffffffu;
      g15++;
    }
  }
  u9 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(((unsigned)(arr12[((unsigned)(1944385604u) & 7u)]) << ((unsigned)(arr12[((unsigned)(u11) & 7u)]) & 31u))) != ((unsigned)(((unsigned)(u7) % ((unsigned)(arr12[((unsigned)(2724235348u) & 7u)]) | 1u))) ^ cs))) | (unsigned)(((unsigned)(3269980328u) << ((unsigned)(((unsigned)(1641530586u) & (unsigned)(u7))) & 31u))))) & 1u) ? (unsigned)((~((unsigned)(((unsigned)(((unsigned)(st13.f2) <= ((unsigned)(arr12[((unsigned)(u8) & 7u)]) ^ cs))) | (unsigned)(((unsigned)((unsigned)(s5)) | (unsigned)(1925848465u))))) | 0u))) : (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(999779758u) | 1u))) << ((unsigned)(u9) & 31u))) - (unsigned)(((unsigned)(((unsigned)(st13.f2) / ((unsigned)(1525001778u) | 1u))) + (unsigned)(((unsigned)(st13.f0) - (unsigned)(u7))))))))) & 0xffffffffu;
  st13.f1 = (unsigned)((~((unsigned)((-((unsigned)(((unsigned)(arr12[((unsigned)(3732509691u) & 7u)]) | (unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(u11))))) | 0u))) | 0u)));
  u10 = (unsigned)(((unsigned)(u7) / ((unsigned)(((unsigned)(((unsigned)(u8) | (unsigned)(((unsigned)(arr12[((unsigned)(1360164601u) & 7u)]) - (unsigned)(arr12[((unsigned)(u10) & 7u)]))))) + (unsigned)(((unsigned)(((unsigned)(652944607u) % ((unsigned)(u6) | 1u))) | (unsigned)(((unsigned)(u11) + (unsigned)(u9))))))) | 1u))) & 0xffffffffu;
  arr12[((unsigned)(2284525137u) & 7u)] = (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)(4038474464u) >> ((unsigned)(st13.f0) & 31u))) & 1u) ? (unsigned)(1372782414u) : (unsigned)(((unsigned)((unsigned)(s5)) * (unsigned)(st13.f2))))) ^ (unsigned)(((unsigned)(helper2((unsigned)(s5), 756658781u)) << ((unsigned)(((unsigned)(u8) & (unsigned)(st13.f1))) & 31u))))) & (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) & (unsigned)(((unsigned)(u6) >> ((unsigned)(st13.f0) & 31u))))) | (unsigned)(((unsigned)(st13.f1) >> ((unsigned)(st13.f2) & 31u)))))));

  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr12[k]);
  cs = csmix(cs, st13.f0);
  cs = csmix(cs, st13.f1);
  cs = csmix(cs, st13.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
