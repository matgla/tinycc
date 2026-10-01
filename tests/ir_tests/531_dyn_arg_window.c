/* A call passing more stack-argument bytes than the frame reserves drops SP
   for itself alone (`sub sp` ... `add sp`); the other calls use the reserved
   area.  Everything addressed off SP in between -- locals feeding the
   arguments, a struct copied by value, spilled values, the return value's
   home -- has to see the moved SP, and the arguments land at the new one.
   scribble() fills the stack so a word read from the wrong slot shows. */
#include <stdio.h>
#include <string.h>

struct big
{
  unsigned w[40]; /* 160 bytes by value: past the reserved area */
};

struct small
{
  unsigned a, b;
};

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned char junk[4096];
  for (unsigned i = 0; i < sizeof junk; i++)
    junk[i] = (unsigned char)(0xA5 ^ i);
}

__attribute__((noinline)) unsigned many(unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f,
                                        unsigned g, unsigned h, unsigned i, unsigned j)
{
  return a + 2 * b + 3 * c + 5 * d + 7 * e + 11 * f + 13 * g + 17 * h + 19 * i + 23 * j;
}

__attribute__((noinline)) unsigned take_big(unsigned x, struct big b, unsigned y, struct small s, unsigned z)
{
  unsigned h = x ^ y ^ z ^ s.a ^ (s.b << 1);
  for (unsigned k = 0; k < 40; k++)
    h = h * 31 + b.w[k];
  return h;
}

__attribute__((noinline)) unsigned host(unsigned k)
{
  struct big b;
  struct small s = {k * 3, k + 7};
  unsigned loc[24];
  for (unsigned i = 0; i < 24; i++)
    loc[i] = i * k + 1;
  for (unsigned i = 0; i < 40; i++)
    b.w[i] = i ^ (k << 3);
  /* a small stack-argument call: uses the reserved area */
  unsigned r = many(loc[0], loc[1], loc[2], loc[3], loc[4], loc[5], loc[6], loc[7], loc[8], loc[9]);
  /* the big one: its own window; its arguments read locals through SP */
  r += take_big(loc[10] + r, b, loc[11], s, loc[12] ^ r);
  /* the frame is intact afterwards */
  for (unsigned i = 0; i < 24; i++)
    r = r * 7 + loc[i];
  r += many(r, loc[13], loc[14], loc[15], loc[16], loc[17], loc[18], loc[19], loc[20], s.a);
  return r ^ s.b;
}

int main(void)
{
  unsigned total = 0;
  for (unsigned k = 0; k < 5; k++)
  {
    scribble();
    unsigned r = host(k);
    printf("k=%u r=%u\n", k, r);
    total += r;
  }
  printf("total=%u\n", total);
  return 0;
}
