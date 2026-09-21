/* Dead-store elimination bounds a read through an address by the object the
 * address points into (dse.c, tcc_ir_frame_object_at).  The stores into an
 * object read only through a pointer -- passed to a call, indexed at run time,
 * reached from an interior pointer -- must stay; the objects around a large
 * one whose address is taken must keep theirs too. */
#include <stdio.h>

struct big
{
  int v[700]; /* past the 2 KiB the byte set can mark */
};

struct pair
{
  int a, b;
};

__attribute__((noinline)) static int read_at(const int *p, int i)
{
  return p[i];
}

__attribute__((noinline)) static int sum_pair(const struct pair *p)
{
  return p->a * 10 + p->b;
}

__attribute__((noinline)) static int f(int k)
{
  int before[4];
  struct big b;
  struct pair pr;
  int after[3];

  for (int i = 0; i < 4; i++)
    before[i] = 100 + i;
  for (int i = 0; i < 700; i++)
    b.v[i] = i * 3;
  pr.a = k;
  pr.b = k + 1;
  after[0] = 7;
  after[1] = 8;
  after[2] = 9;

  /* An interior pointer reaching back to the start of its object. */
  const int *mid = &after[1];
  int s = mid[-1] + mid[1];

  s += read_at(b.v, k * 7);          /* the big object, through a call */
  s += read_at(before, k & 3);       /* indexed at run time */
  s += sum_pair(&pr);                /* a struct through its address */
  return s;
}

int main(void)
{
  printf("%d %d\n", f(3), f(10));
  return 0;
}
