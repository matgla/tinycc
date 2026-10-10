// Inline asm memory operands ("m") naming a local or a stack parameter.
//
// The operand used to be printed as [fp, #parser-offset]: r11, which IR
// functions never set up (a HardFault at every -O level), and a slot more than
// 255 bytes down did not encode at all.  It is now addressed off SP (or r7 with
// a frame pointer) at the slot's final offset, past any registers the asm's
// prolog pushed.  And an "m" INPUT is a read of memory the optimizer must not
// answer from the preceding store: at -O1 that store was forwarded into the
// asm marker and deleted, leaving the asm to read a slot nobody wrote.
#include <stdio.h>

struct big { int a[40]; };

__attribute__((noinline)) int store_m(int x)
{
  int buf[4];
  asm volatile("str %1, %0" : "=m"(buf[2]) : "r"(x + 1));
  return buf[2];
}

__attribute__((noinline)) int load_m(int x)
{
  int buf[2], r;
  buf[1] = x * 3;
  asm volatile("ldr %0, %1" : "=r"(r) : "m"(buf[1]));
  return r;
}

__attribute__((noinline)) int rw_m(int x)
{
  int v = x;
  asm volatile("ldr r0, %0\n\tadds r0, r0, #5\n\tstr r0, %0" : "+m"(v) : : "r0");
  return v;
}

__attribute__((noinline)) int clobbers_m(int x)
{
  int buf[4];
  /* r4-r6 are pushed around the asm body: SP moves under the operand */
  asm volatile("mov r4, #1\n\tmov r5, #2\n\tmov r6, #3\n\tstr %1, %0"
               : "=m"(buf[1]) : "r"(x) : "r4", "r5", "r6");
  return buf[1];
}

__attribute__((noinline)) int vla_m(int n, int x)
{
  int v[n];
  int buf[4];
  v[0] = 7;
  asm volatile("str %1, %0" : "=m"(buf[3]) : "r"(x));
  return buf[3] + v[0];
}

__attribute__((noinline)) int far_m(int x)
{
  struct big t1, t2;
  int buf[4];
  t1.a[0] = 1;
  t2.a[39] = 2;
  asm volatile("str %1, %0" : "=m"(buf[0]) : "r"(x));
  return buf[0] + t1.a[0] + t2.a[39];
}

__attribute__((noinline)) int param_m(int a, int b, int c, int d, int e)
{
  int r;
  asm volatile("ldr %0, %1" : "=r"(r) : "m"(e));
  return r + a + b + c + d;
}

/* a register-passed parameter named by "=m" / "+m": its home slot is the
 * operand (it was treated as a spilled POINTER and written through) */
__attribute__((noinline)) int wparam_m(int x)
{
  asm volatile("str %1, %0" : "=m"(x) : "r"(5));
  return x;
}

__attribute__((noinline)) int rwparam_m(int x)
{
  asm volatile("ldr r1, %0\n\tadds r1, r1, #2\n\tstr r1, %0" : "+m"(x) : : "r1");
  return x;
}

int main(void)
{
  printf("store=%d load=%d rw=%d\n", store_m(41), load_m(5), rw_m(10));
  printf("clobbers=%d vla=%d far=%d param=%d\n", clobbers_m(99), vla_m(3, 30), far_m(100),
         param_m(1, 2, 3, 4, 50));
  printf("wparam=%d rwparam=%d\n", wparam_m(9), rwparam_m(40));
  return 0;
}
