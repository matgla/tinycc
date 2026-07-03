/*
 *  test_gen_dispatch_smoke.c - Phase 0 feasibility spike for the backend/
 *  binary (build_backend/run_unit_tests_backend).
 *
 *  Proves the codegen_backend_stubs.c link works and that
 *  tcc_gen_machine_*_mop functions can be called DIRECTLY (bypassing
 *  ir/codegen.c's dispatch loop entirely) with hand-built MachineOperand
 *  arguments, emitting real Thumb-2 bytes into a real Section via the real
 *  o()/section_add machinery. No IR, no dispatch loop, no frontend --
 *  see test_thop_alu_reg.c for the analogous "call the low-level encoder
 *  directly, assert on the exact opcode" style this mirrors one level up.
 */

#define USING_GLOBALS
#include "ir.h"
#include "arch/arm/arm.h"
#include "arch/arm/thumb/thumb.h"
#include "arch/arm/thumb/thop_branch.h"
#include "arch/arm/thumb/thop_system.h"
#include "arch/arm/thumb/thop_pld.h"
#include "arch/arm/thumb/thop_alu_imm.h"
#include "arch/arm/thumb/thop_alu_reg.h"
#include "arch/arm/thumb/thop_mem_imm.h"
#include "arch/arm/thumb/thop_ldrd.h"
#include "arch/arm/thumb/thop_mov.h"
#include "ir/machine_op.h"

extern int offset_to_args;
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

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

static MachineOperand mop_reg_deref(int r, int btype)
{
  MachineOperand m = mop_reg(r, btype);
  m.needs_deref = true;
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

static MachineOperand mop_none(void)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_NONE;
  return m;
}

static MachineOperand mop_param_stack(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_PARAM_STACK;
  m.btype = btype;
  m.u.param.offset = offset;
  return m;
}

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static int bytes_match_opcode(int n, thumb_opcode op)
{
  if (n != op.size)
    return 0;
  const unsigned char *d = cur_text_section->data;
  if (op.size == 2)
    return d[0] == (op.opcode & 0xff) && d[1] == ((op.opcode >> 8) & 0xff);
  uint16_t hw0 = (uint16_t)(op.opcode >> 16);
  uint16_t hw1 = (uint16_t)(op.opcode & 0xffff);
  return d[0] == (hw0 & 0xff) && d[1] == ((hw0 >> 8) & 0xff) && d[2] == (hw1 & 0xff) &&
         d[3] == ((hw1 >> 8) & 0xff);
}

/* ------------------------------------------------------------------ arith */

UT_TEST(test_dispatch_add_reg_reg_reg_emits_real_bytes)
{
  setup_gen();

  /* ADD R0, R1, R2 -> real Thumb-1 T1 encoding 0x1888. */
  tcc_gen_machine_data_processing_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                      mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_ADD, 0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x1888);

  return 0;
}

/* ------------------------------------------------------------------ mem */

UT_TEST(test_dispatch_load_reg_offset_zero_emits_real_bytes)
{
  setup_gen();

  /* LDR R3, [R1] -> real Thumb-1 T1 encoding 0x680b. */
  tcc_gen_machine_load_mop(mop_reg_deref(R1, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x680b);

  return 0;
}

UT_TEST(test_dispatch_store_reg_offset_zero_emits_real_bytes)
{
  setup_gen();

  /* STR R3, [R1] -> real Thumb-1 T1 encoding 0x600b. */
  tcc_gen_machine_store_mop(mop_reg_deref(R1, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32), TCCIR_OP_STORE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x600b);

  return 0;
}

/* ------------------------------------------------------------------ branch */

UT_TEST(test_dispatch_jump_forward_uses_32bit_encoding)
{
  setup_gen();

  /* A forward branch (target_ir >= ir_idx, and/or tcc_state->ir unset) can't
   * be proven backward-narrowable, so tcc_gen_machine_jump_mop must choose
   * the 32-bit B.W T4 form: first halfword 0xf000, second 0xb800. */
  int size = tcc_gen_machine_jump_mop(TCCIR_OP_JUMP, /*target_ir=*/5, /*ir_idx=*/0);

  UT_ASSERT_EQ(size, 4);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xf000);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xb800);

  return 0;
}

/* ------------------------------------------------------------------ additional mop smoke tests */

UT_TEST(test_dispatch_indirect_jump_reg_emits_bx)
{
  setup_gen();

  /* tcc_gen_machine_indirect_jump_mop: BX target_reg. */
  tcc_gen_machine_indirect_jump_mop(mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_JUMP);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_bx_reg(R2)));

  return 0;
}

