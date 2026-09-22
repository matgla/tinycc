/* #pragma pack around function bodies that are saved as tokens and generated
   away from their definition: static inline bodies replayed at call sites, and
   -O1 deferred bodies (-finline-functions-called-once) generated at the end of
   the translation unit or replayed at their only call.  Each body must lay out
   its structs under the pack state at its definition, and every directive must
   take effect at its position in the file. */
#include <stdio.h>
#include <stddef.h>

/* A directive right after a body: the token after the closing brace was read
   while the body was still being saved, and the pack(1) went into the body. */
int after_body(void) { return 1; }
#pragma pack(1)
struct S1 { char c; int i; };
#pragma pack()

/* Defined under pack(1), generated at the end of the TU under pack(). */
#pragma pack(1)
int local_under_pack(void)
{
  struct L1 { char c; int i; };
  return sizeof(struct L1);
}
#pragma pack()

/* Defined under pack(2), replayed at a call site under pack(1). */
#pragma pack(2)
static inline int inline_under_pack(int x)
{
  struct L2 { char c; int i; } v;
  v.i = x;
  return (int)sizeof v * 100 + (int)offsetof(struct L2, i) * 10 + v.i;
}
#pragma pack()

/* Called once: its body is replayed at the call, under the caller's pack(1). */
#pragma pack(push, 4)
static int once_under_pack(int x)
{
  struct L3 { char c; short s; int i; } v;
  v.i = x;
  return (int)sizeof v * 100 + (int)offsetof(struct L3, i) * 10 + v.i;
}
#pragma pack(pop)

/* A directive inside a body keeps applying after it, in file order. */
int body_with_pragma(void)
{
#pragma pack(1)
  struct L4 { char c; int i; };
  return sizeof(struct L4);
}
struct S5 { char c; int i; };
#pragma pack()
struct S6 { char c; int i; };

int local_default(void)
{
  struct L5 { char c; int i; };
  return sizeof(struct L5);
}

int main(void)
{
#pragma pack(1)
  struct M { char c; int i; };
  int a = inline_under_pack(3);
  int b = once_under_pack(4);
#pragma pack()
  printf("S1=%d L1=%d L4=%d S5=%d S6=%d L5=%d M=%d\n", (int)sizeof(struct S1), local_under_pack(),
         body_with_pragma(), (int)sizeof(struct S5), (int)sizeof(struct S6), local_default(),
         (int)sizeof(struct M));
  printf("inline=%d once=%d after=%d\n", a, b, after_body());
  return 0;
}
