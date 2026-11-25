/* Complex division in the FP runtime (lib/fp/soft/divc3.c): __divsc3 and
 * __divdc3, called by name, in the base PCS (soft / softfp; a hard-float
 * image takes libgcc's, see the qemu Makefile).  They used to be the YasOS
 * libc's (math_runtime.c).
 *
 * The scaled vectors are gcc.c-torture's cdivchkf / cdivchkd: Smith's
 * algorithm without libgcc's pre-scaling underflows a whole component of
 * each.  A result passes within 32 units in the last place, about gcc's
 * ERRLIM; the distance is taken on the bit patterns, so no FP operation of
 * this file's own runs and none of libm is needed.  Then C11 Annex G's
 * recoveries: nonzero/zero, infinite/finite, finite/infinite. */
#include <stdio.h>

float _Complex __divsc3(float a_real, float a_imag, float b_real, float b_imag);
double _Complex __divdc3(double a_real, double a_imag, double b_real, double b_imag);

typedef union
{
  float f;
  unsigned int u;
} fbits;

typedef union
{
  double d;
  unsigned long long u;
} dbits;

/* units in the last place between two floats of one sign (a signed zero
   matches either zero) */
static unsigned long long ulps_f(float got, float want)
{
  fbits g, w;
  g.f = got;
  w.f = want;
  if ((g.u & 0x7fffffffu) == 0 && (w.u & 0x7fffffffu) == 0)
    return 0;
  if ((g.u ^ w.u) & 0x80000000u)
    return ~0ull;
  return g.u > w.u ? g.u - w.u : w.u - g.u;
}

static unsigned long long ulps_d(double got, double want)
{
  dbits g, w;
  g.d = got;
  w.d = want;
  if ((g.u & 0x7fffffffffffffffull) == 0 && (w.u & 0x7fffffffffffffffull) == 0)
    return 0;
  if ((g.u ^ w.u) & 0x8000000000000000ull)
    return ~0ull;
  return g.u > w.u ? g.u - w.u : w.u - g.u;
}

/* +inf, -inf, or (sign 0) a zero of either sign */
static int is_f(float x, int sign)
{
  fbits b;
  b.f = x;
  if (sign == 0)
    return (b.u & 0x7fffffffu) == 0;
  return b.u == (sign > 0 ? 0x7f800000u : 0xff800000u);
}

static int is_d(double x, int sign)
{
  dbits b;
  b.d = x;
  if (sign == 0)
    return (b.u & 0x7fffffffffffffffull) == 0;
  return b.u == (sign > 0 ? 0x7ff0000000000000ull : 0xfff0000000000000ull);
}

struct fcase
{
  float xr, xi, yr, yi, zr, zi;
};

struct dcase
{
  double xr, xi, yr, yi, zr, zi;
};

/* gcc.c-torture/execute/ieee/cdivchkf.c, and (1+2i)/(3+4i) */
static const struct fcase FC[] = {
    {0x1.0b1600p-133f, 0x1.5e1c28p+54f, -0x1.cdec8cp-119f, 0x1.1e72ccp+32f, 0x1.38e502p+22f, -0x1.f89220p-129f},
    {-0x1.b1bee2p+121f, -0x1.cb403ep-59f, 0x1.480000p-144f, -0x1.c66fc4p+5f, -0x1.60b8cap-34f, -0x1.e8b02ap+115f},
    {-0x1.3f6e00p-97f, -0x1.c00000p-146f, 0x1.000000p-148f, -0x1.0c4e70p-91f, 0x1.aa50d0p-55f, -0x1.30c746p-6f},
    {0x1.000000p-148f, 0x1.f4bc04p-84f, 0x1.00ad74p-20f, 0x1.2ad02ep-85f, 0x1.1102ccp-127f, 0x1.f369a4p-64f},
    {1.0f, 2.0f, 3.0f, 4.0f, 0.44f, 0.08f},
};

