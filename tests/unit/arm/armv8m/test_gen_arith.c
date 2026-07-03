/*
 *  test_gen_arith.c - suite for the MachineOperand-based arithmetic/logic
 *  entry points in arm-thumb-gen.c (backend/ binary,
 *  build_backend/run_unit_tests_backend).
 *
 *  Mirrors test_gen_dispatch_smoke.c's style: call tcc_gen_machine_*_mop
 *  DIRECTLY with hand-built MachineOperand arguments (no IR, no dispatch
 *  loop, no frontend), and assert on the real Thumb-2 bytes emitted into a
 *  real Section via the real o()/section_add machinery.
 *
 *  Every expected byte sequence below was captured by actually running the
 *  call (see docs/plan_codegen_unit_tests.md's "verified by trial-linking,
 *  not guessed" discipline) and then cross-checked against the matching
 *  low-level encoder oracle in test_thop_alu_reg.c / test_thop_alu_imm.c /
 *  test_thop_shift_reg.c / test_thop_mul.c / test_thop_bitfield.c for the
 *  same register/immediate shape.
 */

#define USING_GLOBALS
#include "ir.h"
#include "arch/arm/arm.h"
#include "arch/arm/thumb/thumb.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

/* ------------------------------------------------------------------ setup */

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;
}

static MachineOperand mop_reg(int r, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_REG;
  m.btype = btype;
  m.u.reg.r0 = r;
  m.u.reg.r1 = -1;
  return m;
}

/* 64-bit register-pair operand: r0 holds the low word, r1 the high word. */
static MachineOperand mop_reg64(int r0, int r1, int btype)
{
  MachineOperand m = mop_reg(r0, btype);
  m.is_64bit = true;
  m.u.reg.r1 = r1;
  return m;
}

static MachineOperand mop_imm(int64_t val, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_IMM;
  m.btype = btype;
  m.u.imm.val = val;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

/* ------------------------------------------------------------ data_processing_mop */

UT_TEST(test_dp_sub_reg_reg_reg_t16)
{
  setup_gen();

  /* SUB r0, r1, r2 -> real Thumb-1 T1 3-reg encoding 0x1A88, matching
   * th_sub_reg(0,1,2,NOT_IMPORTANT,DEFAULT,NONE) in test_thop_alu_reg.c
   * (test_sub_reg_t16_low_reg3). */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_SUB, 0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x1A88);

  return 0;
}

UT_TEST(test_dp_and_reg_reg_reg_t32)
{
  setup_gen();

  /* AND r0, r1, r2 -- rd(0) != rn(1), so the rdn-rm T16 form's constraint
   * fails and this falls to T32: base 0xEA000000 | rd<<8 | rn<<16 | rm
   * = 0xEA010002 (no shift, flags NOT_IMPORTANT so S=0). */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_AND, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xEA01);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0002);

  return 0;
}

UT_TEST(test_dp_or_reg_reg_reg_t32)
{
  setup_gen();

  /* OR r0, r1, r2 -- same shape as AND above but ORR base 0xEA400000
   * => 0xEA410002. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_OR, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xEA41);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0002);

  return 0;
}

UT_TEST(test_dp_xor_reg_reg_reg_t32)
{
  setup_gen();

  /* XOR r0, r1, r2 -- EOR base 0xEA800000 => 0xEA810002. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_XOR, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xEA81);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0002);

  return 0;
}

UT_TEST(test_dp_shl_reg_reg_reg_t32)
{
  setup_gen();

  /* SHL r0, r1, r2 -- LSL-by-register requires rd==rn for T1; rd(0)!=rn(1)
   * falls to T3: base 0xFA00F000 | rn<<16 | rd<<8 | rm = 0xFA01F002,
   * matching th_lsl_reg(0,1,2,...) in test_thop_shift_reg.c
   * (test_th_lsl_reg_t1_rd_ne_rn_falls_to_t3). */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_SHL, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFA01);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF002);

  return 0;
}

UT_TEST(test_dp_sar_reg_reg_reg_t32)
{
  setup_gen();

  /* SAR r0, r1, r2 -- ASR-by-register, T3 base 0xFA40F000 => 0xFA41F002. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_SAR, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFA41);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF002);

  return 0;
}

UT_TEST(test_dp_shr_reg_reg_reg_t32)
{
  setup_gen();

  /* SHR r0, r1, r2 -- LSR-by-register, T3 base 0xFA20F000 => 0xFA21F002. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_SHR, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFA21);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF002);

  return 0;
}

UT_TEST(test_dp_add_reg_imm_encoding_path)
{
  setup_gen();

  /* ADD r0, r1, #5 -- exercises the MACH_OP_IMM src2 path
   * (mach_ensure_imm_or_reg -> handler.imm_handler). rd(0)!=rn(1) so the
   * T16 imm8 form (which requires rd==rn) doesn't apply; falls to the T16
   * imm3 form: base 0x1C00 | rd | rn<<3 | imm<<6 = 0x1D48, matching
   * th_add_imm(0,1,5,...) in test_thop_alu_imm.c
   * (test_add_imm_rd_ne_rn_falls_to_t2). */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_imm(5, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_ADD, 0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x1D48);

  return 0;
}

