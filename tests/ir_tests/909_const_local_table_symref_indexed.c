/* The table's values are link-time constants (function addresses), not
   immediates: a private local object written only by one run of such stores
   and only ever read through its address is read from an image carrying
   relocations, so the C backend's syscall dispatcher
   `(struct handler_table){{h0,...,h71}}.table[n](arg)` indexes the table
   instead of copying all of it onto the stack on every call.  The shapes
   that must stay on the stack: the table's address is passed to a call, an
   element is written after the initialiser, and the table is used through a
   pointer that also names it.  */
#include <stdio.h>
#include <stdint.h>

struct result { uint32_t payload; uint16_t error; };
typedef struct result (*handler)(const void *);
typedef struct handler_table { handler table[8]; } handler_table;

static uint32_t seed;
static struct result h0(const void *a) { (void)a; seed += 1; return (struct result){ 1, 0 }; }
static struct result h1(const void *a) { (void)a; seed += 2; return (struct result){ 2, 1 }; }
static struct result h2(const void *a) { (void)a; seed += 4; return (struct result){ 3, 2 }; }
static struct result h3(const void *a) { (void)a; seed += 8; return (struct result){ 4, 3 }; }
static struct result h4(const void *a) { (void)a; seed += 16; return (struct result){ 5, 4 }; }
static struct result h5(const void *a) { (void)a; seed += 32; return (struct result){ 6, 5 }; }
static struct result h6(const void *a) { (void)a; seed += 64; return (struct result){ 7, 6 }; }
static struct result h7(const void *a) { (void)a; seed += 128; return (struct result){ 8, 7 }; }

/* The dispatcher: one handler out of a compound literal, by index. */
__attribute__((noinline)) static struct result dispatch(unsigned n, const void *arg)
{
    return ((struct handler_table){ { h0, h1, h2, h3, h4, h5, h6, h7 } }).table[n](arg);
}

static int add1(int x) { return x + 1; }
static int add2(int x) { return x + 2; }
static int add3(int x) { return x + 3; }
static int dbl(int x) { return x * 2; }

/* A plain array of function pointers, indexed in the caller. */
__attribute__((noinline)) static int apply(unsigned n, int x)
{
    int (*f[4])(int) = { add1, add2, add3, dbl };
    return f[n & 3](x);
}

/* Must stay: the table's address is passed to a call. */
__attribute__((noinline)) static int through(int (*t[])(int), unsigned i)
{
    return t[i & 3](i);
}
__attribute__((noinline)) static int escapes(unsigned i)
{
    int (*f[4])(int) = { add1, add2, add3, dbl };
    return through(f, i) + f[i & 1](i);
}

/* Must stay: an element is written after the initialiser. */
__attribute__((noinline)) static int rewritten(unsigned i)
{
    int (*f[4])(int) = { add1, add2, add3, dbl };
    f[2] = dbl;
    return f[i & 3](3) + f[(i + 1) & 3](1);
}

int main(void)
{
    unsigned acc = 0, err = 0;
    for (unsigned k = 0; k < 11; k++) {
        struct result r = dispatch(k & 7, NULL);
        acc ^= r.payload;
        err += r.error;
    }
    printf("dispatch %u %u %u\n", acc, err, seed);
    printf("apply %d %d %d\n", apply(0, 4), apply(2, 4), apply(3, 4));
    printf("escapes %d %d\n", escapes(0), escapes(1));
    printf("rewritten %d %d\n", rewritten(0), rewritten(2));
    return 0;
}
