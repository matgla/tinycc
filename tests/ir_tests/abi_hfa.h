/* AAPCS-VFP homogeneous float aggregates in VFP registers.  Shared by
 * abi_hfa_a.c (tcc) and abi_hfa_b.c (arm-none-eabi-gcc): each file defines the
 * callees under its own prefix and a driver calling the other's, so every
 * call crosses compilers in both directions.
 *
 * Returns: shapes <= 8 bytes (s0/s1 or d0) that are not "flat struct of
 * scalars": arrays and nested structs must still be recognised as HFAs.
 * Larger HFAs and _Complex double come back in s0-s7 (d0-d3) too, with no
 * hidden result pointer in r0 -- direct, through a function pointer, and as
 * a call result returned again.
 * Arguments: HFAs and _Complex float/double take the lowest run of free
 * s-registers (even d-registers for doubles), back-filling gaps; one that
 * finds no run closes the bank and goes on the stack at its own alignment.
 * Every value is exact in binary, so the weighted sums compare with ==. */
typedef struct { float v[2]; } HArr2;                       /* s0,s1 */
typedef struct { float v[1]; } HArr1;                       /* s0 */
typedef struct { struct { float a; } x; float y; } HNest;   /* s0,s1 */
typedef struct { struct { float a, b; } p; } HNest2;        /* s0,s1 */
typedef struct { double d[1]; } HD1;                        /* d0 */
typedef struct { struct { double d; } q; } HD1n;            /* d0 */
typedef struct { float a, b, c; } HF3;
typedef struct { double a, b; } HD2;
typedef struct { double v[4]; } HD4;
typedef struct { struct { float a; } x[2]; float y; } HFN;  /* nested, 3 floats */
typedef struct { float a, b, c, d; } HF4;                   /* s0-s3 */
typedef struct { float v[3]; } HArr3;                       /* s0-s2 */
typedef struct { struct { double d; } x; double y[1]; } HDN; /* d0,d1 */

#define HFA_DECLARE(P)                                                                   \
  HArr2 P##_arr2(float a, float b);                                                      \
  HArr1 P##_arr1(float a);                                                               \
  HNest P##_nest(float a, float b);                                                      \
  HNest2 P##_nest2(float a, float b);                                                    \
  HD1 P##_d1(double a);                                                                  \
  HD1n P##_d1n(double a);                                                                \
  float P##_sum2(HArr2 a);                                                               \
  float P##_cre(_Complex float c);                                                       \
  double P##_cd(_Complex double c);                                                      \
  double P##_mix(float f0, HF3 h, double d, float f1, int i);                            \
  double P##_bf(float f0, double d, HArr2 a, float f1);                                  \
  double P##_ex(HD4 a, HD4 b, HF3 c, float f, int i, HArr2 d);                           \
  double P##_stk(HD4 a, HD4 b, float f, HD2 c, int i);                                   \
  double P##_nest_arg(HFN n, HD2 d);                                                     \
  double P##_cmix(_Complex float a, HD2 b, _Complex double c, int i);                    \
  double P##_fwd(HArr2 a, _Complex float c, HD2 d);                                     \
  HF3 P##_r3(float a, float b, float c);                                                 \
  HF4 P##_r4(int i, float a, double d);                                                  \
  HD2 P##_rd2(double a, double b);                                                       \
  HD4 P##_rd4(double a, int i);                                                          \
  _Complex double P##_rcd(double re, double im);                                         \
  HArr3 P##_ra3(float a);                                                                \
  HFN P##_rn(float a, float b);                                                          \
  HDN P##_rdn(double a);                                                                 \
  HF3 P##_rsel(int k, HF3 x, HF3 y);                                                     \
  HD2 P##_rcall(HD2 (*f)(double, double), double a);

