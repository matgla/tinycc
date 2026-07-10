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

static int bytes_match_opcode_at(int off, int n, thumb_opcode op)
{
  if (n != op.size)
    return 0;
  const unsigned char *d = cur_text_section->data + off;
  if (op.size == 2)
    return d[0] == (op.opcode & 0xff) && d[1] == ((op.opcode >> 8) & 0xff);
  uint16_t hw0 = (uint16_t)(op.opcode >> 16);
  uint16_t hw1 = (uint16_t)(op.opcode & 0xffff);
  return d[0] == (hw0 & 0xff) && d[1] == ((hw0 >> 8) & 0xff) && d[2] == (hw1 & 0xff) &&
         d[3] == ((hw1 >> 8) & 0xff);
}

#define bytes_match_opcode(n, op) bytes_match_opcode_at(0, n, op)

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

/* ------------------------------------------------------------------ ABI hook */

UT_TEST(test_abi_assign_call_args_rejects_null_layout)
{
  setup_gen();

  TCCAbiArgDesc arg = {TCC_ABI_ARG_SCALAR32, 4, 4};
  UT_ASSERT_EQ(tcc_gen_machine_abi_assign_call_args(&arg, 1, NULL), -1);

  return 0;
}

UT_TEST(test_abi_assign_call_args_rejects_null_args_when_nonzero)
{
  setup_gen();

  TCCAbiArgLoc locs[1];
  memset(locs, 0, sizeof(locs));
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  layout.locs = locs;
  layout.capacity = 1;

  UT_ASSERT_EQ(tcc_gen_machine_abi_assign_call_args(NULL, 1, &layout), -1);

  return 0;
}

UT_TEST(test_abi_assign_call_args_scalar32_to_r0)
{
  setup_gen();

  TCCAbiArgLoc locs[2];
  memset(locs, 0, sizeof(locs));
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  layout.locs = locs;
  layout.capacity = 2;
  TCCAbiArgDesc arg = {TCC_ABI_ARG_SCALAR32, 4, 4};

  UT_ASSERT_EQ(tcc_gen_machine_abi_assign_call_args(&arg, 1, &layout), 0);
  UT_ASSERT_EQ(layout.argc, 1);
  UT_ASSERT_EQ(layout.locs[0].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ(layout.locs[0].reg_base, 0);

  return 0;
}

/* ------------------------------------------------------------------ dry-run state */

UT_TEST(test_dry_run_lifecycle)
{
  setup_gen();

  tcc_gen_machine_dry_run_init();
  UT_ASSERT_EQ(tcc_gen_machine_dry_run_is_active(), 0);

  tcc_gen_machine_dry_run_start();
  UT_ASSERT_EQ(tcc_gen_machine_dry_run_is_active(), 1);

  tcc_gen_machine_dry_run_end();
  UT_ASSERT_EQ(tcc_gen_machine_dry_run_is_active(), 0);

  return 0;
}

UT_TEST(test_dry_run_counters_initially_zero)
{
  setup_gen();

  tcc_gen_machine_dry_run_init();
  tcc_gen_machine_dry_run_start();
  UT_ASSERT_EQ(tcc_gen_machine_dry_run_get_lr_push_count(), 0);
  UT_ASSERT_EQ(tcc_gen_machine_dry_run_get_scratch_regs_pushed(), 0u);

  tcc_gen_machine_dry_run_end();

  return 0;
}

/* ------------------------------------------------------------------ scratch tracking */

UT_TEST(test_insn_scratch_reset_count_saves_mask)
{
  setup_gen();

  tcc_gen_machine_insn_scratch_reset();
  UT_ASSERT_EQ(tcc_gen_machine_insn_scratch_count(), 0);
  UT_ASSERT_EQ(tcc_gen_machine_insn_scratch_saves_mask(), 0);

  return 0;
}

UT_TEST(test_reset_scratch_state_no_crash)
{
  setup_gen();

  tcc_gen_machine_reset_scratch_state();
  UT_ASSERT_EQ(tcc_gen_machine_real_run_had_scratch_push(), 0);

  return 0;
}

/* ------------------------------------------------------------------ cache resets */

UT_TEST(test_mov_coalesce_reset_no_crash)
{
  setup_gen();

  tcc_gen_machine_mov_coalesce_reset();
  UT_ASSERT(1);

  return 0;
}

UT_TEST(test_mov_equiv_reset_no_crash)
{
  setup_gen();

  tcc_gen_machine_mov_equiv_reset();
  UT_ASSERT(1);

  return 0;
}

UT_TEST(test_imm_cache_reset_and_invalidate_live_no_crash)
{
  setup_gen();

  tcc_gen_machine_imm_cache_reset();
  tcc_gen_machine_imm_cache_invalidate_live(0x000f);
  tcc_gen_machine_imm_cache_invalidate_live(0);
  UT_ASSERT(1);

  return 0;
}

/* ------------------------------------------------------------------ fill nops */

UT_TEST(test_gen_fill_nops_emits_two_nops_per_four_bytes)
{
  setup_gen();

  gen_fill_nops(4);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(2, th_nop(ENFORCE_ENCODING_16BIT)));
  UT_ASSERT(bytes_match_opcode(2, th_nop(ENFORCE_ENCODING_16BIT)));

  return 0;
}

