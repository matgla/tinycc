/*
 * complex.h - C99 Complex Number Arithmetic
 *
 * This header provides support for complex number arithmetic as defined
 * in the C99 standard (ISO/IEC 9899:1999, Section 7.3).
 *
 * IMPLEMENTATION STATUS:
 *   DONE: Phase 6 - Basic macros and type definitions
 *   TODO: Phase 6 - Runtime library functions (conj, cabs, cexp, etc.)
 */

#ifndef _COMPLEX_H
#define _COMPLEX_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The complex macro expands to _Complex. This is a keyword that
 * specifies a complex type.
 * DONE: Phase 6
 */
#ifndef complex
#define complex _Complex
#endif

/*
 * The imaginary macro expands to _Imaginary (not yet supported).
 * For now, we only provide _Complex support.
 */
#ifndef imaginary
/* #define imaginary _Imaginary */
#endif

/*
 * _Complex_I is a constant expression of type const float _Complex
 * representing the imaginary unit (i).
 * For now, we define it as a placeholder since imaginary constants
 * require full complex constant support.
 */
#ifndef _Complex_I
#define _Complex_I (0.0f + 1.0fi)
#endif

/*
 * I is a macro that expands to _Complex_I or _Imaginary_I.
 * It represents the imaginary unit i.
 * DONE: Phase 6
 */
#ifndef I
#define I _Complex_I
#endif

/*
 * C11 CMPLX macros for constructing complex values.
 * These avoid issues with compound literals in C99.
 */
#define CMPLX(x, y) ((double _Complex){ (x), (y) })
#define CMPLXF(x, y) ((float _Complex){ (x), (y) })
#define CMPLXL(x, y) ((long double _Complex){ (x), (y) })

/*
 * Thecreal functions return the real part of a complex number.
 * The cimag functions return the imaginary part of a complex number.
 * These can be implemented using the __real__ and __imag__ operators
 * when they are fully supported.
 */

extern double creal(double _Complex z);
extern float crealf(float _Complex z);
extern long double creall(long double _Complex z);

extern double cimag(double _Complex z);
extern float cimagf(float _Complex z);
extern long double cimagl(long double _Complex z);

/*
 * Conjugate functions - return the complex conjugate.
 * conj(a + bi) = a - bi
 */
extern double _Complex conj(double _Complex z);
extern float _Complex conjf(float _Complex z);
extern long double _Complex conjl(long double _Complex z);

/*
 * Absolute value (magnitude) of a complex number.
 * cabs(a + bi) = sqrt(a^2 + b^2)
 */
extern double cabs(double _Complex z);
extern float cabsf(float _Complex z);
extern long double cabsl(long double _Complex z);

/*
 * Argument (phase angle) of a complex number.
 * carg(a + bi) = atan2(b, a)
 */
extern double carg(double _Complex z);
extern float cargf(float _Complex z);
extern long double cargl(long double _Complex z);

/*
 * Projection onto Riemann sphere.
 */
extern double _Complex cproj(double _Complex z);
extern float _Complex cprojf(float _Complex z);
extern long double _Complex cprojl(long double _Complex z);

/*
 * Exponential functions.
 * cexp(a + bi) = e^a * (cos(b) + i*sin(b))
 */
extern double _Complex cexp(double _Complex z);
extern float _Complex cexpf(float _Complex z);
extern long double _Complex cexpl(long double _Complex z);

/*
 * Natural logarithm.
 */
extern double _Complex clog(double _Complex z);
extern float _Complex clogf(float _Complex z);
extern long double _Complex clogl(long double _Complex z);

/*
 * Power function.
 * cpow(x, y) = e^(y * log(x))
 */
extern double _Complex cpow(double _Complex x, double _Complex y);
extern float _Complex cpowf(float _Complex x, float _Complex y);
extern long double _Complex cpowl(long double _Complex x, long double _Complex y);

/*
 * Square root.
 */
extern double _Complex csqrt(double _Complex z);
extern float _Complex csqrtf(float _Complex z);
extern long double _Complex csqrtl(long double _Complex z);

/*
 * Trigonometric functions.
 */
extern double _Complex csin(double _Complex z);
extern float _Complex csinf(float _Complex z);
extern long double _Complex csinl(long double _Complex z);

extern double _Complex ccos(double _Complex z);
extern float _Complex ccosf(float _Complex z);
extern long double _Complex ccosl(long double _Complex z);

extern double _Complex ctan(double _Complex z);
extern float _Complex ctanf(float _Complex z);
extern long double _Complex ctanl(long double _Complex z);

/*
 * Inverse trigonometric functions.
 */
extern double _Complex casin(double _Complex z);
extern float _Complex casinf(float _Complex z);
extern long double _Complex casinl(long double _Complex z);

extern double _Complex cacos(double _Complex z);
extern float _Complex cacosf(float _Complex z);
extern long double _Complex cacosl(long double _Complex z);

extern double _Complex catan(double _Complex z);
extern float _Complex catanf(float _Complex z);
extern long double _Complex catanl(long double _Complex z);

/*
 * Hyperbolic functions.
 */
extern double _Complex csinh(double _Complex z);
extern float _Complex csinhf(float _Complex z);
extern long double _Complex csinhl(long double _Complex z);

extern double _Complex ccosh(double _Complex z);
extern float _Complex ccoshf(float _Complex z);
extern long double _Complex ccoshl(long double _Complex z);

extern double _Complex ctanh(double _Complex z);
extern float _Complex ctanhf(float _Complex z);
extern long double _Complex ctanhl(long double _Complex z);

/*
 * Inverse hyperbolic functions.
 */
extern double _Complex casinh(double _Complex z);
extern float _Complex casinhf(float _Complex z);
extern long double _Complex casinhl(long double _Complex z);

extern double _Complex cacosh(double _Complex z);
extern float _Complex cacoshf(float _Complex z);
extern long double _Complex cacoshl(long double _Complex z);

extern double _Complex catanh(double _Complex z);
extern float _Complex catanhf(float _Complex z);
extern long double _Complex catanhl(long double _Complex z);

#ifdef __cplusplus
}
#endif

#endif /* _COMPLEX_H */
