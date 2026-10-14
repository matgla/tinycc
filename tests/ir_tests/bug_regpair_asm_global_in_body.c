/* r0/r1 aggregate returns: asm in a function body that makes a static
 * aggregate-returning function global or weak, after a direct call of it.
 * At -O1+ the bodies are generated after the TU is parsed, so the direct
 * call had already made the function r0/r1 when the asm ran -- and
 * `.global f` was then a compile error.  Like gcc, the function stays usable
 * by name from the other TU (bug_regpair_asm_global_in_body+.c), which calls
 * it with the AAPCS convention. */
#include <stdio.h>

typedef struct { int a, b; } P;

static P __attribute__((noinline)) f(int x) { P p = {x + 1, x * 3}; return p; }
static P __attribute__((noinline)) g(int x) { P p = {x + 2, x * 5}; return p; }
static P __attribute__((noinline)) h(int x) { P p = {x + 3, x * 7}; return p; }

/* The asm in a static inline helper, replayed into its caller. */
static inline __attribute__((always_inline)) void export_h(void) { __asm__ volatile(".global h"); }

P call_f(int x);
P call_g(int x);
P call_h(int x);

int __attribute__((noinline)) user_f(int x)
{
  P a = f(x);
  __asm__(".global f");
  return a.a;
}

int __attribute__((noinline)) user_g(int x)
{
  P a = g(x);
  __asm__(".weak g");
  return a.b;
}

int __attribute__((noinline)) user_h(int x)
{
  P a = h(x);
  export_h();
  return a.a + a.b;
}

int main(void)
{
  P p = call_f(4), q = call_g(3), r = call_h(2);
  printf("%d %d %d\n", user_f(1), user_g(2), user_h(3));
  printf("%d %d %d %d %d %d\n", p.a, p.b, q.a, q.b, r.a, r.b);
  return 0;
}