#define HFA_DEFINE(P)                                                                    \
  HArr2 P##_arr2(float a, float b) { HArr2 r = {{a, b}}; return r; }                     \
  HArr1 P##_arr1(float a) { HArr1 r = {{a}}; return r; }                                 \
  HNest P##_nest(float a, float b) { HNest r = {{a}, b}; return r; }                     \
  HNest2 P##_nest2(float a, float b) { HNest2 r = {{a, b}}; return r; }                  \
  HD1 P##_d1(double a) { HD1 r = {{a}}; return r; }                                      \
  HD1n P##_d1n(double a) { HD1n r = {{a}}; return r; }                                  \
  float P##_sum2(HArr2 a) { return a.v[0] + 2 * a.v[1]; }                                \
  float P##_cre(_Complex float c) { return __real__ c - 4 * __imag__ c; }                \
  double P##_cd(_Complex double c) { return __real__ c - 4 * __imag__ c; }               \
  double P##_mix(float f0, HF3 h, double d, float f1, int i)                             \
  {                                                                                      \
    return f0 + 2.0 * h.a + 4.0 * h.b + 8.0 * h.c + 16 * d + 32.0 * f1 + 64 * i;         \
  }                                                                                      \
  double P##_bf(float f0, double d, HArr2 a, float f1)                                   \
  {                                                                                      \
    return f0 + 2 * d + 4.0 * a.v[0] + 8.0 * a.v[1] + 16.0 * f1;                         \
  }                                                                                      \
  double P##_ex(HD4 a, HD4 b, HF3 c, float f, int i, HArr2 d)                            \
  {                                                                                      \
    return a.v[0] + 2 * a.v[3] + 4 * b.v[0] + 8 * b.v[3] + 16.0 * c.a + 32.0 * c.c +     \
           64.0 * f + 128 * i + 256.0 * d.v[0] + 512.0 * d.v[1];                         \
  }                                                                                      \
  double P##_stk(HD4 a, HD4 b, float f, HD2 c, int i)                                    \
  {                                                                                      \
    return a.v[1] + 2 * b.v[2] + 4.0 * f + 8 * c.a + 16 * c.b + 32 * i;                  \
  }                                                                                      \
  double P##_nest_arg(HFN n, HD2 d)                                                      \
  {                                                                                      \
    return n.x[0].a + 2.0 * n.x[1].a + 4.0 * n.y + 8 * d.a + 16 * d.b;                   \
  }                                                                                      \
  double P##_cmix(_Complex float a, HD2 b, _Complex double c, int i)                     \
  {                                                                                      \
    return __real__ a + 2.0 * __imag__ a + 4 * b.a + 8 * b.b + 16 * __real__ c +         \
           32 * __imag__ c + 64 * i;                                                     \
  }                                                                                      \
  HF3 P##_r3(float a, float b, float c) { HF3 r = {a, b, c}; return r; }                 \
  HF4 P##_r4(int i, float a, double d) { HF4 r = {a, i, d, a + 1}; return r; }           \
  HD2 P##_rd2(double a, double b) { HD2 r = {a, b}; return r; }                          \
  HD4 P##_rd4(double a, int i) { HD4 r = {{a, a + i, 2 * a, i}}; return r; }             \
  _Complex double P##_rcd(double re, double im)                                          \
  {                                                                                      \
    _Complex double z;                                                                   \
    __real__ z = re;                                                                     \
    __imag__ z = im;                                                                     \
    return z;                                                                            \
  }                                                                                      \
  HArr3 P##_ra3(float a) { HArr3 r = {{a, 2 * a, 3 * a}}; return r; }                    \
  HFN P##_rn(float a, float b) { HFN r = {{{a}, {b}}, a + b}; return r; }                \
  HDN P##_rdn(double a) { HDN r = {{a}, {2 * a}}; return r; }                            \
  HF3 P##_rsel(int k, HF3 x, HF3 y)                                                      \
  {                                                                                      \
    if (k)                                                                               \
      return x;                                                                          \
    return y;                                                                            \
  }                                                                                      \
  HD2 P##_rcall(HD2 (*f)(double, double), double a) { return f(a, 2 * a); }

