/*
 *  test_gen_call.c - backend/ (build_backend) unit tests for the call/param/
 *  return-value MOP entry points in arm-thumb-gen.c:
 *
 *    tcc_gen_machine_func_parameter_mop()
 *    tcc_gen_machine_return_value_mop()
 *    tcc_gen_machine_func_call_mop()
 *
 *  Follows the test_gen_dispatch_smoke.c pattern: the REAL arm-thumb-gen.c
 *  and arm-thumb-callsite.c are linked in (build_backend/run_unit_tests_backend),
 *  so the mop functions are called directly and the emitted Thumb-2 bytes are
 *  read back from the real Section via o()/section_add. No dispatch loop.
 *
 *  func_call_mop needs a real call site (arm-thumb-callsite.c
 *  thumb_get_or_create_call_site()/thumb_get_call_site_for_id()) and, for the
 *  >0-argument case, a real TCCIRState with FUNCPARAMVAL instructions so
 *  thumb_build_call_layout_from_ir() (arm-thumb-callsite.c) can scan
 *  ir->compact_instructions[] backward from call_idx for this call_id -- the
 *  exact sequence ir/codegen.c's real dispatch loop uses for
 *  TCCIR_OP_FUNCPARAMVAL/TCCIR_OP_FUNCCALLVAL (~3932-3938, ~4032-4044).
 */

#define USING_GLOBALS
#include "ir.h"
#include "cfg.h"
#include "ir/ssa.h"
#include "ir/vreg.h"
#include "ir/regalloc.h"
#include "ir/codegen.h"
#include "ir/machine_op.h"
#include "arch/arm/arm.h"
#include "arch/arm/arm_regalloc.h"
#include "arch/arm/thumb/thumb.h"
#include "arm-thumb-defs.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  arm_target_init("armv8-m.main", NULL, "cortex-m33", 0);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->registers_map_for_allocator = (1ull << 13) - 1;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->float_registers_for_allocator = 32;
  tcc_state->float_registers_map_for_allocator = (1ull << 32) - 1;
  tcc_state->optimize = 0;
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

static uint16_t read_le16(const unsigned char *p)
{
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p)
{
  return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* SValue builders, mirroring test_codegen_call.c (kept as an independent
 * static copy per the coordination-hazard note -- do not share via a header). */

static SValue sv_var(int vreg, int vt)
{
  SValue sv;
  svalue_init(&sv);
  sv.vr = vreg;
  sv.type.t = vt;
  return sv;
}

static SValue sv_const(int v)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = v;
  sv.type.t = VT_INT;
  return sv;
}

static SValue sv_param_marker(int call_id, int idx)
{
  SValue sv;
  svalue_init(&sv);
  sv.r = VT_CONST;
  sv.c.i = (int64_t)TCCIR_ENCODE_PARAM(call_id, idx);
  sv.type.t = VT_INT;
  return sv;
}

/* Builds the real IROperand a FUNCCALLVAL's src2 (call_id_op) carries,
 * without needing to insert an actual FUNCCALLVAL instruction into the IR
 * (func_call_mop is called directly, not via the dispatch loop, so no such
 * instruction needs to exist in ir->compact_instructions -- only the
 * FUNCPARAMVAL entries thumb_build_call_layout_from_ir() scans for do). */
static IROperand irop_call_id(int call_id, int argc)
{
  return irop_make_imm32(-1, (int32_t)TCCIR_ENCODE_CALL(call_id, argc), IROP_BTYPE_INT32);
}

/* ------------------------------------------------------------------ func_parameter_mop */

/* A plain FUNCPARAMVAL for a fresh call_id: creates the call site (via
 * thumb_get_or_create_call_site) and marks argument index 0 present. No
 * bytes are ever emitted by this mop -- it only tracks call-site metadata for
 * later use by func_call_mop / build_register_arg_moves. */
