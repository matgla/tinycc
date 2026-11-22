/* -fstack-protector cases (test_stack_protector.py): argv[1] names one.  An
 * overrun of a local buffer past its end reaches the canary, and the
 * function's return calls __stack_chk_fail (newlib's: a message and abort). */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

volatile int vn = 24, vok = 8;

__attribute__((noinline)) static void fill(char *p, int n)
{
  for (int i = 0; i < n; i++)
    p[i] = (char)(0x41 + i);
}

__attribute__((noinline)) static int smash_char(int n)
{
  char buf[16];
  fill(buf, n);
  return buf[3];
}

__attribute__((noinline)) static int smash_int(int n)
{
  int a[4];
  fill((char *)a, n);
  return a[1];
}

__attribute__((noinline)) static int smash_addr(int n)
{
  int x = 1;
  fill((char *)&x, n);
  return x;
}

__attribute__((noinline)) static int smash_alloca(int n)
{
  char *p = __builtin_alloca(16);
  fill(p, 16);
  int r = p[0];
  char buf[8];
  fill(buf, n);
  return r + buf[1];
}

__attribute__((noinline)) static int smash_vla(int m, int n)
{
  char v[m];
  char buf[12];
  fill(v, m);
  int r = v[0];
  fill(buf, n);
  return r + buf[1];
}

__attribute__((noinline)) static long long ret64(int n)
{
  char buf[16];
  fill(buf, n);
  return 0x1122334455667788LL + buf[0];
}

/* stack_protect: a canary under any -fstack-protector*, -explicit too */
__attribute__((noinline, stack_protect)) static int smash_explicit(int n)
{
  char buf[16];
  fill(buf, n);
  return buf[5];
}

static jmp_buf jb;
__attribute__((noinline)) static void jumper(void) { longjmp(jb, 7); }
__attribute__((noinline)) static int with_setjmp(int n)
{
  char buf[16];
  fill(buf, n);
  int r = setjmp(jb);
  if (!r)
    jumper();
  return r + buf[2];
}

int main(int argc, char **argv)
{
  const char *c = argc > 1 ? argv[1] : "none";
  long long r = 0;
  printf("go %s\n", c);
  if (!strcmp(c, "char"))
    r = smash_char(vn);
  else if (!strcmp(c, "char_ok"))
    r = smash_char(vok);
  else if (!strcmp(c, "int"))
    r = smash_int(vn);
  else if (!strcmp(c, "int_ok"))
    r = smash_int(vok);
  else if (!strcmp(c, "addr"))
    r = smash_addr(vn);
  else if (!strcmp(c, "addr_ok"))
    r = smash_addr(4);
  else if (!strcmp(c, "alloca"))
    r = smash_alloca(vn);
  else if (!strcmp(c, "alloca_ok"))
    r = smash_alloca(vok);
  else if (!strcmp(c, "vla"))
    r = smash_vla(vok, vn + 24); /* past the VLA's own slots too */
  else if (!strcmp(c, "vla_ok"))
    r = smash_vla(vok, vok);
  else if (!strcmp(c, "explicit"))
    r = smash_explicit(vn);
  else if (!strcmp(c, "explicit_ok"))
    r = smash_explicit(vok);
  else if (!strcmp(c, "ret64_ok"))
    r = ret64(vok);
  else if (!strcmp(c, "setjmp_ok"))
    r = with_setjmp(vok);
  printf("ok %s %lld\n", c, r);
  return 0;
}
