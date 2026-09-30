/* dead_addrvar deletes the LEA of an address-taken local that is never read,
   and every STORE through that LEA.  A memset lowered by mem_inline writes
   through STORE_INDEXED, which carries its base in a plain dest: the pass
   neither counted it as a use nor deleted it, so the stores stayed behind and
   wrote through the TEMP whose LEA was gone -- through whatever register the
   allocator gave it.  This is Zig's meta.eql for a u64 key as the Zig C backend
   emits it: at -O1 the two zeroing memsets stored eight zeros through the
   caller's r4 and r5, here the canary pointers of probe(). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef uint64_t bitpack__InternPool_AnalUnit_78050;
__attribute__((noinline))
bool meta_eql__anon_81003__30906(bitpack__InternPool_AnalUnit_78050 const a0, bitpack__InternPool_AnalUnit_78050 const a1) {
 bitpack__InternPool_AnalUnit_78050 t0;
 bitpack__InternPool_AnalUnit_78050 t1;
 bool t2;
 memset(&t0, 0x00, sizeof(bitpack__InternPool_AnalUnit_78050));
 t0 = a0;
 memset(&t1, 0x00, sizeof(bitpack__InternPool_AnalUnit_78050));
 t1 = a1;
 t2 = a0 == a1;
 return t2;
}
/* Four pointers live across the call land in the callee-saved registers, so
   the stray stores zero one of the canaries -- a visible difference on bare
   metal too, where a store through a small integer would go unnoticed. */
__attribute__((noinline))
int probe(uint64_t *c0, uint64_t *c1, uint64_t *c2, uint64_t *c3, uint64_t x, uint64_t y) {
  int eq = meta_eql__anon_81003__30906(x, y);
  *c0 += 1; *c1 += 2; *c2 += 3; *c3 += 4;
  return eq;
}
int main(void) {
  uint64_t c[4] = { 0x1111111111111111ull, 0x2222222222222222ull,
                    0x3333333333333333ull, 0x4444444444444444ull };
  int hits = 0;
  for (int i = 0; i < 4; i++)
    hits += probe(&c[0], &c[1], &c[2], &c[3], 0x100000000ull * i + i, (i & 1) ? 0x100000000ull * i + i : 5);
  for (int i = 0; i < 4; i++)
    printf("c%d=%016llx\n", i, (unsigned long long)c[i]);
  printf("hits=%d\n", hits);
  return 0;
}