UT_TEST(test_func_parameter_mop_marks_argument_present)
{
  setup_gen();

  MachineOperand src1 = mop_reg(R0, IROP_BTYPE_INT32); /* unused by the mop */
  MachineOperand src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(100, 0), IROP_BTYPE_INT32);

  tcc_gen_machine_func_parameter_mop(src1, src2, TCCIR_OP_FUNCPARAMVAL);

  UT_ASSERT_EQ(ind, 0); /* no code emitted */

  ThumbGenCallSite *cs = thumb_get_call_site_for_id(100);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->call_id, 100);
  UT_ASSERT_EQ(cs->function_argument_count, 1);
  UT_ASSERT(cs->function_argument_list != NULL);
  UT_ASSERT_EQ(cs->function_argument_list[0], 1);

  return 0;
}

/* A second FUNCPARAMVAL at a higher index grows function_argument_list and
 * back-fills the skipped slot(s) with -1 (~13396-13399 loop). */
UT_TEST(test_func_parameter_mop_grows_argument_list_and_backfills_gap)
{
  setup_gen();

  MachineOperand src1 = mop_reg(R1, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(101, 2), IROP_BTYPE_INT32);

  tcc_gen_machine_func_parameter_mop(src1, src2, TCCIR_OP_FUNCPARAMVAL);

  ThumbGenCallSite *cs = thumb_get_call_site_for_id(101);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->function_argument_count, 3);
  UT_ASSERT_EQ(cs->function_argument_list[0], -1);
  UT_ASSERT_EQ(cs->function_argument_list[1], -1);
  UT_ASSERT_EQ(cs->function_argument_list[2], 1);

  return 0;
}

/* FUNCPARAMVOID: creates the call site (0-argument call marker) but does not
 * touch function_argument_list at all -- (~13379-13382). */
UT_TEST(test_func_parameter_mop_void_creates_site_without_argument_entry)
{
  setup_gen();

  MachineOperand src1 = mop_none();
  MachineOperand src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(102, 0), IROP_BTYPE_INT32);

  tcc_gen_machine_func_parameter_mop(src1, src2, TCCIR_OP_FUNCPARAMVOID);

  UT_ASSERT_EQ(ind, 0);
  ThumbGenCallSite *cs = thumb_get_call_site_for_id(102);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->call_id, 102);
  UT_ASSERT_EQ(cs->function_argument_count, 0);
  UT_ASSERT(cs->function_argument_list == NULL);

  return 0;
}

/* During dry-run, the argument list must not be mutated at all (~13384-13388
 * comment: avoids memory leaks when call sites are restored post-dry-run).
 * The call site itself is still created (thumb_get_or_create_call_site runs
 * unconditionally before the dry-run check). */
UT_TEST(test_func_parameter_mop_dry_run_skips_argument_list_mutation)
{
  setup_gen();

  tcc_gen_machine_dry_run_start();

  MachineOperand src1 = mop_reg(R0, IROP_BTYPE_INT32);
  MachineOperand src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(103, 0), IROP_BTYPE_INT32);
  tcc_gen_machine_func_parameter_mop(src1, src2, TCCIR_OP_FUNCPARAMVAL);

  tcc_gen_machine_dry_run_end();

  ThumbGenCallSite *cs = thumb_get_call_site_for_id(103);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->function_argument_count, 0);
  UT_ASSERT(cs->function_argument_list == NULL);

  return 0;
}

/* ------------------------------------------------------------------ return_value_mop */

/* Fast path: value already in R0 -> no bytes emitted at all (~9506-9508). */
UT_TEST(test_return_value_mop_already_in_r0_emits_nothing)
{
  setup_gen();

  MachineOperand src = mop_reg(R0, IROP_BTYPE_INT32);
  tcc_gen_machine_return_value_mop(src, TCCIR_OP_RETURNVALUE);

  UT_ASSERT_EQ(ind, 0);

  return 0;
}

/* Immediate: materialized directly into R0 via tcc_machine_load_constant.
 * For a small immediate (42) that's a single 16-bit MOVS R0,#42 (T1 encoding
 * 0x2000 | Rd<<8 | imm8 == 0x202A) -- confirmed against
 * test_thop_mov.c's MOVS immediate oracle shape (Rd in bits 10:8, imm8 in
 * bits 7:0, opcode base 0x2000). */
