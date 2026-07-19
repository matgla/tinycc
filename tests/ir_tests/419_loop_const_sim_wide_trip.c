/* ssa:loop_const_sim over the shapes the 16-trip / register-only limits used to
 * keep out — the builtin-bitops-1 class, where a bit-twiddling helper called
 * with a constant should collapse to its answer.
 *
 * Each check must hold at every -O level, so a fold that computes the wrong
 * value (rather than declining) fails here rather than silently shrinking code.
 *
 * The shapes, and the wrong-code hazards each one pins:
 *  1. 32- and 64-trip scalar loops (trip cap) reading a local counter and
 *     accumulator (the register-resident-VAR-is-not-memory rule).
 *  2. 64-bit shifts, which lower to __aeabi_llsl/llsr calls inside the loop.
 *  3. `break` out of a diamond body: the break target and the latch
 *     fall-through are the same edge with a dead NOP between them.
 *  4. a loop as the then-arm of an `if`: folding it must leave an explicit
 *     branch or control drops into the else arm.
 *  5. a loop the preheader enters below its own latch increment (rotated
 *     entry): simulating from the region's first slot would run the increment
 *     an extra time, and the residual would land in a skipped slot.
 *  6. bodies whose operands carry a fused barrel shift, which the operand
 *     itself does not show: the simulator has to apply LSL/LSR/ASR/ROR itself,
 *     including into a fused CMP, or it computes `h + v` for `h + (v << 6)`.
 */
#include <stdio.h>

volatile int vone = 1;
volatile int vzero = 0;

/* 1 + 2: the builtin-bitops-1 helpers, non-static so they stay real functions. */
int my_popcount(unsigned x)
{
  int i;
  int count = 0;
  for (i = 0; i < 32; i++)
    if (x & (1u << i))
      count++;
  return count;
}

int my_parityll(unsigned long long x)
{
  int i;
  int count = 0;
  for (i = 0; i < 64; i++)
    if (x & ((unsigned long long)1 << i))
      count++;
  return count & 1;
}

int my_popcountll(unsigned long long x)
{
  int i;
  int count = 0;
  for (i = 0; i < 64; i++)
    if (x & ((unsigned long long)1 << i))
      count++;
  return count;
}

/* 3: break out of a diamond body. */
int my_ctz(unsigned x)
{
  int i;
  for (i = 0; i < 32; i++)
    if (x & (1u << i))
      break;
  return i;
}

int my_clzll(unsigned long long x)
{
  int i;
  for (i = 0; i < 64; i++)
    if (x & ((unsigned long long)1 << (64 - i - 1)))
      break;
  return i;
}

/* 4: the loop is the then-arm; eliminating it must not fall into the else.
 * The body is empty on purpose — nothing survives the fold, so the branch is
 * all that is left to get right. */
int then_arm_loop(int cond)
{
  int r = 1;
  if (cond)
  {
    for (int i = 0; i < 20; i++)
      ;
  }
  else
  {
    r = 2;
  }
  return r;
}

/* 5: unbounded loop leaving through a data-dependent break — the rotated shape
 * whose preheader jumps past the latch increment. */
int first_set_bit(unsigned x)
{
  unsigned m = 1u;
  int i;
  for (i = 0;; i++)
  {
    if (x & m)
      break;
    m <<= 1; /* work after the break is what rotates the increment up front */
  }
  return i;
}

/* Small dedicated callers: the fold has to happen in a function whose whole
 * body is the inlined loop, which is where the residual's *placement* (rather
 * than just its value) decides the answer.  noinline keeps main from folding a
 * second copy and reading that instead of these bodies. */
__attribute__((noinline)) int fsb_const_40(void) { return first_set_bit(0x00000040u); }
__attribute__((noinline)) int fsb_const_high(void) { return first_set_bit(0x80000000u); }
__attribute__((noinline)) int then_arm_const(void) { return then_arm_loop(1); }

/* 6: `h ^= v + K + (h << 6) + (h >> 2)` — both shifts fuse into ADD operands. */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

unsigned shift_fused_chain(unsigned seed)
{
  unsigned h = seed;
  for (unsigned i = 0u; i < 12u; i++)
    h = csmix(h, i);
  return h;
}

