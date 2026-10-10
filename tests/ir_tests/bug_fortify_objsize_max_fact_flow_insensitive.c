/* Regression: the _FORTIFY_SOURCE __*_chk elision trusted a per-Sym "max
 * value" fact that ignores control flow.  A local length assigned 4 before
 * a loop kept its n <= 4 fact at a call inside the loop body (the next
 * trip's back-edge assignment is parsed after the call), and an address-
 * taken local's fact survived writes through the pointer or a callee.
 * The fix rejects the Sym fact for address-taken locals and for uses
 * inside a loop body, and keeps the runtime check when the objsize
 * argument is not a compile-time constant.
 *
 * Each case prints 1 when the compiler kept the __*_chk call (our
 * definition records it) and 0 when it elided to the base function.
 * Straight-line single-definition locals must stay elided (the 0).
 */
#include <stdio.h>

static const char src[16] = "0123456789abcdef";
volatile int kept;

/* Called only when the compiler kept the runtime check, which makes its
 * choice of callee observable. */
void *__memcpy_chk(void *d, const void *s, unsigned n, unsigned os)
{
  kept = 1;
  return d;
}

int getu(void)
{
  return 4;
}

void readn(unsigned *p)
{
  (void)p;
}

/* n's fact is recorded before the loop; the reassignment later in the
 * body reaches the call on the next trip through the back edge. */
static void case_loop(void)
{
  char buf[8];
  unsigned n = 4;
  int i;
  for (i = 0; i < 2; i++)
  {
    __builtin___memcpy_chk(buf, src, n, sizeof buf);
    n = (unsigned)getu();
  }
}

/* The callee may write n through the escaped address. */
static void case_escaped(void)
{
  char buf[8];
  unsigned n = 4;
  readn(&n);
  __builtin___memcpy_chk(buf, src, n, sizeof buf);
}

/* A write through a pointer never reaches update_local_scalar_max_bound. */
static void case_viaptr(void)
{
  char buf[8];
  unsigned n = 4;
  unsigned *p = &n;
  *p = (unsigned)getu();
  __builtin___memcpy_chk(buf, src, n, sizeof buf);
}

/* Control: no fact exists for a parameter, so the check was always kept. */
static void case_param(unsigned n)
{
  char buf[8];
  __builtin___memcpy_chk(buf, src, n, sizeof buf);
}

/* Must stay elided: straight-line use after the only definition, local
 * never address-taken. */
static void case_straight(void)
{
  char buf[8];
  unsigned n = 4;
  __builtin___memcpy_chk(buf, src, n, sizeof buf);
}

/* A run-time objsize still permits a run-time check: pass it on. */
static void case_dynos(unsigned os)
{
  char buf[8];
  __builtin___memcpy_chk(buf, src, 4u, os);
}

int main(void)
{
  case_loop();
  printf("%d", kept);
  kept = 0;
  case_escaped();
  printf("%d", kept);
  kept = 0;
  case_viaptr();
  printf("%d", kept);
  kept = 0;
  case_param(4);
  printf("%d", kept);
  kept = 0;
  case_straight();
  printf("%d", kept);
  kept = 0;
  case_dynos(8);
  printf("%d", kept);
  printf("\n");
  return 0;
}
