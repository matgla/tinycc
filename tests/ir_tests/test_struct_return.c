#include <stdio.h>

typedef struct
{
  int a;
  int b;
} Pair;

typedef struct
{
  int a;
  int b;
  int c;
  int d;
} Big;

static Pair make_pair(int a, int b)
{
  Pair p = {a, b};
  return p;
}

static Big make_big(int base)
{
  Big r = {base, base + 1, base + 2, base + 3};
  return r;
}

static void fill_big(Big *out, int base)
{
  out->a = base * 10;
  out->b = base * 10 + 1;
  out->c = base * 10 + 2;
  out->d = base * 10 + 3;
}

static int check1(const char *name, int got, int exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%d exp=%d\n", name, got, exp);
    return 1;
  }
  return 0;
}

int main(void)
{
  printf("Testing struct return\n");

  Pair p = make_pair(7, 9);
  if (check1("pair.a", p.a, 7))
    return 1;
  if (check1("pair.b", p.b, 9))
    return 1;

  Big b = make_big(100);
  if (check1("big.a", b.a, 100))
    return 1;
  if (check1("big.b", b.b, 101))
    return 1;
  if (check1("big.c", b.c, 102))
    return 1;
  if (check1("big.d", b.d, 103))
    return 1;

  Big out = {0};
  fill_big(&out, 12);
  if (check1("out.a", out.a, 120))
    return 1;
  if (check1("out.b", out.b, 121))
    return 1;
  if (check1("out.c", out.c, 122))
    return 1;
  if (check1("out.d", out.d, 123))
    return 1;

  printf("PASS\n");
  return 0;
}
