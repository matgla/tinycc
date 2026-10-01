/* A constant initializer materialized as a copy from .rodata
 * (block_copy_init) keeps every store at its own width.
 *
 * The pass wrote every store below 64 bits as a 32-bit word, so a byte or
 * halfword field initialized to a negative constant spilled its sign bits
 * into the next fields, which the zero fill had cleared. */
#include <stdio.h>

struct q
{
  signed char a, b, c, d;
  int e;
  short f, g;
  unsigned char h[3], i;
  int pad[20];
};

__attribute__((noinline)) static void show(const struct q *s)
{
  printf("%d %d %d %d %d %d %d %d %d %d %d %d\n", s->a, s->b, s->c, s->d, s->e, s->f, s->g, s->h[0], s->h[1],
         s->h[2], s->i, s->pad[19]);
}

int g = 5;
struct w
{
  int a, b;
  float c;
  double d;
  signed char e;
  int pad[10];
};

__attribute__((noinline)) static void show_w(const struct w *s)
{
  printf("%d %d %g %g %d %d\n", s->a, s->b, s->c, s->d, s->e, s->pad[9]);
}

int main(void)
{
  /* A load from a global ends the constant run; floats keep their bits. */
  struct w u = {g, -1, 1.5f, -2.25, -7, {[9] = 3}};
  show_w(&u);
  struct q s = {-1, 0, 0, 0, 5, -2, 0, {0xff, 0, 0}, 0, {[19] = 7}};
  show(&s);
  struct q t = {0, -3, 0, -4, 6, 0, -5, {0, 0xfe, 0}, 0x80, {[0] = 9}};
  show(&t);
  return 0;
}
