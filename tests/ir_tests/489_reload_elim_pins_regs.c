/* Post-allocation reload elimination drops `T <- StackLoc[s] [LOAD]` when the
   store that filled the slot read the very register T was allocated.  With the
   load gone T has no definition: it holds the value only because it shares
   that register with the stored value (here the slice length, parameter P1 in
   r1).  Codegen's scratch-conflict fixup then moved T to a free callee-saved
   register that nothing ever wrote, so the loop bound was r6's garbage.  This
   is Zig's std.mem.containsAtLeastScalar as the Zig C backend emits it (zig.h
   helpers replaced by casts; debug_assert and isPowerOfTwo must stay
   inlinable so the branch folds away); under the tcc -O1 Zig compiler
   `zig build-obj` failed at startup with "unable to open zig lib directory:
   BadPathName". */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
void debug_assert__163(bool const a0) {
 bool t0;
 t0 = !a0;
 if (t0) {
  __builtin_unreachable();
 }
 goto zig_block_0;

zig_block_0:;
 return;
}
struct slice_u8_49 { uint8_t const *ptr; uintptr_t len; };
bool math_isPowerOfTwo__anon_6516__6850(void) {
 debug_assert__163(true);
 return true;
}
__attribute__((noinline))
bool mem_containsAtLeastScalar__anon_10167__7865(struct slice_u8_49 const a0, uint8_t const a1, uintptr_t const a2) {
 struct slice_u8_49 const *t1;
 uintptr_t const *t2;
 uintptr_t t3;
 uintptr_t t8;
 uintptr_t t14;
 uintptr_t t16;
 uintptr_t t4;
 uintptr_t t5;
 uintptr_t t7;
 struct slice_u8_49 t9;
 struct slice_u8_49 t0;
 uint8_t const *t10;
 uint32_t t11;
 uint32_t t12;
 bool t6;
 uint8_t t13;
 uint8_t t15;
 t0 = a0;
 t1 = (struct slice_u8_49 const *)&t0;
 t2 = &t1->len;
 t3 = (*t2);
 t4 = (uintptr_t)0ul;
 t5 = (uintptr_t)0ul;
 t6 = math_isPowerOfTwo__anon_6516__6850();
 if (t6) {
  goto zig_block_0;
 }
 goto zig_block_0;

zig_block_0:;
 t7 = (uintptr_t)0ul;
 t8 = t4;
 t9 = (*t1);
 t10 = t9.ptr;
 t10 = (uint8_t const *)(((uintptr_t)t10) + (t8*sizeof(uint8_t)));
 t8 = t3 - t8;
 t9.ptr = t10;
 t9.len = t8;
 t8 = t9.len;
 zig_loop_31:
 t3 = t7;
 t11 = (uint32_t)(t3);
 t12 = (uint32_t)(t8);
 t6 = t11 < t12;
 if (t6) {
  t13 = t9.ptr[t3];
  t14 = t5;
  t6 = t13 == a1;
  t15 = (uint8_t)t6;
  t16 = (uint32_t)(t15);
  t16 = t14 + t16;
  t5 = t16;
  t16 = t5;
  t12 = (uint32_t)(t16);
  t11 = (uint32_t)(a2);
  t6 = t12 >= t11;
  if (t6) {
   return true;
  }
  goto zig_block_3;

zig_block_3:;
  goto zig_block_2;
 }
 goto zig_block_1;

zig_block_2:;
 t3 = t3 + (uintptr_t)1ul;
 t7 = t3;
 goto zig_loop_31;

zig_block_1:;
 return false;
}
int main(void)
{
  static const uint8_t p1[] = "/home/x/lib";
  static const uint8_t p2[] = {'a', 0, 'b'};
  struct slice_u8_49 s1 = {p1, sizeof p1 - 1}, s2 = {p2, 3};
  printf("%d\n", mem_containsAtLeastScalar__anon_10167__7865(s1, 0, 1));
  printf("%d\n", mem_containsAtLeastScalar__anon_10167__7865(s2, 0, 1));
  printf("%d\n", mem_containsAtLeastScalar__anon_10167__7865(s1, '/', 3));
  printf("%d\n", mem_containsAtLeastScalar__anon_10167__7865(s1, '/', 4));
  return 0;
}
