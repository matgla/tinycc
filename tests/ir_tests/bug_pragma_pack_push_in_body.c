/* A bare #pragma pack(push) inside a saved function body was recorded with the
 * stale live top instead of "duplicate the top at replay time". */
#include <stdio.h>

__attribute__((noinline)) int f(void)
{
#pragma pack(1)
#pragma pack(push)
  struct S { char c; int i; };
#pragma pack(pop)
#pragma pack()
  return (int)sizeof(struct S);
}

__attribute__((noinline)) int g(void)
{
#pragma pack(2)
#pragma pack(push)
#pragma pack(1)
  struct A { char c; int i; };
#pragma pack(pop)
  struct B { char c; int i; };
#pragma pack()
  struct C { char c; int i; };
  return (int)sizeof(struct A) * 10000 + (int)sizeof(struct B) * 100 + (int)sizeof(struct C);
}

__attribute__((noinline)) int h(void)
{
#pragma pack(push, 1)
  struct P { char c; int i; };
#pragma pack(pop)
  struct Q { char c; int i; };
  return (int)sizeof(struct P) * 100 + (int)sizeof(struct Q);
}

__attribute__((noinline)) int k(void)
{
#pragma pack(2)
  _Pragma("pack(push)")
  struct S { char c; int i; };
  _Pragma("pack(pop)")
#pragma pack()
  return (int)sizeof(struct S);
}

int main(void)
{
  printf("%d\n%d\n%d\n%d\n", f(), g(), h(), k());
  return 0;
}
