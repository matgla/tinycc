/* Regression guard for the loop-constant-simulation store-drop miscompile in
 * ir/opt_loop_const_sim.c (tcc_ir_opt_loop_const_sim / lcs_exec).
 *
 * The pass collapses a small constant-trip-count loop by simulating its body
 * and replacing it with residual stores capturing the final state.  The STORE
 * handler silently DROPPED any store whose destination address did not resolve
 * to a tracked stack slot — most importantly a deref through a PARAM pointer
 * (`*y = i` for `int *y`), which targets caller-visible memory.  As a result
 * `void v(int *y){ for(i<16) *y=i; }` compiled to a bare `bx lr`, losing the
 * write entirely.  The fix makes the simulator bail (leaving the loop intact)
 * when a store address is unresolvable and the value was not recorded into a
 * register-promotable VAR slot.
 *
 * These observe the FINAL value through the escaping pointer, so a dropped or
 * mis-folded store changes the result.  Exercised at every optimization level. */
#include <stdio.h>

/* Core bug: unconditional store through a parameter pointer in a counted loop. */
static void store_loop(int *y)
{
  for (int i = 0; i < 16; i++)
    *y = i;
}

/* Struct store through a parameter pointer. */
struct S { int a; };
static void store_struct_loop(struct S *y)
{
  for (int i = 0; i < 16; i++)
  {
    struct S t = { i * 2 };
    *y = t;
  }
}

/* Store through a TEMP that holds a parameter-derived pointer (T = y; *T = …). */
static void store_via_temp(int *y)
{
  int *p = y;
  for (int i = 0; i < 8; i++)
    *p = i + 100;
}

/* trip count == 1 (the pr19853 shape: `for(i<1) *d=v;`). */
static void store_once(char *d)
{
  for (int i = 0; i < 1; i++)
    *d = 42;
}

/* Store into the loop, then read it back through a second pointer afterwards —
 * the final stored value must be observable. */
static void store_then_readback(int *y, int *out)
{
  for (int i = 0; i < 10; i++)
    *y = i * 3;
  *out = *y;
}

int main(void)
{
  int ok = 1;

  int a = -1;
  store_loop(&a);
  if (a != 15) { printf("store_loop FAIL: %d\n", a); ok = 0; }

  struct S s = { -1 };
  store_struct_loop(&s);
  if (s.a != 30) { printf("store_struct_loop FAIL: %d\n", s.a); ok = 0; }

  int b = -1;
  store_via_temp(&b);
  if (b != 107) { printf("store_via_temp FAIL: %d\n", b); ok = 0; }

  char c = 0;
  store_once(&c);
  if (c != 42) { printf("store_once FAIL: %d\n", (int)c); ok = 0; }

  int d = -1, e = -1;
  store_then_readback(&d, &e);
  if (d != 27 || e != 27) { printf("store_then_readback FAIL: %d %d\n", d, e); ok = 0; }

  printf("%s\n", ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}
