#include <stdint.h>
#include <stdio.h>

typedef struct
{
  long long a;
  long long b;
  int ge, le, gt, lt, eq, ne;
} s_case;

typedef struct
{
  unsigned long long a;
  unsigned long long b;
  int ge, le, gt, lt, eq, ne;
} u_case;

static int ge_s(long long a, long long b)
{
  return a >= b;
}
static int le_s(long long a, long long b)
{
  return a <= b;
}
static int gt_s(long long a, long long b)
{
  return a > b;
}
static int lt_s(long long a, long long b)
{
  return a < b;
}
static int eq_s(long long a, long long b)
{
  return a == b;
}
static int ne_s(long long a, long long b)
{
  return a != b;
}

static int ge_u(unsigned long long a, unsigned long long b)
{
  return a >= b;
}
static int le_u(unsigned long long a, unsigned long long b)
{
  return a <= b;
}
static int gt_u(unsigned long long a, unsigned long long b)
{
  return a > b;
}
static int lt_u(unsigned long long a, unsigned long long b)
{
  return a < b;
}
static int eq_u(unsigned long long a, unsigned long long b)
{
  return a == b;
}
static int ne_u(unsigned long long a, unsigned long long b)
{
  return a != b;
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

static int run_signed(void)
{
  const s_case cases[] = {
      {0LL, 0LL, 1, 1, 0, 0, 1, 0},
      {1LL, 0LL, 1, 0, 1, 0, 0, 1},
      {0LL, 1LL, 0, 1, 0, 1, 0, 1},
      {-1LL, 0LL, 0, 1, 0, 1, 0, 1},
      {0LL, -1LL, 1, 0, 1, 0, 0, 1},
      {-1LL, -1LL, 1, 1, 0, 0, 1, 0},
      {-9223372036854775807LL - 1LL, 0LL, 0, 1, 0, 1, 0, 1},
      {9223372036854775807LL, -9223372036854775807LL - 1LL, 1, 0, 1, 0, 0, 1},
      {(1LL << 32), (1LL << 32) + 1LL, 0, 1, 0, 1, 0, 1},
      {-(1LL << 33), -(1LL << 34), 1, 0, 1, 0, 0, 1},
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    const s_case *c = &cases[i];

    if (check1("s ge", ge_s(c->a, c->b), c->ge))
      return 1;
    if (check1("s le", le_s(c->a, c->b), c->le))
      return 1;
    if (check1("s gt", gt_s(c->a, c->b), c->gt))
      return 1;
    if (check1("s lt", lt_s(c->a, c->b), c->lt))
      return 1;
    if (check1("s eq", eq_s(c->a, c->b), c->eq))
      return 1;
    if (check1("s ne", ne_s(c->a, c->b), c->ne))
      return 1;
  }

  return 0;
}

static int run_unsigned(void)
{
  const u_case cases[] = {
      {0ULL, 0ULL, 1, 1, 0, 0, 1, 0},
      {1ULL, 0ULL, 1, 0, 1, 0, 0, 1},
      {0ULL, 1ULL, 0, 1, 0, 1, 0, 1},
      {0xffffffffULL, 0x100000000ULL, 0, 1, 0, 1, 0, 1},
      {0x100000000ULL, 0xffffffffULL, 1, 0, 1, 0, 0, 1},
      {0xffffffffffffffffULL, 0ULL, 1, 0, 1, 0, 0, 1},
      {0x8000000000000000ULL, 0x7fffffffffffffffULL, 1, 0, 1, 0, 0, 1},
      {0x7fffffffffffffffULL, 0x8000000000000000ULL, 0, 1, 0, 1, 0, 1},
  };

  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    const u_case *c = &cases[i];

    if (check1("u ge", ge_u(c->a, c->b), c->ge))
      return 1;
    if (check1("u le", le_u(c->a, c->b), c->le))
      return 1;
    if (check1("u gt", gt_u(c->a, c->b), c->gt))
      return 1;
    if (check1("u lt", lt_u(c->a, c->b), c->lt))
      return 1;
    if (check1("u eq", eq_u(c->a, c->b), c->eq))
      return 1;
    if (check1("u ne", ne_u(c->a, c->b), c->ne))
      return 1;
  }

  return 0;
}

int main(void)
{
  printf("Testing long long relational operators\n");

  if (run_signed())
    return 1;
  if (run_unsigned())
    return 1;

  printf("PASS\n");
  return 0;
}
