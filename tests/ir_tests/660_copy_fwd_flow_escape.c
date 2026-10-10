#include <stdio.h>
#include <string.h>

/* ssa:copy_fwd with escapes as events.  A struct copy `t = *p` is read from
 * *p and dropped only where t's address cannot be out yet: Zig builds a value
 * and takes its address later (`t1 = &t0; method(t1)`), often in a different
 * live range of a reused local.  Each function below pins one side: the reads
 * a stashed pointer, a later call or a loop's next iteration can see must
 * still see the copy's bytes (or what was stored through the pointer). */

struct big
{
  int a[40];
  int b[30];
};

static struct big g_p, g_q;
static struct big *g_stash;
static int g_sink;

__attribute__((noinline)) static void stash(struct big *t) { g_stash = t; }
__attribute__((noinline)) static void poke_stash(int v)
{
  if (g_stash)
    g_stash->a[3] = v;
}
__attribute__((noinline)) static int peek_stash(void) { return g_stash ? g_stash->a[4] : -1; }
__attribute__((noinline)) static void poke(struct big *t) { t->a[5] = 555; }

/* The escape comes after the copy, which reaches it: reads before go to *p,
 * the copy stays for the callee and the reads after it. */
__attribute__((noinline)) static int esc_after(struct big *p)
{
  struct big t;
  t = *p;
  int s = t.a[1];
  poke(&t);
  return s * 1000 + t.a[5] + t.a[6];
}

/* Nothing reads the copy by name after its address goes out -- the callee
 * does, through the stash. */
__attribute__((noinline)) static int esc_after_unseen_read(struct big *p)
{
  struct big t;
  t = *p;
  stash(&t);
  int r = peek_stash();
  g_stash = 0;
  return r;
}

/* A copy of the copy after its address went out takes the poked bytes, not
 * the (private, unchanged) source's. */
__attribute__((noinline)) static int esc_then_chain(struct big *p)
{
  struct big a, t, u;
  a.a[5] = p->a[5];
  a.a[6] = p->a[6];
  a.a[7] = p->a[7];
  t = a;
  poke(&t);
  u = t;
  return u.a[5] * 100 + u.a[6] + a.a[7];
}

/* The address went out before the copy: a call may write the copy. */
__attribute__((noinline)) static int esc_before(struct big *p)
{
  struct big t;
  memset(&t, 0, sizeof t);
  stash(&t);
  t = *p;
  poke_stash(77);
  int r = t.a[3];
  g_stash = 0;
  return r;
}

/* ... or read it. */
__attribute__((noinline)) static int esc_before_read(struct big *p)
{
  struct big t;
  memset(&t, 0, sizeof t);
  stash(&t);
  t = *p;
  int r = peek_stash();
  g_stash = 0;
  return r;
}

/* The escape is at the end of the loop body: the next iteration's copy has
 * its address out already. */
__attribute__((noinline)) static int esc_loop(struct big *p, int n)
{
  struct big t;
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    t = *p;
    poke_stash(100 + i);
    s = s * 10 + (t.a[3] - 100);
    stash(&t);
  }
  g_stash = 0;
  return s;
}

/* A reused local: the first live range never escapes (its copy goes), the
 * second one does (its copy stays). */
__attribute__((noinline)) static int reuse_local(struct big *p, struct big *q)
{
  struct big t;
  int s;
  t = *p;
  s = t.a[7] + t.b[2];
  t = *q;
  stash(&t);
  poke_stash(9);
  s = s * 100 + t.a[3] + t.a[8];
  g_stash = 0;
  return s;
}

/* The escape is on the other arm of a branch. */
__attribute__((noinline)) static int esc_other_arm(struct big *p, int c)
{
  struct big t;
  int s = 0;
  if (c)
  {
    memset(&t, 0, sizeof t);
    poke(&t);
    s = t.a[5];
  }
  else
  {
    t = *p;
    s = t.a[9];
  }
  return s;
}

/* The escape is on one arm and the read after the join: the copy reaches it
 * only on the other arm, but the call after the join may see a stashed t. */
__attribute__((noinline)) static int esc_arm_then_call(struct big *p, int c)
{
  struct big t;
  t = *p;
  if (c)
    stash(&t);
  poke_stash(31);
  int r = t.a[3];
  g_stash = 0;
  return r;
}

/* A frame source whose address goes out after the copy: the copy must keep
 * the old bytes. */
__attribute__((noinline)) static int src_esc_after(int v)
{
  struct big a, b;
  memset(&a, 0, sizeof a);
  a.a[5] = v;
  b = a;
  int r = b.a[5];
  poke(&a);
  return r * 10000 + b.a[5] * 10 + a.a[5] % 10;
}

/* A frame source escaped before the copy and written through the stash. */
__attribute__((noinline)) static int src_esc_before(int v)
{
  struct big a, b;
  memset(&a, 0, sizeof a);
  a.a[3] = v;
  stash(&a);
  b = a;
  poke_stash(v + 1);
  int r = b.a[3] * 100 + a.a[3];
  g_stash = 0;
  return r;
}