/* One loop per shift type, each fused into a different consumer, over values
 * with the sign bit live so ASR is distinguishable from LSR.  The `_c` wrappers
 * hold the seed as a literal and are noinline, so the fold has to happen inside
 * these bodies rather than in a specialised copy in main. */
__attribute__((noinline)) int asr_add_c(void)
{
  int s = -2000000000;
  int a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a + (s >> 3);
    s = s * 3 + 1;
  }
  return a;
}

__attribute__((noinline)) unsigned lsr_xor_c(void)
{
  unsigned s = 0x9e3779b9u, a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a ^ (s >> 5);
    s = s * 2654435761u + 7u;
  }
  return a;
}

__attribute__((noinline)) unsigned lsl_sub_c(void)
{
  unsigned s = 0x80000001u, a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a - (s << 9);
    s = s * 1103515245u + 12345u;
  }
  return a;
}

__attribute__((noinline)) int asr_cmp_c(void)
{
  int s = -1234567890, n = 0;
  for (int i = 0; i < 20; i++)
  {
    if (i > (s >> 28))
      n++;
    s = s * 5 + 3;
  }
  return n;
}

int asr_add(int s)
{
  int a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a + (s >> 3);
    s = s * 3 + 1;
  }
  return a;
}

unsigned lsr_xor(unsigned s)
{
  unsigned a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a ^ (s >> 5);
    s = s * 2654435761u + 7u;
  }
  return a;
}

unsigned lsl_sub(unsigned s)
{
  unsigned a = 0;
  for (int i = 0; i < 20; i++)
  {
    a = a - (s << 9);
    s = s * 1103515245u + 12345u;
  }
  return a;
}

unsigned rot_and(unsigned s)
{
  unsigned a = 0xffffffffu;
  for (int i = 0; i < 20; i++)
  {
    a = a & ((s >> 7) | (s << 25));
    s = s * 69069u + 1u;
  }
  return a;
}

/* The shift fuses into the loop's CMP rather than into an ALU result. */
int asr_cmp(int s)
{
  int n = 0;
  for (int i = 0; i < 20; i++)
  {
    if (i > (s >> 28))
      n++;
    s = s * 5 + 3;
  }
  return n;
}

int main(void)
{
  printf("pc=%d %d %d\n", my_popcount(0xa5a5a5a5u), my_popcount(0u),
         my_popcount(0xffffffffu));
  printf("pcll=%d %d\n", my_popcountll(0xcafecafe00000000ULL),
         my_popcountll(0xffffffffffffffffULL));
  printf("parll=%d %d\n", my_parityll(0x0000000100000000ULL),
         my_parityll(0xa5a5a5a5a5a5a5a5ULL));
  printf("ctz=%d %d %d\n", my_ctz(0x00010000u), my_ctz(1u), my_ctz(0u));
  printf("clzll=%d %d\n", my_clzll(0x0000000080000000ULL),
         my_clzll(0x8000000000000000ULL));

  /* The volatile guards keep the runtime path honest against the folded one. */
  printf("then=%d else=%d\n", then_arm_loop(vone), then_arm_loop(vzero));
  printf("fsb=%d %d\n", first_set_bit(0x00000040u), first_set_bit(0x80000000u));
  printf("fsbc=%d %d thc=%d\n", fsb_const_40(), fsb_const_high(), then_arm_const());
  printf("mix=%08x\n", shift_fused_chain(0x12345678u));
  printf("sh=%d %u %u %u %d\n", asr_add(-2000000000), lsr_xor(0x9e3779b9u),
         lsl_sub(0x80000001u), rot_and(0xdeadbeefu), asr_cmp(-1234567890));
  printf("shc=%d %u %u %d\n", asr_add_c(), lsr_xor_c(), lsl_sub_c(), asr_cmp_c());
  printf("shr=%d %u %u %u %d\n", asr_add((int)vone * 7), lsr_xor((unsigned)vone),
         lsl_sub((unsigned)vone * 3u), rot_and((unsigned)vone * 5u),
         asr_cmp((int)vone * 11));

  /* Same helpers with runtime arguments: the fold must agree with execution. */
  {
    unsigned rv = (unsigned)vone * 0xa5a5a5a5u;
    unsigned long long rl = (unsigned long long)vone * 0xcafecafe00000000ULL;
    printf("rt=%d %d %d %d\n", my_popcount(rv), my_popcountll(rl),
           my_ctz(rv), my_clzll(rl));
  }
  return 0;
}