UT_TEST(test_dispatch_trap_mop_emits_udf)
{
  setup_gen();

  tcc_gen_machine_trap_mop();

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_udf(0xfe, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_prefetch_reg_emits_pld)
{
  setup_gen();

  /* PLD [R2] via the MACH_OP_REG indirect path. */
  tcc_gen_machine_prefetch_mop(mop_reg(R2, IROP_BTYPE_INT32), /*rw=*/0);

  UT_ASSERT(bytes_match_opcode(ind, th_pld_imm(R2, 0, 0)));

  return 0;
}

UT_TEST(test_dispatch_vla_sp_save_reg_emits_mov_sp)
{
  setup_gen();

  /* VLA_SP_SAVE with a register destination copies SP directly. */
  tcc_gen_machine_vla_mop(mop_reg(R3, IROP_BTYPE_INT32), mop_none(), mop_none(), TCCIR_OP_VLA_SP_SAVE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R3, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_dispatch_vla_sp_restore_reg_emits_mov_sp)
{
  setup_gen();

  /* VLA_SP_RESTORE loads the saved SP from a register and moves it back to SP. */
  tcc_gen_machine_vla_mop(mop_none(), mop_reg(R3, IROP_BTYPE_INT32), mop_none(), TCCIR_OP_VLA_SP_RESTORE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R_SP, R3, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

UT_TEST(test_dispatch_select_imm_imm_emits_ite_movs)
{
  setup_gen();

  /* SELECT cond=EQ, then=1, else=0, dest=R0. Both operands are inlineable
   * immediates, so the ITE block contains two MOVS instructions. */
  tcc_gen_machine_select_mop(mop_imm(1, IROP_BTYPE_INT32), mop_imm(0, IROP_BTYPE_INT32),
                             mop_reg(R0, IROP_BTYPE_INT32), TOK_EQ);

  UT_ASSERT_EQ(ind, 6);
  /* ITE EQ: same encoding as the setif test uses (mask 0xC). */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0xbf0c);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x2001); /* MOVS R0, #1 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x2000); /* MOVS R0, #0 */

  return 0;
}

UT_TEST(test_dispatch_select_identity_then_uses_inverse_cond)
{
  setup_gen();

  /* SELECT cond=EQ, then=R0 (already dest), else=0. The identity shortcut
   * emits IT NE + MOV R0,#0 instead of ITE + two MOVs. */
  tcc_gen_machine_select_mop(mop_reg(R0, IROP_BTYPE_INT32), mop_imm(0, IROP_BTYPE_INT32),
                             mop_reg(R0, IROP_BTYPE_INT32), TOK_EQ);

  UT_ASSERT_EQ(ind, 4);
  /* IT NE (single instruction, mask 0x8): 0xbf18. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0xbf18);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x2000); /* MOVS R0, #0 */

  return 0;
}

UT_TEST(test_dispatch_backpatch_jump_to_next_insn_becomes_nop)
{
  setup_gen();

  /* Emit a backward-narrowable unconditional branch at ind=20. */
  TCCIRState *ir = (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
  ir->ir_to_code_mapping = (uint32_t *)tcc_mallocz(sizeof(uint32_t) * 8);
  ir->ir_to_code_mapping_size = 8;
  ir->ir_to_code_mapping[1] = 0;
  tcc_state->ir = ir;
  ind = 20;

  tcc_gen_machine_jump_mop(TCCIR_OP_JUMP, /*target_ir=*/1, /*ir_idx=*/5);
  UT_ASSERT_EQ(ind, 22);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 20), 0xe000);

  /* Backpatch the branch so its target is the instruction immediately after
   * the 16-bit branch (lt + 2). th_patch_call replaces it with NOP. */
  tcc_gen_machine_backpatch_jump(/*address=*/20, /*offset=*/22);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 20), 0xbf00);

  tcc_state->ir = NULL;
  tcc_free(ir->ir_to_code_mapping);
  tcc_free(ir);

  return 0;
}

UT_TEST(test_dispatch_store_spill_fp_emits_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* STR R3, [FP, #-8] via the public store-spill helper. */
  tcc_gen_machine_store_spill(R3, -8);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R3, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_try_strd_spill_aligned_emits_strd)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* STRD R2, R3, [FP, #-8] -- adjacent offsets, 8-byte aligned. */
  int ok = tcc_gen_machine_try_strd_spill(R2, -8, R3, -4);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(ind, th_strd_imm(R2, R3, R_FP, 8, 4 /* subtract */)));

  return 0;
}

UT_TEST(test_dispatch_try_ldrd_spill_aligned_emits_ldrd)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* LDRD R2, R3, [FP, #-8] -- adjacent offsets, 8-byte aligned. */
  int ok = tcc_gen_machine_try_ldrd_spill(R2, -8, R3, -4);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(ind, th_ldrd_imm(R2, R3, R_FP, 8, 4 /* subtract */)));

  return 0;
}

