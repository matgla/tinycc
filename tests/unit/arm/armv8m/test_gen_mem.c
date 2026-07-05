/*
 *  test_gen_mem.c - suite for arm-thumb-gen.c's MachineOperand-based memory
 *  mop entry points: tcc_gen_machine_assign_mop, tcc_gen_machine_load_mop,
 *  tcc_gen_machine_store_mop, tcc_gen_machine_load_indexed_mop,
 *  tcc_gen_machine_store_indexed_mop, tcc_gen_machine_lea_mop,
 *  tcc_gen_machine_block_copy_mop.
 *
 *  Mirrors test_gen_dispatch_smoke.c's style: call the mop function directly
 *  (bypassing ir/codegen.c's dispatch loop) with hand-built MachineOperand
 *  arguments, and assert on the real Thumb-2 bytes it emits into a real
 *  Section via the real o()/section_add machinery.
 *
 *  test_gen_dispatch_smoke.c already covers plain load_mop/store_mop (REG
 *  base, needs_deref, offset 0). This file adds: assign_mop reg<->reg and
 *  reg<->spill (FP-relative, non-zero offset), load_mop/store_mop with a
 *  MACH_OP_SPILL src/dest at a non-zero FP-relative offset (the mechanism
 *  arm-thumb-gen.c actually uses to represent "REG at an immediate offset" --
 *  a bare MACH_OP_REG has no offset field at all, see load_mop/store_mop's
 *  MACH_OP_REG case which always emits offset 0), load_indexed_mop /
 *  store_indexed_mop with a REG base + REG index (register-offset LDR/STR),
 *  lea_mop with a MACH_OP_FRAME_ADDR src, and block_copy_mop for a small
 *  fixed-size copy.
 *
 *  Methodology: every expected-byte assertion below is produced by calling
 *  the SAME real, already-unit-tested low-level Thumb-2 encoders
 *  (th_mov_reg, th_str_imm, th_ldr_imm, th_str_reg, th_ldr_reg, th_sub_imm,
 *  th_push, th_pop, th_ldm, th_stm -- all exercised directly in
 *  test_thop_*.c) with the SAME operands the mop under test is given, then
 *  comparing byte-for-byte against what the mop actually emitted. Register
 *  choices and (for block_copy) computed offsets that depend on internal
 *  scratch-allocation/stack-bias bookkeeping were first discovered empirically
 *  (temporary stderr dump of the real output), then re-derived here via the
 *  real encoders -- never hand-computed from ISA-encoding knowledge alone.
 */

#define USING_GLOBALS
#include "ir.h"
#include "arch/arm/arm.h"
#include "arch/arm/thumb/thumb.h"
#include "arch/arm/thumb/thop_alu_imm.h"
#include "arch/arm/thumb/thop_mem_imm.h"
#include "arch/arm/thumb/thop_mem_reg.h"
#include "arch/arm/thumb/thop_mov.h"
#include "arch/arm/thumb/thop_block.h"
#include "ir/machine_op.h"
#include "codegen_backend_stubs.h"
#include "elfsec_stubs.h"
#include "ir_build.h"

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

