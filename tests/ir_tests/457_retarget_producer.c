/* Post-allocation producer retargeting (source/ir/regalloc.c,
 * ra_retarget_producer).
 *
 * The pass runs after register allocation is frozen.  It takes a
 * register-to-register copy whose source was computed by the instruction
 * immediately before it, makes that instruction write the copy's DESTINATION
 * register instead, deletes the copy, and rewrites whatever reads of the
 * source outlive it.  Every part of that names physical registers, so a
 * mistake is a silent wrong-value miscompile and not a crash.
 *
 * `fadd` is the shape it exists for and the reason this file is a whole
 * soft-float addition rather than a handful of small functions: the copies it
 * removes only appear under enough register pressure that the source and the
 * copy's destination land in different registers, and the smaller renderings
 * of the same algorithm are all allocated without a copy at all.  Verified to
 * exercise the pass: `TCC_DISABLE_PASS=ra:retarget_producer` compiles this
 * file to 12 more bytes of .text than without it.
 *
 * The rest pin the refusals.  `from_call` is a producer whose destination
 * register is fixed by the ABI; `joined` puts a label between the producer and
 * the copy, so control can reach the copy without the producer having run;
 * `through_ptr` reads the source again as a store address, which is a read the
 * rewrite does not cover; `redefined` gives the destination a second value
 * underneath a read that would be redirected onto it; `pair` is the 64-bit
 * register-pair form, where a destination owning only its low half aborts the
 * encoder; `volatile_src` is an access that may not be moved or duplicated.
 * Like the refusals in 445, these are documentation of the hazard rather than
 * tests with teeth -- each one still passes with its guard removed, because
 * nothing in the corpus yet produces the shape that guard is there for. */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef unsigned int u32;

#define SGN (1ULL << 63)
#define IMP (1ULL << 52)
#define MANT (IMP - 1)

static inline int dsign(u64 b) { return (int)((b >> 63) & 1); }
static inline int dexp(u64 b) { return (int)((b >> 52) & 0x7FF); }
static inline u64 dmant(u64 b) { return b & MANT; }
static inline u64 mk(int s, int e, u64 m)
{
  return ((u64)(s & 1) << 63) | ((u64)(e & 0x7FF) << 52) | (m & MANT);
}

static u64 round_pack(int sign, int exp, u64 mant)
{
  int e = exp;
  u64 m = mant;
  while (m >= (IMP << 4)) { u64 lsb = m & 1; m = (m >> 1) | lsb; e++; }
  while (m && m < (IMP << 3) && e > 1) { m <<= 1; e--; }
  if (e >= 0x7FF) return mk(sign, 0x7FF, 0);
  u64 rem = m & 7;
  m >>= 3;
  if (rem > 4 || (rem == 4 && (m & 1))) { m++; if (m >= (IMP << 1)) { m >>= 1; e++; } }
  if (m < IMP) e = 0;
  return mk(sign, e, m);
}

u64 fadd(u64 a_bits, u64 b_bits)
{
  int a_sign = dsign(a_bits), b_sign = dsign(b_bits);
  int a_exp = dexp(a_bits), b_exp = dexp(b_bits);
  u64 a_mant = dmant(a_bits), b_mant = dmant(b_bits);
  const int a_max = (a_exp == 0x7FF), b_max = (b_exp == 0x7FF);

  if (a_max && a_mant) return a_bits;
  if (b_max && b_mant) return b_bits;
  if (a_max) { if (b_max && a_sign != b_sign) return 0x7FF8000000000000ULL; return a_bits; }
  if (b_max) return b_bits;
  {
    const int az = (!a_exp && !a_mant), bz = (!b_exp && !b_mant);
    if (az && bz) return (a_sign && b_sign) ? SGN : 0;
    if (az) return b_bits;
    if (bz) return a_bits;
  }
  if (a_exp != 0) a_mant |= IMP; else a_exp = 1;
  if (b_exp != 0) b_mant |= IMP; else b_exp = 1;

  int diff = a_exp - b_exp, rexp, rsign;
  u64 rmant;
  a_mant <<= 3;
  b_mant <<= 3;
  if (diff > 0) {
    if (diff < 64) { u64 lost = b_mant & ((1ULL << diff) - 1); b_mant = (b_mant >> diff) | (lost != 0); }
    else b_mant = (b_mant != 0);
    rexp = a_exp;
  } else if (diff < 0) {
    if (-diff < 64) { u64 lost = a_mant & ((1ULL << -diff) - 1); a_mant = (a_mant >> -diff) | (lost != 0); }
    else a_mant = (a_mant != 0);
    rexp = b_exp;
  } else rexp = a_exp;

  if (a_sign == b_sign) { rmant = a_mant + b_mant; rsign = a_sign; }
  else {
    if (a_mant >= b_mant) { rmant = a_mant - b_mant; rsign = a_sign; }
    else { rmant = b_mant - a_mant; rsign = b_sign; }
    if (rmant == 0) return 0;
  }
  return round_pack(rsign, rexp, rmant);
}