UT_TEST(test_return_value_mop_imm_loads_constant_into_r0)
{
  setup_gen();

  MachineOperand src = mop_imm(42, IROP_BTYPE_INT32);
  tcc_gen_machine_return_value_mop(src, TCCIR_OP_RETURNVALUE);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x202A);

  return 0;
}

/* General register case (value in R5, not R0): materializes via
 * mach_ensure_in_reg (a no-op register read for MACH_OP_REG) then emits a
 * MOV R0, R5 since src_reg (R5) != R0 (~9536-9539). */
UT_TEST(test_return_value_mop_register_emits_mov_to_r0)
{
  setup_gen();

  MachineOperand src = mop_reg(R5, IROP_BTYPE_INT32);
  tcc_gen_machine_return_value_mop(src, TCCIR_OP_RETURNVALUE);

  UT_ASSERT_EQ(ind, 2);
  /* MOV Rd, Rm (T1, hi-register form): 0100 0110 D Rm(4) Rd(3).
   * Rd=R0 (0,D=0), Rm=R5 (0101) -> 0x4600 | (5<<3) | 0 == 0x4628. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x4628);

  return 0;
}

/* 64-bit return: lo -> R0, hi -> R1, delegated to tcc_gen_machine_assign_mop
 * with a synthetic R0:R1 dest (~9493-9504). Source pair (R6,R7) both differ
 * from the dest pair, so two register moves are expected. */
UT_TEST(test_return_value_mop_64bit_moves_pair_to_r0_r1)
{
  setup_gen();

  MachineOperand src = mop_reg(R6, IROP_BTYPE_INT64);
  src.u.reg.r1 = R7;
  src.is_64bit = true;

  tcc_gen_machine_return_value_mop(src, TCCIR_OP_RETURNVALUE);

  UT_ASSERT(ind > 0);
  /* MOV R0, R6 ; MOV R1, R7 (T1 hi-register MOV, low-Rd/low-Rm form since all
   * of R0,R1,R6,R7 < R8): 0100 0110 D Rm(4) Rd(3), D = Rd>>3 (always 0 here).
   * MOV R0,R6 -> 0x4600 | (6<<3) | 0 == 0x4630.
   * MOV R1,R7 -> 0x4600 | (7<<3) | 1 == 0x4639. */
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x4630);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4639);

  return 0;
}

/* ------------------------------------------------------------------ func_call_mop */

/* Zero-argument indirect call (function pointer already resident in a
 * non-arg register, R4) with drop_value=1 (FUNCCALLVOID shape): the ONLY
 * thing emitted is the BLX instruction itself -- no arg setup, no nested-call
 * saves (call_site->registers_map is 0 for a fresh site), no return-value
 * writeback (handle_return_value_mop early-returns on drop_value). */
UT_TEST(test_func_call_mop_zero_arg_indirect_void_emits_only_blx)
{
  setup_gen();

  TCCIRState *ir = tcc_ir_alloc();
  ir->leaffunc = 0;
  ir->tail_call_only = 0;

  int call_id = 200;
  ThumbGenCallSite *cs = thumb_get_or_create_call_site(call_id);
  UT_ASSERT(cs != NULL);

  MachineOperand func_mop = mop_reg(R4, IROP_BTYPE_FUNC);
  IROperand call_id_op = irop_call_id(call_id, 0);
  MachineOperand dest_mop = mop_none();

  tcc_gen_machine_func_call_mop(func_mop, call_id_op, dest_mop, /*drop_value=*/1, ir, /*call_idx=*/0);

  /* BLX R4 (T16): 0100 0111 1 Rm(4) 000 == 0x4780 | (4<<3) == 0x47A0.
   * Confirmed against test_thop_branch.c's th_blx_reg oracle shape
   * (0x4780 | (Rm<<3)). */
  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x47A0);

  tcc_ir_free(ir);
  return 0;
}

/* Same zero-argument indirect call, but drop_value=0 and the destination is
 * a register other than R0 (R2): BLX R4, then the return-value writeback
 * (handle_return_value_mop -> mach_writeback_dest) copies R0 into R2. */
