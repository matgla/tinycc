/* Fuzz regression (bitfield seed 12264; O1/O2 miscompile), minimized:
 * sl_forward's FORWARD-SUBBYTE and CROSS-MERGE read stored_value.u.imm32 raw
 * after only checking irop_is_immediate().  An unsigned 32-bit constant
 * > INT32_MAX (here the packed-bitfield word 0xA011CE00) is encoded as an
 * I64 POOL immediate whose u.imm32 is the pool INDEX, so the byte extracted
 * for a sub-word load came from index 0 instead of the value — the b3 field
 * write vanished.  Fix: read via irop_get_imm64_ex, rebuild merged operands
 * with irop_make_imm32. */
#include <stdio.h>
struct BFP { unsigned b0:8; unsigned b1:13; unsigned b2:7; unsigned b3:4; } __attribute__((packed));
int main(void){
  struct BFP bf = {0,0,0,0};
  bf.b3 = 10;
  bf.b1 = 4558;
  bf.b3 = 2;
  bf.b1 = 4095;
  printf("checksum=%08x\n", (unsigned)bf.b0 ^ (unsigned)(bf.b1*3u) ^ (unsigned)(bf.b2*5u) ^ (unsigned)(bf.b3*7u));
  return 0;
}
