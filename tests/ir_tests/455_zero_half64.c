/* Known-zero halves of 64-bit values (source/opt/flat/fusion/zero_half64.c).
 *
 * The pass tells codegen two things: that a 64-bit operand's low or high word
 * is provably the constant zero, so a bitwise op can skip reading it, and that
 * a half no consumer reads need not be WRITTEN.  The second half of that is
 * what removes instructions -- and it is what makes a mistake silent: codegen
 * simply never writes a register, and whatever the allocator left there is
 * read as the value.  There is nothing to crash on, so every kernel below
 * prints its result and the expected output comes from host gcc.
 *
 * `pack` is the shape the whole pass exists for: make_double in
 * lib/fp/soft/soft_common.h, where two widening casts each materialise a zero
 * high word, two shifts by >= 32 each materialise a zero low word, and three
 * ORs then read all four of them back.
 *
 * The three refusals matter more than the rewrites:
 *
 *  - `imm_operand` -- against a literal the emitter takes its own per-half
 *    peephole, and every arm of it but `AND #0` reads the register the pass
 *    would have declared unwritten.  Getting this wrong miscompiled
 *    bug_ll_shift_ptr_clobber and 394_fuzz_barrel_shift_imm_remat_drop.
 *  - `narrow_dest` -- a 32-bit destination sends the same opcode to the
 *    32-bit lowering, which reads the low words outright.
 *  - `two_defs` -- a value assigned twice is not the one the pass analysed.
 *    The first version of the pass decided multi-def as it walked, so a
 *    conclusion drawn from a value that later turned out to be reassigned
 *    survived; the census now runs first.  `addr_taken` is the same hazard
 *    reached through a pointer.
 */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef unsigned u32;

volatile u64 sink;

/* make_double, verbatim in shape: widening cast, shift by >= 32, OR chain. */
static u64 pack(int sign, int exp, u64 mant)
{
  return ((u64)(sign & 1) << 63) | ((u64)(exp & 0x7FF) << 52) | (mant & 0xFFFFFFFFFFFFFULL);
}

/* Zero halves that must survive into a compare rather than being dropped. */
static int lsb_rem(u64 mant)
{
  u64 lsb = (mant >> 3) & 1;
  u64 rem = mant & 7;
  if (rem > 4 || (rem == 4 && lsb))
    return 1;
  return 0;
}

/* AND/OR/XOR against a LITERAL: the emitter reads rn on almost every arm, so
 * no half of the source may be treated as unwritten. */
static u64 imm_operand(u32 x)
{
  u64 w = (u64)x;             /* high word zero */
  u64 a = w | 0xFF00000000ULL; /* OR literal: reads the zero high word */
  u64 b = w & 0xFFFFFFFFFFULL; /* AND literal: reads it too */
  u64 c = w ^ 0x00000000FFULL;
  return a + b + c;
}

/* A 32-bit destination: the low words are read outright by the 32-bit path. */
static u32 narrow_dest(u32 x, u64 y)
{
  u64 w = (u64)x;
  return (u32)(w | y) + (u32)(w & y);
}

/* Reassigned: the value reaching the OR is not the one a single-def scan sees. */
static u64 two_defs(u64 seed, int go)
{
  u64 bits = 0; /* looks entirely zero here */
  if (go)
    bits = seed | 0x300000004ULL;
  return 0xF0F0ULL | (bits & ~1ULL);
}

/* Carry ops: a zero high word on SRC2 becomes `adc/sbc rd, rn, #0`, so the
 * zero is never materialised.  src1 must NOT get the same treatment -- SUB is
 * not commutative -- and every dispatch path has to carry the annotation or
 * the emitter reads a register the producer was told not to write.  `flagged`
 * takes the flag-setting path (an ALU result branched on directly). */
static u64 carry_add(u64 a, u32 b) { return a + (u64)b; }
static u64 carry_sub(u64 a, u32 b) { return a - (u64)b; }
static u64 carry_rsub(u32 a, u64 b) { return (u64)a - b; }
static u64 carry_chain(u64 a, u32 b, u32 c)
{
  u64 t = a + (u64)b;
  t = t - (u64)c;
  return t + ((u64)b << 32) - (u64)(b & 0xFF);
}
static int flagged(u64 a, u32 b)
{
  if ((a - (u64)b) != 0)
    return 1;
  if ((a + (u64)b) == 0)
    return 2;
  return 3;
}

static void take(u64 *p) { *p |= 0x7000000000ULL; }

/* Address handed out: a store through the pointer the pass cannot follow. */
static u64 addr_taken(u64 seed, int go)
{
  u64 v = 0;
  if (go)
    take(&v);
  return (v & seed) | 0x11ULL;
}

/* Shift by >= 32 either way, feeding a bitwise op -- the producer rules. */
static u64 shift_chain(u32 lo, u32 hi)
{
  u64 a = (u64)hi << 32; /* low word zero */
  u64 b = (u64)lo;       /* high word zero */
  u64 c = a | b;
  u64 d = (c >> 32) & 0xFFFFFFFFULL; /* high word zero again */
  return c ^ d;
}

int main(void)
{
  static const u64 seeds[] = {0ULL,
                              1ULL,
                              7ULL,
                              8ULL,
                              0x000FFFFFFFFFFFFFULL,
                              0x00000000FFFFFFFFULL,
                              0x0000000080000000ULL,
                              0xFFFFFFFFFFFFFFFFULL,
                              0x0123456789ABCDEFULL};
  unsigned i;

  for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
  {
    u64 s = seeds[i];
    u32 lo = (u32)s, hi = (u32)(s >> 32);

    printf("pack   %d %016llx\n", i, (unsigned long long)pack((int)lo, (int)hi, s));
    printf("lsbrem %d %d\n", i, lsb_rem(s));
    printf("imm    %d %016llx\n", i, (unsigned long long)imm_operand(lo));
    printf("narrow %d %08x\n", i, (unsigned)narrow_dest(lo, s));
    printf("twodef %d %016llx %016llx\n", i, (unsigned long long)two_defs(s, 0),
           (unsigned long long)two_defs(s, 1));
    printf("addr   %d %016llx %016llx\n", i, (unsigned long long)addr_taken(s, 0),
           (unsigned long long)addr_taken(s, 1));
    printf("shift  %d %016llx\n", i, (unsigned long long)shift_chain(lo, hi));
    printf("carry  %d %016llx %016llx %016llx\n", i, (unsigned long long)carry_add(s, lo),
           (unsigned long long)carry_sub(s, lo), (unsigned long long)carry_rsub(lo, s));
    printf("chain  %d %016llx %d\n", i, (unsigned long long)carry_chain(s, lo, hi), flagged(s, lo));
    sink = s;
  }
  return 0;
}
