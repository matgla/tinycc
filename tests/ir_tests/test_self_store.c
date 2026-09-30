/* Copies between objects that frame relayout places on the same bytes: after
 * relayout the copy loads and stores the same slot, and the stores (and the
 * loads left without a use) are removed.  The values must survive: whole-word
 * fields, narrow fields, a copy whose source was written on one path only, a
 * copy followed by a store through a pointer into the new object, and fields
 * read again after the copy.  Callees are reached through volatile pointers so
 * nothing is known about what they do with the objects. */
#include <stdio.h>

struct R3
{
  int *p;
  int b;
  int c;
};

struct N
{
  int *p;
  unsigned char f;
  short s;
};

struct W
{
  int a, b, c, d, e;
};

static int sink;

static struct R3 get3_impl(int *m, int k)
{
  struct R3 r = {m + k, k * 3, k & 1};
  return r;
}
static struct N getn_impl(int *m, int k)
{
  struct N r = {m + k, (unsigned char)(200 + k), (short)(-1000 * k)};
  return r;
}
static struct W getw_impl(int k)
{
  struct W w = {k, k + 1, k + 2, k + 3, k + 4};
  return w;
}
static void use3_impl(struct R3 *r)
{
  sink += r->b + r->c;
}
static void usen_impl(struct N *r)
{
  sink += r->f + r->s;
}
static void usew_impl(struct W *w)
{
  sink += w->a + w->b * 2 + w->c * 3 + w->d * 4 + w->e * 5;
}

struct R3 (*volatile get3)(int *, int) = get3_impl;
struct N (*volatile getn)(int *, int) = getn_impl;
struct W (*volatile getw_)(int) = getw_impl;
void (*volatile use3)(struct R3 *) = use3_impl;
void (*volatile usen)(struct N *) = usen_impl;
void (*volatile usew)(struct W *) = usew_impl;

int fields(int *m, int k)
{
  struct R3 t;
  struct R3 u;
  t = get3(m, k);
  u.p = t.p;
  u.b = t.b;
  u.c = t.c;
  use3(&u);
  if (u.c)
    *u.p = k;
  return u.b;
}

int whole(int *m, int k)
{
  struct R3 t;
  struct R3 u;
  t = get3(m, k);
  u = t;
  use3(&u);
  return u.b + u.c;
}

int narrow(int *m, int k)
{
  struct N t;
  struct N u;
  t = getn(m, k);
  u.p = t.p;
  u.f = t.f;
  u.s = t.s;
  usen(&u);
  *u.p += u.f;
  return u.s;
}

int wide(int k)
{
  struct W t = getw_(k);
  struct W u = t;
  usew(&u);
  u.c = 100;
  usew(&u);
  return u.a + u.c + u.e;
}

int branchy(int k)
{
  struct W t = getw_(k);
  if (k & 2)
    t.b = 77;
  struct W u = t;
  usew(&u);
  return u.b;
}

int main(void)
{
  int m[8] = {0};
  printf("fields %d %d\n", fields(m, 2), fields(m, 3));
  printf("whole %d %d\n", whole(m, 4), whole(m, 5));
  printf("narrow %d %d\n", narrow(m, 1), narrow(m, 6));
  printf("wide %d\n", wide(5));
  printf("branchy %d %d\n", branchy(4), branchy(6));
  printf("m %d %d %d %d %d\n", m[1], m[2], m[3], m[5], m[6]);
  printf("sink %d\n", sink);
  return 0;
}
