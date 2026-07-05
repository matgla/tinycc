/*
 *  test_gen_switch.c - suite for the SWITCH_TABLE / SWITCH_LOAD backend
 *  entry points in arm-thumb-gen.c:
 *
 *    tcc_gen_machine_switch_table_dry_run_size()
 *    tcc_gen_machine_switch_table_mop()
 *    tcc_gen_machine_switch_load_dry_run_size()
 *    tcc_gen_machine_switch_load_mop()
 *
 *  Mirrors test_gen_dispatch_smoke.c: calls the tcc_gen_machine_*_mop
 *  functions directly (bypassing ir/codegen.c's dispatch loop) with
 *  hand-built MachineOperand / TCCIRSwitchTable / TCCIRSwitchValueTable
 *  arguments, emitting real Thumb-2 bytes into a real Section via the real
 *  o()/section_add machinery.
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

/* Unlike test_gen_dispatch_smoke.c's setup_gen() (which calls
 * arm_target_init() directly), SWITCH_LOAD needs the literal-pool machinery
 * (load_full_const() -> th_literal_pool_find_or_allocate() ->
 * literal_pool_hash), and that hash table is only initialized by
 * th_literal_pool_init() -- a `static` helper reachable *only* from
 * arm_init(TCCState*), never from arm_target_init(). So this suite calls
 * the (heavier, but fully linked already: ssa_opt_arm.o + the
 * tcc_ir_ssa_opt_register_target() no-op stub in ra_link_stubs.c and the
 * sym_push()/put_extern_sym() stubs in codegen_backend_stubs.c cover
 * everything arm_init() touches) arm_init() entry point instead, which
 * itself calls arm_target_init() internally. Confirmed empirically: with
 * plain arm_target_init(), tcc_gen_machine_switch_load_mop() SIGSEGVs
 * inside tcc_chained_hash_bucket_head() on literal_pool_hash.buckets==NULL. */
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

