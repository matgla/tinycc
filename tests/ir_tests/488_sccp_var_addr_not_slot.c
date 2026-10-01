/* SCCP matched stores and loads to frame slots by offset, and `&t0` (t0 an
   address-taken local, zeroed by a memset through its address) resolves to
   the local's spill placeholder -- an offset shared with unrelated anonymous
   slots.  Here it equalled the slot the by-value struct parameter is spilled
   to, so in `case 0` the load of a0.payload became the memset's 0.  This is
   Zig's InternPool.AnalUnit.wrap as the Zig C backend emits it (zig.h helpers
   replaced by casts; the collision depends on the exact local declaration
   order, so the body is kept verbatim); under the tcc -O1/-O2 Zig compiler
   every comptime unit packed a zero payload. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t enum__InternPool_ComptimeUnit_Id_80413;
typedef uint32_t enum__InternPool_Nav_Index_78147;
typedef uint32_t enum__InternPool_Index_21851;
typedef uint32_t enum__InternPool_MemoizedStateStage_80415;
typedef uint32_t enum__InternPool_AnalUnit_Kind_78055;
typedef uint64_t bitpack__InternPool_AnalUnit_78050;
struct InternPool_AnalUnit_Unwrapped_80411 {
 union {
  enum__InternPool_ComptimeUnit_Id_80413 comptime;
  enum__InternPool_Nav_Index_78147 nav_val;
  enum__InternPool_Nav_Index_78147 nav_ty;
  enum__InternPool_Index_21851 type_layout;
  enum__InternPool_Index_21851 struct_defaults;
  enum__InternPool_Index_21851 func;
  enum__InternPool_MemoizedStateStage_80415 memoized_state;
 } payload;
 enum__InternPool_AnalUnit_Kind_78055 tag;
};
__attribute__((noinline))
bitpack__InternPool_AnalUnit_78050 InternPool_AnalUnit_wrap__23013(struct InternPool_AnalUnit_Unwrapped_80411 const a0) {
 uint64_t t5;
 uint64_t t7;
 bitpack__InternPool_AnalUnit_78050 t0;
 enum__InternPool_AnalUnit_Kind_78055 t1;
 enum__InternPool_ComptimeUnit_Id_80413 t2;
 uint64_t *t3;
 uint64_t *t4;
 uint32_t t6;
 uint64_t *t8;
 enum__InternPool_Nav_Index_78147 t9;
 enum__InternPool_Index_21851 t10;
 enum__InternPool_MemoizedStateStage_80415 t11;
 memset(&t0, 0x00, sizeof(bitpack__InternPool_AnalUnit_78050));
 t1 = a0.tag;
 switch (t1) {
  case UINT32_C(0): {
   t2 = a0.payload.comptime;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t5 = (*t4);
   t5 = t5 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(0));
   t7 = (uint64_t)(t6);
   t7 = t7 << UINT8_C(0);
   t7 = t5 | t7;
   (*t4) = t7;
   goto zig_block_1;

zig_block_1:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t2);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_2;

zig_block_2:;
   goto zig_block_0;
  }
  case UINT32_C(1): {
   t9 = a0.payload.nav_val;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(1));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_3;

zig_block_3:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t9);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_4;

zig_block_4:;
   goto zig_block_0;
  }
  case UINT32_C(2): {
   t9 = a0.payload.nav_ty;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(2));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_5;

zig_block_5:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t9);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_6;

zig_block_6:;
   goto zig_block_0;
  }
  case UINT32_C(3): {
   t10 = a0.payload.type_layout;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(3));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_7;

zig_block_7:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t10);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_8;

zig_block_8:;
   goto zig_block_0;
  }
  case UINT32_C(4): {
   t10 = a0.payload.struct_defaults;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(4));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_9;

zig_block_9:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t10);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_10;

zig_block_10:;
   goto zig_block_0;
  }
  case UINT32_C(5): {
   t10 = a0.payload.func;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(5));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_11;

zig_block_11:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t10);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_12;

zig_block_12:;
   goto zig_block_0;
  }
  case UINT32_C(6): {
   t11 = a0.payload.memoized_state;
   t3 = (uint64_t *)&t0;
   t4 = (uint64_t *)t3;
   t7 = (*t4);
   t7 = t7 & UINT64_C(18446744069414584320);
   t6 = (uint32_t)(UINT32_C(6));
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(0);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_13;

zig_block_13:;
   t8 = (uint64_t *)&t0;
   t6 = (uint32_t)(t11);
   t4 = (uint64_t *)t8;
   t7 = (*t4);
   t7 = t7 & UINT64_C(4294967295);
   t5 = (uint64_t)(t6);
   t5 = t5 << UINT8_C(32);
   t5 = t7 | t5;
   (*t4) = t5;
   goto zig_block_14;

zig_block_14:;
   goto zig_block_0;
  }
  default: {
   __builtin_unreachable();
  }
 }

zig_block_0:;
 return t0;
}
int main(void)
{
  for (uint32_t k = 0; k < 7; k++) {
    struct InternPool_AnalUnit_Unwrapped_80411 u;
    u.payload.comptime = 0x1234 + k;
    u.tag = k;
    printf("%llx\n", (unsigned long long)InternPool_AnalUnit_wrap__23013(u));
  }
  return 0;
}