UT_TEST(test_dispatch_try_strd_base_aligned_emits_strd)
{
  setup_gen();

  /* STRD R2, R3, [R4, #8] via the generic-base helper. */
  int ok = tcc_gen_machine_try_strd_base(R2, R3, R4, 8);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(ind, th_strd_imm(R2, R3, R4, 8, 6 /* add */)));

  return 0;
}

UT_TEST(test_dispatch_try_ldrd_base_aligned_emits_ldrd)
{
  setup_gen();

  /* LDRD R2, R3, [R4, #8] via the generic-base helper. */
  int ok = tcc_gen_machine_try_ldrd_base(R2, R3, R4, 8);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(ind, th_ldrd_imm(R2, R3, R4, 8, 6 /* add */)));

  return 0;
}

UT_TEST(test_dispatch_load_postinc_int32_emits_ldr)
{
  setup_gen();

  /* LDR R0, [R1], #4 */
  tcc_gen_machine_load_postinc_mop(mop_reg(R0, IROP_BTYPE_INT32), mop_reg(R1, IROP_BTYPE_INT32),
                                   mop_imm(4, IROP_BTYPE_INT32), TCCIR_OP_LOAD_POSTINC);

  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R0, R1, 4, 3 /* post-index add writeback */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_store_postinc_int32_emits_str)
{
  setup_gen();

  /* STR R2, [R1], #4 */
  tcc_gen_machine_store_postinc_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                    mop_imm(4, IROP_BTYPE_INT32), TCCIR_OP_STORE_POSTINC);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R1, 4, 3 /* post-index add writeback */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_return_value_imm_emits_mov_r0)
{
  setup_gen();

  /* Return immediate 42: MOV R0, #42. */
  tcc_gen_machine_return_value_mop(mop_imm(42, IROP_BTYPE_INT32), TCCIR_OP_RETURNVALUE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_imm(R0, 42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_lea_param_stack_emits_add)
{
  setup_gen();
  tcc_state->need_frame_pointer = 0;
  offset_to_args = 0;

  /* LEA of caller argument slot at SP+8. */
  tcc_gen_machine_lea_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_param_stack(8, IROP_BTYPE_INT32));

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_add_imm(R2, R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_dispatch_func_parameter_void_creates_empty_site)
{
  setup_gen();

  MachineOperand src1 = mop_none();
  MachineOperand src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(999, 0), IROP_BTYPE_INT32);

  tcc_gen_machine_func_parameter_mop(src1, src2, TCCIR_OP_FUNCPARAMVOID);

  /* No code emitted; call site should exist with zero argument count. */
  UT_ASSERT_EQ(ind, 0);
  ThumbGenCallSite *cs = thumb_get_call_site_for_id(999);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->call_id, 999);
  UT_ASSERT_EQ(cs->function_argument_count, 0);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_dispatch_smoke)
{
  UT_RUN(test_dispatch_add_reg_reg_reg_emits_real_bytes);
  UT_RUN(test_dispatch_load_reg_offset_zero_emits_real_bytes);
  UT_RUN(test_dispatch_store_reg_offset_zero_emits_real_bytes);
  UT_RUN(test_dispatch_jump_forward_uses_32bit_encoding);

  UT_RUN(test_dispatch_indirect_jump_reg_emits_bx);
  UT_RUN(test_dispatch_trap_mop_emits_udf);
  UT_RUN(test_dispatch_prefetch_reg_emits_pld);
  UT_RUN(test_dispatch_vla_sp_save_reg_emits_mov_sp);
  UT_RUN(test_dispatch_vla_sp_restore_reg_emits_mov_sp);
  UT_RUN(test_dispatch_select_imm_imm_emits_ite_movs);
  UT_RUN(test_dispatch_select_identity_then_uses_inverse_cond);
  UT_RUN(test_dispatch_backpatch_jump_to_next_insn_becomes_nop);
  UT_RUN(test_dispatch_store_spill_fp_emits_str);
  UT_RUN(test_dispatch_try_strd_spill_aligned_emits_strd);
  UT_RUN(test_dispatch_try_ldrd_spill_aligned_emits_ldrd);
  UT_RUN(test_dispatch_try_strd_base_aligned_emits_strd);
  UT_RUN(test_dispatch_try_ldrd_base_aligned_emits_ldrd);
  UT_RUN(test_dispatch_load_postinc_int32_emits_ldr);
  UT_RUN(test_dispatch_store_postinc_int32_emits_str);
  UT_RUN(test_dispatch_return_value_imm_emits_mov_r0);
  UT_RUN(test_dispatch_lea_param_stack_emits_add);
  UT_RUN(test_dispatch_func_parameter_void_creates_empty_site);
}
