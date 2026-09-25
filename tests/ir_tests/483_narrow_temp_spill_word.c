/* A byte or halfword temporary that the register allocator spills is stored
   to its slot as a word, because every reload of that slot is a word load.
   The STORE handler wrote it with its own width, so the reload picked up the
   slot's three stale bytes: gcc_execute/pr82524 computed `foo(y->c.b, w)`
   from two such operands and got 0x55 instead of 0xff for the blue channel.

   bar() is pr82524's; enough values are live to spill.  scribble() leaves
   0xA5 in the stack bar reuses, so the stale bytes are not zero on any
   target. */
#include <stdio.h>

struct S
{
  unsigned char b, g, r, a;
};
union U
{
  struct S c;
  unsigned v;
};

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned char junk[256];
  for (int i = 0; i < 256; i++)
    junk[i] = 0xA5;
}

static inline unsigned char foo(unsigned char a, unsigned char b)
{
  return ((a + 1) * b) >> 8;
}

__attribute__((noinline)) unsigned bar(union U *x, union U *y)
{
  union U z;
  unsigned char v = x->c.a;
  unsigned char w = foo(y->c.a, 255 - v);
  z.c.r = foo(x->c.r, v) + foo(y->c.r, w);
  z.c.g = foo(x->c.g, v) + foo(y->c.g, w);
  z.c.b = foo(x->c.b, v) + foo(y->c.b, w);
  z.c.a = 0;
  return z.v;
}

/* The same shape with halfwords and signed bytes. */
static inline short hfoo(unsigned short a, unsigned short b)
{
  return (short)(((a + 1) * b) >> 16);
}

__attribute__((noinline)) int hbar(unsigned short *x, unsigned short *y)
{
  unsigned short v = x[3];
  unsigned short w = (unsigned short)hfoo(y[3], 65535 - v);
  short r = (short)(hfoo(x[2], v) + hfoo(y[2], w));
  short g = (short)(hfoo(x[1], v) + hfoo(y[1], w));
  short b = (short)(hfoo(x[0], v) + hfoo(y[0], w));
  return r + g * 3 + b * 7;
}

__attribute__((noinline)) int sbar(signed char *x, signed char *y)
{
  signed char v = x[3];
  signed char w = (signed char)(y[3] - v);
  int r = (x[2] + 1) * v + (y[2] + 1) * w;
  int g = (x[1] + 1) * v + (y[1] + 1) * w;
  int b = (x[0] + 1) * v + (y[0] + 1) * w;
  return r + g * 3 + b * 7;
}

int main(void)
{
  union U a, b;
  a.c = (struct S){255, 255, 255, 0};
  /* Not 255: (255 + 1) * w >> 8 is w's own low byte, whatever its stale
     upper bytes hold, which is how pr82524's inputs hid the bug on a host. */
  b.c = (struct S){200, 150, 100, 255};
  scribble();
  printf("%08x\n", bar(&a, &b));

  unsigned short ha[4] = {65535, 65535, 65535, 0}, hb[4] = {65535, 65535, 65535, 65535};
  scribble();
  printf("%d\n", hbar(ha, hb));

  signed char sa[4] = {-3, 5, -7, 2}, sb[4] = {4, -6, 8, -9};
  scribble();
  printf("%d\n", sbar(sa, sb));
  return 0;
}
