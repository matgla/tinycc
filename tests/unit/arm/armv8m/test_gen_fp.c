/*
 *  test_gen_fp.c - backend/ binary suite for tcc_gen_machine_fp_mop()
 *  (arm-thumb-gen.c), the MachineOperand-based entry point for floating-point
 *  IR ops (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_ITOF/CVT_FTOI/CVT_FTOF).
 *
 *  Mirrors test_gen_dispatch_smoke.c: calls tcc_gen_machine_fp_mop() DIRECTLY
 *  (bypassing ir/codegen.c's dispatch loop) with hand-built MachineOperand
 *  arguments, and asserts on the real Thumb-2 bytes emitted into a real
 *  Section via the real o()/section_add machinery.
 *
 *  IMPORTANT, established empirically (see docs/plan_vfp_hard_float.md,
 *  "What is missing is the codegen path"): tcc_gen_machine_fp_mop()
 *  UNCONDITIONALLY lowers every FP op to a soft-float `__aeabi_*` library
 *  call via R0-R3, regardless of tcc_state->float_abi. There is no VFP
 *  (VADD.F32/VSUB.F32/...) instruction-emission branch yet -- th_vadd_f() &
 *  friends (arch/arm/thumb/thop_vfp.c, covered by test_thop_vfp.c) are not
 *  called anywhere from arm-thumb-gen.c. Setting float_abi = ARM_HARD_FLOAT
 *  only affects other layers (register-allocator hints in ir/vreg.c,
 *  AAPCS/ELF flags); it does NOT change tcc_gen_machine_fp_mop()'s own
 *  behavior. So every test below uses plain GPR (R0-R3-range) MachineOperand
 *  registers -- the only operand shape this function's real, implemented
 *  code path actually handles -- and asserts on the soft-float call
 *  sequence (arg-setup MOVs, a placeholder BL, result-writeback MOVs) it
 *  really emits. See bugs_found in this suite's handoff notes for what
 *  happens if a VFP-numbered register (as the allocator would assign under
 *  ARM_HARD_FLOAT) is passed in instead.
 *
 *  Every BL is to a NULL Sym: stubs.c's external_global_sym() (one of the
 *  four link stubs backend/ provides) always returns NULL, so
 *  gcall_or_jump_mop() never has a reloc_sym and always falls back to
 *  th_encbranch(ind, ind+0) -- i.e. a "branch to self" placeholder that
 *  encodes as the fixed halfword pair 0xf7ff 0xfffe (opcode 0xf7fffffe)
 *  regardless of which __aeabi_* function was requested. This is confirmed
 *  by elfsec_reloc_call_count() == 0 after every call in this file: no
 *  greloc() ever fires, so the BL bytes cannot distinguish FADD from FMUL
 *  from CVT_ITOF, etc. -- what IS distinguishable, and what these tests
 *  assert on, is the argument-loading/result-writeback MOV sequence around
 *  the BL, which differs per opcode/operand shape and does go through the
 *  real encoder (ot_check_mov_reg -> th_mov_reg, a Thumb-1 hi-register MOV).
 */

#define USING_GLOBALS
#include "ir.h"
#include "arch/arm/arm.h"
#include "arch/arm/thumb/thumb.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

/* Uses arm_init() (not the lighter arm_target_init()) because TCCIR_OP_FNEG
 * needs load_full_const() -> th_literal_pool_find_or_allocate() ->
 * literal_pool_hash, which is only initialized by th_literal_pool_init(), a
 * `static` helper reachable *only* from arm_init(TCCState*). Confirmed
 * empirically: with plain arm_target_init(), the FNEG mop SIGSEGVs inside
 * tcc_chained_hash_bucket_head() on literal_pool_hash.buckets==NULL (same
 * root cause test_gen_switch.c's setup_gen() documents and works around). */
static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  tcc_state->march_str = "armv8-m.main";
  tcc_state->fpu_type = 0;
  tcc_state->float_abi = ARM_HARD_FLOAT;
  tcc_state->text_and_data_separation = 0;
  tcc_state->pic = 0;
  arm_init(tcc_state);
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

/* 64-bit (double / long long) register-pair operand: r0 = lo, r1 = hi. */
static MachineOperand mop_reg64(int r0, int r1, int btype)
{
  MachineOperand m = mop_reg(r0, btype);
  m.is_64bit = true;
  m.u.reg.r1 = r1;
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

/* All BL placeholders in this suite encode identically -- see file header. */
#define BL_PLACEHOLDER_HI 0xf7ff
#define BL_PLACEHOLDER_LO 0xfffe

/* ------------------------------------------------------------------ FADD/FSUB/FMUL/FDIV (f32) */

UT_TEST(test_fadd_f32_loads_args_into_r0_r1_dest_already_r0)
{
  setup_gen();

  /* FADD dest=R0, src1=R2, src2=R3 (all float32): src1/src2 need moving into
   * R0/R1 for the soft-float call; dest is already R0 so no writeback MOV. */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand src2 = mop_reg(R3, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, src2, dest, TCCIR_OP_FADD, 0);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4619); /* MOV R1, R3 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(elfsec_reloc_call_count(), 0); /* external_global_sym() stub -> NULL sym, no greloc */

  return 0;
}