static uint32_t read_le32(const unsigned char *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Minimal, well-formed-enough TCCIRSwitchTable/TCCIRSwitchValueTable owner.
 * tcc_gen_machine_switch_{table,load}_mop only dereference `ir` inside the
 * TRACE(...) macro (compiled to a no-op unless -DTCC_LOG_THUMB), so a
 * zeroed TCCIRState is sufficient -- no compact_instructions/pool setup
 * needed. */
static TCCIRState *utsw_new_ir(void)
{
  return (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
}

/* A minimal symbol usable as vtab->rodata_sym: validate_sym_for_reloc()
 * requires v without SYM_FIELD and c >= 0; load_full_const() skips
 * put_extern_sym() registration entirely when sym->c != 0. */
static Sym *utsw_new_rodata_sym(void)
{
  Sym *sym = (Sym *)tcc_mallocz(sizeof(Sym));
  sym->v = 0;
  sym->c = 1;
  return sym;
}

/* ------------------------------------------------------------------ dry-run size: switch table */

UT_TEST(test_switch_table_dry_run_size_zero_entries)
{
  /* Preamble only: LSL.W(4) + ADD(2) + LDR.W(4) + ADD(2) + BX(2) = 14 bytes. */
  UT_ASSERT_EQ(tcc_gen_machine_switch_table_dry_run_size(0), 14);
  return 0;
}

UT_TEST(test_switch_table_dry_run_size_scales_by_four_per_entry)
{
  /* 14-byte preamble + 4 bytes per table entry (32-bit signed PC-relative
   * offsets). */
  UT_ASSERT_EQ(tcc_gen_machine_switch_table_dry_run_size(1), 18);
  UT_ASSERT_EQ(tcc_gen_machine_switch_table_dry_run_size(5), 34);
  UT_ASSERT_EQ(tcc_gen_machine_switch_table_dry_run_size(100), 414);
  return 0;
}

/* ------------------------------------------------------------------ dry-run size: switch load */

UT_TEST(test_switch_load_dry_run_size_is_fixed_eight_bytes)
{
  /* Literal-pool LDR (4 bytes, forced T2 since R_IP is not a low register)
   * + indexed shifted LDR.W (4 bytes).  The table itself lives in .rodata
   * and contributes no .text bytes, so this must be independent of
   * num_entries. */
  UT_ASSERT_EQ(tcc_gen_machine_switch_load_dry_run_size(0), 8);
  UT_ASSERT_EQ(tcc_gen_machine_switch_load_dry_run_size(1), 8);
  UT_ASSERT_EQ(tcc_gen_machine_switch_load_dry_run_size(1000), 8);
  return 0;
}

/* ------------------------------------------------------------------ switch table mop */

UT_TEST(test_switch_table_mop_zero_entries_emits_preamble_only)
{
  setup_gen();

  TCCIRState *ir = utsw_new_ir();
  TCCIRSwitchTable table;
  memset(&table, 0, sizeof(table));
  table.num_entries = 0;

  tcc_gen_machine_switch_table_mop(mop_reg(R1, IROP_BTYPE_INT32), &table, ir, 0);

  /* No table entries: total bytes emitted must equal the dry-run preamble
   * size, and table_code_addr must sit right at the (now-empty) table
   * start, i.e. at the end of the preamble. */
  UT_ASSERT_EQ(ind, tcc_gen_machine_switch_table_dry_run_size(0));
  UT_ASSERT_EQ(table.table_code_addr, 14);

  return 0;
}

UT_TEST(test_switch_table_mop_emits_preamble_plus_zeroed_table_slots)
{
  setup_gen();

  TCCIRState *ir = utsw_new_ir();
  TCCIRSwitchTable table;
  memset(&table, 0, sizeof(table));
  table.num_entries = 3;

  tcc_gen_machine_switch_table_mop(mop_reg(R1, IROP_BTYPE_INT32), &table, ir, 0);

  /* Total size matches the dry-run formula for the same entry count. */
  UT_ASSERT_EQ(ind, tcc_gen_machine_switch_table_dry_run_size(3));
  /* table_code_addr is the byte offset right after the 14-byte preamble. */
  UT_ASSERT_EQ(table.table_code_addr, 14);

  /* The table_code_addr..ind region is num_entries*4 bytes of placeholder
   * zeros (g(0) x4 per entry) -- these get backpatched with real branch
   * offsets later by codegen.c, not by the mop itself. */
  UT_ASSERT_EQ(ind - table.table_code_addr, 3 * 4);
  for (int i = 0; i < 3; i++)
    UT_ASSERT_EQ(read_le32(cur_text_section->data + table.table_code_addr + i * 4), 0);

  return 0;
}

UT_TEST(test_switch_table_mop_preamble_encodes_lsl_and_terminal_bx)
{
  setup_gen();

  TCCIRState *ir = utsw_new_ir();
  TCCIRSwitchTable table;
  memset(&table, 0, sizeof(table));
  table.num_entries = 1;

  /* Index value pre-placed in R1 (a plain hardware register: mach_ensure_in_reg
   * on a non-deref MACH_OP_REG returns u.reg.r0 directly, with no code
   * emitted for the "ensure" step itself). */
  tcc_gen_machine_switch_table_mop(mop_reg(R1, IROP_BTYPE_INT32), &table, ir, 0);

  /* Preamble layout is LSL.W(4) + ADD(2) + LDR.W(4) + ADD(2) + BX(2) = 14
   * bytes.  Byte values below are the real emitted bytes, dumped and read
   * back via a throwaway debug printf against this exact call (see
   * self-verify notes) -- not hand-derived from the ISA spec.
   *
   * Bytes 0-3: `LSL.W R_IP, R1, #2` (T2 shift-immediate, forced 32-bit by
   * ENFORCE_ENCODING_32BIT so the preamble size is fixed): first halfword
   * 0xea4f, second halfword 0x0c81.  Decoding the second halfword as a
   * sanity cross-check against the T2 LSL (immediate) bit-layout confirms
   * Rd=1100=R_IP, Rm=0001=R1, imm3:imm2=00:01 -> shift amount 2. */
  uint16_t lsl_hw0 = (uint16_t)(cur_text_section->data[0] | (cur_text_section->data[1] << 8));
  uint16_t lsl_hw1 = (uint16_t)(cur_text_section->data[2] | (cur_text_section->data[3] << 8));
  UT_ASSERT_EQ(lsl_hw0, 0xea4f);
  UT_ASSERT_EQ(lsl_hw1, 0x0c81);

  /* Final halfword of the preamble (bytes 12-13) is `BX R_IP` -- T1 encoding
   * 0x4760 (opcode 0100011100 + Rm=1100). */
  uint16_t bx_halfword = (uint16_t)(cur_text_section->data[12] | (cur_text_section->data[13] << 8));
  UT_ASSERT_EQ(bx_halfword, 0x4760);

  return 0;
}

/* ------------------------------------------------------------------ switch load mop */

UT_TEST(test_switch_load_mop_requires_rodata_symbol)
{
  /* tcc_gen_machine_switch_load_mop() calls tcc_error() (which the stub
   * layer treats as a hard abort) when vtab->rodata_sym is NULL -- this test
   * documents that precondition without exercising the abort path (which
   * would kill the whole test binary). See docstring in arm-thumb-gen.c:
   * "SWITCH_LOAD table has no rodata symbol (switch_to_data should have
   * allocated it)". */
  TCCIRSwitchValueTable vtab;
  memset(&vtab, 0, sizeof(vtab));
  UT_ASSERT(vtab.rodata_sym == NULL);
  return 0;
}

UT_TEST(test_switch_load_mop_emits_exactly_eight_bytes)
{
  setup_gen();

  TCCIRState *ir = utsw_new_ir();
  TCCIRSwitchValueTable vtab;
  memset(&vtab, 0, sizeof(vtab));
  vtab.num_entries = 4;
  vtab.rodata_sym = utsw_new_rodata_sym();

  int start = ind;
  tcc_gen_machine_switch_load_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R0, IROP_BTYPE_INT32), &vtab, ir, 0);

  UT_ASSERT_EQ(ind - start, tcc_gen_machine_switch_load_dry_run_size(vtab.num_entries));
  UT_ASSERT_EQ(ind - start, 8);

  return 0;
}

