/*
 *  test_gen_branch.c - suite for the branch/setif/bool MachineOperand mop
 *  family in arm-thumb-gen.c.
 *
 *  Mirrors test_gen_dispatch_smoke.c: calls tcc_gen_machine_jump_mop,
 *  tcc_gen_machine_conditional_jump_mop, tcc_gen_machine_setif_mop and
 *  tcc_gen_machine_bool_mop DIRECTLY with hand-built MachineOperand /
 *  TCCIRState state and asserts on the real emitted Thumb-2 bytes.
 */

#define USING_GLOBALS
#include "ir.h"
#include "source/backend/arch/arm/arm.h"
#include "source/backend/arch/arm/thumb/thumb.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"
#include "ir_build.h"

#include "ut.h"

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->ir = NULL;
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

/* ------------------------------------------------------------------ branch */

/* Build a minimal TCCIRState carrying only ir_to_code_mapping, populated at
 * target_ir with target_addr, so can_narrow_backward_branch() (arm-thumb-gen.c)
 * sees a genuinely-backward, already-emitted target. */
static TCCIRState *ut_branch_ir_new(int target_ir, uint32_t target_addr, int mapping_size)
{
  TCCIRState *ir = utb_new();
  ir->ir_to_code_mapping = (uint32_t *)tcc_mallocz(sizeof(uint32_t) * (size_t)mapping_size);
  ir->ir_to_code_mapping_size = mapping_size;
  ir->ir_to_code_mapping[target_ir] = target_addr;
  return ir;
}

UT_TEST(test_jump_mop_backward_narrows_to_16bit)
{
  setup_gen();

  /* target_ir=1 emitted at code address 0; branch itself is IR index 5,
   * emitted at ind=20.  offset = 0 - 20 - 4 = -24: negative, even, well
   * within both T1 (-256..254) and T2 (-2048..2046) ranges, so the backward
   * branch must narrow to the 16-bit T2 encoding. */
  TCCIRState *ir = ut_branch_ir_new(/*target_ir=*/1, /*target_addr=*/0, /*mapping_size=*/8);
  tcc_state->ir = ir;
  ind = 20;

  int size = tcc_gen_machine_jump_mop(TCCIR_OP_JUMP, /*target_ir=*/1, /*ir_idx=*/5);

  UT_ASSERT_EQ(size, 2);
  UT_ASSERT_EQ(ind, 22);
  /* th_b_t2(0) narrow unconditional B, T2 base encoding 0xe000 (placeholder
   * immediate 0; the real branch offset is backpatched later by the caller,
   * not by tcc_gen_machine_jump_mop itself -- see the forward-branch
   * placeholder check in test_gen_dispatch_smoke.c for the same pattern). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 20), 0xe000);

  tcc_state->ir = NULL;
  utb_free(ir);

  return 0;
}

UT_TEST(test_conditional_jump_mop_backward_eq_narrows_to_16bit)
{
  setup_gen();

  TCCIRState *ir = ut_branch_ir_new(/*target_ir=*/1, /*target_addr=*/0, /*mapping_size=*/8);
  tcc_state->ir = ir;
  ind = 20;

  int size = tcc_gen_machine_conditional_jump_mop(TOK_EQ, TCCIR_OP_JUMPIF, /*target_ir=*/1, /*ir_idx=*/5);

  UT_ASSERT_EQ(size, 2);
  UT_ASSERT_EQ(ind, 22);
  /* th_b_t1(cond=EQ=0, 0): T1 conditional-branch base 0xd000 | cond<<8 | imm8.
   * mapcc(TOK_EQ) == 0x0 (EQ), matching arm-thumb-gen.c's mapcc() table and
   * the standard ARM condition-code encoding used throughout thop_branch.c. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 20), 0xd000);

  tcc_state->ir = NULL;
  utb_free(ir);

  return 0;
}

UT_TEST(test_conditional_jump_mop_not_backward_uses_32bit_ne)
{
  setup_gen();

  /* tcc_state->ir stays NULL (set by setup_gen()): can_narrow_backward_branch
   * bails out immediately (`!ir`), so this must take the 32-bit T3 path
   * regardless of target/ir_idx values. */
  int size = tcc_gen_machine_conditional_jump_mop(TOK_NE, TCCIR_OP_JUMPIF, /*target_ir=*/5, /*ir_idx=*/0);

  UT_ASSERT_EQ(size, 4);
  UT_ASSERT_EQ(ind, 4);
  /* th_b_t3(cond=NE=0x1, 0): 32-bit conditional B.W T3, cond nibble placed at
   * bits [25:22] of the 32-bit word (rd_place {22,4} in thop_branch.c's
   * SHAPE_B_T3), imm fields all 0 for the placeholder immediate.
   * op = 0xf0008000 | (cond << 22); cond=1 -> hi=0xf040, lo=0x8000. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xf040);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x8000);

  return 0;
}

/* ------------------------------------------------------------------ setif */

