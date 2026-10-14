/* ra:branch_thread: a SELECT join split into per-side blocks (bt_thread_select)
 * must not share its insertion point with another join's pad copies.
 *
 * J2 = `T8 <- T18; CMP T8,#0; JUMPIF != -> J` (the narrowing copy of the
 * inlined callee's uint16_t result) threads its decided edge
 * `T18 <- #67; JMP J2` to J with the pad copy `T8 <- T18` in front of J;
 * J = `CMP T8,#0; T15 <- #200 SELECT #100` has every edge decided by the
 * twin compare.  When both were queued in front of J, the padded edge ran
 * J2's copy and fell into J's first side block (the e == 0 value), so
 * m5(3,1,5) returned 205 instead of 105.  m7 puts J before J2 (a backward
 * pad, J processed first). */
#include <stdio.h>
#include <stdint.h>

volatile unsigned sink;

struct eu
{
  uintptr_t payload;
  uint16_t error;
};

static struct eu callee(uintptr_t a, uintptr_t b)
{
  if (a > b)
    return (struct eu){.error = 67, .payload = 0xaaaaaaaau};
  sink += 2;
  return (struct eu){.error = 0, .payload = a + b};
}

static uint16_t callee3(uintptr_t a, uintptr_t b)
{
  if (a > b)
    return 67;
  sink += 2;
  return 0;
}

__attribute__((noinline)) int m3(uintptr_t a, uintptr_t b, int w)
{
  uint16_t e = callee(a, b).error;
  if (e != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
J:
  return (e != 0 ? 100 : 200) + w;
}

__attribute__((noinline)) int m4(uintptr_t a, uintptr_t b, int w)
{
  struct eu t = callee(a, b);
  uint16_t e = t.error;
  if (e != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
J:
  return (e != 0 ? 100 : 200) + w;
}

__attribute__((noinline)) int m5(uintptr_t a, uintptr_t b, int w)
{
  uint16_t e = callee3(a, b);
  if (e != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
J:
  return (e != 0 ? 100 : 200) + w;
}

__attribute__((noinline)) int m6(uintptr_t a, uintptr_t b, int w)
{
  struct eu t = callee(a, b);
  if (t.error != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
J:
  return (t.error == 0 ? 100 : 200) + w;
}

__attribute__((noinline)) int m7(uintptr_t a, uintptr_t b, int w)
{
  uint16_t e;
  goto S;
J:
  return (e != 0 ? 100 : 200) + w;
S:
  e = callee3(a, b);
  if (e != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
}

__attribute__((noinline)) int m8(uintptr_t a, uintptr_t b, int w)
{
  uint16_t e = callee3(a, b);
  if (e != 0)
    goto J;
  if (sink & 1)
    goto J;
  sink += 3;
  return -1;
J:
  return (e == 0 ? 7 : 9) + w;
}

typedef int (*fn)(uintptr_t, uintptr_t, int);

static void run(const char *name, fn f)
{
  int a, b, c;
  sink = 0;
  a = f(3, 1, 5); /* e != 0 */
  b = f(1, 3, 4); /* e == 0, sink even */
  sink = 1;
  c = f(1, 3, 1); /* e == 0, sink odd */
  printf("%s %d %d %d sink=%u\n", name, a, b, c, sink);
}

int main(void)
{
  run("m3", m3);
  run("m4", m4);
  run("m5", m5);
  run("m6", m6);
  run("m7", m7);
  run("m8", m8);
  return 0;
}
