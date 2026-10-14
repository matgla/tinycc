/*
 * Bug: tcc_ir_opt_cmp_expr_fold (source/opt/flat/scalar/cmp_expr_fold.c) folds
 * `CMP (x + K), (x + K)` to "equal" when both ADD/SUB defs read the same base
 * vreg, without checking that the base still holds the same value at the two
 * defs.  The pass runs on flat pre-SSA IR, where a named local or parameter has
 * several definitions, so
 *
 *   V1 = x + 1;  x = <anything>;  V2 = x + 1;  CMP V1, V2
 *
 * was folded to "equal".  The same hole exists one level down, when the two
 * bases are distinct vregs that are each assigned from one shared source vreg.
 *
 * The functions marked "must be 0/1" below are the cases where the fold is
 * genuinely valid or genuinely wrong, so a fix that simply turned the
 * comparison around (or dropped the fold's result) cannot pass.
 */

#include <stdio.h>

/* Local base, reassigned between the two adds. */
__attribute__((noinline)) int eq_local_base(int n)
{
  int x = n * 7;
  int a = x + 1;
  x = n * 5;
  int b = x + 1;
  return a == b; /* 0 for n != 0 */
}

__attribute__((noinline)) int ne_local_base(int n)
{
  int x = n * 7;
  int a = x + 1;
  x = n * 5;
  int b = x + 1;
  return a != b; /* 1 for n != 0 */
}

/* The fold also decides relational compares, not just == / !=. */
__attribute__((noinline)) int gt_local_base(int n)
{
  int x = n * 7;
  int a = x + 1;
  x = n * 5;
  int b = x + 1;
  return a > b; /* 1 for n > 0 */
}

/* Branch form: the folded compare drove a JUMPIF, so the wrong arm was taken. */
__attribute__((noinline)) int branch_form(int n)
{
  int x = n * 7;
  int a = x + 1;
  x = n * 5;
  int b = x + 1;
  if (a == b)
    return 11;
  return 22; /* 22 for n != 0 */
}

/* SUB instead of ADD. */
__attribute__((noinline)) int sub_local_base(int n)
{
  int x = n * 7;
  int a = x - 3;
  x = n * 5;
  int b = x - 3;
  return a == b; /* 0 for n != 0 */
}

__attribute__((noinline)) int sub_ne_local_base(int n)
{
  int x = n * 7;
  int a = x - 3;
  x = n * 5;
  int b = x - 3;
  return a != b; /* 1 for n != 0 */
}

/* Parameter base, reassigned between the two adds. */
__attribute__((noinline)) int eq_param_base(int x)
{
  int a = x + 1;
  x = x * 3;
  int b = x + 1;
  return a == b; /* 0 for x != 0 */
}

__attribute__((noinline)) int ne_param_base(int x)
{
  int a = x + 1;
  x = x * 3;
  int b = x + 1;
  return a != b; /* 1 for x != 0 */
}

/* The second hole: distinct base vregs, both assigned from one shared source. */
__attribute__((noinline)) int eq_indirect_base(int n)
{
  int x = n * 7;
  int y = x;
  int a = y + 1;
  x = n * 5;
  int z = x;
  int b = z + 1;
  return a == b; /* 0 for n != 0 */
}

/* Control: the base is NOT reassigned, so the fold is valid and must stay. */
__attribute__((noinline)) int eq_same_base(int n)
{
  int x = n * 7;
  int a = x + 1;
  int b = x + 1;
  return a == b; /* 1 */
}

__attribute__((noinline)) int ne_same_base(int n)
{
  int x = n * 7;
  int a = x + 1;
  int b = x + 1;
  return a != b; /* 0 */
}

/* Control: the base IS reassigned, but to the same value - still equal. */
__attribute__((noinline)) int eq_reassigned_same(int n)
{
  int x = n * 7;
  int a = x + 1;
  x = n * 7;
  int b = x + 1;
  return a == b; /* 1 */
}

/* Control: same base, different immediates - never equal. */
__attribute__((noinline)) int diff_imm(int n)
{
  int x = n * 7;
  int a = x + 1;
  int b = x + 2;
  return a == b; /* 0 */
}

int main(void)
{
  __builtin_printf("eq_local=%d\n", eq_local_base(3));       /* 0 */
  __builtin_printf("ne_local=%d\n", ne_local_base(3));       /* 1 */
  __builtin_printf("gt_local=%d\n", gt_local_base(3));       /* 1 */
  __builtin_printf("branch=%d\n", branch_form(3));           /* 22 */
  __builtin_printf("sub_eq=%d\n", sub_local_base(3));        /* 0 */
  __builtin_printf("sub_ne=%d\n", sub_ne_local_base(3));     /* 1 */
  __builtin_printf("eq_param=%d\n", eq_param_base(5));       /* 0 */
  __builtin_printf("ne_param=%d\n", ne_param_base(5));       /* 1 */
  __builtin_printf("indirect=%d\n", eq_indirect_base(3));    /* 0 */
  __builtin_printf("same_base=%d\n", eq_same_base(3));       /* 1 */
  __builtin_printf("same_ne=%d\n", ne_same_base(3));         /* 0 */
  __builtin_printf("reass_same=%d\n", eq_reassigned_same(3)); /* 1 */
  __builtin_printf("diff_imm=%d\n", diff_imm(3));            /* 0 */
  return 0;
}