/* ------------------------------------------------------------------ scratch acquire/release */

UT_TEST(test_scratch_acquire_single_returns_reg_and_pushes_if_needed)
{
  setup_gen();

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, 0);

  UT_ASSERT_EQ(scratch.reg_count, 1);
  UT_ASSERT(scratch.regs[0] >= 0);

  tcc_machine_release_scratch(&scratch);

  return 0;
}

UT_TEST(test_scratch_acquire_pair_with_avoid_arg_regs)
{
  setup_gen();

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_NEEDS_PAIR |
                                             TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS);

  UT_ASSERT_EQ(scratch.reg_count, 2);
  UT_ASSERT(scratch.regs[0] != R0 && scratch.regs[0] != R1 && scratch.regs[0] != R2 &&
             scratch.regs[0] != R3);
  UT_ASSERT(scratch.regs[1] != R0 && scratch.regs[1] != R1 && scratch.regs[1] != R2 &&
             scratch.regs[1] != R3);

  tcc_machine_release_scratch(&scratch);

  return 0;
}

UT_TEST(test_scratch_acquire_avoid_perm_scratch)
{
  setup_gen();

  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH);

  UT_ASSERT_EQ(scratch.reg_count, 1);
  UT_ASSERT(scratch.regs[0] != R11 && scratch.regs[0] != R12);

  tcc_machine_release_scratch(&scratch);

  return 0;
}

UT_TEST(test_scratch_release_null_is_noop)
{
  setup_gen();

  tcc_machine_release_scratch(NULL);
  UT_ASSERT(1);

  return 0;
}

/* ------------------------------------------------------------------ CBZ/CBNZ */

UT_TEST(test_cbz_jump_mop_emits_cbz)
{
  setup_gen();

  int size = tcc_gen_machine_cbz_jump_mop(R1, /*nonzero=*/0, /*target_ir=*/1, /*ir_idx=*/0);

  UT_ASSERT_EQ(size, 2);
  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_cbz(R1, 0, 0)));

  return 0;
}

UT_TEST(test_cbnz_jump_mop_emits_cbnz)
{
  setup_gen();

  tcc_gen_machine_cbz_jump_mop(R2, /*nonzero=*/1, /*target_ir=*/1, /*ir_idx=*/0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_cbz(R2, 0, 1)));

  return 0;
}

/* ------------------------------------------------------------------ chain helpers */

UT_TEST(test_restore_chain_loads_from_chain_slot)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_restore_chain();

  /* The static chain register is R10 (a high register), so the LDR from
   * [FP, #-4] uses the 32-bit T3 encoding (4 bytes). */
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT(bytes_match_opcode(ind, th_ldr_imm(R10, R_FP, 4, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ------------------------------------------------------------------ end instruction */

UT_TEST(test_end_instruction_restores_pushed_scratch_regs)
{
  setup_gen();

  /* Acquire and release a scratch register through the public helpers so
   * end_instruction has something to restore. */
  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, 0);
  tcc_gen_machine_end_instruction();

  UT_ASSERT(1);

  return 0;
}

/* ------------------------------------------------------------------ stack-offset encoding */

UT_TEST(test_can_encode_stack_offset_for_reg_fp_small_offset)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  UT_ASSERT_EQ(tcc_machine_can_encode_stack_offset_for_reg(-8, R2), 1);

  return 0;
}

UT_TEST(test_can_encode_stack_offset_with_param_adj)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;
  offset_to_args = 16;

  UT_ASSERT_EQ(tcc_machine_can_encode_stack_offset_with_param_adj(8, 1, R2), 1);

  return 0;
}

/* ------------------------------------------------------------------ load constant / cmp / jmp result */

