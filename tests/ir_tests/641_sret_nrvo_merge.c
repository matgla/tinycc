/* A local copied whole into the returned struct lives in the caller's buffer
   too when its lifetime ends at that copy: Zig's `t15 = f(); t13.payload =
   t15; return t13;`.  That needs the two never to be live together on any
   path -- gotos and loops included -- and no address of either to escape (a
   callee could still read the first one through a pointer it kept).  Here:
   merges that hold (into a member, through a call's result buffer, several
   returns) and shapes where they must not happen. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct B
{
  unsigned w[32];
};

struct EB
{
  struct B payload;
  uint16_t error;
};

static void showb(const char *tag, const struct B *b)
{
  unsigned sum = 0;
  for (int i = 0; i < 32; i++)
    sum = sum * 31 + b->w[i];
  printf("%s %u %u %u %u %u %u\n", tag, b->w[0], b->w[1], b->w[2], b->w[5], b->w[6], sum);
}

static struct B *bstash;
__attribute__((noinline)) static void bfill_keep(struct B *p)
{
  memset(p, 0, sizeof *p);
  p->w[0] = 11;
  p->w[1] = 12;
  bstash = p;
}
__attribute__((noinline)) static unsigned bpeek(void) { return bstash->w[0] + bstash->w[1] * 100; }

/* t0 escaped: the call after the copy still reads t0 through the stash */
__attribute__((noinline)) static struct B merged_escape(void)
{
  struct B t0;
  bfill_keep(&t0);
  struct B t1 = t0;
  t1.w[0] = 99;
  t1.w[2] = bpeek();
  return t1;
}

/* a goto carries a write of o's bytes in between s's writes and the copy */
__attribute__((noinline)) static struct B via_goto(int c)
{
  struct B s, o;
  memset(&s, 0, sizeof s);
  s.w[5] = 7;
  if (c)
    goto M;
K:
  o = s;
  o.w[1] = 1;
  return o;
M:
  o.w[5] = 55;
  s.w[6] = 3;
  goto K;
}

/* a loop: s is read again after o was written */
__attribute__((noinline)) static struct B acc(int n)
{
  struct B s, o;
  memset(&s, 0, sizeof s);
  memset(&o, 0, sizeof o);
  for (int i = 0; i < n; i++)
  {
    s.w[0] += i;
    s.w[2] += s.w[1];
    o = s;
    o.w[1] = s.w[0] * 2 + 1;
  }
  return o;
}

/* a return path that never copies s in: s must not clobber o's bytes */
__attribute__((noinline)) static struct B early(int c)
{
  struct B s, o;
  memset(&o, 0, sizeof o);
  o.w[0] = 1;
  o.w[5] = 5;
  for (int i = 0; i < 32; i++)
    s.w[i] = 100 + i;
  if (c)
    return o;
  o = s;
  return o;
}

__attribute__((noinline)) static struct B mkb(unsigned k, const struct B *in)
{
  struct B r;
  memset(&r, 0, sizeof r);
  r.w[0] = k;
  r.w[1] = in ? in->w[0] + in->w[1] : 0;
  r.w[5] = in ? in->w[5] : 9;
  return r;
}

/* the Zig error union: a call's result copied into the payload, another
   path's literal too, two returns */
__attribute__((noinline)) static struct EB eb(unsigned k, const struct B *in)
{
  struct EB t13;
  if (k == 0)
  {
    struct B lit;
    memset(&lit, 0xaa, sizeof lit);
    lit.w[0] = 0;
    t13.payload = lit;
    t13.error = 7;
    return t13;
  }
  struct B t15 = mkb(k, in);
  t13.payload = t15;
  t13.error = 0;
  return t13;
}

/* the payload read back after the call, and the result's address passed on */
__attribute__((noinline)) static unsigned sumb(const struct EB *e) { return e->payload.w[0] + e->payload.w[1] + e->error; }
__attribute__((noinline)) static struct EB eb2(unsigned k, const struct B *in)
{
  struct EB t13;
  struct B t15 = mkb(k, in);
  t13.payload = t15;
  t13.error = 3;
  t13.payload.w[6] = sumb(&t13);
  t13.payload.w[2] = t13.payload.w[0] * 3;
  return t13;
}

/* chained: the result buffer handed to a callee that builds into it */
__attribute__((noinline)) static struct EB eb3(unsigned k)
{
  struct EB r = eb(k, NULL);
  r.error += 1;
  return r;
}

/* copies both ways between the returned local and another one (Zig's
   parameter round trip); a write to the returned one in between must stay
   overwritten by the copy back */
