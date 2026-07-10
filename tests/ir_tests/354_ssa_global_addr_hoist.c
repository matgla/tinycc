/* ssa:global_addr_hoist — park a global address reloaded across calls in a
 * callee-saved register instead of a literal-pool load at each use.
 *
 * `vv` (a global int) is used inside address arithmetic `table[vv*3+k]` on both
 * sides of six opaque sink() calls.  sink() might change vv, so the loaded
 * value can't be reused across the call — but &vv and &table are link-time
 * constants, so at -O2 the pass materializes each once into a callee-saved
 * register kept live across all six calls instead of reloading from the literal
 * pool at every use (this shape — a global dereferenced inside an MLA/MUL
 * address computation — is exactly the strlen-4 test_array_ptr residual the
 * existing simple-address hoisting leaves behind).
 *
 * The contract pinned here is CORRECTNESS: the hoisted registers must alias the
 * same globals the pool loads did, so the indexed reads land on the right slots.
 */
#include <stdio.h>

int vv = 2;
int table[32];
int sink_calls;

void sink(void) { sink_calls++; }

int run(void)
{
  int s = 0;
  s += table[vv * 3 + 1]; sink();
  s += table[vv * 3 + 2]; sink();
  s += table[vv * 3 + 3]; sink();
  s += table[vv * 3 + 4]; sink();
  s += table[vv * 3 + 5]; sink();
  s += table[vv * 3 + 6]; sink();
  return s;
}

int main(void)
{
  for (int i = 0; i < 32; i++)
    table[i] = i * i;
  int r = run();  /* vv*3=6 -> table[7..12] = 49+64+81+100+121+144 = 559 */
  printf("s=%d calls=%d\n", r, sink_calls);
  return 0;
}