/* A frame source that is never escaped and only read: calls cannot touch it. */
__attribute__((noinline)) static int src_private(struct big *p)
{
  struct big a, b;
  a = *p;
  b = a;
  poke_stash(1);
  return b.a[10] + b.b[11];
}

/* An address taken through a select: it escapes before the copy, and the
 * store through it lands in the copy. */
__attribute__((noinline)) static int esc_select(struct big *p, int c)
{
  struct big t, u;
  struct big *pp = c ? &t : &u;
  memset(&u, 0, sizeof u);
  t = *p;
  pp->a[2] = 222;
  return t.a[2] + u.a[2];
}

/* A dead copy: the second one covers it before any read. */
__attribute__((noinline)) static int dead_copy(struct big *p, struct big *q)
{
  struct big t;
  t = *p;
  t = *q;
  return t.a[11];
}

/* memcpy's result is the copy's address. */
__attribute__((noinline)) static int memcpy_result(struct big *p)
{
  struct big t;
  struct big *r = memcpy(&t, p, sizeof t);
  r->a[12] += 5;
  return t.a[12] + r->a[13];
}

__attribute__((noinline)) static int sum_ptr(const struct big *r) { return r->a[15] + r->b[4]; }

/* The result is the only way the callee gets at the copy. */
__attribute__((noinline)) static int memcpy_result_only(struct big *p)
{
  struct big a, t;
  a = *p;
  a.a[15] = 15000;
  int x = sum_ptr(memcpy(&t, &a, sizeof t));
  return x + t.a[16];
}

/* Out of order: the copy into `a` comes later in the code but runs first.
 * The read of `b` moved to `a` must count as a read of the later copy. */
__attribute__((noinline)) static int goto_order(struct big *p, struct big *q)
{
  struct big a, b;
  a = *p;
  goto second;
first:
  b = a;
  return b.a[14] + b.b[3];
second:
  a = *q;
  goto first;
}

/* The copy's bytes reach a call through a loop back edge where the address
 * is already out. */
__attribute__((noinline)) static int esc_backedge_read(struct big *p, int n)
{
  struct big t;
  int s = 0;
  stash(0);
  for (int i = 0; i < n; i++)
  {
    s = s * 10 + peek_stash() % 10;
    t = *p;
    stash(&t);
  }
  g_stash = 0;
  return s;
}

__attribute__((noinline)) static void use_int(int v) { g_sink = g_sink * 3 + v; }
__attribute__((noinline)) static void use(struct big *t)
{
  g_sink += t->a[3] + t->a[4];
  t->a[3] = t->a[4] = -1;
}

/* The frame-size shapes of test_frame_sizes.py, run. */
__attribute__((noinline)) static int src_escapes_later(int x)
{
  struct big a, b;
  a.a[3] = x;
  a.a[4] = x + 1;
  b = a;
  use_int(b.a[3]);
  int r = b.a[4];
  use(&a);
  return r * 1000 + a.a[3];
}

__attribute__((noinline)) static int reused_dst_escapes_later(struct big *p, int x)
{
  struct big a, t;
  a.a[3] = x;
  a.a[4] = x + 1;
  t = a;
  use_int(t.a[3]);
  int r = t.a[4];
  t = *p;
  use(&t);
  return r * 1000 + t.a[4] + t.a[5];
}

int main(void)
{
  for (int i = 0; i < 40; i++)
  {
    g_p.a[i] = i;
    g_q.a[i] = 1000 + i;
  }
  for (int i = 0; i < 30; i++)
  {
    g_p.b[i] = 200 + i;
    g_q.b[i] = 3000 + i;
  }
  g_p.a[4] = 44;

  printf("esc_after %d\n", esc_after(&g_p));
  printf("esc_unseen %d %d\n", esc_after_unseen_read(&g_p), esc_then_chain(&g_p));
  printf("esc_before %d %d\n", esc_before(&g_p), esc_before_read(&g_p));
  printf("esc_loop %d\n", esc_loop(&g_p, 4));
  printf("reuse %d\n", reuse_local(&g_p, &g_q));
  printf("other_arm %d %d\n", esc_other_arm(&g_p, 0), esc_other_arm(&g_p, 1));
  printf("arm_call %d %d\n", esc_arm_then_call(&g_p, 0), esc_arm_then_call(&g_p, 1));
  printf("src_esc %d %d\n", src_esc_after(7), src_esc_before(5));
  printf("src_private %d\n", src_private(&g_q));
  printf("select %d %d\n", esc_select(&g_p, 1), esc_select(&g_p, 0));
  printf("dead %d\n", dead_copy(&g_p, &g_q));
  printf("memcpy_result %d %d\n", memcpy_result(&g_p), memcpy_result_only(&g_q));
  printf("goto %d\n", goto_order(&g_p, &g_q));
  printf("backedge %d\n", esc_backedge_read(&g_p, 3));
  int l1 = src_escapes_later(12);
  int l2 = reused_dst_escapes_later(&g_q, 20);
  printf("later %d %d\n", l1, l2);
  printf("sink %d\n", g_sink);
  return 0;
}