/* --- the refusals ------------------------------------------------------- */

/* The producer's destination is the ABI return register: not retargetable. */
static unsigned scale(unsigned v) { return v * 3u + 1u; }

static unsigned from_call(unsigned v)
{
  unsigned a = scale(v);
  unsigned b = a;
  return b + (a & 1u);
}

/* A label between the producer and the copy: on the other edge into `mid` the
 * copy is the only thing that establishes the destination. */
static unsigned joined(unsigned v, int take)
{
  unsigned t = v ^ 0x5A5Au;
  unsigned r;
  if (take)
    goto mid;
  t = v + 7u;
mid:
  r = t;
  return r + (t >> 3);
}

/* The source is read again as the address of a store, which is a DEST operand
 * and so a read the rewrite would leave behind. */
static unsigned through_ptr(unsigned *base, unsigned v)
{
  unsigned *p = base + (v & 3u);
  unsigned *q = p;
  *p = v;
  return (unsigned)(q - base) + *q;
}

/* The destination gets a second value while a redirected read is still due. */
static unsigned redefined(unsigned v, int flip)
{
  unsigned a = v * 5u;
  unsigned b = a;
  if (flip)
    b = 0xFFu;
  return b + (a & 0xFu);
}

/* 64-bit values travel in register pairs. */
static u64 pair(u64 x)
{
  u64 a = x * 0x1000000001ULL;
  u64 b = a;
  return b + (a >> 40);
}

/* A volatile read is an access, not a value to be moved around. */
static volatile unsigned vsrc = 0x1234;

static unsigned volatile_src(void)
{
  unsigned a = vsrc;
  unsigned b = a;
  return b + (a & 7u);
}

int main(void)
{
  static const u64 v[] = {
      0x400921FB54442D18ULL, 0x3FF0000000000000ULL, 0xC00921FB54442D18ULL,
      0x0008000000000000ULL, 0x0004000000000000ULL, 0x7FF0000000000000ULL,
      0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0x0000000000000000ULL,
      0x8000000000000000ULL, 0x0000000000000001ULL, 0x7FEFFFFFFFFFFFFFULL,
  };
  const int n = (int)(sizeof v / sizeof v[0]);
  for (int i = 0; i < n; i++)
  {
    printf("row%d=", i);
    for (int j = 0; j < n; j++)
      printf("%llx,", (unsigned long long)fadd(v[i], v[j]));
    printf("\n");
  }

  printf("call=%u,%u\n", from_call(7u), from_call(8u));
  printf("joined=%u,%u\n", joined(9u, 0), joined(9u, 1));

  unsigned buf[8];
  for (int i = 0; i < 8; i++)
    buf[i] = 0;
  printf("ptr=%u,%u\n", through_ptr(buf, 2u), through_ptr(buf, 5u));

  printf("redef=%u,%u\n", redefined(6u, 0), redefined(6u, 1));
  printf("pair=%llx\n", (unsigned long long)pair(0x0123456789ABCDEFULL));
  printf("vol=%u\n", volatile_src());
  printf("OK\n");
  return 0;
}