UT_TEST(test_setif_mop_eq_32bit_emits_ite_and_movs)
{
  setup_gen();

  /* src carries the raw condition code in u.imm.val, per
   * tcc_gen_machine_setif_mop's documented contract. */
  MachineOperand src = mop_imm(TOK_EQ, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);

  tcc_gen_machine_setif_mop(src, dest, TCCIR_OP_SETIF);

  UT_ASSERT_EQ(ind, 6);
  /* ITE EQ: th_it(cond=0, mask) with mask = ((cond^1)&1)<<3 | 0x4 = 0xC
   * (T-arm keeps cond, E-arm is the opposite -- ARM's ITE mask convention).
   * Base encoding 0xbf00 | cond<<4 | mask = 0xbf00 | 0 | 0xC = 0xbf0c.
   * First halfword low byte 0x0c has the IT-block signature nibble non-zero
   * in bits[3:0], the canonical "this is an IT prefix" pattern (0xBF.. with
   * a non-zero low nibble), matching the IT encodings used elsewhere in
   * arm-thumb-gen.c (e.g. th_it(mapcc(TOK_EQ), 0x8) at line 6558). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xbf0c);
  /* movs r0, #1 (T1, flags NOT_IMPORTANT since tcc_state->ir is NULL here ->
   * flags_safe() returns FLAGS_BEHAVIOUR_NOT_IMPORTANT): 0x2000 | rd<<8 | imm8
   * (test_thop_mov.c's th_mov_imm oracle, e.g. movs r0,#255 => 0x20FF). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x2001);
  /* movs r0, #0. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x2000);

  return 0;
}

/* ------------------------------------------------------------------ bool */

UT_TEST(test_bool_mop_or_reg_reg_emits_orr_it_sequence)
{
  setup_gen();

  MachineOperand src1 = mop_reg(R1, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_reg(R2, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);

  tcc_gen_machine_bool_mop(src1, src2, dest, TCCIR_OP_BOOL_OR);

  /* ORRS r0,r1,r2 (T3, rd!=rn forces the 32-bit 3-operand form; see
   * th_orr_reg's V_REG_RDN_RM(0x4300)/V_REGS(0xEA400000) table in
   * thop_alu_reg.c and test_thop_alu_reg.c's T1-vs-T3 oracle for the same
   * helper) + MOV r0,#0 (T3, flags blocked) + IT NE (2) + MOVNE r0,#1 (T1, 2).
   * Total size verified below; exact opcodes verified against the same
   * th_orr_reg/th_mov_imm/th_it oracles used by the other tests in this file. */
  UT_ASSERT_EQ(ind, 4 + 4 + 2 + 2);

  /* ORRS r0, r1, r2: 0xEA400000 | S(1)<<20 | rn(r1=1)<<16 | rd(r0=0)<<8 | rm(r2=2)
   * = 0xEA510002. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xea51);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x0002);
  /* MOV r0, #0 with FLAGS_BEHAVIOUR_BLOCK forces the flag-preserving 32-bit
   * T3 "mov" (not "movs") encoding: 0xF04F0000 | rd<<8 (MOVW-style modified
   * immediate #0 encoding, same family as the th_mov_imm T3 oracle in
   * test_thop_mov.c: mov r0,#0xFF000000 => 0xF04F407F). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0xf04f);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0x0000);
  /* IT NE: th_it(cond=NE=1, mask=0x8) = 0xbf00 | 1<<4 | 0x8 = 0xbf18. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0xbf18);
  /* MOVNE r0, #1 (T1, NOT_IMPORTANT flags since tcc_state->ir is NULL). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 10), 0x2001);

  return 0;
}

UT_TEST(test_bool_mop_and_reg_reg_emits_cmp_it_sequence)
{
  setup_gen();

  MachineOperand src1 = mop_reg(R1, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_reg(R2, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_INT32);

  tcc_gen_machine_bool_mop(src1, src2, dest, TCCIR_OP_BOOL_AND);

  /* CMP r1,#0 (T1,2) + IT NE (2) + CMPNE r2,#0 (T1,2) + MOV r0,#0 (T3,4,
   * flags blocked) + IT NE (2) + MOVNE r0,#1 (T1,2). */
  UT_ASSERT_EQ(ind, 2 + 2 + 2 + 4 + 2 + 2);

  /* CMP r1, #0: th_cmp_imm oracle 0x2800 | rn<<8 | imm8 (test_thop_cmp.c:
   * CMP R0,#0xFF => 0x28FF), rn=r1=1, imm=0 -> 0x2900. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x2900);
  /* IT NE (first): 0xbf18. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xbf18);
  /* CMPNE r2, #0: rn=r2=2, imm=0 -> 0x2A00. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x2a00);
  /* MOV r0, #0 (T3, flags blocked): 0xF04F0000. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0xf04f);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0x0000);
  /* IT NE (second): 0xbf18. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 10), 0xbf18);
  /* MOVNE r0, #1: 0x2001. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 12), 0x2001);

  return 0;
}
