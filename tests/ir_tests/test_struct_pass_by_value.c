#include <stdint.h>
#include <stdio.h>

typedef struct
{
  int a;
  int b;
} Pair;

typedef struct
{
  uint8_t u8;
  int a;
  uint16_t u16;
} Mixed;

static int sum_pair(Pair p)
{
  return p.a * 1000 + p.b;
}

static int sum_two(Pair p, Pair q)
{
  return p.a + p.b + q.a + q.b;
}

static int sum_three(Pair p, int x, Pair q, int y)
{
  return p.a + x + q.b + y;
}

static int sum_mixed(Mixed m)
{
  return (int)m.u8 + m.a + (int)m.u16;
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
  printf("Testing struct pass by value\n");

  Pair p = {11, 22};
  Pair q = {1, 2};
  Pair r = {3, 4};

  if (check1("sum_pair", sum_pair(p), 11022))
    return 1;
  if (check1("sum_two", sum_two(q, r), 10))
    return 1;
  if (check1("sum_three", sum_three((Pair){5, 6}, 7, (Pair){8, 9}, 10), 31))
    return 1;

  Mixed m = {200u, 1234, 4567u};
  if (check1("sum_mixed", sum_mixed(m), 6001))
    return 1;

  printf("PASS\n");
  return 0;
}