UT_TEST(test_fsub_f32_args_already_in_place_dest_needs_writeback)
{
  setup_gen();

  /* FSUB src1=R0, src2=R1 (already in the argument registers -- no setup
   * MOVs at all), dest=R2 (forces exactly one writeback MOV after the BL). */
  MachineOperand src1 = mop_reg(R0, IROP_BTYPE_FLOAT32);
  MachineOperand src2 = mop_reg(R1, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R2, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, src2, dest, TCCIR_OP_FSUB, 0);

  UT_ASSERT_EQ(ind, 6);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x4602); /* MOV R2, R0 (writeback) */

  return 0;
}

UT_TEST(test_fmul_f32_full_sequence_with_writeback)
{
  setup_gen();

  /* FMUL src1=R5, src2=R6, dest=R4: exercises both setup MOVs AND the
   * writeback MOV in a single call. */
  MachineOperand src1 = mop_reg(R5, IROP_BTYPE_FLOAT32);
  MachineOperand src2 = mop_reg(R6, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R4, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, src2, dest, TCCIR_OP_FMUL, 0);

  UT_ASSERT_EQ(ind, 10);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4628); /* MOV R0, R5 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4631); /* MOV R1, R6 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0x4604); /* MOV R4, R0 (writeback) */

  return 0;
}

UT_TEST(test_fdiv_f64_uses_two_register_pairs_and_pair_writeback)
{
  setup_gen();

  /* FDIV, double precision: src1={R4:R5}, src2={R6:R7}, dest={R8:R9}.
   * Binary double ops load lo/hi of both operands into R0:R1 / R2:R3, then
   * write the R0:R1 result pair back to dest's own register pair. */
  MachineOperand src1 = mop_reg64(R4, R5, IROP_BTYPE_FLOAT64);
  MachineOperand src2 = mop_reg64(R6, R7, IROP_BTYPE_FLOAT64);
  MachineOperand dest = mop_reg64(R8, R9, IROP_BTYPE_FLOAT64);

  tcc_gen_machine_fp_mop(src1, src2, dest, TCCIR_OP_FDIV, 0);

  UT_ASSERT_EQ(ind, 16);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4620);  /* MOV R0, R4 (src1 lo) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4629);  /* MOV R1, R5 (src1 hi) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x4632);  /* MOV R2, R6 (src2 lo) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0x463b);  /* MOV R3, R7 (src2 hi) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 10), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 12), 0x4680); /* MOV R8, R0 (dest lo writeback) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 14), 0x4689); /* MOV R9, R1 (dest hi writeback) */

  return 0;
}

/* ------------------------------------------------------------------ FNEG / FCMP */

UT_TEST(test_fneg_f32_xor_sign_bit_no_call)
{
  setup_gen();

  /* FNEG never calls a soft-float helper: it loads src into R0, flips the
   * sign bit via a scratch register + EOR, and writes back. src1=R2,
   * dest=R5 (forces both the initial load MOV and final writeback MOV). */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R5, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_FNEG, 0);

  UT_ASSERT_EQ(ind, 12);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610);  /* MOV R0, R2 (load src1) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0xb402);  /* PUSH {R1} (save scratch) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), 0x4900);  /* LDR R1, [PC, #0] (0x80000000 literal) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0x4048);  /* EORS R0, R1 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0xbc02);  /* POP {R1} (restore scratch) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 10), 0x4605); /* MOV R5, R0 (writeback) */

  /* No BL at all: no external call is made for FNEG. */
  UT_ASSERT_EQ(elfsec_reloc_call_count(), 0);

  return 0;
}

UT_TEST(test_fcmp_f32_never_writes_back_a_result_register)
{
  setup_gen();

  /* FCMP sets CPSR flags via the soft-float compare helper; dest is unused
   * (MACH_OP_NONE, as the real dispatch site would pass), and per
   * tcc_gen_machine_fp_mop()'s own comment ("if (op != TCCIR_OP_FCMP)
   * fp_mop_writeback_result(...)") there must be NO writeback MOV after the
   * BL, unlike every arithmetic op above. */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand src2 = mop_reg(R3, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, src2, mop_none(), TCCIR_OP_FCMP, 0);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4619); /* MOV R1, R3 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), BL_PLACEHOLDER_LO);
  /* ind stops right after the BL -- no trailing writeback MOV. */

  return 0;
}

/* ------------------------------------------------------------------ CVT_ITOF / CVT_FTOI */

