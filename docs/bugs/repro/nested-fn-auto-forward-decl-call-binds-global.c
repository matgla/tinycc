/* docs/bugs/nested-fn-auto-forward-decl-call-binds-global.md
 * A call to a nested function made between its `auto` forward declaration
 * and its definition binds to a global function of that name.
 *
 *   cd tests/ir_tests && python3 run.py \
 *     -c ../../docs/bugs/repro/nested-fn-auto-forward-decl-call-binds-global.c --cflags=-O0
 *
 * Expected (gcc): 8 17        tcc, every level: link error "undefined symbol 'later'" */
#include <stdio.h>
__attribute__((noinline)) int p(int x)
{
  auto int later(int);
  int r = later(2);
  int later(int v) { return v + x; }
  return r + later(x);
}
int main(void) { printf("%d %d\n", p(2), p(5)); return 0; }
