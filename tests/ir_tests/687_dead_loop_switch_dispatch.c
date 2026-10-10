/* dead_loop: a counting loop left empty by copy_fwd (the copy into the
   unused `l4` deleted) is killed by turning its header into a jump to its
   exit.  The switch dispatch is laid out after the case bodies, between the
   loop's back-edge and that exit, and was taken for loop body and deleted
   with it -- `case 1` never ran.  The body is the natural loop of the
   back-edge, not the instructions up to the exit target. */
#include <stdio.h>

typedef struct { unsigned a[8]; } B;
static B G;
static unsigned acc;

__attribute__((noinline)) unsigned sum(const B *b)
{
  unsigned s = 0;
  for (int i = 0; i < 8; i++)
    s = s * 3u + b->a[i];
  return s;
}

/* Constant trip count: the loop is deleted outright. */
__attribute__((noinline)) void h4(unsigned k, B *pp)
{
  B r = *pp;
  switch (k % 4u) {
  case 1:
    for (int i = 0; i < 8; i++)
      r.a[i] += i * 276u;
    break;
  default:
    for (unsigned j = 0; j < 4u; j++) {
      B l4 = G;
      (void)l4;
    }
    break;
  }
  acc += sum(&r);
}

/* Trip count unknown: the loop becomes a select on its entry compare, and
   the count it leaves behind is read after the switch. */
__attribute__((noinline)) unsigned h5(unsigned k, unsigned n, B *pp)
{
  B r = *pp;
  unsigned last = 100;
  switch (k % 3u) {
  case 0:
    for (int i = 0; i < 8; i++)
      r.a[i] ^= 0x55u + i;
    break;
  case 2:
    for (unsigned j = 0; j < n; j++) {
      B l5 = G;
      (void)l5;
      last = 7;
    }
    break;
  default:
    r.a[3] += 9;
    break;
  }
  return sum(&r) + last;
}

int main(void)
{
  B Z;
  for (int i = 0; i < 8; i++)
    Z.a[i] = 7u + i;
  for (unsigned k = 0; k < 4; k++) {
    acc = 0;
    h4(213u + k, &Z);
    printf("h4 %u acc %u\n", k, acc);
  }
  for (unsigned k = 0; k < 3; k++)
    for (unsigned n = 0; n < 2; n++)
      printf("h5 %u %u -> %u\n", k, n, h5(k, n, &Z));
  return 0;
}
