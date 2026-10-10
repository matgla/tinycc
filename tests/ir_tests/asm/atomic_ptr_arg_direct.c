/* The generic atomic forms' pointer operands -- a compare-exchange's
 * desired, an exchange's or store's value, as the Zig C backend writes them
 * -- with the value in a parameter, so a round trip through the local's
 * slot cannot hide behind constant folding (test_atomic_ptr_arg_flows_direct).
 * parse_atomic defers the & marking when the expansion is inline, and the
 * value flows from its register straight into STREX/STL: no LEA, no slot
 * traffic. */
#include <stdbool.h>
#include <stdint.h>

__attribute__((noinline)) bool cas_desired_direct(uint32_t *p, uint32_t wanted)
{
  uint32_t expected = 0;
  uint32_t desired = wanted;
  return __atomic_compare_exchange(p, &expected, &desired, false, 2, 0);
}

__attribute__((noinline)) void store_value_direct(uint32_t *p, uint32_t v)
{
  uint32_t val = v;
  __atomic_store(p, &val, 3);
}

__attribute__((noinline)) uint32_t xchg_value_direct(uint32_t *p, uint32_t v)
{
  uint32_t val = v;
  uint32_t ret;
  __atomic_exchange(p, &val, &ret, 2);
  return ret;
}
