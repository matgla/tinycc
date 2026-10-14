/* DSE's dead-pointer-chain sweep may only delete a store through a pointer all
 * of whose definitions are the dead local's address.  A `?:` result or a
 * reassigned pointer variable that may also hold the caller's pointer keeps
 * its store. */
int printf(const char *, ...);

__attribute__((noinline)) void sel(int *p, int c)
{
  int buf[4];
  int *q = c ? buf : p;
  *q = 5;
}

__attribute__((noinline)) void reassign(int *p, int c)
{
  int buf[4];
  int *q = buf;
  if (c)
    q = p;
  *q = 6;
}

__attribute__((noinline)) void copy_chain(int *p, int c)
{
  int buf[4];
  int *q = c ? buf : p;
  int *r = q;
  r[1] = 7;
}

/* the local really is dead here: no store may be required for correctness,
 * only that the result stays right */
__attribute__((noinline)) int dead_local(int c)
{
  int buf[4];
  int *q = buf;
  q[0] = c;
  return c + 1;
}

int main(void)
{
  int x = 1, y = 1, z[3] = {1, 1, 1}, w = 1;
  sel(&x, 0);
  sel(&w, 1);
  reassign(&y, 1);
  copy_chain(z, 0); /* r[1] = z[1] */
  printf("%d %d %d %d %d\n", x, y, z[1], w, dead_local(9));
  return 0;
}