/* gcc.c-torture/execute/ieee/cdivchkd.c, and (1+2i)/(3+4i) */
static const struct dcase DC[] = {
    {-0x1.16e7fad79e45ep+651, -0x1.f7f75b94c6c6ap-860, -0x1.2f40d8ff7e55ep+245, -0x0.0000000004ebcp-1022,
     0x1.d6e4b0e282869p+405, -0x1.e9095e311e706p-900},
    {-0x1.21ff587f953d3p-310, -0x1.5a526dcc59960p+837, 0x1.b88b8b552eaadp+735, -0x1.873e2d6544d92p-327,
     0x1.65734a88b2de0p-961, -0x1.927e85b8b5770p+101},
    {0x1.4612e41aa8080p-846, -0x0.0000000613e07p-1022, 0x1.df9cd0d58caafp-820, -0x1.e47051a9036dbp-584,
     0x1.9b194f3fffa32p-469, 0x1.58a00ab740a6bp-263},
    {0x1.cb27eece7c585p-355, 0x0.000000223b8a8p-1022, -0x1.74e7ed2b9189fp-22, 0x1.3d80439e9a119p-731,
     -0x1.3b35ed806ae5ap-333, -0x0.05e01bcbfd9f6p-1022},
    {1.0, 2.0, 3.0, 4.0, 0.44, 0.08},
};

#define ULP_LIMIT 32

int main(void)
{
  volatile float finf = 1.0f / 0.0f;
  volatile double dinf = 1.0 / 0.0;
  float _Complex fz;
  double _Complex dz;

  for (unsigned i = 0; i < sizeof FC / sizeof FC[0]; i++)
  {
    const struct fcase *c = &FC[i];
    fz = __divsc3(c->xr, c->xi, c->yr, c->yi);
    unsigned long long er = ulps_f(__real__ fz, c->zr), ei = ulps_f(__imag__ fz, c->zi);
    if (er > ULP_LIMIT || ei > ULP_LIMIT)
    {
      printf("FAIL divsc3 %u: %llu %llu ulps\n", i, er, ei);
      return 1;
    }
  }

  for (unsigned i = 0; i < sizeof DC / sizeof DC[0]; i++)
  {
    const struct dcase *c = &DC[i];
    dz = __divdc3(c->xr, c->xi, c->yr, c->yi);
    unsigned long long er = ulps_d(__real__ dz, c->zr), ei = ulps_d(__imag__ dz, c->zi);
    if (er > ULP_LIMIT || ei > ULP_LIMIT)
    {
      printf("FAIL divdc3 %u: %llu %llu ulps\n", i, er, ei);
      return 2;
    }
  }

  /* nonzero / zero: an infinity of the numerator's signs */
  fz = __divsc3(1.0f, -2.0f, 0.0f, 0.0f);
  dz = __divdc3(1.0, -2.0, 0.0, 0.0);
  if (!is_f(__real__ fz, 1) || !is_f(__imag__ fz, -1) || !is_d(__real__ dz, 1) || !is_d(__imag__ dz, -1))
  {
    printf("FAIL nonzero/zero\n");
    return 3;
  }

  /* infinite / finite: infinite, where the plain formula gives NaN+iNaN */
  fz = __divsc3(finf, finf, 1.0f, 0.0f);
  dz = __divdc3(dinf, dinf, 1.0, 0.0);
  if (!is_f(__real__ fz, 1) || !is_f(__imag__ fz, 1) || !is_d(__real__ dz, 1) || !is_d(__imag__ dz, 1))
  {
    printf("FAIL infinite/finite\n");
    return 4;
  }

  /* finite / infinite: zero */
  fz = __divsc3(1.0f, 1.0f, finf, finf);
  dz = __divdc3(1.0, 1.0, dinf, dinf);
  if (!is_f(__real__ fz, 0) || !is_f(__imag__ fz, 0) || !is_d(__real__ dz, 0) || !is_d(__imag__ dz, 0))
  {
    printf("FAIL finite/infinite\n");
    return 5;
  }

  printf("OK\n");
  return 0;
}
