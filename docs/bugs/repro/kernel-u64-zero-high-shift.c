/* kernel-u64-zero-high-shift: 64-bit shifts of a zero-extended narrow value
 * keep computing (and materializing) the high word.
 *
 * Compile exactly like the kernel C (from the yasos.zig checkout root):
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
 *     -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
 *     -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib \
 *     -c libs/tinycc/docs/bugs/repro/kernel-u64-zero-high-shift.c -o /tmp/zh.o
 *   arm-none-eabi-objdump -d /tmp/zh.o
 * Contrast: arm-none-eabi-gcc -O2 -mcpu=cortex-m33 -mthumb on the plain_* part
 * (compile with -DPLAIN_ONLY to skip zig.h).
 */
#ifndef PLAIN_ONLY
#define ZIG_TARGET_SOFT_COMPILER_RT_F80_ABI
#define ZIG_TARGET_SOFT_COMPILER_RT_F128_ABI
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include "zig.h"
#endif
#include <stdint.h>

/* ---- plain C: smallest shapes ---------------------------------------- */
/* gcc: movs r1,#0; lsls r0,r0,#16.  tcc: 6 insns, shifts a materialized 0. */
uint64_t plain_shl16(uint16_t k) { uint64_t n = k; return n << 16; }
/* gcc: movs r1,#0; lsrs r0,r0,#8.  tcc: orr lo with (0 << 24), lsr of 0. */
uint64_t plain_shr8(uint16_t k) { uint64_t n = k; return n >> 8; }
/* gcc: 2 insns.  tcc: eor of the high literal with a materialized 0. */
uint64_t plain_xor_hi(uint16_t k) { uint64_t n = k; return n ^ 0xe7037ed1a0b428dbull; }
/* IntegerContext(u16).hash's key expansion.  gcc 7 insns, tcc 19. */
uint64_t plain_expand(uint16_t k)
{
  uint64_t n = k;
  return ((n & 0xff) << 16) | ((n >> 8) << 8) | (n >> 8);
}

#ifndef PLAIN_ONLY
/* ---- Zig C backend spelling (kernel.c, inlined into
 * hash_map.Custom(u16,ProcessInterface.FileHandle,...).getIndex) ----------- */
uint64_t u16_key_hash(uint16_t const a1)
{
  uint64_t t7, t8, t9, t10;
  zig_u128 t12, t13;
  uint64_t const *t11;
  uint16_t t6;
  t6 = zig_u16_bitCast_u16(a1, UINT8_C(16));
  t7 = zig_u64_intCast_u16(t6);
  t9 = t7 & UINT64_C(255);
  t9 = zig_shlw_u64(t9, UINT8_C(16), UINT8_C(64));
  t10 = zig_shr_u64(t7, UINT8_C(8));
  t10 = zig_shlw_u64(t10, UINT8_C(8), UINT8_C(64));
  t10 = t9 | t10;
  t7 = zig_shr_u64(t7, UINT8_C(8));
  t7 = t10 | t7;
  t8 = t7;
  t11 = (uint64_t const *)&t8;
  t7 = (*t11);
  t7 = t7 ^ UINT64_C(16646288086500911323);
  t12 = zig_u128_intCast_u64(t7);
  t12 = zig_mulw_u128(t12, zig_make_u128(UINT64_C(0), UINT64_C(2302960717771869484)), UINT8_C(128));
  t7 = zig_u64_truncate_u128(t12, UINT8_C(64));
  t12 = zig_shr_u128(t12, UINT8_C(64));
  t10 = zig_u64_truncate_u128(t12, UINT8_C(64));
  t7 = t7 ^ UINT64_C(11562461410679940143);
  t7 = t7 ^ UINT64_C(2);
  t12 = zig_u128_intCast_u64(t7);
  t10 = t10 ^ UINT64_C(16646288086500911323);
  t13 = zig_u128_intCast_u64(t10);
  t13 = zig_mulw_u128(t12, t13, UINT8_C(128));
  t10 = zig_u64_truncate_u128(t13, UINT8_C(64));
  t13 = zig_shr_u128(t13, UINT8_C(64));
  t7 = zig_u64_truncate_u128(t13, UINT8_C(64));
  return t10 ^ t7;
}

/* Control: identical hash, key expansion done in 32 bits so the high word is
 * literally zero.  73 -> 61 instructions; the difference is what a 64-bit
 * known-bits fold would recover in the kernel's getIndex. */
uint64_t u16_key_hash_narrow(uint16_t const a1)
{
  uint64_t t7, t8, t10;
  zig_u128 t12, t13;
  uint64_t const *t11;
  {
    uint32_t n = a1;
    t8 = (uint64_t)(((n & 0xffu) << 16) | ((n >> 8) << 8) | (n >> 8));
  }
  t11 = (uint64_t const *)&t8;
  t7 = (*t11);
  t7 = t7 ^ UINT64_C(16646288086500911323);
  t12 = zig_u128_intCast_u64(t7);
  t12 = zig_mulw_u128(t12, zig_make_u128(UINT64_C(0), UINT64_C(2302960717771869484)), UINT8_C(128));
  t7 = zig_u64_truncate_u128(t12, UINT8_C(64));
  t12 = zig_shr_u128(t12, UINT8_C(64));
  t10 = zig_u64_truncate_u128(t12, UINT8_C(64));
  t7 = t7 ^ UINT64_C(11562461410679940143);
  t7 = t7 ^ UINT64_C(2);
  t12 = zig_u128_intCast_u64(t7);
  t10 = t10 ^ UINT64_C(16646288086500911323);
  t13 = zig_u128_intCast_u64(t10);
  t13 = zig_mulw_u128(t12, t13, UINT8_C(128));
  t10 = zig_u64_truncate_u128(t13, UINT8_C(64));
  t13 = zig_shr_u128(t13, UINT8_C(64));
  t7 = zig_u64_truncate_u128(t13, UINT8_C(64));
  return t10 ^ t7;
}
#endif
