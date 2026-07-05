/* Functional end-to-end test for the C11 6.10.9 `_Pragma' operator.
   Verifies that `_Pragma("pack(...)")' affects struct layout at runtime
   exactly like the `#pragma pack(...)' directive, both when written
   literally and when produced by macro expansion (the `DO_PRAGMA' idiom),
   including push/pop. */
#include <stdio.h>

/* The classic portable-header idiom: stringize an argument and hand it to
   the operator so a macro can emit a pragma. */
#define DO_PRAGMA(x) _Pragma(#x)

/* Literal operator. */
_Pragma("pack(1)")
struct Packed
{
  char c;
  int i;
};
_Pragma("pack()")
struct Normal
{
  char c;
  int i;
};

/* Operator produced by macro expansion, exercising push/pop. */
DO_PRAGMA(pack(push, 2))
struct Pack2
{
  char c;
  int i;
};
DO_PRAGMA(pack(pop))
struct AfterPop
{
  char c;
  int i;
};

int main(void)
{
  struct Packed p;
  int off = (int)((char *)&p.i - (char *)&p);

  printf("packed=%d normal=%d pack2=%d afterpop=%d\n",
         (int)sizeof(struct Packed), (int)sizeof(struct Normal),
         (int)sizeof(struct Pack2), (int)sizeof(struct AfterPop));
  printf("packed_i_off=%d\n", off);
  return (int)sizeof(struct Packed);
}