UT_TEST(test_cvt_itof_int32_to_float32)
{
  setup_gen();

  /* CVT_ITOF, 32-bit int -> 32-bit float: single-register unary load into
   * R0, BL __aeabi_i2f (or _ui2f -- indistinguishable in bytes, see file
   * header), dest already R0 so no writeback. */
  MachineOperand src1 = mop_reg(R1, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_ITOF, 0);

  UT_ASSERT_EQ(ind, 6);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4608); /* MOV R0, R1 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_LO);

  return 0;
}

UT_TEST(test_cvt_itof_int64_src_uses_double_arg_load)
{
  setup_gen();

  /* CVT_ITOF with a 64-bit (long long) source: the unary path's
   * `if (src1.is_64bit)` branch loads a register PAIR into R0:R1 instead of
   * a single register into R0. src1={R2:R3}, dest=R0 (float32, so
   * __aeabi_l2f -- unsigned vs signed only changes func_name, not bytes). */
  MachineOperand src1 = mop_reg64(R2, R3, IROP_BTYPE_INT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_ITOF, 0);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 (lo) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4619); /* MOV R1, R3 (hi) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), BL_PLACEHOLDER_LO);

  return 0;
}

UT_TEST(test_cvt_ftoi_float32_to_int32_writes_back)
{
  setup_gen();

  /* CVT_FTOI, 32-bit float -> 32-bit int: dest=R5 forces a writeback MOV
   * after the BL, same single-register shape as CVT_ITOF's inverse. */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R5, IROP_BTYPE_INT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_FTOI, 0);

  UT_ASSERT_EQ(ind, 8);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0x4605); /* MOV R5, R0 (writeback) */

  return 0;
}

UT_TEST(test_cvt_ftoi_float32_to_int64_pair_writeback)
{
  setup_gen();

  /* CVT_FTOI with a 64-bit (long long) dest: `fp_mop_writeback_result(dest,
   * dest.is_64bit)` writes BOTH R0 (lo) and R1 (hi) back to dest's pair,
   * mirroring the CVT_ITOF 64-bit-source case in reverse. dest={R4:R5}. */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg64(R4, R5, IROP_BTYPE_INT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_FTOI, 0);

  UT_ASSERT_EQ(ind, 10);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), BL_PLACEHOLDER_HI);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 4), BL_PLACEHOLDER_LO);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 6), 0x4604); /* MOV R4, R0 (lo writeback) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 8), 0x460d); /* MOV R5, R1 (hi writeback) */

  return 0;
}

/* ------------------------------------------------------------------ CVT_FTOF identity */

UT_TEST(test_cvt_ftof_f32_to_f32_is_a_direct_copy_no_call)
{
  setup_gen();

  /* CVT_FTOF with matching widths (both single-precision here) is an
   * identity conversion: tcc_gen_machine_fp_mop() special-cases it to a
   * direct tcc_gen_machine_assign_mop() copy, bypassing R0/BL entirely. */
  MachineOperand src1 = mop_reg(R2, IROP_BTYPE_FLOAT32);
  MachineOperand dest = mop_reg(R0, IROP_BTYPE_FLOAT32);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_FTOF, 0);

  UT_ASSERT_EQ(ind, 2);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4610); /* MOV R0, R2 */
  UT_ASSERT_EQ(elfsec_reloc_call_count(), 0);

  return 0;
}

UT_TEST(test_cvt_ftof_f64_to_f64_is_a_direct_pair_copy_no_call)
{
  setup_gen();

  /* Same identity short-circuit, but for the double-precision (register
   * pair) case: src1={R4:R5}, dest={R0:R1} -> two plain MOVs, still no BL. */
  MachineOperand src1 = mop_reg64(R4, R5, IROP_BTYPE_FLOAT64);
  MachineOperand dest = mop_reg64(R0, R1, IROP_BTYPE_FLOAT64);

  tcc_gen_machine_fp_mop(src1, mop_none(), dest, TCCIR_OP_CVT_FTOF, 0);

  UT_ASSERT_EQ(ind, 4);
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 0), 0x4620); /* MOV R0, R4 (lo) */
  UT_ASSERT_EQ(read_le16(cur_text_section->data + 2), 0x4629); /* MOV R1, R5 (hi) */
  UT_ASSERT_EQ(elfsec_reloc_call_count(), 0);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_fp)
{
  UT_RUN(test_fadd_f32_loads_args_into_r0_r1_dest_already_r0);
  UT_RUN(test_fsub_f32_args_already_in_place_dest_needs_writeback);
  UT_RUN(test_fmul_f32_full_sequence_with_writeback);
  UT_RUN(test_fdiv_f64_uses_two_register_pairs_and_pair_writeback);
  UT_RUN(test_fneg_f32_xor_sign_bit_no_call);
  UT_RUN(test_fcmp_f32_never_writes_back_a_result_register);
  UT_RUN(test_cvt_itof_int32_to_float32);
  UT_RUN(test_cvt_itof_int64_src_uses_double_arg_load);
  UT_RUN(test_cvt_ftoi_float32_to_int32_writes_back);
  UT_RUN(test_cvt_ftoi_float32_to_int64_pair_writeback);
  UT_RUN(test_cvt_ftof_f32_to_f32_is_a_direct_copy_no_call);
  UT_RUN(test_cvt_ftof_f64_to_f64_is_a_direct_pair_copy_no_call);
}
