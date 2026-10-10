/* Hard-float: homogeneous float aggregates and complex values larger than
 * 8 bytes must come back intact (previously only d0 was transferred).
 * Self-consistent tcc-to-tcc check; returns 1 on success. */
typedef struct { float a, b, c; } F3;
typedef struct { float a, b, c, d; } F4;
typedef struct { float a, b; } F2;
typedef struct { double a, b; } D2;
typedef struct { double a, b, c; } D3;

__attribute__((noinline)) F3 mk3(float x, float y, float z) { F3 r = {x, y, z}; return r; }
__attribute__((noinline)) F4 mk4(float x, float y, float z, float w) { F4 r = {x, y, z, w}; return r; }
__attribute__((noinline)) F2 mk2(float x, float y) { F2 r = {x, y}; return r; }
__attribute__((noinline)) D2 mkd(double x, double y) { D2 r = {x, y}; return r; }
__attribute__((noinline)) D3 mkd3(double x, double y, double z) { D3 r = {x, y, z}; return r; }
__attribute__((noinline)) _Complex double mkc(double re, double im)
{
  _Complex double z = re + im * 1.0i;
  return z;
}

int main(void)
{
  volatile float x = 1.0f, y = 2.0f, z = 3.0f, w = 4.0f;
  volatile double p = 5.0, q = 6.0, r = 7.0;
  F3 t = mk3(x, y, z);
  if (t.a != 1.0f || t.b != 2.0f)
    return 10;
  if (t.c != 3.0f)
    return 11;
  D2 d = mkd(p, q);
  if (d.a != 5.0)
    return 12;
  if (d.b != 6.0)
    return 13;
  F4 f4 = mk4(x, y, z, w);
  if (f4.a != 1.0f || f4.b != 2.0f || f4.c != 3.0f || f4.d != 4.0f)
    return 14;
  F2 f2 = mk2(x, y);
  if (f2.a != 1.0f || f2.b != 2.0f)
    return 15;
  D3 d3 = mkd3(p, q, r);
  if (d3.a != 5.0 || d3.b != 6.0 || d3.c != 7.0)
    return 16;
  _Complex double c = mkc(p, q);
  if (__real__ c != 5.0 || __imag__ c != 6.0)
    return 17;
  return 1;
}
