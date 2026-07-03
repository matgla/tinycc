/* Regression: known_bits (ir/opt_knownbits.c) re-applied sub-word width
 * extension to an IMMEDIATE operand while const-folding a binary op, corrupting
 * an `unsigned char` value.
 *
 * From combo fuzz seed 1053 (verbatim).  tcc -O0/-O1/-Os agreed with the
 * gcc -m32 -funsigned-char oracle (checksum=6516cc61); only tcc -O2 diverged
 * (ea36cc61).  Culprit pass (bisect: TCC_DISABLE_PASS=known_bits restores it):
 * known_bits, exposed by store-load-fwd + const-prop.
 *
 * Root cause: struct SB5 has an `unsigned char b`; sbh5() returns `r.a = p.b`
 * with p.b = 208.  At -O2 the byte value is forwarded/tracked as an immediate
 * whose is_unsigned flag was dropped (btype=INT8, is_unsigned=0).  When
 * kb_operand_const_u64 read that immediate it called kb_apply_const_width,
 * which sign-extended the low byte 0xd0 to -48 -- so the final csmix folded
 * `v + K` with v = -48 (0xffffffd0) instead of 208.  An immediate already holds
 * its actual signed/unsigned value in u.imm32, so it must be read raw; only
 * memory loads model sub-word extension.  Fix: read immediates raw in
 * kb_operand_const_u64.
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
  if ((unsigned)(((unsigned)((~((unsigned)(pa) | 0u))) >> ((unsigned)(864087384u) & 31u))) & 1u) lr += (unsigned)(pb);
  lr = (unsigned)(pb);
  if ((unsigned)(((unsigned)(((unsigned)(lr) ^ (unsigned)(2182666289u))) / ((unsigned)(((unsigned)(669727483u) >> ((unsigned)(pb) & 31u))) | 1u))) & 1u) lr += (unsigned)(((unsigned)(1785835700u) / ((unsigned)(((unsigned)(pa) + (unsigned)(lr))) | 1u)));
  lr = (unsigned)((-((unsigned)((-((unsigned)(1714278713u) | 0u))) | 0u)));
  return (unsigned)(3536014470u) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(1485302935u) & 1u) lr += (unsigned)(pb);
  lr = (unsigned)((((unsigned)(((unsigned)(758438458u) >> ((unsigned)(1213492653u) & 31u))) & 1u) ? (unsigned)((((unsigned)(pa) & 1u) ? (unsigned)(184083171u) : (unsigned)(((unsigned)(pb) * (unsigned)(((unsigned)(pb) ^ lr)))))) : (unsigned)(3277100657u)));
  lr = (unsigned)(lr);
  lr = (unsigned)(((unsigned)((((unsigned)(pa) & 1u) ? (unsigned)(((unsigned)(pa) ^ (unsigned)(((unsigned)(pa) ^ lr)))) : (unsigned)(((unsigned)(3018676079u) + (unsigned)(pa))))) * (unsigned)(pb)));
  lr = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(lr) << ((unsigned)(1795672494u) & 31u))) | 0u))) ^ (unsigned)(343393703u)));
  return (unsigned)((~((unsigned)(178213984u) | 0u))) ^ lr;
}

struct SB1 { unsigned char a; };

struct SB4 { unsigned a; };

struct SB5 { unsigned a; unsigned char b; };

struct SB8 { unsigned a; unsigned b; };

union UB { unsigned w; unsigned char b; };

static struct SB4 sbh3(struct SB5 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(((unsigned)((~((unsigned)(p.a) | 0u))) > ((unsigned)((~((unsigned)((((unsigned)(p.b) & 1u) ? (unsigned)(2704463789u) : (unsigned)(1374305466u))) | 0u))) ^ x))) & 0xffffffffu;
  return r;
}

static struct SB1 sbh4(struct SB8 p, unsigned x)
{
  struct SB1 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffu };
  r.a = (unsigned)(((unsigned)(((unsigned)(((unsigned)(889129803u) | (unsigned)(2319579035u))) * (unsigned)(((unsigned)(x) - (unsigned)(4095471698u))))) >> ((unsigned)(((unsigned)(((unsigned)(p.b) << ((unsigned)(1247476842u) & 31u))) ^ (unsigned)((~((unsigned)(1634082534u) | 0u))))) & 31u))) & 0xffu;
  return r;
}

static struct SB4 sbh5(struct SB5 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(2344609350u) & 0xffffffffu;
  r.a = (unsigned)(p.b) & 0xffffffffu;
  return r;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

struct BF {
  unsigned b0 : 11;
  unsigned b1 : 5;
  unsigned b2 : 5;
};

#pragma pack(push, 1)
struct BFP {
  unsigned b0 : 2;
  unsigned b1 : 8;
  unsigned b2 : 7;
  unsigned b3 : 6;
} __attribute__((packed));
#pragma pack(pop)

int main(void)
{
  unsigned cs = 0x12345678u;
  char s6 = (char)(56784264u & 0xff);
  unsigned u7 = 1370092613u;
  unsigned u8 = 3413812048u;
  unsigned *p9 = &u8;
  unsigned *p10 = &u8;
  struct S st11 = { 3143615363u, 2824222432u, 426586370u };
  struct BF bf12 = { 0u, 0u, 0u };
  struct BF bf13 = { 0u, 0u, 0u };

  { struct SB5 sba14 = { (unsigned)((*p10)) & 0xffffffffu, (unsigned)(((unsigned)(((unsigned)(st11.f1) != ((unsigned)((*p9)) ^ cs))) % ((unsigned)(3015075024u) | 1u))) & 0xffu };
    struct SB4 sbt15 = sbh3(sba14, (unsigned)(helper1(((unsigned)(((unsigned)(u7) % ((unsigned)((unsigned)(s6)) | 1u))) ^ (unsigned)(u8)), (*p10))));
    cs = csmix(cs, sbt15.a);
  }
  { unsigned sel16 = (unsigned)(((unsigned)((unsigned)(s6)) / ((unsigned)((*p10)) | 1u))) & 63u;
    switch (sel16) {
    case 0:
      cs = csmix(cs, 2971763966u);
      break;
    case 1:
      { union UB ub17; ub17.w = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) % ((unsigned)((*p9)) | 1u))) | (unsigned)(((unsigned)(u8) + (unsigned)(2779704705u))))) % ((unsigned)(((unsigned)(((unsigned)(u7) * (unsigned)(u8))) | (unsigned)(((unsigned)(1949219607u) / ((unsigned)((*p10)) | 1u))))) | 1u))); cs = csmix(cs, ub17.w); }
      cs = csmix(cs, 3952321968u);
      break;
    case 12:
      cs = csmix(cs, 877874004u);
      break;
    case 17:
      bf13.b1 = (unsigned)(u7) & ((1u << 5) - 1u);
      cs = csmix(cs, 3253657851u);
      break;
    case 25:
      { unsigned g18 = (unsigned)(((unsigned)(133952560u) * (unsigned)(u7))) & 1u;
        if (g18) goto L1;
        cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(st11.f0) % ((unsigned)((unsigned)(s6)) | 1u))) * (unsigned)(u8))) | 0u))) <= ((unsigned)(u7) ^ cs))));
      L1:;
        cs = csmix(cs, 207u); }
      { union UB ub19; ub19.w = (unsigned)(((unsigned)(st11.f2) << ((unsigned)(((unsigned)(((unsigned)(u8) - (unsigned)(st11.f2))) - (unsigned)((*p10)))) & 31u))); cs = csmix(cs, ub19.w); }
      cs = csmix(cs, 3625318651u);
      break;
    case 37:
      for (unsigned g21 = 0u; g21 < 1u; g21++) {
        unsigned i20 = g21;
        cs = csmix(cs, i20);
        cs = csmix(cs, (unsigned)(helper2(((unsigned)(2333472437u) * (unsigned)(u8)), u7)));
      }
      bf13.b0 = (unsigned)(((unsigned)(((unsigned)((*p9)) - (unsigned)((~((unsigned)(helper2(3862539940u, u8)) | 0u))))) % ((unsigned)((unsigned)(s6)) | 1u))) & ((1u << 11) - 1u);
      cs = csmix(cs, 1006343385u);
      break;
    default: cs = csmix(cs, 179u); break;
    } }
  { unsigned sel22 = (unsigned)(u8) & 63u;
    switch (sel22) {
    case 2:
      cs = csmix(cs, 3616001954u);
      break;
    case 21:
      cs = csmix(cs, 3225521097u);
      break;
    case 26:
      cs = csmix(cs, 3809877311u);
      break;
    case 32:
      u8 = (unsigned)(helper1(((unsigned)(u7) - (unsigned)(((unsigned)(st11.f0) - (unsigned)(((unsigned)(2334499144u) - (unsigned)(st11.f2)))))), 4203621497u)) & 0xffffffffu;
      cs = csmix(cs, 3240440765u);
      break;
    case 46:
      { struct SB5 sba23 = { (unsigned)(((unsigned)(helper1(1991764850u, (unsigned)(s6))) << ((unsigned)(((unsigned)(2833857322u) & (unsigned)(3426312052u))) & 31u))) & 0xffffffffu, (unsigned)(helper2(((unsigned)(4106019086u) * (unsigned)(st11.f1)), u8)) & 0xffu };
        struct SB4 sbt24 = sbh5(sba23, (unsigned)((((unsigned)(((unsigned)((*p9)) - (unsigned)(((unsigned)(1263626877u) + (unsigned)(u7))))) & 1u) ? (unsigned)(((unsigned)(u8) >> ((unsigned)((((unsigned)((-((unsigned)(u8) | 0u))) & 1u) ? (unsigned)(((unsigned)(1223018769u) << ((unsigned)((*p9)) & 31u))) : (unsigned)(((unsigned)(3057409830u) * (unsigned)(1756973933u))))) & 31u))) : (unsigned)((((unsigned)(u8) & 1u) ? (unsigned)(((unsigned)(st11.f1) / ((unsigned)(((unsigned)(st11.f0) * (unsigned)((unsigned)(s6)))) | 1u))) : (unsigned)(helper1(((unsigned)(u8) * (unsigned)(1861051699u)), ((unsigned)((unsigned)(s6)) << ((unsigned)(((unsigned)((unsigned)(s6)) ^ cs)) & 31u)))))))));
        cs = csmix(cs, sbt24.a);
      }
      { unsigned g26 = 0u;
        while (g26 < 2u) {
          unsigned i25 = g26;
          cs = csmix(cs, i25);
          bf12.b1 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(i25) % ((unsigned)((unsigned)(s6)) | 1u))) | (unsigned)(((unsigned)(st11.f1) - (unsigned)(((unsigned)(st11.f2) / ((unsigned)(2893932870u) | 1u))))))) % ((unsigned)(((unsigned)(((unsigned)((*p9)) << ((unsigned)(helper1(266374842u, 2151777774u)) & 31u))) - (unsigned)(st11.f2))) | 1u))) & ((1u << 5) - 1u);
          g26++;
        }
      }
      cs = csmix(cs, 1444261104u);
      break;
    case 47:
      { union UB ub27; ub27.w = (unsigned)(((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)(((unsigned)(st11.f0) & (unsigned)(2150068790u))) : (unsigned)((-((unsigned)(2761452948u) | 0u))))) ^ (unsigned)(((unsigned)((*p9)) >> ((unsigned)(((unsigned)(u8) / ((unsigned)(841297011u) | 1u))) & 31u))))); cs = csmix(cs, ub27.w); }
      cs = csmix(cs, 1768339354u);
      break;
    default: cs = csmix(cs, 3u); break;
    } }
  if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st11.f2) - (unsigned)((unsigned)(s6)))) ^ (unsigned)(3890964394u))) + (unsigned)(2234605239u))) <= ((unsigned)((unsigned)(s6)) ^ cs))) & 1u) {
    cs = csmix(cs, (unsigned)((*p9)));
    for (unsigned g29 = 0u; g29 < 11u; g29++) {
      unsigned i28 = g29;
      cs = csmix(cs, i28);
      st11.f0 = (unsigned)(((unsigned)(((unsigned)((*p9)) & (unsigned)(((unsigned)((~((unsigned)((*p9)) | 0u))) == ((unsigned)(((unsigned)(u8) % ((unsigned)((unsigned)(s6)) | 1u))) ^ cs))))) % ((unsigned)(u7) | 1u)));
    }
    { unsigned g30 = (unsigned)(((unsigned)((((unsigned)(helper1(((unsigned)((unsigned)(s6)) * (unsigned)((*p9))), ((unsigned)(3752221437u) - (unsigned)((*p9))))) & 1u) ? (unsigned)((~((unsigned)(((unsigned)(u7) & (unsigned)((unsigned)(s6)))) | 0u))) : (unsigned)(((unsigned)((*p10)) - (unsigned)((((unsigned)(st11.f2) & 1u) ? (unsigned)(st11.f1) : (unsigned)((unsigned)(s6)))))))) == ((unsigned)((unsigned)(s6)) ^ cs))) & 1u;
      if (g30) goto L2;
      cs = csmix(cs, (unsigned)(u7));
      cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(((unsigned)((*p9)) << ((unsigned)(u8) & 31u))) / ((unsigned)((~((unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)(u7) ^ cs)) | 1u))) | 0u))) | 1u))) | 0u))));
      cs = csmix(cs, (unsigned)(((unsigned)(helper2(((unsigned)(((unsigned)(u7) >> ((unsigned)((*p10)) & 31u))) | (unsigned)(((unsigned)(st11.f2) % ((unsigned)(u8) | 1u)))), ((unsigned)(((unsigned)((unsigned)(s6)) * (unsigned)(2279608994u))) - (unsigned)(st11.f2)))) | (unsigned)(u8))));
    L2:;
      cs = csmix(cs, 74u); }
  } else {
    { struct SB8 sba31 = { (unsigned)(st11.f1) & 0xffffffffu, (unsigned)(2947916872u) & 0xffffffffu };
      struct SB1 sbt32 = sbh4(sba31, (unsigned)(u7));
      cs = csmix(cs, sbt32.a);
    }
    { unsigned g34 = 0u;
      while (g34 < 11u) {
        unsigned i33 = g34;
        cs = csmix(cs, i33);
        cs = csmix(cs, (unsigned)(st11.f1));
        u8 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s6)) ^ (unsigned)((*p9)))) < ((unsigned)(u8) ^ cs))) ^ (unsigned)((((unsigned)(1849358195u) & 1u) ? (unsigned)(st11.f0) : (unsigned)(((unsigned)(i33) % ((unsigned)(st11.f0) | 1u))))))) & (unsigned)(((unsigned)(st11.f0) + (unsigned)((~((unsigned)((-((unsigned)(3566090600u) | 0u))) | 0u))))))) & 0xffffffffu;
        bf13.b0 = (unsigned)((((unsigned)(3348209757u) & 1u) ? (unsigned)(((unsigned)(1621197313u) * (unsigned)(((unsigned)((*p9)) < ((unsigned)(u8) ^ cs))))) : (unsigned)(((unsigned)((-((unsigned)(((unsigned)((unsigned)(s6)) << ((unsigned)(u7) & 31u))) | 0u))) * (unsigned)(u8))))) & ((1u << 11) - 1u);
        u8 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(u8) % ((unsigned)((unsigned)(s6)) | 1u))) | 0u))) - (unsigned)(3244939268u))) - (unsigned)(((unsigned)((~((unsigned)(((unsigned)((unsigned)(s6)) << ((unsigned)(u7) & 31u))) | 0u))) - (unsigned)(((unsigned)((*p10)) + (unsigned)(helper1(u8, 1447291546u)))))))) & 0xffffffffu;
        g34++;
      }
    }
    u7 = (unsigned)(((unsigned)(u7) > ((unsigned)((((unsigned)(((unsigned)(((unsigned)(st11.f0) ^ (unsigned)(1115852186u))) >> ((unsigned)(((unsigned)((*p10)) & (unsigned)(4250852513u))) & 31u))) & 1u) ? (unsigned)((*p10)) : (unsigned)((((unsigned)((-((unsigned)((unsigned)(s6)) | 0u))) & 1u) ? (unsigned)((~((unsigned)(1246327732u) | 0u))) : (unsigned)(((unsigned)((*p9)) << ((unsigned)(u8) & 31u))))))) ^ cs))) & 0xffffffffu;
    if ((unsigned)(((unsigned)(((unsigned)(u8) / ((unsigned)((-((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)((*p9)) : (unsigned)(st11.f2))) | 0u))) | 1u))) / ((unsigned)(1535970775u) | 1u))) & 1u) {
      { struct SB5 sba35 = { (unsigned)(((unsigned)(((unsigned)((*p9)) | (unsigned)(u8))) ^ (unsigned)(((unsigned)(3519091049u) & (unsigned)((*p9)))))) & 0xffffffffu, (unsigned)((~((unsigned)(((unsigned)((*p10)) & (unsigned)(((unsigned)((*p10)) ^ cs)))) | 0u))) & 0xffu };
        struct SB4 sbt36 = sbh3(sba35, (unsigned)(((unsigned)(((unsigned)(u8) - (unsigned)(st11.f0))) | (unsigned)(((unsigned)((-((unsigned)((-((unsigned)(3471541990u) | 0u))) | 0u))) & (unsigned)(u8))))));
        cs = csmix(cs, sbt36.a);
      }
      u7 = (unsigned)((~((unsigned)(u7) | 0u))) & 0xffffffffu;
      *p10 = (unsigned)(((unsigned)(u8) - (unsigned)((-((unsigned)((~((unsigned)(((unsigned)((*p9)) >> ((unsigned)((*p10)) & 31u))) | 0u))) | 0u)))));
      cs = csmix(cs, *p9);
      *p9 = (unsigned)(st11.f2);
      cs = csmix(cs, *p10);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(st11.f0) & 31u))) >> ((unsigned)(((unsigned)(3600433973u) - (unsigned)(3800364923u))) & 31u))) >> ((unsigned)((unsigned)(s6)) & 31u))) << ((unsigned)((-((unsigned)((unsigned)(s6)) | 0u))) & 31u))));
    }
  }
  if ((unsigned)((~((unsigned)(u8) | 0u))) & 1u) {
    { unsigned g38 = 0u;
      while (g38 < 7u) {
        unsigned i37 = g38;
        cs = csmix(cs, i37);
        u7 = (unsigned)((*p9)) & 0xffffffffu;
        { struct SB5 sba39 = { (unsigned)(((unsigned)(((unsigned)(2892101232u) + (unsigned)(st11.f0))) > ((unsigned)(helper2(u7, (*p9))) ^ cs))) & 0xffffffffu, (unsigned)(((unsigned)(1944101403u) / ((unsigned)((unsigned)(s6)) | 1u))) & 0xffu };
          struct SB4 sbt40 = sbh3(sba39, (unsigned)(((unsigned)((*p9)) * (unsigned)(458981269u))));
          cs = csmix(cs, sbt40.a);
        }
        { union UB ub41; ub41.w = (unsigned)((-((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)(((unsigned)(st11.f1) | (unsigned)((unsigned)(s6)))) : (unsigned)(4036020240u))) | 0u))); cs = csmix(cs, ub41.w); }
        bf12.b2 = (unsigned)(st11.f2) & ((1u << 5) - 1u);
        i37 = (unsigned)(helper2((*p10), 2418812704u)) & 0xffffffffu;
        cs = csmix(cs, (unsigned)((unsigned)(s6)));
        g38++;
      }
    }
    { unsigned g42 = (unsigned)(((unsigned)(((unsigned)(4002371237u) ^ (unsigned)(st11.f2))) - (unsigned)((((unsigned)(762004493u) & 1u) ? (unsigned)(3231571488u) : (unsigned)(((unsigned)(((unsigned)(st11.f1) << ((unsigned)(2170335114u) & 31u))) - (unsigned)(((unsigned)(3647691925u) % ((unsigned)((unsigned)(s6)) | 1u))))))))) & 1u;
      if (g42) goto L3;
      cs = csmix(cs, (unsigned)(3592550810u));
      cs = csmix(cs, (unsigned)(3074124518u));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((*p10)) * (unsigned)(st11.f2))) ^ (unsigned)(helper1(((unsigned)(st11.f1) | (unsigned)(u7)), (-((unsigned)((unsigned)(s6)) | 0u)))))) << ((unsigned)((~((unsigned)(((unsigned)(((unsigned)((unsigned)(s6)) >> ((unsigned)(st11.f2) & 31u))) | (unsigned)(u8))) | 0u))) & 31u))));
    L3:;
      cs = csmix(cs, 112u); }
    bf13.b1 = (unsigned)(u7) & ((1u << 5) - 1u);
    { union UB ub43; ub43.w = (unsigned)((((unsigned)(((unsigned)(2369626164u) <= ((unsigned)(((unsigned)(1458327011u) | (unsigned)(u8))) ^ cs))) & 1u) ? (unsigned)((*p10)) : (unsigned)(((unsigned)(((unsigned)(809868623u) % ((unsigned)(4233118856u) | 1u))) & (unsigned)((((unsigned)((unsigned)(s6)) & 1u) ? (unsigned)((*p10)) : (unsigned)(st11.f2))))))); cs = csmix(cs, ub43.w); }
    cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(u7) ^ (unsigned)(((unsigned)(((unsigned)(890021896u) >> ((unsigned)(st11.f0) & 31u))) % ((unsigned)(1167862260u) | 1u))))) & 1u) ? (unsigned)(((unsigned)((*p10)) & (unsigned)(((unsigned)(((unsigned)((unsigned)(s6)) % ((unsigned)(((unsigned)((unsigned)(s6)) ^ cs)) | 1u))) ^ (unsigned)(((unsigned)(3351749979u) / ((unsigned)(u8) | 1u))))))) : (unsigned)(((unsigned)((-((unsigned)(u7) | 0u))) % ((unsigned)((unsigned)(s6)) | 1u))))));
    u7 = (unsigned)(((unsigned)(2839216393u) % ((unsigned)((-((unsigned)((((unsigned)(((unsigned)((unsigned)(s6)) ^ (unsigned)((*p10)))) & 1u) ? (unsigned)(1302568480u) : (unsigned)(((unsigned)((unsigned)(s6)) << ((unsigned)(4205085618u) & 31u))))) | 0u))) | 1u))) & 0xffffffffu;
  }

  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s6);
  cs = csmix(cs, st11.f0);
  cs = csmix(cs, st11.f1);
  cs = csmix(cs, st11.f2);
  cs = csmix(cs, bf12.b0);
  cs = csmix(cs, bf12.b1);
  cs = csmix(cs, bf12.b2);
  cs = csmix(cs, bf13.b0);
  cs = csmix(cs, bf13.b1);
  cs = csmix(cs, bf13.b2);
  cs = csmix(cs, *p9);
  cs = csmix(cs, *p10);
  { struct SB5 sba44 = { 1u, 2u };
    struct SB4 sbt45 = sbh3(sba44, cs);
    cs = csmix(cs, sbt45.a); }
  { struct SB8 sba46 = { 19088744u, 19088745u };
    struct SB1 sbt47 = sbh4(sba46, cs);
    cs = csmix(cs, sbt47.a); }
  { struct SB5 sba48 = { 38177487u, 208u };
    struct SB4 sbt49 = sbh5(sba48, cs);
    cs = csmix(cs, sbt49.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
