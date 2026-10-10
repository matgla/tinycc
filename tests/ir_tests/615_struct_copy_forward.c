#include <stdio.h>
#include <string.h>

/* ssa:copy_fwd reads a struct copy's fields from where it was copied from,
 * and drops the copy.  The Zig C backend's `self: Self` by value:
 * `t19 = *t14; f(t19)` with f copying its parameter again -- three 584-byte
 * copies to read one field in the yasos kernel's RankedMutex.lock.  Each case
 * below either must forward (the first few) or must not (a write to the source
 * or the copy between, a call, an escape, a loop-carried read, volatile). */

struct big
{
  int a[76];
  const void *waiting_for;
  int b[69];
};

struct inner
{
  int x, y, z;
};

struct outer
{
  int pad[3];
  struct inner in;
  int tail[20];
};

static struct big g_big, g_big2;
static struct outer g_outer;
static int sink;

__attribute__((noinline)) static struct big *cur(int i) { return i ? &g_big2 : &g_big; }

/* `self: Self` by value, as Zig lowers it: copy the parameter, read a field. */
static int is_blocked_on(struct big a0, const void *a1)
{
  struct big t0;
  const struct big *t1;
  const void *t3;
  t0 = a0;
  t1 = &t0;
  t3 = t1->waiting_for;
  if (t3 != 0)
    return t3 == a1;
  return 0;
}

__attribute__((noinline)) static int lock_shape(void *key, int which)
{
  struct big t19;
  struct big *t14 = cur(which);
  int n = 0;
  for (int i = 0; i < 3; i++)
  {
    t19 = *t14;
    n += is_blocked_on(t19, key);
  }
  return n;
}

/* Callee modifies its by-value parameter: the caller's object stays. */
__attribute__((noinline)) static int clobber_param(struct big b)
{
  b.a[0] = 777;
  b.b[68] = 778;
  return b.a[0] + b.a[1] + b.b[68];
}

__attribute__((noinline)) static int byval_from_copy(struct big *p)
{
  struct big t;
  t = *p;
  return clobber_param(t) + p->a[0] + p->b[68];
}

__attribute__((noinline)) static int byval_then_read(struct big *p)
{
  struct big t;
  t = *p;
  int r = clobber_param(t);
  return r + t.a[0] + t.b[68];
}

__attribute__((noinline)) static int field_read(struct big *p)
{
  struct big t, u;
  t = *p;
  u = t;
  return u.a[2] * 10 + t.b[3];
}

/* The source changes between the copy and the read. */
__attribute__((noinline)) static int src_written(struct big *p)
{
  struct big t;
  t = *p;
  p->a[4] = 4444;
  return t.a[4];
}

__attribute__((noinline)) static void mutate(struct big *p) { p->a[5] += 1000; }

__attribute__((noinline)) static int src_written_by_call(struct big *p)
{
  struct big t;
  t = *p;
  mutate(p);
  return t.a[5] + p->a[5];
}

/* The copy changes. */
__attribute__((noinline)) static int copy_written(struct big *p)
{
  struct big t;
  t = *p;
  t.a[6] = 66;
  return t.a[6] + t.a[7];
}

/* A chain whose middle link changes after the next copy. */
__attribute__((noinline)) static int chain_middle_written(struct big *p)
{
  struct big a, b;
  a = *p;
  b = a;
  a.a[8] = 88;
  return b.a[8] + a.a[8];
}

/* Frame to frame, the source local written afterwards. */
__attribute__((noinline)) static int local_src_written(int v)
{
  struct big a, b;
  memset(&a, 0, sizeof a);
  a.a[9] = v;
  b = a;
  a.a[9] = v + 1;
  return b.a[9] * 100 + a.a[9];
}

/* Reads at more offsets than there are spare instructions for ADDs. */
__attribute__((noinline)) static int many_offsets(struct big *p)
{
  struct big t;
  t = *p;
  return t.a[10] + t.a[20] + t.a[30] + t.a[40] + t.a[50] + t.b[10] + t.b[60];
}

/* A sub-struct copied out of a bigger object, and passed on by value. */
__attribute__((noinline)) static int take_inner(struct inner in) { return in.x * 100 + in.y * 10 + in.z; }

__attribute__((noinline)) static int sub_struct(struct outer *o)
{
  struct inner t;
  struct outer w;
  t = o->in;
  w = *o;
  return t.y + take_inner(w.in);
}

/* The copy's address escapes: every read stays on the copy. */
__attribute__((noinline)) static void poke(struct big *t) { t->a[11] = 1111; }

__attribute__((noinline)) static int escaped(struct big *p)
{
  struct big t;
  t = *p;
  poke(&t);
  return t.a[11] + t.a[12];
}

/* The read in the next iteration sees the previous iteration's copy. */
__attribute__((noinline)) static int loop_carried(struct big *p, int n)
{
  struct big t;
  int s = 0;
  t = *p;
  for (int i = 0; i < n; i++)
  {
    s += t.a[13];
    p->a[13] += 1;
    t = *p;
  }
  return s + t.a[13];
}

