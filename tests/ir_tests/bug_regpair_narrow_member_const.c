/* r0/r1 aggregate return of a static function with sub-word members and a
 * constant result.  The caller stores the returned register pair into its
 * frame slot member by member (regpair_store_call_result); once
 * const_call_replace has made the call its 64-bit constant, a halfword or
 * byte member's piece must still be just that member's bits.  It was the
 * whole low word -- q.a of {1, 2, 3} read 0x20001 -- at -O2 and -Os. */
#include <stdio.h>

typedef struct { unsigned short a, b; unsigned c; } Q;
typedef struct { int a, b; } P;
typedef struct { unsigned short s; unsigned char c, d; unsigned v; } R;
typedef struct { unsigned char a; unsigned short b; unsigned char c; unsigned d; } S; /* padding at 1 */
typedef struct { signed char a; short b; unsigned char c; int d; } T;

static Q __attribute__((noinline)) qq(void) { Q q = {1, 2, 3}; return q; }
static P __attribute__((noinline)) one(void) { P p = {1, 2}; return p; }
static P __attribute__((noinline)) neg(void) { P p = {-1, 0x7fffffff}; return p; }
static R __attribute__((noinline)) rr(void) { R r = {0xbeef, 0x12, 0x34, 0xdeadbeef}; return r; }
static S __attribute__((noinline)) ss(void) { S s = {0xa1, 0xb2c3, 0xd4, 0xe5f60718}; return s; }
static T __attribute__((noinline)) tt(void) { T t = {-2, -3, 0xfe, -5}; return t; }

static unsigned __attribute__((noinline)) get_a(void) { Q q = qq(); return q.a; }
static unsigned __attribute__((noinline)) get_b(void) { Q q = qq(); return q.b; }
static unsigned __attribute__((noinline)) get_c(void) { Q q = qq(); return q.c; }

static int __attribute__((noinline)) mixed(void)
{
  P a = one(), b = neg();
  R r = rr();
  return a.a + a.b * 10 + b.a * 100 + (b.b == 0x7fffffff) * 1000 + (r.s == 0xbeef) + (r.c == 0x12) * 2 +
         (r.d == 0x34) * 4 + (r.v == 0xdeadbeef) * 8;
}
static int __attribute__((noinline)) r_flags(void)
{
  R r = rr();
  return (r.s == 0xbeef) + (r.c == 0x12) * 2 + (r.d == 0x34) * 4 + (r.v == 0xdeadbeef) * 8;
}
static int __attribute__((noinline)) r_eq(void) { R r = rr(); return r.s == 0xbeef; }
static int __attribute__((noinline)) r_s(void) { R r = rr(); return r.s; }
static unsigned __attribute__((noinline)) r_c(void) { R r = rr(); return r.c; }
static unsigned __attribute__((noinline)) r_d(void) { R r = rr(); return r.d; }
static unsigned __attribute__((noinline)) s_sum(void) { S s = ss(); return s.a + s.b + s.c + (s.d >> 16); }
static unsigned __attribute__((noinline)) s_a(void) { S s = ss(); return s.a; }
static unsigned __attribute__((noinline)) s_b(void) { S s = ss(); return s.b; }
static int __attribute__((noinline)) t_sum(void) { T t = tt(); return t.a * 1000000 + t.b * 1000 + t.c + t.d; }
static int __attribute__((noinline)) t_a(void) { T t = tt(); return t.a; }
static int __attribute__((noinline)) t_b(void) { T t = tt(); return t.b; }

int main(void)
{
  printf("%u %u %u\n", get_a(), get_b(), get_c());
  printf("%d %d %d %x %x %x\n", mixed(), r_flags(), r_eq(), r_s(), r_c(), r_d());
  printf("%x %x %x\n", s_sum(), s_a(), s_b());
  printf("%d %d %d\n", t_sum(), t_a(), t_b());
  return 0;
}
