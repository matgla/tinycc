/* A constant stored through a register while every register holds a live
   value.

   sret_nrvo builds make()'s result straight in *P0, so the zero fill is
   `*(P0 + 64) = 0; *(P0 + 0) = 0; ...` with P0 in r0 and r1-r3 holding the
   other parameters: no register is free.  The constant-displacement store
   took a scratch for the 0 without excluding the base, got r0 (saved and
   restored around the use), and emitted
       str r0,[sp]; movs r0,#0; str r0,[r0,#64]
   -- a store to address 64, so the result's tail word kept the caller's
   0x55 (gcc torture pr108498-1, device only). */
#include <stdio.h>
#include <string.h>

struct R
{
  unsigned char a, b;
  signed char c;
  unsigned char d;
  int w[15];
  int tail;
};

__attribute__((noinline)) static struct R make(unsigned char a, unsigned b, int c, int e, int f)
{
  struct R x = {.a = b, .c = c, .d = a, .w = {[2] = e, [9] = f}};
  return x;
}

/* Leave 0x55 in the stack the result's temporary will occupy. */
__attribute__((noinline)) void dirty(struct R *x)
{
  char buf[1024];
  memset(buf, 0x55, sizeof(buf));
  __asm__ volatile("" : : "r"(buf) : "memory");
  memset(x, 0x55, sizeof(*x));
}

int main(void)
{
  struct R x;
  dirty(&x);
  x = make(1, 2, -1, 30, 90);
  int sum = 0;
  for (int i = 0; i < 15; i++)
    sum += x.w[i];
  printf("a=%d b=%d c=%d d=%d sum=%d tail=%d\n", x.a, x.b, x.c, x.d, sum, x.tail);
  return 0;
}
