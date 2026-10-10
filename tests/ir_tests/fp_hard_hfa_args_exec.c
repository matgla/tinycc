/* Hard-float HFA and _Complex arguments in VFP registers, tcc to tcc, from
 * sources abi_hfa_* (the gcc interop test) does not produce: an HFA read
 * through a pointer held in a core register, a _Complex float coming out of
 * complex arithmetic or a constant, an HFA parameter live across calls and
 * forwarded, recursion, and an int after a spilled HFA still in r0. */
typedef struct { float x, y, z; } V3;
typedef struct { double re, im; } D2;

__attribute__((noinline)) float dot(V3 a, V3 b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

__attribute__((noinline)) float cnorm(_Complex float c)
{
  return __real__ c * __real__ c + __imag__ c * __imag__ c;
}

__attribute__((noinline)) double dsum(D2 a, _Complex double b, int k)
{
  return a.re + 2 * a.im + 4 * __real__ b + 8 * __imag__ b + 16 * k;
}

/* the pointer argument arrives in r0 and is still there for the VLDRs */
__attribute__((noinline)) float dot_ptr(const V3 *p, const V3 *q)
{
  return dot(*q, *p);
}

/* `v` stays live across both calls, so its home outlives s0-s2 */
__attribute__((noinline)) float twice(V3 v, float s)
{
  float d = dot(v, v);
  return d + dot(v, (V3){s, s, s});
}

__attribute__((noinline)) float depth(V3 v, int n)
{
  if (n == 0)
    return v.x + v.y + v.z;
  V3 w = {v.y, v.z, v.x + 1.0f};
  return depth(w, n - 1);
}

/* four D2s fill d0-d7; the fifth goes on the stack, `k` still to r0 */
__attribute__((noinline)) double spill(D2 a, D2 b, D2 c, D2 d, D2 e, int k)
{
  return a.re + b.im + c.re + d.im + 2 * e.re + 4 * e.im + 8 * k;
}

int main(void)
{
  volatile float one = 1.0f;
  V3 a = {one, 2.0f, 3.0f}, b = {4.0f, 5.0f, 6.0f};
  if (dot(a, b) != 32.0f)
    return 2;
  if (dot_ptr(&a, &b) != 32.0f)
    return 3;
  _Complex float c;
  __real__ c = 3.0f * one;
  __imag__ c = 4.0f;
  if (cnorm(c) != 25.0f)
    return 4;
  if (cnorm(c * c) != 625.0f) /* (-7 + 24i) */
    return 5;
  if (cnorm(1.0f + 2.0fi) != 5.0f)
    return 6;
  D2 p = {0.5, 0.25};
  _Complex double q;
  __real__ q = 1.0 * one;
  __imag__ q = 0.125;
  if (dsum(p, q, 3) != 54.0)
    return 7;
  if (twice(a, 2.0f) != 26.0f)
    return 8;
  if (depth(a, 4) != 10.0f)
    return 9;
  D2 e = {0.5, 0.75};
  if (spill(p, p, p, p, e, 2) != 21.5)
    return 10;
  return 1;
}
