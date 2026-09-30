/* Frame objects inside loops: an object fully overwritten before each use in
 * an iteration (struct result, block copy, a store per field) carries nothing
 * round the back edge, so objects of different switch cases may share bytes.
 * One carried across iterations, read before it is rewritten, or only partly
 * rewritten, must not.  All locals at the top of the function, as the Zig C
 * backend writes them. */
#include <stdio.h>

struct quad
{
  int v[4];
};

struct pair
{
  int a, b;
  short c;
};

__attribute__((noinline)) static struct quad make_quad(int x)
{
  struct quad q = {{x, x + 1, x + 2, x + 3}};
  return q;
}

__attribute__((noinline)) static struct pair make_pair(int x)
{
  struct pair p = {x * 3, x * 5, (short)(x * 7)};
  return p;
}

__attribute__((noinline)) static int sum_quad(const struct quad *q)
{
  return q->v[0] + q->v[1] + q->v[2] + q->v[3];
}

__attribute__((noinline)) static int dispatch(int n)
{
  struct quad carried;
  struct quad partial;
  struct quad by_call;
  struct quad by_fields;
  struct quad copy;
  struct pair pr;
  struct pair pr2;
  int total = 0;

  carried = make_quad(100);
  partial = make_quad(200);
  for (int i = 0; i < n; i++)
  {
    switch (i % 4)
    {
    case 0:
      by_call = make_quad(i);
      total += by_call.v[0] * 3 + by_call.v[3];
      break;
    case 1:
      by_fields.v[0] = i;
      by_fields.v[1] = i * 2;
      by_fields.v[2] = i * 3;
      by_fields.v[3] = i * 4;
      total += sum_quad(&by_fields);
      break;
    case 2:
      pr = make_pair(i);
      pr2 = pr;
      total += pr2.a - pr2.b + pr2.c;
      break;
    default:
      copy = carried;
      total += copy.v[i % 4] - copy.v[0];
      break;
    }
    /* read before rewrite: the previous iteration's value */
    total += carried.v[i % 4];
    if (i % 5 == 4)
      carried = make_quad(i * 10);
    /* only partly rewritten: v[1..3] come from before the loop */
    total += partial.v[(i + 1) % 4];
    partial.v[0] = i;
  }
  return total;
}

int main(void)
{
  printf("%d %d %d\n", dispatch(1), dispatch(7), dispatch(40));
  return 0;
}
