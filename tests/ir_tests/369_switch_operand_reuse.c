/* Guard for reusing the switch operand's vreg instead of copying it into a
 * fresh temp (tccgen.c, TOK_SWITCH).  The dispatch block is emitted AFTER the
 * case bodies but executes BEFORE them, so anything that reads the switch
 * value at the dispatch site must still observe the value the switch was
 * entered with.  These cases pin the situations where the copy is the only
 * thing making that true:
 *
 *   mutate_param / mutate_local : the body assigns to the very object being
 *       switched on, and then falls through to a later case body.  Every arm
 *       must dispatch on the entry value.
 *   narrow_*  : a char/short operand needs one widening before the compare
 *       chain; the copy is what performs it (and reusing the narrow vreg used
 *       to re-extend at every compare node).  Signedness must survive.
 *   deref     : the operand is a memory read, not a register value.
 *   wide      : long long, which takes the compare-chain path rather than a
 *       jump table.
 */
#include <stdio.h>

__attribute__((noinline)) int mutate_param(int x)
{
  int seen = 0;
  switch (x)
  {
  case 1:
    x = 99;
    seen += 1;
    break;
  case 2:
    x = 98;
    seen += 2;
    break;
  case 99:
    seen += 1000;
    break;
  default:
    seen += 7;
    break;
  }
  return seen * 10 + (x == 99 || x == 98 ? 1 : 0);
}

__attribute__((noinline)) int mutate_local(int a)
{
  int v = a * 2;
  int seen = 0;
  switch (v)
  {
  case 4:
    v = 6;
    seen = 1;
    break;
  case 6:
    seen = 2;
    break;
  default:
    seen = 3;
    break;
  }
  return seen * 100 + v;
}

__attribute__((noinline)) int narrow_signed(signed char c)
{
  switch (c)
  {
  case -128: return 1;
  case -1: return 2;
  case 0: return 3;
  case 1: return 4;
  case 127: return 5;
  }
  return 0;
}

__attribute__((noinline)) int narrow_unsigned(unsigned char c)
{
  switch (c)
  {
  case 0: return 1;
  case 1: return 2;
  case 127: return 3;
  case 128: return 4;
  case 255: return 5;
  }
  return 0;
}

__attribute__((noinline)) int narrow_short(short s)
{
  switch (s)
  {
  case -32768: return 1;
  case -1: return 2;
  case 0: return 3;
  case 32767: return 4;
  }
  return 0;
}

__attribute__((noinline)) int deref(const int *p)
{
  switch (*p)
  {
  case 10: return 1;
  case 20: return 2;
  case 30: return 3;
  }
  return 0;
}

__attribute__((noinline)) int wide(long long v)
{
  switch (v)
  {
  case -1LL: return 1;
  case 0LL: return 2;
  case 0x100000000LL: return 3;
  case 0x7fffffffffffffffLL: return 4;
  }
  return 0;
}

/* Empty case list: the dispatch has nothing to land on. */
__attribute__((noinline)) int only_default(int x)
{
  switch (x)
  {
  default:
    return 42;
  }
}

int main(void)
{
  if (mutate_param(1) != 11 || mutate_param(2) != 21 || mutate_param(99) != 10001 ||
      mutate_param(5) != 70)
  {
    printf("FAIL mutate_param %d %d %d %d\n", mutate_param(1), mutate_param(2), mutate_param(99),
           mutate_param(5));
    return 1;
  }
  if (mutate_local(2) != 106 || mutate_local(3) != 206 || mutate_local(0) != 300)
  {
    printf("FAIL mutate_local %d %d %d\n", mutate_local(2), mutate_local(3), mutate_local(0));
    return 2;
  }
  if (narrow_signed(-128) != 1 || narrow_signed(-1) != 2 || narrow_signed(0) != 3 ||
      narrow_signed(1) != 4 || narrow_signed(127) != 5 || narrow_signed(42) != 0)
  {
    printf("FAIL narrow_signed\n");
    return 3;
  }
  if (narrow_unsigned(0) != 1 || narrow_unsigned(1) != 2 || narrow_unsigned(127) != 3 ||
      narrow_unsigned(128) != 4 || narrow_unsigned(255) != 5 || narrow_unsigned(42) != 0)
  {
    printf("FAIL narrow_unsigned\n");
    return 4;
  }
  if (narrow_short(-32768) != 1 || narrow_short(-1) != 2 || narrow_short(0) != 3 ||
      narrow_short(32767) != 4 || narrow_short(5) != 0)
  {
    printf("FAIL narrow_short\n");
    return 5;
  }
  static const int vals[] = {10, 20, 30, 40};
  if (deref(&vals[0]) != 1 || deref(&vals[1]) != 2 || deref(&vals[2]) != 3 || deref(&vals[3]) != 0)
  {
    printf("FAIL deref\n");
    return 6;
  }
  if (wide(-1LL) != 1 || wide(0LL) != 2 || wide(0x100000000LL) != 3 ||
      wide(0x7fffffffffffffffLL) != 4 || wide(7LL) != 0)
  {
    printf("FAIL wide\n");
    return 7;
  }
  if (only_default(0) != 42 || only_default(-9) != 42)
  {
    printf("FAIL only_default\n");
    return 8;
  }

  printf("OK\n");
  return 0;
}
