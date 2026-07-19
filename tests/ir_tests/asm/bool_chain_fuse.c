/* Bool-check chain folding (source/opt/flat/scalar/branch.c):
 *   bool_call_norm    — `c = f()` where f returns _Bool skips the defensive
 *                       CMP #0 / SETIF renormalization (AAPCS bool is 0/1)
 *   setif_xor_invert  — (SETIF cond) ^ 1 -> SETIF !cond;  ^ 0 -> SETIF cond
 *   setif_branch_fuse — NOP/alias-tolerant CMP;SETIF;TEST_ZERO;JUMPIF fusion
 * Together an inlined `if ((c != k) ^ b) abort()` bool check compiles to
 * `bl f; cmp; b<cond>` with no ITE materialization (20141107-1). */
#define bool _Bool
extern void abort(void);
bool f(int a, bool c) __attribute__((noinline));

void chk_direct(int a, bool b)
{
  bool c = f(a, b);
  if ((c != 1) ^ 0)
    abort();
}

void chk_inverted(int a, bool b)
{
  bool c = f(a, b);
  if ((c != 1) ^ 1)
    abort();
}