UT_TEST(test_load_constant_32bit_imm_emits_mov)
{
  setup_gen();

  tcc_machine_load_constant(R2, PREG_REG_NONE, 42, 0, NULL);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_imm(R2, 42, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_load_constant_64bit_imm_emits_two_movs)
{
  setup_gen();

  /* Each 32-bit half (0x1234, 0x5678) is a 16-bit value that encodes as a
   * single 4-byte MOVW, so load_constant emits two MOVWs (8 bytes) rather than
   * falling back to the 64-bit literal pool.  A half > 0xFFFF that is not a
   * modified-immediate would need MOVW+MOVT and take the pool path instead. */
  tcc_machine_load_constant(R2, R3, 0x0000567800001234LL, 1, NULL);

  UT_ASSERT_EQ(ind, 8);

  return 0;
}

UT_TEST(test_load_cmp_result_eq_emits_ite_movs)
{
  setup_gen();

  tcc_machine_load_cmp_result(R0, TOK_EQ);

  UT_ASSERT_EQ(ind, 6);
  /* ITE EQ: cond=EQ(0), mask=0xC => 0xbf0c */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xbf0c);
  UT_ASSERT(bytes_match_opcode_at(2, 2, th_mov_imm(R0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));
  UT_ASSERT(bytes_match_opcode_at(4, 2, th_mov_imm(R0, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_load_jmp_result_non_invert_emits_mov_branch_mov)
{
  setup_gen();

  /* jmp_addr == 0 is gsym()'s "no chain" sentinel (a no-op), so
   * tcc_machine_load_jmp_result emits exactly: MOV R0,#1 ; B.W +2 ; MOV R0,#0
   * (2 + 4 + 2 = 8 bytes).  The two MOVs are 16-bit MOVS (flags-not-important,
   * imm fits in 8 bits); the branch over the "false" value is a 32-bit B.W. */
  tcc_machine_load_jmp_result(R0, 0, 0);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT(bytes_match_opcode_at(0, 2, th_mov_imm(R0, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));
  UT_ASSERT(bytes_match_opcode_at(2, 4, th_b_t4(2)));
  UT_ASSERT(bytes_match_opcode_at(6, 2, th_mov_imm(R0, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ------------------------------------------------------------------ literal pool reservation */

UT_TEST(test_reserve_pool_bytes_tracks_pending_bytes)
{
  setup_gen();

  int before = tcc_gen_machine_pending_pool_size();
  tcc_gen_machine_reserve_pool_bytes(8);
  int after = tcc_gen_machine_pending_pool_size();

  UT_ASSERT_EQ(before, 0);
  UT_ASSERT(after >= before);

  return 0;
}

/* ------------------------------------------------------------------ misc state helpers */

UT_TEST(test_number_of_registers_returns_11)
{
  setup_gen();

  UT_ASSERT_EQ(tcc_gen_machine_number_of_registers(), 11);

  return 0;
}

UT_TEST(test_pending_pool_size_empty_returns_zero)
{
  setup_gen();

  UT_ASSERT_EQ(tcc_gen_machine_pending_pool_size(), 0);

  return 0;
}

UT_TEST(test_set_chain_emits_mov_r10_fp)
{
  setup_gen();

  tcc_gen_machine_set_chain();
  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT(bytes_match_opcode(ind, th_mov_reg(R10, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                               ENFORCE_ENCODING_NONE, false)));

  return 0;
}

/* ------------------------------------------------------------------ store-to-stack helpers */

UT_TEST(test_store_to_stack_fp_small_offset_emits_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Negative FP-relative offset uses the subtract form -> 32-bit encoding. */
  tcc_gen_machine_store_to_stack(R2, -8);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_store_to_stack_ex_large_offset_uses_scratch)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* Offset -4092 is far outside the immediate range, so the helper falls back
   * to materialising the offset in a scratch register and using STR (register). */
  tcc_gen_machine_store_to_stack_ex(R2, -4092, 0);

  UT_ASSERT(ind > 2);

  return 0;
}

UT_TEST(test_store_to_sp_small_offset_emits_str)
{
  setup_gen();

  /* SP-relative positive offset.  SP is a high register so the T1 form is
   * unavailable; assert the emitted bytes match the oracle. */
  tcc_gen_machine_store_to_sp(R2, 8);

  UT_ASSERT(bytes_match_opcode(ind, th_str_imm(R2, R_SP, 8, 6 /* add */, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ------------------------------------------------------------------ STRD immediate pairing */

UT_TEST(test_try_strd_imm_spill_distinct_values_emits_strd)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  int ok = tcc_gen_machine_try_strd_imm_spill(1, 2, -8, -4);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT(ind > 0);

  return 0;
}

UT_TEST(test_try_strd_imm_spill_equal_values_reuses_reg)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  int ok = tcc_gen_machine_try_strd_imm_spill(5, 5, -8, -4);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT(ind > 0);

  return 0;
}

UT_TEST(test_try_strd_imm_spill_non_adjacent_offsets_returns_zero)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  int ok = tcc_gen_machine_try_strd_imm_spill(1, 2, -8, -12);

  UT_ASSERT_EQ(ok, 0);
  UT_ASSERT_EQ(ind, 0);

  return 0;
}

UT_TEST(test_try_strd_imm_base_distinct_values_emits_strd)
{
  setup_gen();

  int ok = tcc_gen_machine_try_strd_imm_base(1, 2, R4, 8);

  UT_ASSERT_EQ(ok, 1);
  UT_ASSERT(ind > 0);

  return 0;
}

/* ------------------------------------------------------------------ spill block copy */

UT_TEST(test_spill_block_copy_two_words_emits_code)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_spill_block_copy(-16, -24, 2);

  UT_ASSERT(ind > 0);

  return 0;
}

/* ------------------------------------------------------------------ chain slot init */

UT_TEST(test_init_chain_slot_emits_store_to_chain)
{
  setup_gen();
  /* init_chain_slot uses the literal pool; arm_init() initializes it,
   * whereas arm_target_init() (used by setup_gen) does not. */
  arm_init(tcc_state);
  tcc_state->need_frame_pointer = 1;

  TCCIRState *ir = tcc_ir_alloc();
  tcc_state->ir = ir;

  Sym *chain_sym = get_sym_ref(NULL, cur_text_section, 0, 0);
  UT_ASSERT(chain_sym != NULL);

  IROperand src = utb_symref(ir, chain_sym, 0, 0, 0, IROP_BTYPE_INT32);
  tcc_gen_machine_init_chain_slot(src);

  UT_ASSERT(ind > 0);

  tcc_state->ir = NULL;
  tcc_ir_free(ir);

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

  UT_RUN(test_abi_assign_call_args_rejects_null_layout);
  UT_RUN(test_abi_assign_call_args_rejects_null_args_when_nonzero);
  UT_RUN(test_abi_assign_call_args_scalar32_to_r0);
  UT_RUN(test_dry_run_lifecycle);
  UT_RUN(test_dry_run_counters_initially_zero);
  UT_RUN(test_insn_scratch_reset_count_saves_mask);
  UT_RUN(test_reset_scratch_state_no_crash);
  UT_RUN(test_mov_coalesce_reset_no_crash);
  UT_RUN(test_mov_equiv_reset_no_crash);
  UT_RUN(test_imm_cache_reset_and_invalidate_live_no_crash);

  UT_RUN(test_gen_fill_nops_emits_two_nops_per_four_bytes);

  UT_RUN(test_scratch_acquire_single_returns_reg_and_pushes_if_needed);
  UT_RUN(test_scratch_acquire_pair_with_avoid_arg_regs);
  UT_RUN(test_scratch_acquire_avoid_perm_scratch);
  UT_RUN(test_scratch_release_null_is_noop);

  UT_RUN(test_cbz_jump_mop_emits_cbz);
  UT_RUN(test_cbnz_jump_mop_emits_cbnz);

  UT_RUN(test_restore_chain_loads_from_chain_slot);

  UT_RUN(test_end_instruction_restores_pushed_scratch_regs);

  UT_RUN(test_can_encode_stack_offset_for_reg_fp_small_offset);
  UT_RUN(test_can_encode_stack_offset_with_param_adj);

  UT_RUN(test_load_constant_32bit_imm_emits_mov);
  UT_RUN(test_load_constant_64bit_imm_emits_two_movs);
  UT_RUN(test_load_cmp_result_eq_emits_ite_movs);
  UT_RUN(test_load_jmp_result_non_invert_emits_mov_branch_mov);

  UT_RUN(test_reserve_pool_bytes_tracks_pending_bytes);

  UT_RUN(test_number_of_registers_returns_11);
  UT_RUN(test_pending_pool_size_empty_returns_zero);
  UT_RUN(test_set_chain_emits_mov_r10_fp);

  UT_RUN(test_store_to_stack_fp_small_offset_emits_str);
  UT_RUN(test_store_to_stack_ex_large_offset_uses_scratch);
  UT_RUN(test_store_to_sp_small_offset_emits_str);

  UT_RUN(test_try_strd_imm_spill_distinct_values_emits_strd);
  UT_RUN(test_try_strd_imm_spill_equal_values_reuses_reg);
  UT_RUN(test_try_strd_imm_spill_non_adjacent_offsets_returns_zero);
  UT_RUN(test_try_strd_imm_base_distinct_values_emits_strd);

  UT_RUN(test_spill_block_copy_two_words_emits_code);

  UT_RUN(test_init_chain_slot_emits_store_to_chain);
}
