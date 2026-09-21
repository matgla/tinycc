/* Dead-store elimination runs on functions with 64-bit values.  It used to
 * give up on any function with a double or long long operand; the stores into
 * objects read through a pointer, a field, or at a partial width must stay,
 * and a 64-bit temporary no one reads may go. */
#include <stdio.h>
#include <string.h>

struct S
{
  double a, b, *c;
  unsigned long long d;
};

__attribute__((noinline)) static double sum(const struct S *s)
{
  return s->a + s->b + s->c[0] + s->c[3] + (double)s->d;
}

__attribute__((noinline)) static long long low_word(const long long *p)
{
  int w;
  memcpy(&w, p, sizeof w); /* reads only part of the object */
  return w;
}

__attribute__((noinline)) static double f(int k)
{
  double c[4] = {1.5, 2.5, 3.5, 4.5};
  struct S s;
  long long dead = 12345678901LL * k; /* never read */
  long long half = 0x100000002LL * k;
  s.a = k;
  s.b = 0.25;
  s.c = c;
  s.d = 1ULL << 40;
  (void)dead;
  return sum(&s) + (double)low_word(&half);
}

int main(void)
{
  printf("%.2f %.2f\n", f(1), f(3));
  return 0;
}