UT_TEST(test_func_call_mop_zero_arg_indirect_writes_back_return_value)
{
  setup_gen();

  TCCIRState *ir = tcc_ir_alloc();
  ir->leaffunc = 0;
  ir->tail_call_only = 0;

  int call_id = 201;
  ThumbGenCallSite *cs = thumb_get_or_create_call_site(call_id);
  UT_ASSERT(cs != NULL);

  MachineOperand func_mop = mop_reg(R4, IROP_BTYPE_FUNC);
  IROperand call_id_op = irop_call_id(call_id, 0);
  MachineOperand dest_mop = mop_reg(R2, IROP_BTYPE_INT32);

  tcc_gen_machine_func_call_mop(func_mop, call_id_op, dest_mop, /*drop_value=*/0, ir, /*call_idx=*/0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x47A0); /* BLX R4 */
  /* MOV R2, R0 (T1 hi-register form): 0x4600 | (0<<3) | 2 == 0x4602. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4602);

  tcc_ir_free(ir);
  return 0;
}

/* Direct (symbol) call, zero arguments, drop_value=1: gcall_or_jump_mop's
 * MACH_OP_SYMBOL branch emits a BL with a placeholder immediate and records
 * an R_ARM_THM_JUMP24 relocation against the symbol via greloc() (recorded
 * by elfsec_stubs.c, not a real ELF writer). */
UT_TEST(test_func_call_mop_zero_arg_direct_symbol_emits_bl_and_relocation)
{
  setup_gen();

  TCCIRState *ir = tcc_ir_alloc();
  ir->leaffunc = 0;
  ir->tail_call_only = 0;

  int call_id = 202;
  ThumbGenCallSite *cs = thumb_get_or_create_call_site(call_id);
  UT_ASSERT(cs != NULL);

  Sym *fn_sym = get_sym_ref(NULL, cur_text_section, 0, 0);
  UT_ASSERT(fn_sym != NULL);

  MachineOperand func_mop;
  memset(&func_mop, 0, sizeof(func_mop));
  func_mop.kind = MACH_OP_SYMBOL;
  func_mop.btype = IROP_BTYPE_FUNC;
  func_mop.u.sym.sym = fn_sym;
  func_mop.u.sym.addend = 0;

  IROperand call_id_op = irop_call_id(call_id, 0);
  MachineOperand dest_mop = mop_none();

  tcc_gen_machine_func_call_mop(func_mop, call_id_op, dest_mop, /*drop_value=*/1, ir, /*call_idx=*/0);

  /* BL T1 placeholder, confirmed empirically (temporary printf of the raw
   * bytes, per the self-verify methodology): gcall_or_jump_mop's MACH_OP_SYMBOL
   * branch, when a real relocation will be emitted, calls th_bl_t1() with
   * imm=(uint32_t)-4 as a placeholder (the real offset is unresolved until
   * link time) -- an all-ones offset field, which th_bl_t1 encodes as hi
   * halfword 0xF7FF, lo halfword 0xFFFE. */
  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0xF7FF);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xFFFE);

  UT_ASSERT_EQ(elfsec_reloc_call_count(), 1);
  const ElfSecRelocCall *rc = elfsec_nth_reloc_call(0);
  UT_ASSERT(rc != NULL);
  UT_ASSERT(rc->sym == fn_sym);
  UT_ASSERT_EQ(rc->type, R_ARM_THM_JUMP24);
  UT_ASSERT_EQ((int)rc->offset, 0); /* call_pos = ind(4) - 4 == 0 */

  tcc_ir_free(ir);
  return 0;
}

/* One real argument, driven through the same real-IR shape ir/codegen.c's
 * dispatch loop builds: ASSIGN #7 -> arg (a TEMP vreg), then a real
 * FUNCPARAMVAL instruction (via tcc_ir_put), registered with a real
 * tcc_gen_machine_func_parameter_mop() call (creating the call site exactly
 * as the dispatch loop's TCCIR_OP_FUNCPARAMVAL case does), then regalloc, then
 * func_call_mop directly with call_idx pointing just past the last real IR
 * instruction (mirroring "i" in the dispatch loop) so
 * thumb_build_call_layout_from_ir() finds the FUNCPARAMVAL by scanning
 * ir->compact_instructions[] backward from call_idx.
 *
 * The call target is R6 (outside R0-R3, so no pre-save-indirect-target move
 * is needed) and drop_value=1, isolating the arg-setup codegen: this must be
 * exactly one instruction (a MOV/identity into R0, the sole int arg's AAPCS
 * home), then the BLX. */
