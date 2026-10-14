/* The Zig C backend's SpinLock.lock: a weak compare-exchange whose success
 * goes into an optional's is_null, the optional is copied, and the copy's
 * bool is tested.  Once SRA scalarizes the optionals the branch tests the
 * compare-exchange's own compare -- no 0/1 value (setif chain fold) -- and
 * `expected` stays out of memory (test_zig_optional_cas_branches_directly). */
#include <stdbool.h>
#include <stdint.h>

struct opt_u32 { uint32_t payload; bool is_null; };
struct lk { uint32_t raw; };

void zig_lock(struct lk *const a0)
{
  struct lk *const *t1;
  struct lk *t0;
  uint32_t t2;
  struct opt_u32 t8, t12;
  bool t13;
  uint32_t *t11;
  t0 = a0;
  t1 = &t0;
  t2 = ((uint32_t)(uint8_t)*(volatile uint32_t *)0x4001f000 & 0x7fffffff) + 1;
zig_loop:
  t11 = &(*t1)->raw;
  t12.payload = 0;
  t12.is_null = __atomic_compare_exchange_n((_Atomic uint32_t *)t11, &(t12.payload), t2, 1, 2, 0);
  t8 = t12;
  t13 = !t8.is_null;
  if (t13)
  {
    __asm volatile("wfe" ::: "memory");
    goto zig_loop;
  }
}
