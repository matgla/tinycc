/* run_register_coalescing swaps two intervals' registers after allocation so
   a variable can sit where its value arrives -- here from_type's result wants
   R0, which the reload of the by-value struct argument had.  The swap updated
   only the linear-scan intervals; the per-vreg allocation that ra:reload_elim
   reads kept the old registers until compute_stack_layout.  reload_elim saw
   `slot <- R0(a)` then `R0(t) <- slot`, deleted the reload as a no-op, and t's
   new register (R2) was never written: from_type compared and returned
   whatever R2 held.  Zig's compiler (Air.Inst.Ref.fromType, this shape in
   zig.c) segfaulted in its compile-to-C job at -finline-limit=200. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static inline uint32_t shr_u32(uint32_t lhs, uint8_t rhs) { return lhs >> rhs; }
static inline uint32_t intcast_u32(uint32_t arg) { return arg; }
static inline uint32_t truncate_u32(uint32_t arg, uint8_t bits) { return arg & shr_u32(0xffffffffu, 32 - bits); }
static inline uint32_t bitcast_u32(uint32_t arg, uint8_t bits) { return truncate_u32(arg, bits); }

struct Type { uint32_t ip_index; };

static void debug_assert(bool const a0) {
 bool t0;
 t0 = !a0;
 if (t0) {
  __builtin_unreachable();
 }
 goto block_0;
block_0:;
 return;
}

static uint32_t to_intern(struct Type const a0) {
 struct Type const *t1;
 uint32_t const *t2;
 uint32_t t3;
 struct Type t0;
 bool t4;
 t0 = a0;
 t1 = (struct Type const *)&t0;
 t2 = (uint32_t const *)&t1->ip_index;
 t3 = (*t2);
 t4 = t3 != UINT32_MAX;
 debug_assert(t4);
 t2 = (uint32_t const *)&t1->ip_index;
 t3 = (*t2);
 return t3;
}

static uint32_t from_intern(uint32_t const a0) {
 uint32_t t0;
 uint32_t t2;
 uint32_t t3;
 bool t1;
 switch (a0) {
  case UINT32_MAX: {
   goto block_0;
  }
  default: {
   t0 = bitcast_u32(a0, UINT8_C(32));
   t0 = shr_u32(t0, UINT8_C(31));
   t1 = t0 == UINT32_C(0);
   debug_assert(t1);
   t0 = bitcast_u32(a0, UINT8_C(32));
   t2 = intcast_u32(t0);
   t0 = intcast_u32(t2);
   t3 = bitcast_u32(t0, UINT8_C(32));
   return t3;
  }
 }

block_0:;
 return UINT32_MAX;
}

__attribute__((noinline)) uint32_t from_type(struct Type const a0) {
 struct Type const *t1;
 struct Type t2;
 struct Type t0;
 uint32_t t3;
 uint32_t t4;
 t0 = a0;
 t1 = (struct Type const *)&t0;
 t2 = (*t1);
 t3 = to_intern(t2);
 t4 = from_intern(t3);
 return t4;
}

/* Arrives with something other than the argument in R2. */
__attribute__((noinline)) uint32_t call(uint32_t junk0, uint32_t junk1, uint32_t junk2, uint32_t v)
{
    struct Type t = { v };
    (void)junk0;
    (void)junk1;
    (void)junk2;
    return from_type(t);
}

int main(void)
{
    printf("%u\n", (unsigned)call(0, 0, 0xdead, 7));
    printf("%u\n", (unsigned)call(1, 2, 0xbeef, 12345));
    printf("%u\n", (unsigned)call(3, 4, 5, 0x7fffffff));
    return 0;
}
