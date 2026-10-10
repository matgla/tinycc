/* Hard-float HFA and _Complex double returns larger than 8 bytes come back in
 * s0-s7 (AAPCS-VFP), with no hidden result pointer in r0: from a hand-written
 * callee, into a hand-written caller, and tcc to tcc through tail-position
 * calls, inlining, function pointers, cleanups that clobber s0-s3, recursion,
 * and result buffers claimed from the assignment's destination.
 * Returns 1 on success. */
typedef struct { float a, b, c; } F3;
typedef struct { float a, b, c, d; } F4;
typedef struct { double a, b; } D2;
typedef struct { double v[4]; } D4;

/* s0-s3 = 1.5, 2.5, 3.5, 4.5 by hand, as any AAPCS-VFP callee leaves them */
__attribute__((naked, noinline)) F4 asm_f4(void)
{
  __asm__ volatile("movw r0, #0\n"
                   "movt r0, #0x3fc0\n"
                   "vmov s0, r0\n"
                   "movt r0, #0x4020\n"
                   "vmov s1, r0\n"
                   "movt r0, #0x4060\n"
                   "vmov s2, r0\n"
                   "movt r0, #0x4090\n"
                   "vmov s3, r0\n"
                   "bx lr\n");
}

/* a naked body reads its arguments where they arrive: r0-r3 are its own */
__attribute__((naked, noinline)) F4 asm_bits(int a, int b, int c, int d)
{
  __asm__ volatile("vmov s0, r0\n"
                   "vmov s1, r1\n"
                   "vmov s2, r2\n"
                   "vmov s3, r3\n"
                   "bx lr\n");
}

/* calls fn(k) and returns s0 + s1 + s2 + s3 of its result: k must arrive
 * in r0 and the value in s0-s3 */
__attribute__((naked, noinline)) float asm_sum_f4(F4 (*fn)(int), int k)
{
  __asm__ volatile("push {r4, lr}\n"
                   "mov r4, r0\n"
                   "mov r0, r1\n"
                   "blx r4\n"
                   "vadd.f32 s0, s0, s1\n"
                   "vadd.f32 s2, s2, s3\n"
                   "vadd.f32 s0, s0, s2\n"
                   "pop {r4, pc}\n");
}

__attribute__((noinline)) F4 mk4i(int k)
{
  F4 r = {k, k + 1, k + 2, k + 3};
  return r;
}

__attribute__((noinline)) F3 mk3(float x, float y, float z)
{
  F3 r = {x, y, z};
  return r;
}

/* inlined at -O1 and above */
static F3 add3(F3 p, F3 q)
{
  F3 r = {p.a + q.a, p.b + q.b, p.c + q.c};
  return r;
}

/* the call result is the return value: never a tail call */
__attribute__((noinline)) F4 pass4(int k) { return mk4i(k + 1); }
__attribute__((noinline)) F4 pass_asm(void) { return asm_f4(); }

__attribute__((noinline)) D4 rec(int n)
{
  if (n == 0)
  {
    D4 r = {{1.0, 2.0, 3.0, 4.0}};
    return r;
  }
  D4 r = rec(n - 1);
  r.v[0] += 1.0;
  r.v[3] *= 2.0;
  return r;
}

__attribute__((noinline)) _Complex double cmul(_Complex double a, _Complex double b) { return a * b; }

volatile float sink;
static void scrub(int *p)
{
  /* float work that leaves s0-s3 holding something else */
  volatile float x = 7.0f;
  sink = x * x + (float)*p;
}

/* the cleanup runs after the return value is built */
__attribute__((noinline)) F4 with_cleanup(int k)
{
  __attribute__((cleanup(scrub))) int g = k;
  F4 r = {k * 0.5f, 1.0f, 2.0f, 3.0f};
  if (k > 10)
    return mk4i(k);
  return r;
}

__attribute__((noinline)) D2 pick(int k, D2 x, D2 y) { return k ? x : y; }

__attribute__((noinline)) float sum3p(const F3 *p) { return p->a + 2 * p->b + 4 * p->c; }

/* word copies of a float parameter's home: its s-register is not the core
 * register of the same number (k sits in r0, x.a in s0) */
__attribute__((noinline)) float pick_sum(int k, F3 x, F3 y)
{
  F3 b;
  if (k)
    b = x;
  else
    b = y;
  return sum3p(&b);
}

struct Holder
{
  int tag;
  F4 v;
  D2 d[2];
};

int main(void)
{
  volatile int one = 1;
  F4 a = asm_f4();
  if (a.a != 1.5f || a.b != 2.5f || a.c != 3.5f || a.d != 4.5f)
    return 10;
  F4 n = asm_bits(0x3fc00000, 0x40200000, 0x40600000, 0x40900000 * one);
  if (n.a != 1.5f || n.b != 2.5f || n.c != 3.5f || n.d != 4.5f)
    return 24;
  if (asm_sum_f4(mk4i, 3) != 18.0f)
    return 11;
  if (asm_sum_f4(pass4, 3) != 22.0f)
    return 12;
  F4 b = pass_asm();
  if (b.a != 1.5f || b.d != 4.5f)
    return 13;
  F3 s = add3(mk3(1.0f, 2.0f, 3.0f), mk3(0.5f, 0.25f, 0.125f));
  if (s.a != 1.5f || s.b != 2.25f || s.c != 3.125f)
    return 14;
  D4 r = rec(3 * one);
  if (r.v[0] != 4.0 || r.v[1] != 2.0 || r.v[2] != 3.0 || r.v[3] != 32.0)
    return 15;
  _Complex double x, y;
  __real__ x = 1.0 * one;
  __imag__ x = 2.0;
  __real__ y = 3.0;
  __imag__ y = -1.0;
  _Complex double z = cmul(x, y); /* (1+2i)(3-i) = 5+5i */
  if (__real__ z != 5.0 || __imag__ z != 5.0)
    return 16;
  F4 c = with_cleanup(4 * one);
  if (c.a != 2.0f || c.b != 1.0f || c.c != 2.0f || c.d != 3.0f || sink != 53.0f)
    return 17;
  c = with_cleanup(12 * one);
  if (c.a != 12.0f || c.d != 15.0f)
    return 18;
  /* destinations claimed as the result buffer */
  struct Holder h;
  h.tag = 9;
  h.v = mk4i(5 * one);
  D2 p = {0.5, 0.25}, q = {8.0, 16.0};
  h.d[one] = pick(one, p, q);
  h.d[0] = pick(0, p, q);
  if (h.tag != 9 || h.v.a != 5.0f || h.v.d != 8.0f || h.d[1].a != 0.5 || h.d[0].b != 16.0)
    return 19;
  F4 *hp = &h.v;
  *hp = mk4i((int)hp->b); /* the destination is read for the argument */
  if (h.v.a != 6.0f || h.v.d != 9.0f)
    return 20;
  F4 (*volatile fp)(int) = pass4;
  F4 t = fp(one);
  if (t.a != 2.0f || t.d != 5.0f)
    return 21;
  /* a value live across the calls */
  float acc = 0.0f;
  for (int i = 0; i < 4; i++)
  {
    F4 u = mk4i(i * one);
    acc += u.a + u.d;
  }
  if (acc != 24.0f)
    return 22;
  F3 x3 = {1.0f, 2.0f, 3.0f}, y3 = {0.5f, 0.25f, 0.125f};
  if (pick_sum(one, x3, y3) != 17.0f || pick_sum(0, x3, y3) != 1.5f)
    return 23;
  return 1;
}
