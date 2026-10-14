/* A call claims an assignment's destination as its struct-return buffer only
 * when its result IS the value assigned: `x = f(...);`.  Anything after the
 * call -- a member or element access, a call through a returned pointer, a
 * binary operator, a ternary -- may still read the destination's old value,
 * which the call would already have overwritten.  Also: a GNU empty struct
 * returned into a destination that is rerouted through a temporary must not
 * have a byte copied past it. */
#include <stdio.h>
#include <string.h>

struct S
{
  long a, b;
};
struct G
{
  struct S (*fp)(struct S);
  long pad;
};
struct F
{
  struct S arr[2];
};
struct W6
{
  unsigned a, b, c, d, e, f;
};
struct O
{
  unsigned h;
  struct W6 m;
};

static void show(const char *t, const struct W6 *s)
{
  printf("%s %u %u %u %u %u %u\n", t, s->a, s->b, s->c, s->d, s->e, s->f);
}

__attribute__((noinline)) struct S ident(struct S s)
{
  s.b += 100;
  return s;
}
__attribute__((noinline)) struct G getg(void)
{
  struct G g;
  g.fp = ident;
  g.pad = 77;
  return g;
}
__attribute__((noinline)) struct F getf(void)
{
  struct F f;
  f.arr[0].a = 5;
  f.arr[0].b = 6;
  f.arr[1].a = 7;
  f.arr[1].b = 8;
  return f;
}
__attribute__((noinline)) struct W6 mk(unsigned k)
{
  struct W6 r = {k, k + 1, k + 2, k + 3, k * 2, k & 1};
  return r;
}

struct PS
{
  struct S *p;
  long pad[5];
};
static struct S *gps;
__attribute__((noinline)) struct PS mkp(void)
{
  struct PS r;
  r.p = gps;
  for (int i = 0; i < 5; i++)
    r.pad[i] = 2000 + i;
  return r;
}

__attribute__((noinline)) _Complex double cmk(double r, double i)
{
  _Complex double z;
  __real__ z = r;
  __imag__ z = i;
  return z;
}
__attribute__((noinline)) _Complex float cmkf(float r, float i)
{
  _Complex float z;
  __real__ z = r;
  __imag__ z = i;
  return z;
}

struct E0
{
};
struct WE
{
  struct E0 e;
  unsigned char tail[4];
};
static struct WE *gwe;
static struct E0 *ge0;
__attribute__((noinline)) struct E0 mk0(int k)
{
  struct E0 r;
  (void)k;
  return r;
}
__attribute__((noinline)) void keepw(struct WE *p) { gwe = p; }
__attribute__((noinline)) void keep0(struct E0 *p) { ge0 = p; }

#define CALL_MK(k) mk(k)
#define ADD(a, b) ((a) + (b))

struct W6 G6;

__attribute__((noinline)) void postfix(void)
{
  struct S x = {1, 2};
  x = getg().fp(x); /* fp gets the old x */
  printf("fp %ld %ld\n", x.a, x.b);

  struct S w = {2, 9};
  w = getf().arr[w.a - 1]; /* the index reads the old w */
  printf("arr %ld %ld\n", w.a, w.b);

  struct S y = {40, 41};
  gps = &y;
  y = mkp().p[0]; /* reads y through the returned pointer */
  printf("ptr %ld %ld\n", y.a, y.b);

  struct W6 u = mk(1), v = mk(100);
  u = mk(0).f ? v : u; /* keeps the old u */
  show("tern", &u);
  u = mk(0).f == 0 ? u : v;
  show("tern2", &u);

  struct W6 h = mk(1), *p = &h;
  *p = mk(0).f ? v : *p;
  show("deref", &h);

  struct O o;
  memset(&o, 0, sizeof o);
  o.m = mk(3);
  o.m = mk(0).f ? v : o.m;
  show("member", &o.m);

  G6 = mk(4);
  G6 = mk(0).f ? v : G6;
  show("global", &G6);

  struct W6 arr6[2];
  arr6[1] = mk(5);
  int i = 1;
  arr6[i] = mk(0).f ? v : arr6[i];
  show("elem", &arr6[1]);
}

__attribute__((noinline)) void complex_ops(void)
{
  _Complex double z = cmk(1.0, 2.0);
  z = cmk(10.0, 20.0) + z;
  printf("add %d %d\n", (int)__real__ z, (int)__imag__ z);
  z = cmk(3.0, 4.0) * z;
  printf("mul %d %d\n", (int)__real__ z, (int)__imag__ z);
  _Complex float f = cmkf(1.0f, 2.0f);
  f = cmkf(10.0f, 20.0f) - f;
  printf("sub %d %d\n", (int)__real__ f, (int)__imag__ f);
  z = 1.0 + 1.0i;
  z = cmk(1.0, 2.0) == z; /* 0; 1 if the call overwrote z first */
  printf("eq %d\n", (int)__real__ z);
  z = 1.0 + 1.0i;
  z = cmk(2.0, 3.0) != z;
  printf("ne %d\n", (int)__real__ z);
  z = 3.0 + 1.0i;
  _Complex double *pz = &z;
  *pz = cmk(2.0, 0.0) * *pz;
  printf("pmul %d %d\n", (int)__real__ z, (int)__imag__ z);
}

/* The plain shapes still claim (and must still be right). */
__attribute__((noinline)) void plain(void)
{
  struct W6 a = CALL_MK(ADD(1, 2)), b = mk(a.a + __LINE__ - __LINE__);
  show("init", &a);
  show("init2", &b);
  a = CALL_MK(7);
  show("macro", &a);
  struct W6 c[2];
  c[0] = mk(8), c[1] = mk(c[0].a + 1);
  show("comma0", &c[0]);
  show("comma1", &c[1]);
  b = (mk(9));
  show("paren", &b);
  int k = 1;
  b = k ? mk(10) : a;
  show("cond", &b);
  struct S s = {3, 4};
  s = ident(s);
  printf("self %ld %ld\n", s.a, s.b);
}

__attribute__((noinline)) void empty(void)
{
  struct WE w;
  memset(&w, 0x33, sizeof w);
  keepw(&w);
  w.e = mk0(1);
  printf("emember %02x %02x\n", w.tail[0], gwe->tail[3]);

  struct WE v;
  memset(&v, 0x44, sizeof v);
  struct WE *pv = &v;
  pv->e = mk0(2);
  printf("eptr %02x\n", v.tail[0]);

  struct
  {
    unsigned char before[8];
    struct E0 e;
    unsigned char after[8];
  } s;
  memset(s.before, 0x11, 8);
  memset(s.after, 0x22, 8);
  keep0(&s.e);
  s.e = mk0(3);
  printf("eside %02x %02x\n", s.before[7], s.after[0]);
}

int main(void)
{
  postfix();
  complex_ops();
  plain();
  empty();
  return 0;
}