/* Driver: call the OTHER side's callees; return 0 when every value arrives. */
#define HFA_DRIVER(ME, OTHER)                                                            \
  int run_##ME(void)                                                                     \
  {                                                                                      \
    volatile float f1 = 1.5f, f2 = 2.5f;                                                 \
    volatile double d1 = 3.25;                                                           \
    int bad = 0;                                                                         \
    HArr2 a2 = OTHER##_arr2(f1, f2);                                                     \
    if (a2.v[0] != 1.5f || a2.v[1] != 2.5f) bad |= 1;                                    \
    HArr1 a1 = OTHER##_arr1(f2);                                                         \
    if (a1.v[0] != 2.5f) bad |= 2;                                                       \
    HNest n = OTHER##_nest(f1, f2);                                                      \
    if (n.x.a != 1.5f || n.y != 2.5f) bad |= 4;                                          \
    HNest2 n2 = OTHER##_nest2(f2, f1);                                                   \
    if (n2.p.a != 2.5f || n2.p.b != 1.5f) bad |= 8;                                      \
    HD1 dd = OTHER##_d1(d1);                                                             \
    if (dd.d[0] != 3.25) bad |= 16;                                                      \
    HD1n ddn = OTHER##_d1n(d1);                                                          \
    if (ddn.q.d != 3.25) bad |= 32;                                                      \
    HArr2 x2 = {{f1, f2}};                                                               \
    if (OTHER##_sum2(x2) != 6.5f) bad |= 1 << 6;                                         \
    _Complex float cf;                                                                   \
    __real__ cf = f1;                                                                    \
    __imag__ cf = f2;                                                                    \
    if (OTHER##_cre(cf) != -8.5f) bad |= 1 << 7;                                         \
    _Complex double cd;                                                                  \
    __real__ cd = d1;                                                                    \
    __imag__ cd = f1;                                                                    \
    if (OTHER##_cd(cd) != -2.75) bad |= 1 << 8;                                          \
    HF3 h3 = {f1, f2, 0.25f};                                                            \
    if (OTHER##_mix(f2, h3, d1, f1, 3) != 309.5) bad |= 1 << 9;                          \
    if (OTHER##_bf(f1, d1, x2, f2) != 74.0) bad |= 1 << 10;                              \
    HD4 q1 = {{d1, 1.0, 2.0, 0.5}}, q2 = {{1.5, 2.0, 0.75, 4.0}};                        \
    HArr2 y2 = {{0.5f, f1}};                                                             \
    if (OTHER##_ex(q1, q2, h3, f2, 2, y2) != 1386.25) bad |= 1 << 11;                    \
    HD2 p2 = {d1, 0.125};                                                                \
    if (OTHER##_stk(q1, q2, f1, p2, 5) != 196.5) bad |= 1 << 12;                         \
    HFN nn = {{{f1}, {f2}}, 0.75f};                                                      \
    if (OTHER##_nest_arg(nn, p2) != 37.5) bad |= 1 << 13;                                \
    if (OTHER##_cmix(cf, p2, cd, 1) != 184.5) bad |= 1 << 14;                            \
    if (OTHER##_fwd(x2, cf, p2) != 43.0) bad |= 1 << 15;                                 \
    float keep = f1 * 2;                                                                 \
    HF3 r3 = OTHER##_r3(f1, f2, 0.25f);                                                  \
    if (r3.a != 1.5f || r3.b != 2.5f || r3.c != 0.25f) bad |= 1 << 16;                   \
    HF4 r4 = OTHER##_r4(7, f2, d1);                                                      \
    if (r4.a != 2.5f || r4.b != 7 || r4.c != 3.25f || r4.d != 3.5f || keep != 3.0f)      \
      bad |= 1 << 17;                                                                    \
    HD2 rd2 = OTHER##_rd2(d1, 0.125);                                                    \
    if (rd2.a != 3.25 || rd2.b != 0.125) bad |= 1 << 18;                                 \
    HD4 rd4 = OTHER##_rd4(d1, 2);                                                        \
    if (rd4.v[0] != 3.25 || rd4.v[1] != 5.25 || rd4.v[2] != 6.5 || rd4.v[3] != 2)       \
      bad |= 1 << 19;                                                                    \
    _Complex double rcd = OTHER##_rcd(d1, -0.5);                                         \
    if (__real__ rcd != 3.25 || __imag__ rcd != -0.5) bad |= 1 << 20;                    \
    HArr3 ra3 = OTHER##_ra3(f1);                                                         \
    if (ra3.v[0] != 1.5f || ra3.v[1] != 3.0f || ra3.v[2] != 4.5f) bad |= 1 << 21;        \
    HFN rn = OTHER##_rn(f1, f2);                                                         \
    if (rn.x[0].a != 1.5f || rn.x[1].a != 2.5f || rn.y != 4.0f) bad |= 1 << 22;          \
    HDN rdn = OTHER##_rdn(d1);                                                           \
    if (rdn.x.d != 3.25 || rdn.y[0] != 6.5) bad |= 1 << 23;                              \
    HF3 s0 = OTHER##_rsel(0, r3, h3), s1 = OTHER##_rsel(1, r3, h3);                      \
    if (s0.a != 1.5f || s0.b != 2.5f || s0.c != 0.25f || s1.c != 0.25f || s1.b != 2.5f)  \
      bad |= 1 << 24;                                                                    \
    HD2 c1 = OTHER##_rcall(OTHER##_rd2, d1), c2 = OTHER##_rcall(ME##_rd2, 0.5);          \
    if (c1.a != 3.25 || c1.b != 6.5 || c2.a != 0.5 || c2.b != 1.0) bad |= 1 << 25;       \
    HD4 (*volatile pd4)(double, int) = OTHER##_rd4;                                      \
    HD4 q4 = pd4(0.5, 1);                                                                \
    if (q4.v[0] != 0.5 || q4.v[1] != 1.5 || q4.v[2] != 1.0 || q4.v[3] != 1) bad |= 1 << 26; \
    if (OTHER##_nest_arg(OTHER##_rn(0.5f, 0.25f), OTHER##_rd2(1.0, 0.5)) != 20.0)        \
      bad |= 1 << 27;                                                                    \
    return bad;                                                                          \
  }                                                                                      \
  /* A body forwarding the VFP aggregates it received to the other side. */              \
  double ME##_fwd(HArr2 a, _Complex float c, HD2 d)                                      \
  {                                                                                      \
    return OTHER##_sum2(a) + OTHER##_cre(c) + OTHER##_nest_arg((HFN){{{1}, {2}}, 3}, d); \
  }