/* A volatile source is read where the copy is. */
__attribute__((noinline)) static int volatile_src(volatile struct big *vp)
{
  struct big t;
  t = *(struct big *)vp;
  vp->a[14] = 1414;
  return t.a[14];
}

/* The source is less aligned than the copy: the yasos kernel's MBR parse read
 * each 16-byte partition entry out of a byte buffer at +446 with memcpy, then
 * copied it into the result with an 8-byte word copy.  Taken straight from
 * the buffer, that copy was an LDM from an unaligned address: HardFault. */
struct entry
{
  unsigned long long lo, hi;
};

struct mbr
{
  struct entry part[4];
  unsigned short sig;
};

static struct entry entry_from_bytes(const unsigned char *p)
{
  struct entry t;
  memcpy(&t, p, sizeof t);
  return t;
}

__attribute__((noinline)) static struct mbr parse_mbr(const unsigned char *buf)
{
  struct mbr m;
  for (int i = 0; i < 4; i++)
  {
    struct entry e = entry_from_bytes(buf + 446 + 16 * i);
    m.part[i] = e;
  }
  memcpy(&m.sig, buf + 510, 2);
  return m;
}

static union
{
  unsigned long long align;
  unsigned char b[512];
} g_sector;

/* Two copies into the same local: each read belongs to the copy before it,
 * and a read after a join sees whichever copy ran. */
__attribute__((noinline)) static int same_dst_seq(struct big *p, struct big *q)
{
  struct big t;
  int s;
  t = *p;
  s = t.a[20];
  t = *q;
  s += t.a[21] * 10;
  return s;
}

__attribute__((noinline)) static int same_dst_join(struct big *p, struct big *q, int c)
{
  struct big t;
  int s = 0;
  t = *p;
  s += t.a[17];
  if (c)
  {
    t = *q;
    s += t.a[18] * 10;
  }
  return s + t.a[19] * 100;
}

/* Inline asm reads or rewrites a field of the copy through its operand. */
__attribute__((noinline)) static int asm_reads_copy(struct big *p)
{
  struct big t;
  int out;
  t = *p;
  __asm volatile("mov %0, %1" : "=r"(out) : "r"(t.a[15]));
  return out;
}

__attribute__((noinline)) static int asm_on_copy(struct big *p)
{
  struct big t;
  t = *p;
  __asm volatile("adds %0, %0, #5" : "+r"(t.a[16]) : : "cc");
  return t.a[16] * 100 + p->a[16];
}

int main(void)
{
  for (int i = 0; i < 76; i++)
    g_big.a[i] = g_big2.a[i] = i;
  for (int i = 0; i < 69; i++)
    g_big.b[i] = g_big2.b[i] = 1000 + i;
  g_big.waiting_for = &sink;
  g_big2.waiting_for = 0;
  for (int i = 0; i < 3; i++)
    g_outer.pad[i] = -1;
  g_outer.in.x = 1;
  g_outer.in.y = 2;
  g_outer.in.z = 3;

  printf("lock %d %d %d\n", lock_shape(&sink, 0), lock_shape(&g_big, 0), lock_shape(&sink, 1));
  printf("byval %d %d\n", byval_from_copy(&g_big), byval_then_read(&g_big));
  printf("field %d\n", field_read(&g_big));
  int r = src_written(&g_big);
  printf("src_written %d %d\n", r, g_big.a[4]);
  printf("src_call %d\n", src_written_by_call(&g_big));
  printf("copy_written %d\n", copy_written(&g_big));
  printf("chain %d\n", chain_middle_written(&g_big));
  printf("local %d\n", local_src_written(5));
  printf("many %d\n", many_offsets(&g_big));
  printf("sub %d\n", sub_struct(&g_outer));
  printf("escaped %d\n", escaped(&g_big));
  printf("loop %d\n", loop_carried(&g_big, 4));
  r = volatile_src(&g_big);
  printf("volatile %d %d\n", r, g_big.a[14]);
  printf("asm %d %d\n", asm_reads_copy(&g_big), asm_on_copy(&g_big));
  for (int i = 0; i < 512; i++)
    g_sector.b[i] = (unsigned char)(i * 7 + 1);
  struct mbr m = parse_mbr(g_sector.b);
  printf("mbr %u %u %u %u %x\n", (unsigned)m.part[0].lo, (unsigned)(m.part[1].hi >> 32), (unsigned)m.part[2].lo,
         (unsigned)(m.part[3].hi >> 8), m.sig);
  g_big2.a[19] = 5000;
  printf("same_dst %d %d %d\n", same_dst_seq(&g_big, &g_big2), same_dst_join(&g_big, &g_big2, 0),
         same_dst_join(&g_big, &g_big2, 1));
  return 0;
}
