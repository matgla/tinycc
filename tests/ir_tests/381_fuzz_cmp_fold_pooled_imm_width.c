/* Fuzz seed int:945 (also 2399/5117 across profiles): cpt_try_cmp_setif_fold
 * evaluated CMP #0xC573C000, #0xD88D8850 (both 32-bit unsigned) as a 64-BIT
 * unsigned compare because the second constant was pool-stored (IROP_TAG_I64
 * on a non-INT64 operand — it doesn't fit a signed imm32) and
 * cmp_operands_unsigned_width keyed on the storage tag instead of btype.
 * Mixed sign-/zero-extension of the two 32-bit values in their i64 slots then
 * flipped ULE to 0.  Fix: semantic compare width comes from btype only
 * (cond_util.c), matching fold_read_imm32's documented convention. */
#include <stdio.h>

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u8 = 893788631u;
  u8 = (0xC573C000u <= ((~u8) ^ cs));
  printf("checksum=%08x\n", u8);
  return 0;
}
