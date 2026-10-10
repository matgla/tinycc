/* A struct returned through the hidden pointer may be built straight in the
   caller's buffer even across calls, because the caller never passes a buffer
   the callee can reach any other way (the convention GCC and LLVM use).  The
   caller side must keep that promise: a destination whose address is passed in
   the same call, was stored somewhere earlier, or is taken later in a loop goes
   through a fresh temporary.  Here: every aliasing shape the caller must
   de-alias, and callees that build their result across calls, read it back,
   return it on several paths, recurse, and pass its address around. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

struct S
{
  unsigned a, b, c, d;
  unsigned pad[6];
};

static void show(const char *tag, const struct S *s)
{
  printf("%s %u %u %u %u %u %u\n", tag, s->a, s->b, s->c, s->d, s->pad[0], s->pad[5]);
}

/* reads *in after writing part of its result */
__attribute__((noinline)) static struct S swap(const struct S *in)
{
  struct S r;
  r.a = in->b;
  r.b = in->a;
  r.c = in->d + in->a;
  r.d = in->c;
  for (int i = 0; i < 6; i++)
    r.pad[i] = in->pad[i] + 1;
  return r;
}

/* the same with a call in the middle: the callee reads *in too */
__attribute__((noinline)) static unsigned peek(const struct S *p) { return p->a * 10 + p->b; }
__attribute__((noinline)) static struct S swap_call(const struct S *in)
{
  struct S r;
  r.a = in->b;
  r.b = in->a;
  r.c = peek(in);
  r.d = peek(&r);
  for (int i = 0; i < 6; i++)
    r.pad[i] = in->pad[i] + 2;
  return r;
}

/* a pointer stashed by an earlier call, read back by a later one */
static struct S *stash;
__attribute__((noinline)) static void keep(struct S *p) { stash = p; }
__attribute__((noinline)) static unsigned peek_stash(void) { return stash->a + stash->b * 100; }
__attribute__((noinline)) static struct S via_stash(unsigned k)
{
  struct S r;
  memset(&r, 0, sizeof r);
  r.a = k;
  r.b = peek_stash();
  r.c = peek_stash();
  r.d = stash->d;
  return r;
}

/* a callback that receives both the result being built and the caller's
   destination, through another argument */
typedef void (*cb_t)(struct S *, struct S *);
__attribute__((noinline)) static void cb_mix(struct S *self, struct S *other)
{
  self->c = other->a + 1000;
  other->d = 77;
  self->pad[0] = other->d;
}
__attribute__((noinline)) static struct S build_cb(struct S *other, cb_t cb)
{
  struct S r;
  memset(&r, 0, sizeof r);
  r.a = other->a + 1;
  r.b = other->b + 1;
  cb(&r, other);
  r.d = other->d + r.c;
  r.pad[5] = other->a;
  return r;
}

/* several returns, of the same local and of another one */
__attribute__((noinline)) static struct S multi(int k, const struct S *in)
{
  struct S r, o;
  memset(&r, 0, sizeof r);
  r.a = in->a;
  if (k == 1)
  {
    r.b = peek(in);
    return r;
  }
  o = *in;
  o.c = r.a + 5;
  if (k == 2)
    return o;
  r.c = peek(&r) + in->c;
  r.d = in->d;
  return r;
}

/* recursion: the inner result lands in the outer one's buffer */
__attribute__((noinline)) static struct S rec(int n, const struct S *in)
{
  struct S r;
  if (n == 0)
  {
    r = *in;
    return r;
  }
  r = rec(n - 1, in);
  r.a += n;
  r.b = r.b * 2 + in->a;
  return r;
}

/* returning a parameter and a global */
static struct S gs;
__attribute__((noinline)) static struct S ret_param(struct S p)
{
  p.a += 1;
  p.b = gs.a;
  return p;
}
__attribute__((noinline)) static struct S ret_global(unsigned k)
{
  gs.a += k;
  return gs;
}

/* the partly built result read back across a call that writes a global the
   caller then assigns the result to */
__attribute__((noinline)) static void bump_gs(void) { gs.a += 100; gs.b += 100; }
__attribute__((noinline)) static struct S from_gs(void)
{
  struct S r = gs;
  bump_gs();
  r.c = gs.a;
  r.d = r.a + r.b;
  return r;
}

/* a by-value argument that is the destination */
__attribute__((noinline)) static struct S twist(struct S p, unsigned k)
{
  struct S r;
  r.a = p.b + k;
  r.b = p.a + k;
  r.c = p.c;
  r.d = p.d;
  memcpy(r.pad, p.pad, sizeof r.pad);
  return r;
}

/* an object copied whole into the returned one, whose address escaped:
   a later call still reads the first object through the stash */
__attribute__((noinline)) static void fill_keep(struct S *p)
{
  memset(p, 0, sizeof *p);
  p->a = 11;
  p->b = 12;
  stash = p;
}
__attribute__((noinline)) static struct S merged_escape(void)
{
  struct S t0;
  fill_keep(&t0);
  struct S t1 = t0;
  t1.a = 99;
  t1.c = peek_stash();
  return t1;
}

/* a call writes its result into a local that is then copied into the returned
   one (Zig: t15 = f(); t13.payload = t15; t13.error = 0; return t13;) */