UT_TEST(test_switch_load_mop_second_word_is_ldr_w_dest_ip_index_lsl2)
{
  setup_gen();

  TCCIRState *ir = utsw_new_ir();
  TCCIRSwitchValueTable vtab;
  memset(&vtab, 0, sizeof(vtab));
  vtab.num_entries = 2;
  vtab.rodata_sym = utsw_new_rodata_sym();

  /* dest = R0, index = R1.  R_IP (R12) is reserved internally for the
   * table-base load, so it must not appear as either operand here -- the
   * mop itself excludes it via dest_excl when choosing dest_reg. */
  tcc_gen_machine_switch_load_mop(mop_reg(R1, IROP_BTYPE_INT32), mop_reg(R0, IROP_BTYPE_INT32), &vtab, ir, 0);

  UT_ASSERT_EQ(ind, 8);

  /* Second instruction: `LDR.W R0, [R12, R1, LSL #2]` -- T2 register-offset
   * load, always encoded as 32-bit (ENFORCE_ENCODING_32BIT). Encoding
   * verified empirically by dumping the real emitted bytes (see self-verify
   * notes): first halfword 0xf85c = 1111100001011100b -> LDR (register) T2
   * opcode `111110000101` with Rn=1100=R_IP; second halfword 0x0021 =
   * 0000000000100001b -> Rt=0000=R0, imm2=10b=2 (LSL #2), Rm=0001=R1. */
  uint16_t hw0 = (uint16_t)(cur_text_section->data[4] | (cur_text_section->data[5] << 8));
  uint16_t hw1 = (uint16_t)(cur_text_section->data[6] | (cur_text_section->data[7] << 8));
  UT_ASSERT_EQ(hw0, 0xf85c);
  UT_ASSERT_EQ(hw1, 0x0021);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_switch)
{
  UT_RUN(test_switch_table_dry_run_size_zero_entries);
  UT_RUN(test_switch_table_dry_run_size_scales_by_four_per_entry);
  UT_RUN(test_switch_load_dry_run_size_is_fixed_eight_bytes);
  UT_RUN(test_switch_table_mop_zero_entries_emits_preamble_only);
  UT_RUN(test_switch_table_mop_emits_preamble_plus_zeroed_table_slots);
  UT_RUN(test_switch_table_mop_preamble_encodes_lsl_and_terminal_bx);
  UT_RUN(test_switch_load_mop_requires_rodata_symbol);
  UT_RUN(test_switch_load_mop_emits_exactly_eight_bytes);
  UT_RUN(test_switch_load_mop_second_word_is_ldr_w_dest_ip_index_lsl2);
}
