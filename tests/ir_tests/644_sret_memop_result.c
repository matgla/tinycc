/* memcpy, memmove and memset return their destination: an address of a
 * local that leaves through that result has escaped as surely as `G = &x`.
 *   - caller side: such a local is no call's private struct-return buffer
 *     (the callee reads the old value through the escaped pointer);
 *   - callee side: a local whose address escaped that way is not merged
 *     into the returned object, and a fill or copy the callee-side NRVO
 *     removes still yields its result. */
#include <stdio.h>
#include <string.h>

typedef struct
{
  long v[16];
} S;
S *G;
volatile int N = 16;
__attribute__((noinline)) void fill(S *r)
{
  for (int i = 0; i < N; i++)
    r->v[i] = G->v[N - 1 - i] + 100 * i;
}
__attribute__((noinline)) S f(void)
{
  S r;
  fill(&r);
  return r;
}
static const S init = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};

__attribute__((noinline)) void via_memcpy(void)
{
  S x;
  G = memcpy(&x, &init, sizeof x);
  x = f();
  printf("A %ld %ld %ld %ld\n", x.v[0], x.v[1], x.v[14], x.v[15]);
}
__attribute__((noinline)) void via_memset(void)
{
  S x;
  S *p = memset(&x, 0, sizeof x);
  p->v[15] = 1;
  p->v[14] = 2;
  G = p;
  x = f();
  printf("B %ld %ld %ld %ld\n", x.v[0], x.v[1], x.v[14], x.v[15]);
}
__attribute__((noinline)) void via_memmove(void)
{
  S x;
  S *p = memmove(&x, &init, sizeof x);
  p->v[0] = 50;
  G = p;
  x = f();
  printf("C %ld %ld %ld %ld\n", x.v[0], x.v[1], x.v[14], x.v[15]);
}
__attribute__((noinline)) void via_chain(void)
{
  S x;
  S *p = memset(memcpy(&x, &init, sizeof x), 7, 4);
  G = (S *)((char *)p + 0);
  x = f();
  printf("D %ld %ld %ld %ld\n", x.v[0], x.v[1], x.v[14], x.v[15]);
}

typedef struct
{
  long v[12];
} P;
typedef struct
{
  P payload;
  unsigned short err;
} EU;
static const P pinit = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}};
P *volatile keep;
EU *volatile ekeep;

/* t's address leaves through memcpy's result: t is not merged into r */
__attribute__((noinline)) EU m1(void)
{
  EU r;
  P t;
  P *q = memcpy(&t, &pinit, sizeof t);
  r.payload = t;
  r.err = 0;
  q->v[0] = 55;
  keep = 0;
  return r;
}
__attribute__((noinline)) EU m2(int k)
{
  EU r;
  P t;
  P *q = memset(&t, 0, sizeof t);
  q->v[1] = k;
  r.payload = t;
  r.err = 1;
  q->v[1] = 99;
  return r;
}
__attribute__((noinline)) EU m3(int k)
{
  EU r;
  P t;
  keep = memset(&t, 0, sizeof t);
  r.payload = t;
  r.err = 2;
  keep->v[2] = 77; /* writes t, not the result */
  r.payload.v[3] = k;
  return r;
}

/* the result of a fill of the returned object itself */
__attribute__((noinline)) void note(long x) { printf("note %ld\n", x); }
__attribute__((noinline)) EU r1(int k)
{
  EU r;
  void *q = memset(&r, 0, sizeof r);
  r.err = 1;
  r.payload.v[0] = k;
  note(q == (void *)&r);
  return r;
}
__attribute__((noinline)) EU r2(int k)
{
  EU r;
  EU *q = memset(&r, 0, sizeof r);
  q->payload.v[1] = k;
  r.err = 3;
  return r;
}
__attribute__((noinline)) EU r3(int k)
{
  EU r;
  P t = pinit;
  t.v[2] = k;
  EU *q = memcpy(&r.payload, &t, sizeof t);
  r.err = 4;
  ekeep = q;
  note(ekeep == &r);
  return r;
}

int main(void)
{
  via_memcpy();
  via_memset();
  via_memmove();
  via_chain();
  EU a = m1();
  printf("m1 %ld %ld %u\n", a.payload.v[0], a.payload.v[1], a.err);
  EU b = m2(4);
  printf("m2 %ld %ld %u\n", b.payload.v[0], b.payload.v[1], b.err);
  EU c = m3(5);
  printf("m3 %ld %ld %ld %u\n", c.payload.v[2], c.payload.v[3], c.payload.v[0], c.err);
  EU d = r1(3);
  printf("r1 %ld %ld %u\n", d.payload.v[0], d.payload.v[1], d.err);
  EU e = r2(6);
  printf("r2 %ld %ld %u\n", e.payload.v[0], e.payload.v[1], e.err);
  EU g = r3(8);
  printf("r3 %ld %ld %u\n", g.payload.v[2], g.payload.v[11], g.err);
  return 0;
}