struct EU
{
  struct S payload;
  uint16_t error;
};
__attribute__((noinline)) static struct S mk(unsigned k, const struct S *in)
{
  struct S r;
  memset(&r, 0, sizeof r);
  r.a = k;
  r.b = in ? in->a : 1;
  r.c = in ? peek(in) : 2;
  return r;
}
__attribute__((noinline)) static struct EU eu(unsigned k, const struct S *in)
{
  struct EU t13;
  if (k == 0)
  {
    t13.payload = (struct S){1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
    t13.error = 7;
    return t13;
  }
  struct S t15 = mk(k, in);
  t13.payload = t15;
  t13.error = 0;
  return t13;
}

/* setjmp: a longjmp out of the callee leaves the caller's local unchanged */
static jmp_buf jb;
__attribute__((noinline)) static void jump_out(void) { longjmp(jb, 1); }
__attribute__((noinline)) static struct S build_jump(void)
{
  struct S r;
  memset(&r, 0xff, sizeof r);
  r.a = 5;
  jump_out();
  r.b = 6;
  return r;
}

__attribute__((noinline)) static void jump_test(void)
{
  struct S j = {1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
  if (!setjmp(jb))
    j = build_jump();
  show("jump", &j);
}

/* called through a pointer: inlined, its setjmp would make main return twice,
   and every destination in main would get a fresh buffer */
void (*volatile jt)(void) = jump_test;

static inline struct S inl_swap(const struct S *in)
{
  struct S r;
  r.a = in->b;
  r.b = in->a;
  r.c = in->c + in->a;
  r.d = in->d;
  memcpy(r.pad, in->pad, sizeof r.pad);
  return r;
}

struct Outer
{
  unsigned tag;
  struct S in;
  struct S arr[2];
};

int main(void)
{
  struct S x = {1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
  x = swap(&x);
  show("local-self", &x);

  struct S *px = &x;
  *px = swap(px);
  show("deref-self", &x);

  x = swap_call(&x);
  show("call-self", &x);

  struct S y = {3, 4, 5, 6, {0}};
  keep(&y);
  y = via_stash(9);
  show("stash-before", &y);

  /* kept as the first call's buffer; once its address is out, not */
  struct S y2 = via_stash(1);
  keep(&y2);
  y2 = via_stash(2);
  show("stash-after", &y2);

  struct S z;
  memset(&z, 0, sizeof z);
  for (int i = 0; i < 3; i++)
  {
    z = via_stash(i);
    keep(&z);
  }
  show("stash-loop", &z);

  struct S w = {20, 30, 40, 50, {1, 1, 1, 1, 1, 1}};
  w = build_cb(&w, cb_mix);
  show("cb-local", &w);
  struct S w2 = {21, 31, 41, 51, {2, 2, 2, 2, 2, 2}};
  struct S *pw = &w2;
  *pw = build_cb(pw, cb_mix);
  show("cb-deref", &w2);

  for (int k = 1; k <= 3; k++)
  {
    struct S m = {7, 8, 9, 10, {k}};
    m = multi(k, &m);
    show("multi", &m);
  }

  struct S r = {1, 1, 1, 1, {0}};
  r = rec(4, &r);
  show("rec", &r);

  gs = (struct S){3, 4, 5, 6, {7}};
  gs = ret_param(gs);
  show("param-g", &gs);
  gs = ret_global(5);
  show("global", &gs);
  gs = from_gs();
  show("from-gs", &gs);
  struct S fg = from_gs();
  show("from-gs2", &fg);

  struct S t = {1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
  t = twist(t, 10);
  show("twist", &t);
  t = twist(twist(t, 1), 2);
  show("twist2", &t);
  t = t;
  show("self", &t);

  struct S me = merged_escape();
  show("merged", &me);

  for (unsigned k = 0; k < 3; k++)
  {
    struct EU e = eu(k, k == 2 ? &x : NULL);
    printf("eu %u %u ", e.error, k);
    show("", &e.payload);
  }
  struct EU e2;
  e2.payload = x;
  e2 = eu(2, &e2.payload);
  printf("eu-self %u ", e2.error);
  show("", &e2.payload);

  jt();

  struct S il = {1, 2, 3, 4, {5, 6, 7, 8, 9, 10}};
  il = inl_swap(&il);
  show("inline", &il);
  struct S *pil = &il;
  *pil = inl_swap(pil);
  show("inline-deref", &il);

  struct Outer ou = {1, {1, 2, 3, 4, {5}}, {{6, 7, 8, 9, {1}}, {10, 11, 12, 13, {2}}}};
  ou.in = swap(&ou.in);
  show("field", &ou.in);
  for (int i = 0; i < 2; i++)
    ou.arr[i] = swap_call(&ou.arr[1 - i]);
  show("arr0", &ou.arr[0]);
  show("arr1", &ou.arr[1]);
  ou.arr[0] = swap(&ou.arr[0]);
  show("arr0b", &ou.arr[0]);

  struct S cl = (struct S){4, 3, 2, 1, {0}};
  cl = swap(&(struct S){cl.a, cl.b, cl.c, cl.d, {0}});
  show("compound", &cl);

  struct S (*fp)(const struct S *) = swap_call;
  x = fp(&x);
  show("fnptr", &x);
  return 0;
}