/* -------------------------------------------------------- data_processing_mop_flags */

UT_TEST(test_dp_flags_ands_reg_reg_reg)
{
  setup_gen();

  /* ANDS r0, r1, r2 via the flag-setting entry point: same T32 AND shape
   * as test_dp_and_reg_reg_reg_t32 but with S=1 (bit 20) set:
   * 0xEA000000 | S | rd<<8 | rn<<16 | rm = 0xEA110002. */
  tcc_gen_machine_data_processing_mop_flags(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                            mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_AND);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xEA11);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0002);

  return 0;
}

/* -------------------------------------------------------------------- muldiv_mop */

UT_TEST(test_muldiv_mul_reg_reg_reg)
{
  setup_gen();

  /* MUL r0, r1, r2 -- rd(0)!=rm(2) [thumb_mul_regonly(rd,rn,rm) forces the
   * T32 comparison rd!=rm] so falls to T32 MUL: base 0xFB00F000 | rd<<8 |
   * rn<<16 | rm = 0xFB01F002. */
  tcc_gen_machine_muldiv_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                             mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_MUL);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFB01);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF002);

  return 0;
}

UT_TEST(test_muldiv_sdiv_reg_reg_reg)
{
  setup_gen();

  /* DIV r0 = r1 / r2 (signed) -- cortex-m33 has hardware SDIV, so this must
   * emit a direct SDIV instruction, not a softcall. base 0xFB90F0F0 |
   * rd<<8 | rn<<16 | rm = 0xFB91F0F2. */
  tcc_gen_machine_muldiv_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                             mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_DIV);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFB91);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF0F2);

  return 0;
}

UT_TEST(test_muldiv_udiv_reg_reg_reg)
{
  setup_gen();

  /* UDIV r0 = r1 / r2 (unsigned) -- direct hardware UDIV, base 0xFBB0F0F0
   * | rd<<8 | rn<<16 | rm = 0xFBB1F0F2. */
  tcc_gen_machine_muldiv_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                             mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_UDIV);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFBB1);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF0F2);

  return 0;
}

/* ----------------------------------------------------------------------- mla_mop */

UT_TEST(test_mla_dest_eq_src1_mul_src2_plus_accum)
{
  setup_gen();

  /* MLA: dest(r0) = src1(r1) * src2(r2) + accum(r3).
   * th_mla(rd,rn,rm,ra) base 0xFB000000 | rd<<8 | rn<<16 | rm | ra<<12
   * = 0xFB013002. */
  tcc_gen_machine_mla_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                          mop_reg(R0, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFB01);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x3002);

  return 0;
}

/* --------------------------------------------------------------------- umull_mop */

UT_TEST(test_umull_dest_pair_lo_hi)
{
  setup_gen();

  /* UMULL {dest_hi:dest_lo} = src1 * src2 (unsigned).
   * dest = {r0(lo), r1(hi)}, src1=r2, src2=r3.
   * th_umull(rdlo,rdhi,rn,rm) base 0xFBA00000 | rdhi<<8 | rn<<16 | rm |
   * rdlo<<12 = 0xFBA20103. */
  tcc_gen_machine_umull_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32),
                            mop_reg64(R0, R1, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFBA2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0103);

  return 0;
}

/* --------------------------------------------------------------------- smull_mop */

UT_TEST(test_smull_dest_pair_lo_hi)
{
  setup_gen();

  /* SMULL {dest_hi:dest_lo} = src1 * src2 (signed). Mirrors umull_mop's
   * shape but with SMULL base 0xFB800000 => 0xFB820103. */
  tcc_gen_machine_smull_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32),
                            mop_reg64(R0, R1, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xFB82);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0103);

  return 0;
}

/* -------------------------------------------------------------------- pack64_mop */

UT_TEST(test_pack64_lo_hi_into_reg_pair)
{
  setup_gen();

  /* PACK64: dest={r0(lo),r1(hi)} <- src_lo=r2, src_hi=r3.  No register
   * aliasing between src/dst, so this degrades to two plain MOV Rd,Rm
   * assigns (each via tcc_gen_machine_assign_mop): "mov r0,r2" (T1 hi-reg
   * mov form 0x4600 | Rm<<3 | (D:Rd)) = 0x4610, then "mov r1,r3" = 0x4619. */
  tcc_gen_machine_pack64_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32),
                             mop_reg64(R0, R1, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x4610);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4619);

  return 0;
}

/* ---------------------------------------------------------------------- ubfx_mop */

