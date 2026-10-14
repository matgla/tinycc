#include <stdio.h>
#include <string.h>

/* ssa:copy_fwd with a global as the copy's source: `t = g; ... t.x` reads
 * g.x.  Zig copies its constants (`static const __anon_N`) and its globals
 * into locals before reading them -- a 2664-byte process manager in the yasos
 * kernel's root_process.  Read-only data nothing can write, so every read
 * goes to it; a writable global any call or unknown store may change, so a
 * read after one keeps the copy. */

struct inner
{
  int x, y, z;
};

struct big
{
  int a[40];
  struct inner in;
  int b[30];
};

static const struct big cg = {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {11, 12, 13}, {21, 22, 23, 24, 25}};
static struct big g;
static struct big *g_alias;
static int g_sink;

__attribute__((noinline)) static void touch_g(void)
{
  g.a[3] += 100;
  g.in.y += 100;
}
__attribute__((noinline)) static void use_int(int v) { g_sink = g_sink * 7 + v; }

/* Read-only data: reads after a call still go to it. */
__attribute__((noinline)) static int from_const(void)
{
  struct big t;
  t = cg;
  use_int(t.a[1]);
  touch_g();
  return t.a[3] * 100 + t.in.z * 10 + t.b[4];
}

/* A call may change a writable global: the copy keeps the old bytes. */
__attribute__((noinline)) static int from_global_call(void)
{
  struct big t;
  t = g;
  int r = t.a[3];
  touch_g();
  return r * 10000 + t.a[3] * 10 + t.in.y % 10;
}

/* So may a store to it by name ... */
__attribute__((noinline)) static int from_global_store(int v)
{
  struct big t;
  t = g;
  g.a[5] = v;
  return t.a[5] * 1000 + g.a[5];
}

/* ... or through a pointer. */
__attribute__((noinline)) static int from_global_ptr_store(struct big *p, int v)
{
  struct big t;
  t = g;
  p->b[2] = v;
  return t.b[2] * 1000 + g.b[2];
}

/* A sub-object of a global, and a copy of the copy. */
__attribute__((noinline)) static int take_inner(struct inner in) { return in.x * 100 + in.y * 10 + in.z; }

__attribute__((noinline)) static int sub_object(void)
{
  struct inner t, u;
  t = g.in;
  u = t;
  return take_inner(u) + t.z;
}

/* A by-value argument from the copy is the copy-time value, even though the
 * callee changes the global first. */
__attribute__((noinline)) static int byval_then_touch(struct big b)
{
  touch_g();
  return b.a[3] + b.in.y;
}

__attribute__((noinline)) static int from_global_byval(void)
{
  struct big t;
  t = g;
  return byval_then_touch(t) * 1000 + g.a[3] % 1000;
}

/* A byte buffer at an odd offset: a word copy taken straight from it would
 * be an LDM from an unaligned address. */
struct entry
{
  unsigned long long lo, hi;
};

static union
{
  unsigned long long align;
  unsigned char b[64];
} g_bytes;

__attribute__((noinline)) static unsigned entry_at_odd(void)
{
  struct entry e, f;
  memcpy(&e, g_bytes.b + 3, sizeof e);
  f = e;
  return (unsigned)(f.lo >> 8) ^ (unsigned)f.hi;
}

/* The yasos MBR parse's shape: records out of a byte buffer, then a word
 * copy (memmove8) each into the result. */
struct rec
{
  unsigned long long w[6];
};

struct two_recs
{
  struct rec part[2];
};

static union
{
  unsigned long long align;
  unsigned char b[128];
} g_rec_bytes;

__attribute__((noinline)) static void recs_at_odd(struct two_recs *out)
{
  struct rec e0, e1;
  memcpy(&e0, g_rec_bytes.b + 3, sizeof e0);
  memcpy(&e1, g_rec_bytes.b + 61, sizeof e1);
  out->part[0] = e0;
  out->part[1] = e1;
}

/* A loop: the copy runs again each iteration, the global changes between. */
__attribute__((noinline)) static int from_global_loop(int n)
{
  struct big t;
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    t = g;
    s = s * 10 + t.a[7] % 10;
    g.a[7] += 1;
  }
  return s;
}

/* Fields far into the object load through LOAD_INDEXED from the copy's
 * base (`T <-- &t LOAD_INDEXED #412`): forwarded, each becomes a plain load
 * of the symbol at its own offset, with its own width and signedness. */
struct far
{
  int pad[103];
  signed char sc;
  unsigned char uc;
  short ss;
  unsigned short us;
  char odd;
  long long ll;
  int tail[3];
};
static const struct far cfar = {{1}, -5, 250, -1234, 60000, 'q', -123456789012LL, {7, -8, 9}};
static struct far gfar;
__attribute__((noinline)) static long long from_const_far(void)
{
  struct far t = cfar;
  g_sink += t.pad[1];
  touch_g();
  return t.sc * 1000003LL + t.uc * 1009 + t.ss * 101 + t.us + t.odd + t.tail[1] * 7 + t.tail[2];
}
__attribute__((noinline)) static long long from_const_far_ll(void)
{
  struct far t = cfar;
  touch_g();
  return t.ll;
}
__attribute__((noinline)) static long long from_global_far(int k)
{
  struct far t = gfar;
  long long r = t.sc * 1000003LL + t.uc * 1009 + t.ss * 101 + t.us + t.odd + t.tail[2] * 3 + t.tail[k];
  gfar.tail[k] = 5;
  return r + t.tail[k];
}

int main(void)
{
  for (int i = 0; i < 40; i++)
    g.a[i] = i;
  g.in.x = 7;
  g.in.y = 8;
  g.in.z = 9;
  for (int i = 0; i < 30; i++)
    g.b[i] = 300 + i;
  for (int i = 0; i < 64; i++)
    g_bytes.b[i] = (unsigned char)(i * 13 + 5);
  g_alias = &g;

  int r1 = from_const();
  printf("const %d\n", r1);
  int r2 = from_global_call();
  printf("call %d\n", r2);
  int r3 = from_global_store(55);
  printf("store %d\n", r3);
  int r4 = from_global_ptr_store(g_alias, 66);
  printf("ptr_store %d\n", r4);
  int r5 = sub_object();
  printf("sub %d\n", r5);
  int r6 = from_global_byval();
  printf("byval %d\n", r6);
  unsigned r7 = entry_at_odd();
  printf("odd %u\n", r7);
  for (int i = 0; i < 128; i++)
    g_rec_bytes.b[i] = (unsigned char)(i * 7 + 3);
  struct two_recs te;
  recs_at_odd(&te);
  printf("odd2 %u %u\n", (unsigned)(te.part[0].w[1] >> 16), (unsigned)te.part[1].w[5]);
  int r8 = from_global_loop(4);
  printf("loop %d\n", r8);
  gfar = cfar;
  gfar.sc = -100;
  gfar.tail[2] = 1000;
  printf("const_far %lld %lld\n", from_const_far(), from_const_far_ll());
  printf("global_far %lld\n", from_global_far(2));
  printf("sink %d\n", g_sink);
  return 0;
}
