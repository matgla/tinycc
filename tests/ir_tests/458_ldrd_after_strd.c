/* The encoder's frame-slot reload cache, reaching the 64-bit pair forms
 * (source/backend/arch/arm/thumb/arm-thumb-gen.c,
 * strldr_cache_ldrd_is_redundant).
 *
 * A `strd rA,rB,[slot]` immediately followed by `ldrd rA,rB,[slot]` loads the
 * registers with what they already hold, so the load is not emitted.  Every
 * soft-float helper ends with that round trip -- `ur.u = <64-bit value>;
 * return ur.d;` writes the union's slot from a register pair and reads it
 * straight back into the return pair -- and it accounted for 13,613 executed
 * no-op LDRDs across the five double benchmarks, all of which this removes.
 *
 * `addd` is that shape and is why this file is a whole soft-float addition:
 * the round trip needs real `double` parameters and a union written in many
 * branches, and the smaller renderings never produce a `strd` at all.
 * Verified to exercise it: an `armv8m-tcc` whose
 * strldr_cache_ldrd_is_redundant() returns 0 unconditionally compiles this
 * file to 8 more bytes of .text.  The corpus files that also reach it are
 * 235_fuzz_retval_reg_share_store_ptr, 252_fuzz_knownbits_imm_subword_sext,
 * 263_fuzz_scratch_push_sp_offset and 270_fuzz_dse_mla_accum_deref.
 *
 * The rest are the cases where a skipped load would return the value from
 * before something changed it: the slot written through a pointer, written by
 * a callee handed its address, written one word at a time so only half the
 * pair goes stale, and read back through a volatile lvalue.  `swapped` reloads
 * the pair into the opposite registers, which moves real data.  Every case
 * prints what it read, so a wrong elision is a wrong number, not a crash.
 *
 * Values are carried as bit patterns and printed as bit patterns so nothing
 * here depends on printf's float formatting. */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef unsigned int u32;

#define IMP (1ULL << 52)

static double from_bits(u64 b)
{
  union { double d; u64 u; } v;
  v.u = b;
  return v.d;
}

static u64 to_bits(double d)
{
  union { double d; u64 u; } v;
  v.d = d;
  return v.u;
}

double addd(double a, double b)
{
  union { double d; u64 u; } ua, ub, ur;
  ua.d = a; ub.d = b;
  u64 ab = ua.u, bb = ub.u;
  int as = (int)(ab>>63), bs = (int)(bb>>63);
  int ae = (int)((ab>>52)&0x7FF), be = (int)((bb>>52)&0x7FF);
  u64 am = ab & (IMP-1), bm = bb & (IMP-1);
  const int amx = (ae==0x7FF), bmx = (be==0x7FF);
  if (amx && am) { ur.u = ab; return ur.d; }
  if (bmx && bm) { ur.u = bb; return ur.d; }
  if (amx) { if (bmx && as!=bs) { ur.u = 0x7FF8000000000000ULL; return ur.d; } ur.u = ab; return ur.d; }
  if (bmx) { ur.u = bb; return ur.d; }
  if (!ae && !am) { if (!be && !bm) { ur.u = (as&&bs) ? (1ULL<<63) : 0; return ur.d; } ur.u = bb; return ur.d; }
  if (!be && !bm) { ur.u = ab; return ur.d; }
  if (ae) am |= IMP; else ae = 1;
  if (be) bm |= IMP; else be = 1;
  int d = ae - be, re;
  u64 rm; int rs;
  am <<= 3; bm <<= 3;
  if (d > 0) { if (d<64) { u64 l = bm & ((1ULL<<d)-1); bm = (bm>>d)|(l!=0);} else bm = (bm!=0); re = ae; }
  else if (d < 0) { if (-d<64) { u64 l = am & ((1ULL<<-d)-1); am = (am>>-d)|(l!=0);} else am = (am!=0); re = be; }
  else re = ae;
  if (as==bs) { rm = am+bm; rs = as; }
  else { if (am>=bm) { rm = am-bm; rs = as; } else { rm = bm-am; rs = bs; } if (!rm) { ur.u = 0; return ur.d; } }
  while (rm >= (IMP<<4)) { rm = (rm>>1)|(rm&1); re++; }
  while (rm && rm < (IMP<<3) && re > 1) { rm <<= 1; re--; }
  rm >>= 3;
  ur.u = ((u64)(rs&1)<<63) | ((u64)(re&0x7FF)<<52) | (rm & (IMP-1));
  return ur.d;
}

/* --- the slot must be reloaded when something changed it ----------------- */

static void poke(u64 *p) { *p = 0xDEADBEEFCAFEF00DULL; }

static u64 through_call(u64 x)
{
  u64 slot = x + 1ULL;
  poke(&slot);
  return slot;
}

static u64 through_ptr(u64 x, int rewrite)
{
  u64 slot = x + 2ULL;
  u64 *p = &slot;
  if (rewrite)
    *p = ~x;
  return slot;
}

/* Only the HIGH word goes stale; the low half of the pair is still current. */
static u64 half_stale(u64 x)
{
  u64 slot = x + 3ULL;
  u32 *w = (u32 *)&slot;
  w[1] = 0x12345678u;
  return slot;
}

static u64 volatile_read(u64 x)
{
  volatile u64 slot = x + 4ULL;
  u64 a = slot;
  u64 b = slot;
  return a + b;
}

/* The reload lands in the other order, so it moves real data. */
static u64 swapped(u64 x)
{
  union { u64 u; u32 w[2]; } v;
  v.u = x + 5ULL;
  return ((u64)v.w[0] << 32) | (u64)v.w[1];
}

static u64 restore_between(u64 x, int flip)
{
  u64 slot = x + 6ULL;
  u64 keep = slot;
  if (flip)
    slot = keep ^ 0xFFFFFFFFFFFFFFFFULL;
  return slot + (keep >> 8);
}

int main(void)
{
  static const u64 v[] = {
      0x3FF0000000000000ULL, 0x4002000000000000ULL, 0xBFF8000000000000ULL,
      0x0000000000000000ULL, 0x8000000000000000ULL, 0x7FF0000000000000ULL,
      0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0x0008000000000000ULL,
      0x0000000000000001ULL, 0x7FEFFFFFFFFFFFFFULL, 0x400921FB54442D18ULL,
  };
  const int n = (int)(sizeof v / sizeof v[0]);
  for (int i = 0; i < n; i++)
  {
    printf("add%d=", i);
    for (int j = 0; j < n; j++)
      printf("%llx,", (unsigned long long)to_bits(addd(from_bits(v[i]), from_bits(v[j]))));
    printf("\n");
  }

  for (int i = 0; i < n; i++)
  {
    u64 x = v[i];
    printf("slot%d: call=%llx ptr=%llx,%llx half=%llx vol=%llx swap=%llx res=%llx,%llx\n", i,
           (unsigned long long)through_call(x), (unsigned long long)through_ptr(x, 0),
           (unsigned long long)through_ptr(x, 1), (unsigned long long)half_stale(x),
           (unsigned long long)volatile_read(x), (unsigned long long)swapped(x),
           (unsigned long long)restore_between(x, 0), (unsigned long long)restore_between(x, 1));
  }
  printf("OK\n");
  return 0;
}