UT_TEST(test_ubfx_lsb8_width4)
{
  setup_gen();

  /* UBFX r0, r1, #8, #4 -- src2 packs lsb (bits 0-4) | width<<5 (bits 5-9).
   * Base 0xF3C00000 | rn<<16 | imm3<<12 | rd<<8 | imm2<<6 | (width-1)
   * = 0xF3C12003.  Cross-checked against test_thop_bitfield.c's
   * test_sbfx_basic (same lsb/width, SBFX base 0xF3400000 -> 0xf3412003):
   * UBFX differs only in the fixed op field (0xC vs 0x4), confirming the
   * lsb/imm3/imm2/width-1 bit placement here is correct. */
  tcc_gen_machine_ubfx_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_imm(8 | (4 << 5), IROP_BTYPE_INT32),
                           mop_reg(R0, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xF3C1);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x2003);

  return 0;
}

/* ----------------------------------------------------------------------- bfi_mop */

UT_TEST(test_bfi_inplace_host_word_eq_dest)
{
  setup_gen();

  /* BFI r0, r1, #8, #4 with src1(host word)==dest==r0: rd==rword so no
   * MOV is inserted before the BFI. params packs lsb (bits 0-7) |
   * width<<8 (bits 8-15). Result matches test_thop_bitfield.c's
   * test_bfi_basic exactly: 0xf361200b. */
  tcc_gen_machine_bfi_mop(mop_reg(R0, IROP_BTYPE_INT32), mop_reg(R1, IROP_BTYPE_INT32),
                          mop_reg(R0, IROP_BTYPE_INT32), 8 | (4 << 8));

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xF361);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x200B);

  return 0;
}

UT_TEST(test_bfi_dest_ne_host_word_inserts_mov)
{
  setup_gen();

  /* BFI dest=r2, src1(host word)=r3, src2(value)=r1, lsb=0, width=8.
   * rd(2)!=rword(3), so a "mov r2, r3" is inserted first (T1 hi-reg mov
   * 0x4600 | Rm<<3 | D:Rd = 0x461A), then BFI r2,r1,#0,#8:
   * 0xF3600000 | rn(1)<<16 | rd(2)<<8 | msb(7) = 0xF3610207. */
  tcc_gen_machine_bfi_mop(mop_reg(R3, IROP_BTYPE_INT32), mop_reg(R1, IROP_BTYPE_INT32),
                          mop_reg(R2, IROP_BTYPE_INT32), 0 | (8 << 8));

  UT_ASSERT_EQ(ind, 6);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x461A);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xF361);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x0207);

  return 0;
}

/* ------------------------------------------------------------------ cmp_eq64_mop */

UT_TEST(test_cmp_eq64_reg_pairs)
{
  setup_gen();

  /* CMP_EQ64 {r1:r0} vs {r3:r2}: emits CMP hi,hi ("cmp r1,r3" -> T1 CMP-reg
   * base 0x4280 | Rm<<3 | Rn = 0x4299), IT EQ (0xBF08), CMPEQ lo,lo
   * ("cmp r0,r2" -> 0x4290). */
  tcc_gen_machine_cmp_eq64_mop(mop_reg64(R0, R1, IROP_BTYPE_INT32), mop_reg64(R2, R3, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 6);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x4299);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xBF08);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x4290);

  return 0;
}

/* ------------------------------------------------------------------------ suite */

UT_SUITE(gen_arith)
{
  /* data_processing_mop */
  UT_RUN(test_dp_sub_reg_reg_reg_t16);
  UT_RUN(test_dp_and_reg_reg_reg_t32);
  UT_RUN(test_dp_or_reg_reg_reg_t32);
  UT_RUN(test_dp_xor_reg_reg_reg_t32);
  UT_RUN(test_dp_shl_reg_reg_reg_t32);
  UT_RUN(test_dp_sar_reg_reg_reg_t32);
  UT_RUN(test_dp_shr_reg_reg_reg_t32);
  UT_RUN(test_dp_add_reg_imm_encoding_path);

  /* data_processing_mop_flags */
  UT_RUN(test_dp_flags_ands_reg_reg_reg);

  /* muldiv_mop */
  UT_RUN(test_muldiv_mul_reg_reg_reg);
  UT_RUN(test_muldiv_sdiv_reg_reg_reg);
  UT_RUN(test_muldiv_udiv_reg_reg_reg);

  /* mla_mop */
  UT_RUN(test_mla_dest_eq_src1_mul_src2_plus_accum);

  /* umull_mop / smull_mop */
  UT_RUN(test_umull_dest_pair_lo_hi);
  UT_RUN(test_smull_dest_pair_lo_hi);

  /* pack64_mop */
  UT_RUN(test_pack64_lo_hi_into_reg_pair);

  /* ubfx_mop */
  UT_RUN(test_ubfx_lsb8_width4);

  /* bfi_mop */
  UT_RUN(test_bfi_inplace_host_word_eq_dest);
  UT_RUN(test_bfi_dest_ne_host_word_inserts_mov);

  /* cmp_eq64_mop */
  UT_RUN(test_cmp_eq64_reg_pairs);
}