__attribute__((noinline)) static struct B round_trip(const struct B *in, int c)
{
  struct B t0 = *in;
  struct B t1 = t0;
  t1.w[3] = 33;
  if (c)
    t0.w[4] = 44;
  t0 = t1;
  return t0;
}
__attribute__((noinline)) static struct B round_trip2(const struct B *in, int c)
{
  struct B t0 = *in;
  struct B t1 = t0;
  if (c)
    t0.w[4] = 44;
  t0 = t1;
  return t0;
}
__attribute__((noinline)) static struct B round_loop(const struct B *in, int n)
{
  struct B t0 = *in, t1;
  for (int i = 0; i < n; i++)
  {
    t1 = t0;
    t1.w[0] += i;
    t1.w[6] = t0.w[0];
    t0 = t1;
  }
  return t0;
}

__attribute__((noinline)) static struct B round_loop2(const struct B *in, int n)
{
  struct B t0 = *in, t1;
  for (int i = 0; i < n; i++)
  {
    t1 = t0;
    t1.w[0] += i;
    t1.w[6] = t1.w[0] * 2;
    t0 = t1;
  }
  return t0;
}

/* a call fills the whole result; on the error path a literal replaces the
   payload (Zig's `return error.X` after a failed call).  The second one
   reads the old payload while the literal is half built. */
__attribute__((noinline)) static struct EB get_err(unsigned k)
{
  struct EB t7 = eb(k, NULL);
  if (t7.error)
  {
    struct B lit;
    memset(&lit, 0x55, sizeof lit);
    lit.w[0] = t7.error;
    t7.payload = lit;
    t7.error = 9;
    return t7;
  }
  t7.payload.w[1] += 1;
  return t7;
}
__attribute__((noinline)) static struct EB get_err_reads(unsigned k)
{
  struct EB t7 = eb(k, NULL);
  if (t7.error)
  {
    struct B lit;
    memset(&lit, 0x55, sizeof lit);
    lit.w[0] = t7.error;
    lit.w[1] = t7.payload.w[0] + 1;
    lit.w[2] = t7.payload.w[1] + t7.payload.w[0];
    t7.payload = lit;
    t7.error = 9;
    return t7;
  }
  t7.payload.w[1] += 1;
  return t7;
}

/* the result's address escapes after the call that filled it (kept as that
   call's buffer), or before it (then the callee, reading through the escaped
   pointer, must not be handed the same bytes) */
static struct B *seen;
__attribute__((noinline)) static void see(struct B *p) { seen = p; }
__attribute__((noinline)) static struct B mkb_seen(unsigned k)
{
  struct B q;
  memset(&q, 0, sizeof q);
  q.w[0] = k;
  q.w[1] = seen->w[0];
  q.w[2] = seen->w[1];
  return q;
}
__attribute__((noinline)) static struct B after_esc(unsigned k)
{
  struct B r = mkb(k, NULL);
  see(&r);
  r.w[2] = seen->w[0] + 1;
  return r;
}
__attribute__((noinline)) static struct B before_esc(unsigned k)
{
  struct B r;
  memset(&r, 0, sizeof r);
  r.w[0] = 5;
  r.w[1] = 6;
  see(&r);
  r = mkb_seen(k);
  return r;
}

int main(void)
{
  struct B a = merged_escape();
  showb("merged-escape", &a);
  for (int c = 0; c < 2; c++)
  {
    struct B g = via_goto(c);
    showb("goto", &g);
  }
  struct B l = acc(5);
  showb("loop", &l);
  for (int c = 0; c < 2; c++)
  {
    struct B e = early(c);
    showb("early", &e);
  }
  struct B in = mkb(4, NULL);
  for (unsigned k = 0; k < 3; k++)
  {
    struct EB e = eb(k, k ? &in : NULL);
    printf("eb %u ", e.error);
    showb("", &e.payload);
  }
  struct EB e2 = eb2(6, &in);
  printf("eb2 %u ", e2.error);
  showb("", &e2.payload);
  struct EB e3 = eb3(0);
  printf("eb3 %u ", e3.error);
  showb("", &e3.payload);
  e3 = eb3(5);
  printf("eb3 %u ", e3.error);
  showb("", &e3.payload);
  for (int c = 0; c < 2; c++)
  {
    struct B r = round_trip(&in, c);
    showb("round", &r);
    r = round_trip2(&in, c);
    showb("round2", &r);
  }
  struct B rl = round_loop(&in, 4);
  showb("round-loop", &rl);
  rl = round_loop2(&in, 4);
  showb("round-loop2", &rl);
  for (unsigned k = 0; k < 2; k++)
  {
    struct EB e = get_err(k);
    printf("get-err %u ", e.error);
    showb("", &e.payload);
    e = get_err_reads(k);
    printf("get-err-reads %u ", e.error);
    showb("", &e.payload);
  }
  struct B ae = after_esc(3);
  showb("after-esc", &ae);
  struct B be = before_esc(4);
  showb("before-esc", &be);
  /* the destination passed in the same call */
  e2 = eb(8, &e2.payload);
  printf("eb-self %u ", e2.error);
  showb("", &e2.payload);
  return 0;
}
