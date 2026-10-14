/* Image forwarding (source/ir/frame_dfe.c): a temporary built just before a
 * copy -- a .rodata image, a fill, stores, a callee's struct result -- and
 * read only by that copy is built in the copy's destination instead.  The
 * Zig C backend returns every `error.X` that way: the payload's 0xaa image in
 * a temporary, copied into the error union.  The shapes it must refuse: the
 * destination read or written between the build and the copy, the build in
 * another block, the destination reachable by the callee building the
 * temporary. */
#include <stdio.h>
#include <string.h>

struct big
{
  int f[40];
};

struct eu
{
  struct big payload;
  unsigned short error;
};

static const struct big image = {{0xaa, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                  20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39}};
static const struct big image2 = {{100, 101, 102, 103}};

/* fatfs.File.open's error path. */
__attribute__((noinline)) struct eu open_like(int fail, int v)
{
  struct eu r;
  if (fail)
  {
    struct big t = image;
    r.payload = t;
    r.error = (unsigned short)fail;
    return r;
  }
  memset(&r.payload, 0, sizeof r.payload);
  r.payload.f[5] = v;
  r.error = 0;
  return r;
}

/* The destination read between the build and the copy. */
__attribute__((noinline)) int read_between(const struct big *src, int k)
{
  struct big d = *src, s;
  s = image;
  s.f[k & 3] = 50;
  int x = d.f[3];
  d = s;
  return x * 1000 + d.f[3] + d.f[0];
}

/* The destination written between the build and the copy. */
__attribute__((noinline)) int write_between(const struct big *src, int k)
{
  struct big d = *src, s;
  s = image;
  d.f[3] = 7 + k;
  d = s;
  return d.f[3] + d.f[39];
}

/* Built in two blocks, copied after they join. */
__attribute__((noinline)) int two_blocks(int c)
{
  struct big d, s;
  memset(&d, 0, sizeof d);
  d.f[2] = 9;
  if (c)
    s = image;
  else
    s = image2;
  d = s;
  return d.f[2] + d.f[39];
}

__attribute__((noinline)) struct big make(int v)
{
  struct big b;
  for (int k = 0; k < 40; k++)
    b.f[k] = v + k;
  return b;
}

/* A callee's result copied: the callee writes the destination directly. */
__attribute__((noinline)) int from_call(int v)
{
  struct big d, s;
  s = make(v);
  d = s;
  return d.f[0] + d.f[39];
}

__attribute__((noinline)) struct big make_from(const struct big *p)
{
  struct big b;
  for (int k = 0; k < 40; k++)
    b.f[k] = p->f[39 - k] + 1;
  return b;
}

/* The callee reads the destination: its result may not be built there. */
__attribute__((noinline)) int callee_sees_dest(int v)
{
  struct big d, s;
  for (int k = 0; k < 40; k++)
    d.f[k] = v * k;
  s = make_from(&d);
  d = s;
  return d.f[0] + d.f[39] + d.f[20];
}

/* A loop: the temporary built and copied every iteration, the destination
 * read after. */
__attribute__((noinline)) int in_loop(int n)
{
  struct big d;
  int s = 0;
  memset(&d, 0, sizeof d);
  for (int k = 0; k < n; k++)
  {
    s += d.f[1];
    struct big t = image;
    t.f[1] = k;
    d = t;
  }
  return s + d.f[1] + d.f[30];
}

/* The Zig shape proper: compound literals assigned into the result, two
 * returns and a call (so the result is no NRVO candidate). */
struct named
{
  int f[40];
  char name[64];
};

struct eun
{
  struct named payload;
  unsigned short error;
};

__attribute__((noinline)) int bump(int v)
{
  return v + 1;
}

__attribute__((noinline)) struct eun open_zig(int fail, int v)
{
  struct eun t13;
  int k = bump(v);
  if (fail)
  {
    t13.payload = (struct named){{0xaa, 0xaa, 0xaa, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, "\252\252\252\252\252"};
    t13.error = (unsigned short)(fail + k);
    return t13;
  }
  t13.payload = (struct named){{1, 2, 3}, "x"};
  t13.payload.f[5] = k;
  t13.error = 0;
  return t13;
}

/* The literal reads the destination it is assigned to. */
__attribute__((noinline)) struct eun self_ref(int v)
{
  struct eun t13;
  t13.payload = (struct named){{v, 1, 2, 3}, "y"};
  t13.error = (unsigned short)bump(v);
  t13.payload = (struct named){{t13.payload.f[0] + 100, t13.payload.f[2] * 3, 0xaa}, "z"};
  return t13;
}

__attribute__((noinline)) struct named mk(int v)
{
  struct named n;
  memset(&n, 0, sizeof n);
  n.f[0] = v;
  n.f[39] = 2 * v;
  n.name[0] = 'm';
  return n;
}

__attribute__((noinline)) struct named mk_from(const struct named *p)
{
  struct named n;
  for (int k = 0; k < 40; k++)
    n.f[k] = p->f[39 - k] + 1;
  memcpy(n.name, p->name, sizeof n.name);
  return n;
}

/* A callee's result assigned into the result: built there directly; and the
 * callee that reads the destination, whose result may not be. */
__attribute__((noinline)) struct eun from_callee(int v, int fail)
{
  struct eun t13;
  int k = bump(v);
  if (fail)
  {
    for (int j = 0; j < 40; j++)
      t13.payload.f[j] = j;
    t13.payload.name[0] = 'a';
    t13.payload = mk_from(&t13.payload);
    t13.error = 1;
    return t13;
  }
  t13.payload = mk(k);
  t13.error = 0;
  return t13;
}

int main(void)
{
  struct eu a = open_like(3, 0), b = open_like(0, 42);
  printf("open %d %d %d %d %d\n", a.error, a.payload.f[0], a.payload.f[39], b.error, b.payload.f[5]);
  struct big src;
  for (int k = 0; k < 40; k++)
    src.f[k] = 1000 + k;
  printf("read_between %d %d\n", read_between(&src, 0), read_between(&src, 3));
  printf("write_between %d\n", write_between(&src, 2));
  printf("two_blocks %d %d\n", two_blocks(1), two_blocks(0));
  printf("from_call %d\n", from_call(5));
  printf("callee_sees_dest %d\n", callee_sees_dest(2));
  printf("in_loop %d\n", in_loop(4));
  struct eun z1 = open_zig(3, 1), z2 = open_zig(0, 5);
  printf("open_zig %d %d %d %d %d %d %d\n", z1.error, z1.payload.f[0], z1.payload.f[15], (unsigned char)z1.payload.name[4],
         z2.error, z2.payload.f[5], z2.payload.name[0]);
  struct eun s1 = self_ref(7);
  printf("self_ref %d %d %d %d %c\n", s1.error, s1.payload.f[0], s1.payload.f[1], s1.payload.f[2], s1.payload.name[0]);
  struct eun c1 = from_callee(4, 0), c2 = from_callee(4, 1);
  printf("from_callee %d %d %d %c %d %d %d %c\n", c1.error, c1.payload.f[0], c1.payload.f[39], c1.payload.name[0],
         c2.error, c2.payload.f[0], c2.payload.f[39], c2.payload.name[0]);
  return 0;
}
