/* An asm statement that is only system instructions becomes a call the
   backend emits as those instructions (asm_machine_call_name), so the tiny
   accessors holding one inline like any leaf; and a small static loop leaf
   inlines too (inline:small_loop).  Checks both keep their meaning: the
   interrupt mask they set and read back, the "=r" output and "r" input, an
   asm the lowering must leave alone ("+r", two operands), and loop helpers
   expanded in expression context, in a loop condition and as a goto loop
   like the Zig C backend's. */
#include <stdio.h>

typedef unsigned int u32;

static inline u32 irq_save(void)
{
  u32 r;
  __asm volatile(" mrs %[ret], PRIMASK\n cpsid i" : [ret] "=r"(r)::"memory");
  return r;
}

static inline void irq_restore(u32 m)
{
  __asm volatile(" msr PRIMASK, %[mask]" ::[mask] "r"(m) : "memory");
}

static inline u32 primask(void)
{
  u32 r;
  __asm volatile("mrs %0, primask" : "=r"(r));
  return r;
}

static inline u32 basepri(void)
{
  u32 r;
  __asm volatile("mrs %0, BASEPRI" : "=r"(r));
  return r;
}

static inline void set_basepri(u32 v)
{
  __asm volatile("msr basepri, %0" ::"r"(v) : "memory");
}

static inline void hints(void)
{
  __asm volatile("sev" ::: "memory");
  __asm volatile("nop; dmb; dsb sy\n\tisb" ::: "memory");
}

/* Not lowered: a read-write operand, and two operands. */
static inline u32 twice(u32 x)
{
  __asm volatile("add %0, %0, %0" : "+r"(x));
  return x;
}

static inline u32 sum(u32 a, u32 b)
{
  u32 r;
  __asm volatile("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}

static volatile u32 counter;

__attribute__((noinline)) u32 critical(u32 add)
{
  u32 flags = irq_save();
  u32 inside = primask();
  counter += add;
  hints();
  irq_restore(flags);
  return inside;
}

/* Small loop leaves */
static int span(const char *s, char stop)
{
  int n = 0;
  while (s[n] && s[n] != stop)
    n++;
  return n;
}

static u32 spin_until(volatile u32 *p, u32 want)
{
  u32 tries = 0;
zig_loop:
  tries++;
  if (*p != want)
  {
    *p = *p + 1;
    goto zig_loop;
  }
  return tries;
}

int main(void)
{
  u32 before = primask();
  u32 inside = critical(5);
  u32 after = primask();
  printf("primask before=%u inside=%u after=%u counter=%u\n", before, inside, after, counter);

  u32 outer = irq_save();
  u32 nested = critical(2);
  u32 still = primask();
  irq_restore(outer);
  printf("nested inside=%u still=%u restored=%u counter=%u\n", nested, still, primask(), counter);

  set_basepri(0x40);
  u32 bp = basepri();
  set_basepri(0);
  printf("basepri=%#x then %#x\n", bp, basepri());
  printf("twice=%u sum=%u\n", twice(21), sum(40, 2));

  const char *a = "abcxdef", *b = "yy";
  int hits = 0;
  if (span(a, 'x') == 3 && span(b, 'q') > 1)
    hits++;
  for (int i = 0; i < span("12345", '4'); i++)
    hits += 10;
  hits += span(a, 0) + span("", 'z');
  volatile u32 v = 3;
  u32 spins = spin_until(&v, 7);
  printf("hits=%d spin=%u v=%u\n", hits, spins, v);
  return 0;
}
