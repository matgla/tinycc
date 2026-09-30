/* SCCP matches stores and loads to frame slots by offset.  A store to a VAR
   (`V8 <-- #0`, the zeroing memset of an address-taken local) carries the
   VAR's spill placeholder, not a slot -- every load site refuses to match a
   VAR operand by its offset, but the store side took it as one.  Here the
   placeholder equalled the slot the by-value parameter a0 is spilled to, so
   in case 0 the load of a0.payload folded to the memset's 0.  This is Zig's
   InternPool.CaptureValue.wrap as the Zig C backend emits it (zig.h helpers
   replaced by equivalents; the collision depends on the exact local order,
   so the body is kept verbatim).  Same bug class as test 488. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t enum__InternPool_Index_21851;
typedef uint32_t enum__InternPool_Nav_Index_78147;
typedef uint32_t bitpack__InternPool_CaptureValue_80627;
typedef uint8_t enum___40typeInfo_28InternPool_CaptureValue_Unwrapped_29__40_22union_22_tag_type__3f_197373;
struct InternPool_CaptureValue_Unwrapped_197371 {
 union {
  enum__InternPool_Index_21851 comptime;
  enum__InternPool_Index_21851 runtime;
  enum__InternPool_Nav_Index_78147 nav_val;
  enum__InternPool_Nav_Index_78147 nav_ref;
 } payload;
 enum___40typeInfo_28InternPool_CaptureValue_Unwrapped_29__40_22union_22_tag_type__3f_197373 tag;
};
static inline uint8_t zig_u8_bitCast_u8(uint8_t v, uint8_t b) { (void)b; return v; }
static inline uint32_t zig_u32_bitCast_u32(uint32_t v, uint8_t b) { (void)b; return v; }
static inline uint32_t zig_u32_intCast_u8(uint8_t v) { return v; }
static inline uint32_t zig_u32_intCast_u32(uint32_t v) { return v; }
__attribute__((noinline))
bitpack__InternPool_CaptureValue_80627 InternPool_CaptureValue_wrap__30813(struct InternPool_CaptureValue_Unwrapped_197371 const a0) {
 enum__InternPool_Index_21851 t2;
 uint32_t *t3;
 uint32_t *t4;
 uint32_t t5;
 uint32_t t7;
 uint32_t *t8;
 uint32_t t9;
 enum__InternPool_Nav_Index_78147 t10;
 bitpack__InternPool_CaptureValue_80627 t0;
 enum___40typeInfo_28InternPool_CaptureValue_Unwrapped_29__40_22union_22_tag_type__3f_197373 t1;
 uint8_t t6;
 memset(&t0, 0x00, sizeof(bitpack__InternPool_CaptureValue_80627));
 t1 = a0.tag;
 switch (t1) {
  case UINT8_C(0): {
   t2 = a0.payload.comptime;
   t3 = (uint32_t *)&t0;
   t4 = (uint32_t *)t3;
   t5 = (*t4);
   t5 = t5 & UINT32_C(4294967292);
   t6 = zig_u8_bitCast_u8(UINT8_C(0), UINT8_C(2));
   t7 = zig_u32_intCast_u8(t6);
   t7 = t7 << UINT8_C(0);
   t7 = t5 | t7;
   (*t4) = t7;
   goto zig_block_1;

zig_block_1:;
   t8 = (uint32_t *)&t0;
   t7 = zig_u32_bitCast_u32(t2, UINT8_C(32));
   t9 = zig_u32_intCast_u32(t7);
   t4 = (uint32_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT32_C(3);
   t5 = zig_u32_intCast_u32(t9);
   t5 = t5 << UINT8_C(2);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_2;

zig_block_2:;
   goto zig_block_0;
  }
  case UINT8_C(1): {
   t2 = a0.payload.runtime;
   t3 = (uint32_t *)&t0;
   t4 = (uint32_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT32_C(4294967292);
   t6 = zig_u8_bitCast_u8(UINT8_C(1), UINT8_C(2));
   t5 = zig_u32_intCast_u8(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_3;

zig_block_3:;
   t8 = (uint32_t *)&t0;
   t7 = zig_u32_bitCast_u32(t2, UINT8_C(32));
   t9 = zig_u32_intCast_u32(t7);
   t4 = (uint32_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT32_C(3);
   t5 = zig_u32_intCast_u32(t9);
   t5 = t5 << UINT8_C(2);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_4;

zig_block_4:;
   goto zig_block_0;
  }
  case UINT8_C(2): {
   t10 = a0.payload.nav_val;
   t3 = (uint32_t *)&t0;
   t4 = (uint32_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT32_C(4294967292);
   t6 = zig_u8_bitCast_u8(UINT8_C(2), UINT8_C(2));
   t5 = zig_u32_intCast_u8(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_5;

zig_block_5:;
   t8 = (uint32_t *)&t0;
   t7 = zig_u32_bitCast_u32(t10, UINT8_C(32));
   t9 = zig_u32_intCast_u32(t7);
   t4 = (uint32_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT32_C(3);
   t5 = zig_u32_intCast_u32(t9);
   t5 = t5 << UINT8_C(2);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_6;

zig_block_6:;
   goto zig_block_0;
  }
  case UINT8_C(3): {
   t10 = a0.payload.nav_ref;
   t3 = (uint32_t *)&t0;
   t4 = (uint32_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT32_C(4294967292);
   t6 = zig_u8_bitCast_u8(UINT8_C(3), UINT8_C(2));
   t5 = zig_u32_intCast_u8(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_7;

zig_block_7:;
   t8 = (uint32_t *)&t0;
   t7 = zig_u32_bitCast_u32(t10, UINT8_C(32));
   t9 = zig_u32_intCast_u32(t7);
   t4 = (uint32_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT32_C(3);
   t5 = zig_u32_intCast_u32(t9);
   t5 = t5 << UINT8_C(2);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_8;

zig_block_8:;
   goto zig_block_0;
  }
  default: __builtin_unreachable();

 }

zig_block_0:;
 return t0;
}
int main(void) {
  for (int tag = 0; tag < 4; tag++) {
    struct InternPool_CaptureValue_Unwrapped_197371 u;
    u.payload.comptime = 0x1234 + tag;
    u.tag = (uint8_t)tag;
    printf("%d: %08x\n", tag, (unsigned)InternPool_CaptureValue_wrap__30813(u));
  }
  return 0;
}