UT_TEST(test_func_call_mop_one_arg_via_real_ir_places_arg_then_calls)
{
  setup_gen();

  TCCIRState *ir = tcc_ir_alloc();

  ir->leaffunc = 0;
  ir->tail_call_only = 0;

  int call_id = 203;

  int arg = tcc_ir_vreg_alloc_temp(ir);
  SValue s_arg = sv_var(arg, VT_INT);
  SValue s_seven = sv_const(7);
  tcc_ir_put(ir, TCCIR_OP_ASSIGN, &s_seven, NULL, &s_arg);

  SValue s_param = sv_param_marker(call_id, 0);
  tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, &s_arg, &s_param, NULL);

  int call_idx = ir->next_instruction_index; /* one past the FUNCPARAMVAL, like dispatch loop's "i" would be at FUNCCALLVAL */

  tcc_ir_ssa_regalloc(ir, arm_get_regalloc_target(), 0);

  /* Real call-site registration sequence, mirroring ir/codegen.c's
   * TCCIR_OP_FUNCPARAMVAL dispatch case (~3932-3938): src1 is unused by the
   * mop, src2 is the packed call_id/param_idx immediate. */
  MachineOperand fp_src1 = mop_reg(R0, IROP_BTYPE_INT32);
  MachineOperand fp_src2 = mop_imm((int64_t)TCCIR_ENCODE_PARAM(call_id, 0), IROP_BTYPE_INT32);
  tcc_gen_machine_func_parameter_mop(fp_src1, fp_src2, TCCIR_OP_FUNCPARAMVAL);

  MachineOperand func_mop = mop_reg(R6, IROP_BTYPE_FUNC);
  IROperand call_id_op = irop_call_id(call_id, 1);
  MachineOperand dest_mop = mop_none();

  tcc_gen_machine_func_call_mop(func_mop, call_id_op, dest_mop, /*drop_value=*/1, ir, call_idx);

  /* Empirically (via a temporary printf of ind/bytes, per the self-verify
   * methodology): the sole -O0 linear-scan allocator run here places `arg`
   * directly in R0 (its natural AAPCS home for the only int arg), so
   * build_register_arg_moves finds an identity move and emits nothing --
   * the ONLY bytes emitted are the BLX R6 itself. This is allocator-outcome
   * dependent in general (a different placement would add a MOV), so the
   * assertion only pins down the shape actually observed rather than
   * asserting a specific allocator choice was mandatory. */
  UT_ASSERT_EQ(ind, 2);
  /* BLX R6 (T16): 0x4780 | (6<<3) == 0x4780 | 0x30 == 0x47B0. */
  UT_ASSERT_EQ(read_le16(cur_text_section->data), 0x47B0);

  tcc_ir_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_call)
{
  UT_RUN(test_func_parameter_mop_marks_argument_present);
  UT_RUN(test_func_parameter_mop_grows_argument_list_and_backfills_gap);
  UT_RUN(test_func_parameter_mop_void_creates_site_without_argument_entry);
  UT_RUN(test_func_parameter_mop_dry_run_skips_argument_list_mutation);
  UT_RUN(test_return_value_mop_already_in_r0_emits_nothing);
  UT_RUN(test_return_value_mop_imm_loads_constant_into_r0);
  UT_RUN(test_return_value_mop_register_emits_mov_to_r0);
  UT_RUN(test_return_value_mop_64bit_moves_pair_to_r0_r1);
  UT_RUN(test_func_call_mop_zero_arg_indirect_void_emits_only_blx);
  UT_RUN(test_func_call_mop_zero_arg_indirect_writes_back_return_value);
  UT_RUN(test_func_call_mop_zero_arg_direct_symbol_emits_bl_and_relocation);
  UT_RUN(test_func_call_mop_one_arg_via_real_ir_places_arg_then_calls);
}
