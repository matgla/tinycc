/* Frame dead bytes (source/ir/frame_dfe.c) against control flow and memory
 * the IR does not show as a straight run:
 *  - setjmp called as a library function between a temporary's build and the
 *    copy out of it: the second return re-runs the copy, so the build may
 *    not be moved into the destination;
 *  - a computed goto landing between the build and the copy (no
 *    is_jump_target marks a `&&label` target);
 *  - an inline asm "m" operand on an object whose address TEMP is also used
 *    by constant-index accesses: the asm reads through that TEMP's register,
 *    so the TEMP must keep naming the object's start;
 *  - an overlapping memmove inside one object, most of its destination
 *    dead: lowered to word moves it would read words it already wrote. */
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

struct big
{
  int f[40];
};

struct eu
{
  struct big payload;
  unsigned short error;
};

static const struct big image = {{0xaa, 1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                  14,   15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                  28,   29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39}};

/* ---- setjmp from <setjmp.h> ---------------------------------------------- */

static jmp_buf jb;
static int g_r;
static struct eu g_out;

__attribute__((noinline)) static void jumper(void)
{
  longjmp(jb, 1);
}

__attribute__((noinline)) static void sj_result(int k)
{
  struct eu r;
  struct big t = image;
  g_r = setjmp(jb);
  r.payload = t; /* runs again after the longjmp */
  r.error = (unsigned short)k;
  if (g_r == 0)
  {
    r.payload.f[3] = 77;
    r.payload.f[0] = 66;
    g_out = r;
    jumper();
  }
  g_out = r;
}

static jmp_buf jb2;

__attribute__((noinline)) static void jump2(void)
{
  longjmp(jb2, 1);
}

__attribute__((noinline)) static int sj_local(int v)
{
  struct big s, d;
  s = image;
  s.f[39] = v;
  int r = setjmp(jb2);
  d = s;
  if (!r)
  {
    d.f[0] = 99;
    d.f[39] = 98;
    jump2();
  }
  return d.f[0] * 1000 + d.f[39];
}

/* ---- computed goto ------------------------------------------------------- */

static const struct big image7 = {{7,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                   14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                   28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39}};

__attribute__((noinline)) static int cg(int n)
{
  static void *tab[] = {&&L, &&E};
  struct big d, s;
  int r = 0, i = 0;
  s = image7;
L:
  d = s; /* re-entered through the computed goto: d is rebuilt each time */
  r = r * 100 + d.f[0] + d.f[39];
  d.f[0] = 90 + i;
  d.f[39] = 0;
  i++;
  goto *tab[i >= n];
E:
  return r * 1000 + d.f[0];
}

/* ---- inline asm "m" on a rebased address --------------------------------- */

__attribute__((noinline)) static int asm_m(int v)
{
  struct big s;
  int r;
  s = image7;
  s.f[30] = v;
#if defined(__arm__) || defined(__thumb__)
  __asm__("ldr %0, %1" : "=r"(r) : "m"(s.f[0]));
#else
  r = s.f[0];
#endif
  return r + s.f[30];
}

/* ---- overlapping memmove within one object ------------------------------- */

#ifdef __TINYC__
void __aeabi_memmove(void *d, const void *s, size_t n);
#define MOVE(d, s, n) __aeabi_memmove(d, s, n)
#else
#define MOVE(d, s, n) memmove(d, s, n)
#endif

__attribute__((noinline)) static int overlap_up(void)
{
  int buf[24] = {100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
                 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123};
  MOVE(&buf[10], &buf[0], 48); /* buf[10..21] = old buf[0..11] */
  return buf[10] * 1000 + buf[20];
}

__attribute__((noinline)) static int overlap_down(void)
{
  int buf[24] = {100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
                 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123};
  MOVE(&buf[0], &buf[10], 48); /* buf[0..11] = old buf[10..21] */
  return buf[0] * 1000 + buf[10];
}

int main(void)
{
  sj_result(5);
  printf("sj_result %d %d %d %d\n", g_out.payload.f[0], g_out.payload.f[3], g_out.payload.f[39], g_out.error);
  printf("sj_local %d\n", sj_local(5));
  printf("cg %d %d\n", cg(3), cg(1));
  printf("asm_m %d\n", asm_m(8));
  printf("overlap %d %d\n", overlap_up(), overlap_down());
  return 0;
}