static void setup_gen(void)
{
  elfsec_reset();
  cgb_reset();
  tcc_state->march_str = "armv8-m.main";
  tcc_state->float_abi = ARM_HARD_FLOAT;
  /* arm_init() (not just arm_target_init()) is required so the literal-pool
   * hash table (th_literal_pool_init(), a static helper only reachable via
   * arm_init()) is ready before any mop that materializes a Sym* through
   * tcc_machine_load_constant() (e.g. block_copy_mop's source-address load).
   * Without this, that path dereferences an uninitialized chained-hash table
   * and segfaults (confirmed by running this suite before adding the call). */
  arm_init(tcc_state);
  cur_text_section = elfsec_new_section(".text");
  ind = 0;
  tcc_state->registers_for_allocator = 13;
  tcc_state->need_frame_pointer = 0;
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

static MachineOperand mop_spill(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_SPILL;
  m.btype = btype;
  m.u.spill.offset = offset;
  return m;
}

static MachineOperand mop_spill_u(int32_t offset, int btype)
{
  MachineOperand m = mop_spill(offset, btype);
  m.is_unsigned = 1;
  return m;
}

static MachineOperand mop_spill_s(int32_t offset, int btype)
{
  MachineOperand m = mop_spill(offset, btype);
  m.is_unsigned = 0;
  return m;
}

static MachineOperand mop_frame_addr(int32_t offset, int btype)
{
  MachineOperand m;
  memset(&m, 0, sizeof(m));
  m.kind = MACH_OP_FRAME_ADDR;
  m.btype = btype;
  m.u.frame.offset = offset;
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

/* Compare the N bytes just emitted (cur_text_section->data[0..n)) against a
 * single real-encoder opcode (2 or 4 bytes, little-endian halfword order --
 * matching how o()/ot() lay bytes into the section, see test_gen_dispatch_smoke.c). */
static int bytes_match_opcode(int n, thumb_opcode op)
{
  if (n != op.size)
    return 0;
  const unsigned char *d = cur_text_section->data;
  if (op.size == 2)
    return d[0] == (op.opcode & 0xff) && d[1] == ((op.opcode >> 8) & 0xff);
  /* 4-byte T32: first halfword (bits 31:16) is emitted first, low byte first. */
  uint16_t hw0 = (uint16_t)(op.opcode >> 16);
  uint16_t hw1 = (uint16_t)(op.opcode & 0xffff);
  return d[0] == (hw0 & 0xff) && d[1] == ((hw0 >> 8) & 0xff) && d[2] == (hw1 & 0xff) && d[3] == ((hw1 >> 8) & 0xff);
}

/* ------------------------------------------------------------------ assign_mop */

UT_TEST(test_assign_reg_to_reg_distinct_regs_emits_mov)
{
  setup_gen();

  /* src = R2 (plain reg), dest = R0 (plain reg, distinct) -> MOV R0, R2.
   * assign_mop's REG->REG fast path calls mach_writeback_dest(), which for a
   * MACH_OP_REG dest emits ot_check_mov_reg(dest, src, flags_safe(), ...);
   * flags_safe() is NOT_IMPORTANT here since tcc_state->ir is NULL. */
  tcc_gen_machine_assign_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_reg(R0, IROP_BTYPE_INT32), TCCIR_OP_ASSIGN);

  thumb_opcode expect = th_mov_reg(R0, R2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

UT_TEST(test_assign_reg_to_spill_emits_store_with_offset)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* src = R3 (plain reg), dest = spill slot at FP-8 -> STR R3, [FP, #-8].
   * With need_frame_pointer=1 and callee_push_size=0 (never touched by this
   * mop), fp_adjust_local_offset() leaves -8 unchanged: sign=1, abs_off=8. */
  tcc_gen_machine_assign_mop(mop_reg(R3, IROP_BTYPE_INT32), mop_spill(-8, IROP_BTYPE_INT32), TCCIR_OP_ASSIGN);

  thumb_opcode expect = th_str_imm(R3, R_FP, 8, 4 /* P=1,U=0,W=0: subtract */, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

UT_TEST(test_assign_spill_to_reg_emits_load_with_offset)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* src = spill slot at FP-8, dest = R3 (plain reg) -> LDR R3, [FP, #-8]. */
  tcc_gen_machine_assign_mop(mop_spill(-8, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32), TCCIR_OP_ASSIGN);

  thumb_opcode expect = th_ldr_imm(R3, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

/* ------------------------------------------------------------------ load_mop / store_mop */

/* A bare MACH_OP_REG src/dest has no offset field at all -- load_mop's/
 * store_mop's MACH_OP_REG case always dereferences at offset 0 (see
 * arm-thumb-gen.c's `load_from_base(dest_reg, ..., 0, 0, (uint32_t)src.u.reg.r0)`,
 * already covered by test_gen_dispatch_smoke.c). The mechanism that actually
 * carries a non-zero immediate offset through these mop signatures is
 * MACH_OP_SPILL (FP-relative addressing, resolved via fp_adjust_local_offset()
 * + load_from_base/th_store*_imm_or_reg_ex), so that's what these two tests
 * exercise. */

UT_TEST(test_load_mop_spill_nonzero_offset_emits_immediate_ldr)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* src = spill slot at FP-12 (non-zero offset), dest = R2 -> LDR R2, [FP, #-12]. */
  tcc_gen_machine_load_mop(mop_spill(-12, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  thumb_opcode expect = th_ldr_imm(R2, R_FP, 12, 4 /* subtract */, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

UT_TEST(test_store_mop_spill_nonzero_offset_emits_immediate_str)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  /* src = R2, dest = spill slot at FP-12 (non-zero offset) -> STR R2, [FP, #-12]. */
  tcc_gen_machine_store_mop(mop_spill(-12, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_STORE);

  thumb_opcode expect = th_str_imm(R2, R_FP, 12, 4 /* subtract */, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

/* ------------------------------------------------------------------ indexed */

UT_TEST(test_load_indexed_reg_base_reg_index_emits_register_offset_ldr)
{
  setup_gen();

  /* dest = R3, base = R1 (REG), index = R2 (REG), scale = 0 (IMM) ->
   * LDR R3, [R1, R2]. A REG index (not IMM) plus REG base skips both
   * "fold into FP/SP-relative" and "constant-displacement" fast paths in
   * load_indexed_mop, landing in the generic th_ldr_reg() register-offset path. */
  tcc_gen_machine_load_indexed_mop(mop_reg(R3, IROP_BTYPE_INT32), mop_reg(R1, IROP_BTYPE_INT32),
                                    mop_reg(R2, IROP_BTYPE_INT32), mop_imm(0, IROP_BTYPE_INT32),
                                    TCCIR_OP_LOAD_INDEXED);

  thumb_opcode expect = th_ldr_reg(R3, R1, R2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

UT_TEST(test_store_indexed_reg_base_reg_index_emits_register_offset_str)
{
  setup_gen();

  /* base = R1 (REG), index = R2 (REG), scale = 0 (IMM), value = R3 ->
   * STR R3, [R1, R2]. */
  tcc_gen_machine_store_indexed_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R2, IROP_BTYPE_INT32),
                                     mop_imm(0, IROP_BTYPE_INT32), mop_reg(R3, IROP_BTYPE_INT32),
                                     TCCIR_OP_STORE_INDEXED);

  thumb_opcode expect = th_str_reg(R3, R1, R2, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

/* ------------------------------------------------------------------ lea */

UT_TEST(test_lea_frame_addr_emits_stack_address_computation)
{
  setup_gen();
  tcc_state->need_frame_pointer = 0;

  /* dest = R2, src = &local at FP-16 (MACH_OP_FRAME_ADDR). With
   * need_frame_pointer=0, fp_adjust_local_offset() folds FP-16 to an
   * SP-relative offset (allocated_stack_size=0, scratch_push_sp_bias()=0
   * here since no scratch has been pushed yet) -> still -16, so
   * tcc_machine_addr_of_stack_slot() emits SUB R2, SP, #16. */
  tcc_gen_machine_lea_mop(mop_reg(R2, IROP_BTYPE_INT32), mop_frame_addr(-16, IROP_BTYPE_INT32));

  thumb_opcode expect = th_sub_imm(R2, R_SP, 16, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT(bytes_match_opcode(ind, expect));

  return 0;
}

/* ------------------------------------------------------------------ block_copy */

UT_TEST(test_block_copy_small_fixed_size_emits_ldm_stm_pair)
{
  setup_gen();

  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  /* src must be a real SYMREF operand: block_copy_mop unconditionally derefs
   * symref->sym (irop_get_symref_ex + validate_sym_for_reloc), unlike the
   * ir/codegen.c *dispatch* path (test_codegen_mem.c's block_copy dispatch
   * test) which never touches the Sym* and can pass NULL. get_sym_ref()
   * (elfsec_stubs.c) hands out a zeroed, tcc_mallocz'd Sym -- v=0 (not
   * SYM_FIELD) and c=0 (not <0), so validate_sym_for_reloc() accepts it. */
  Sym *sym = get_sym_ref(NULL, NULL, 0, 0);
  IROperand src = utb_symref(ir, sym, 0, 1, 1, IROP_BTYPE_INT32);
  IROperand dest = utb_stackoff(-16, 0, 0, 0, IROP_BTYPE_INT32);

  /* size=8 (2 words): below TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES (64), so this
   * takes the inline LDM/STM path, not the memcpy call. */
  tcc_gen_machine_block_copy_mop(ir, dest, src, 8);

  /* tcc_state->ir is NULL (setup_gen), so get_scratch_reg_with_save() can't
   * consult liveness/allocator state and always falls back to "save a
   * register to the stack", preferring R0-R3 in order and PUSHing each.
   * Empirically confirmed (temporary stderr byte dump) allocation order:
   *   r_src=R0 (pushed), r_dst=R1 (pushed), then two data regs R2, R3 (each
   *   pushed) since size=8 needs exactly ndata=2 -- then one LDM/STM pair,
   *   then POPs in reverse order (data regs first, high-to-low, then dst,
   *   then src). Every opcode below is independently re-derived through the
   *   real encoders with those same registers, not hand-guessed. */
  int off = 0;
  const unsigned char *d = cur_text_section->data;

  thumb_opcode push_r0 = th_push(1u << R0);
  UT_ASSERT(off + push_r0.size <= ind);
  UT_ASSERT_EQ(d[off], push_r0.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (push_r0.opcode >> 8) & 0xff);
  off += push_r0.size;

  thumb_opcode push_r1 = th_push(1u << R1);
  UT_ASSERT_EQ(d[off], push_r1.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (push_r1.opcode >> 8) & 0xff);
  off += push_r1.size;

  /* LDR r0, [pc, #0]: literal-pool placeholder for the symbol address load
   * (tcc_machine_load_constant -> load_full_const -> literal pool), dumped
   * before th_literal_pool_generate() ever runs so the immediate field is
   * still the unpatched 0 -- cross-checked against test_thop_ldr_literal.c's
   * `ldr r0,[pc,#4] => 0x4801` (imm field = byte_offset/4), so `ldr r0,[pc,#0]
   * => 0x4800`. */
  UT_ASSERT_EQ(d[off], 0x00);
  UT_ASSERT_EQ(d[off + 1], 0x48);
  off += 2;

  /* SUB R1, SP, #8: address-of the dest stack slot (FP-16, folded to
   * SP-relative). NOT #16 -- by this point two 4-byte scratch PUSHes are
   * live, and fp_adjust_local_offset() adds scratch_push_sp_bias()==8, so
   * -16+8 = -8 (empirically confirmed, see suite banner comment). */
  thumb_opcode sub_r1 = th_sub_imm(R1, R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(sub_r1.size, 4);
  UT_ASSERT_EQ(((uint32_t)d[off] | ((uint32_t)d[off + 1] << 8)), (sub_r1.opcode >> 16) & 0xffff);
  UT_ASSERT_EQ(((uint32_t)d[off + 2] | ((uint32_t)d[off + 3] << 8)), sub_r1.opcode & 0xffff);
  off += 4;

  thumb_opcode push_r2 = th_push(1u << R2);
  UT_ASSERT_EQ(d[off], push_r2.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (push_r2.opcode >> 8) & 0xff);
  off += push_r2.size;

  thumb_opcode push_r3 = th_push(1u << R3);
  UT_ASSERT_EQ(d[off], push_r3.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (push_r3.opcode >> 8) & 0xff);
  off += push_r3.size;

  thumb_opcode ldm = th_ldm(R0, (1u << R2) | (1u << R3), 1 /* writeback */, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(d[off], ldm.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (ldm.opcode >> 8) & 0xff);
  off += ldm.size;

  thumb_opcode stm = th_stm(R1, (1u << R2) | (1u << R3), 1 /* writeback */, ENFORCE_ENCODING_NONE);
  UT_ASSERT_EQ(d[off], stm.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (stm.opcode >> 8) & 0xff);
  off += stm.size;

  thumb_opcode pop_r3 = th_pop((uint16_t)(1u << R3));
  UT_ASSERT_EQ(d[off], pop_r3.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (pop_r3.opcode >> 8) & 0xff);
  off += pop_r3.size;

  thumb_opcode pop_r2 = th_pop((uint16_t)(1u << R2));
  UT_ASSERT_EQ(d[off], pop_r2.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (pop_r2.opcode >> 8) & 0xff);
  off += pop_r2.size;

  thumb_opcode pop_r1 = th_pop((uint16_t)(1u << R1));
  UT_ASSERT_EQ(d[off], pop_r1.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (pop_r1.opcode >> 8) & 0xff);
  off += pop_r1.size;

  thumb_opcode pop_r0 = th_pop((uint16_t)(1u << R0));
  UT_ASSERT_EQ(d[off], pop_r0.opcode & 0xff);
  UT_ASSERT_EQ(d[off + 1], (pop_r0.opcode >> 8) & 0xff);
  off += pop_r0.size;

  UT_ASSERT_EQ(off, ind);

  utb_free(ir);

  return 0;
}

/* ------------------------------------------------------------------ sub-word loads/stores */

UT_TEST(test_load_mop_spill_signed_byte_emits_ldrsb)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_load_mop(mop_spill_s(-8, IROP_BTYPE_INT8), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT(bytes_match_opcode(ind, th_ldrsb_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_load_mop_spill_unsigned_byte_emits_ldrb)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_load_mop(mop_spill_u(-8, IROP_BTYPE_INT8), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT(bytes_match_opcode(ind, th_ldrb_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_load_mop_spill_signed_halfword_emits_ldrsh)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_load_mop(mop_spill_s(-8, IROP_BTYPE_INT16), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT(bytes_match_opcode(ind, th_ldrsh_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_load_mop_spill_unsigned_halfword_emits_ldrh)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_load_mop(mop_spill_u(-8, IROP_BTYPE_INT16), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_LOAD);

  UT_ASSERT(bytes_match_opcode(ind, th_ldrh_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_store_mop_spill_byte_emits_strb)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_store_mop(mop_spill_u(-8, IROP_BTYPE_INT8), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_STORE);

  UT_ASSERT(bytes_match_opcode(ind, th_strb_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

UT_TEST(test_store_mop_spill_halfword_emits_strh)
{
  setup_gen();
  tcc_state->need_frame_pointer = 1;

  tcc_gen_machine_store_mop(mop_spill_u(-8, IROP_BTYPE_INT16), mop_reg(R2, IROP_BTYPE_INT32), TCCIR_OP_STORE);

  UT_ASSERT(bytes_match_opcode(ind, th_strh_imm(R2, R_FP, 8, 4 /* subtract */, ENFORCE_ENCODING_NONE)));

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_mem)
{
  UT_RUN(test_assign_reg_to_reg_distinct_regs_emits_mov);
  UT_RUN(test_assign_reg_to_spill_emits_store_with_offset);
  UT_RUN(test_assign_spill_to_reg_emits_load_with_offset);
  UT_RUN(test_load_mop_spill_nonzero_offset_emits_immediate_ldr);
  UT_RUN(test_store_mop_spill_nonzero_offset_emits_immediate_str);
  UT_RUN(test_load_indexed_reg_base_reg_index_emits_register_offset_ldr);
  UT_RUN(test_store_indexed_reg_base_reg_index_emits_register_offset_str);
  UT_RUN(test_lea_frame_addr_emits_stack_address_computation);
  UT_RUN(test_block_copy_small_fixed_size_emits_ldm_stm_pair);
  UT_RUN(test_load_mop_spill_signed_byte_emits_ldrsb);
  UT_RUN(test_load_mop_spill_unsigned_byte_emits_ldrb);
  UT_RUN(test_load_mop_spill_signed_halfword_emits_ldrsh);
  UT_RUN(test_load_mop_spill_unsigned_halfword_emits_ldrh);
  UT_RUN(test_store_mop_spill_byte_emits_strb);
  UT_RUN(test_store_mop_spill_halfword_emits_strh);
}
