/*
 *  test_gen_callsite.c - suite for arm-thumb-callsite.c
 *
 *  Covers the call-site table (thumb_get_or_create_call_site /
 *  thumb_get_call_site_for_id / thumb_free_call_sites) and
 *  thumb_build_call_layout_from_ir's two argument-discovery paths (the
 *  argc_hint fast path and the legacy backward-scan path), asserting on the
 *  REAL AAPCS register/stack layout produced by the real
 *  tcc_gen_machine_abi_assign_call_args (arm-thumb-gen.c) ->
 *  tcc_abi_classify_argument (arch/arm/arm_aapcs.c) chain -- both already
 *  linked for real into build_backend (see test_gen_dispatch_smoke.c, this
 *  file's template).
 */

#define USING_GLOBALS
#include "ir.h"
#include "arm-thumb-defs.h"
#include "ir_build.h"

#include "ut.h"

/* ------------------------------------------------------------------ helpers */

/* thumb_gen_state.call_sites_by_id is a process-global table (declared in
 * arm-thumb-defs.h, defined in arm-thumb-gen.c); every test that touches it
 * must start from a known-empty state. */
static void setup_callsites(void)
{
  thumb_free_call_sites();
}

/* ---- local copies of the IR-immediate helpers, per the task's no-shared-
 * header rule (this file already gets utb_imm from ir_build.h, but keeps its
 * own tiny wrapper for the FUNCPARAMVAL-index encoding to stay self-contained
 * and readable at the call site). ---- */

static IROperand cs_param_marker(int call_id, int param_idx)
{
  return utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, param_idx), IROP_BTYPE_INT32);
}

static void cs_layout_free(TCCAbiCallLayout *layout, IROperand *args, MachineOperand *mops)
{
  if (layout->locs)
    tcc_free(layout->locs);
  if (args)
    tcc_free(args);
  if (mops)
    tcc_free(mops);
}

/* ------------------------------------------------------------ call-site table */

UT_TEST(test_get_or_create_call_site_first_call_grows_to_16)
{
  setup_callsites();

  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 0);
  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);

  ThumbGenCallSite *cs = thumb_get_or_create_call_site(0);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->call_id, 0);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 16);
  UT_ASSERT(thumb_gen_state.call_sites_by_id != NULL);

  return 0;
}

UT_TEST(test_get_or_create_call_site_within_first_batch_does_not_regrow)
{
  setup_callsites();

  thumb_get_or_create_call_site(0);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 16);

  ThumbGenCallSite *cs15 = thumb_get_or_create_call_site(15);
  UT_ASSERT(cs15 != NULL);
  UT_ASSERT_EQ(cs15->call_id, 15);
  /* id 15 still fits in the initial batch of 16 (indices 0..15). */
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 16);

  return 0;
}

UT_TEST(test_get_or_create_call_site_doubles_past_16)
{
  setup_callsites();

  thumb_get_or_create_call_site(0);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 16);

  /* id 16 is out of the current [0,16) table -> must grow, doubling to 32. */
  ThumbGenCallSite *cs16 = thumb_get_or_create_call_site(16);
  UT_ASSERT(cs16 != NULL);
  UT_ASSERT_EQ(cs16->call_id, 16);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 32);

  return 0;
}

UT_TEST(test_get_or_create_call_site_large_id_doubles_repeatedly)
{
  setup_callsites();

  /* Starting from empty, id 40 must grow past a single doubling
   * (16 -> 32 -> 64) to fit index 40. */
  ThumbGenCallSite *cs = thumb_get_or_create_call_site(40);
  UT_ASSERT(cs != NULL);
  UT_ASSERT_EQ(cs->call_id, 40);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 64);

  return 0;
}

UT_TEST(test_get_or_create_call_site_negative_id_returns_null)
{
  setup_callsites();

  ThumbGenCallSite *cs = thumb_get_or_create_call_site(-1);
  UT_ASSERT(cs == NULL);
  /* Must not have allocated anything for a rejected id. */
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 0);
  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);

  return 0;
}

UT_TEST(test_get_or_create_call_site_idempotent_same_id_returns_same_slot)
{
  setup_callsites();

  ThumbGenCallSite *first = thumb_get_or_create_call_site(5);
  UT_ASSERT(first != NULL);
  first->registers_map = 0xABCD;
  first->function_argument_count = 3;

  ThumbGenCallSite *second = thumb_get_or_create_call_site(5);
  UT_ASSERT(second != NULL);
  UT_ASSERT(second == first);
  UT_ASSERT_EQ(second->call_id, 5);
  UT_ASSERT_EQ(second->registers_map, 0xABCD);
  UT_ASSERT_EQ(second->function_argument_count, 3);

  return 0;
}

UT_TEST(test_get_call_site_for_id_null_table_returns_null)
{
  setup_callsites();

  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);
  UT_ASSERT(thumb_get_call_site_for_id(0) == NULL);

  return 0;
}

UT_TEST(test_get_call_site_for_id_negative_returns_null)
{
  setup_callsites();

  thumb_get_or_create_call_site(3);
  UT_ASSERT(thumb_get_call_site_for_id(-1) == NULL);

  return 0;
}

UT_TEST(test_get_call_site_for_id_at_or_beyond_size_returns_null)
{
  setup_callsites();

  thumb_get_or_create_call_site(0); /* grows table to size 16 */
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 16);

  UT_ASSERT(thumb_get_call_site_for_id(16) == NULL); /* == size */
  UT_ASSERT(thumb_get_call_site_for_id(100) == NULL); /* well beyond size */

  return 0;
}

UT_TEST(test_get_call_site_for_id_returns_created_slot)
{
  setup_callsites();

  ThumbGenCallSite *created = thumb_get_or_create_call_site(7);
  UT_ASSERT(created != NULL);

  ThumbGenCallSite *found = thumb_get_call_site_for_id(7);
  UT_ASSERT(found == created);
  UT_ASSERT_EQ(found->call_id, 7);

  return 0;
}

UT_TEST(test_free_call_sites_resets_table_to_null_and_zero)
{
  setup_callsites();

  ThumbGenCallSite *cs = thumb_get_or_create_call_site(20); /* forces growth */
  UT_ASSERT(cs != NULL);
  UT_ASSERT(thumb_gen_state.call_sites_by_id_size > 0);

  /* Give the slot a heap-allocated argument list, so thumb_free_call_sites
   * exercises its per-slot free loop (not just the top-level table free). */
  cs->function_argument_list = (int *)tcc_mallocz(sizeof(int) * 4);
  cs->function_argument_count = 4;

  thumb_free_call_sites();

  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 0);

  return 0;
}

UT_TEST(test_free_call_sites_on_already_empty_state_is_noop)
{
  setup_callsites();

  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);
  thumb_free_call_sites(); /* must not crash on a table that's already NULL */
  UT_ASSERT(thumb_gen_state.call_sites_by_id == NULL);
  UT_ASSERT_EQ(thumb_gen_state.call_sites_by_id_size, 0);

  return 0;
}

/* ------------------------------------------------------ build_call_layout: argc_hint */

/* Fast path: argc_hint >= 0, five plain int args -> real AAPCS layout is
 * R0-R3 for args 0-3 and one stack word (offset 0) for arg 4. Verified
 * against arch/arm/arm_aapcs.c's tcc_abi_classify_argument, the same
 * production code test_arm_aapcs.c already exercises directly. */
UT_TEST(test_build_call_layout_argc_hint_five_int_args_r0_r3_then_stack)
{
  TCCIRState *ir = utb_new();
  const int call_id = 3;
  const int32_t vals[5] = { 10, 20, 30, 40, 50 };

  for (int i = 0; i < 5; i++)
    utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(vals[i], IROP_BTYPE_INT32), cs_param_marker(call_id, i));

  int call_idx = ir->next_instruction_index;

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = NULL;
  MachineOperand *out_mops = NULL;

  int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, 5, &layout, &out_args, &out_mops);

  UT_ASSERT_EQ(argc, 5);
  UT_ASSERT_EQ(layout.argc, 5);
  UT_ASSERT(layout.locs != NULL);

  for (int i = 0; i < 4; i++)
  {
    UT_ASSERT_EQ((int)layout.locs[i].kind, TCC_ABI_LOC_REG);
    UT_ASSERT_EQ((int)layout.locs[i].reg_base, i);
    UT_ASSERT_EQ((int)layout.locs[i].reg_count, 1);
  }
  UT_ASSERT_EQ((int)layout.locs[4].kind, TCC_ABI_LOC_STACK);
  UT_ASSERT_EQ((int)layout.locs[4].stack_off, 0);

  UT_ASSERT(out_args != NULL);
  UT_ASSERT(out_mops != NULL);
  for (int i = 0; i < 5; i++)
  {
    UT_ASSERT_EQ(irop_get_imm32(out_args[i]), vals[i]);
    UT_ASSERT_EQ((int)out_mops[i].kind, MACH_OP_IMM);
    UT_ASSERT_EQ((long long)out_mops[i].u.imm.val, vals[i]);
  }

  cs_layout_free(&layout, out_args, out_mops);
  utb_free(ir);
  return 0;
}

/* argc_hint == 0: the argc<=0 short-circuit must return 0 immediately, with
 * out_args/out_mops both set to NULL (no allocation at all) and an
 * argc-0/stack_size-0 layout -- ir/../arm-thumb-callsite.c's own early-return
 * block. No FUNCPARAMVAL needs to exist for this call_id at all. */
UT_TEST(test_build_call_layout_argc_hint_zero_short_circuits)
{
  TCCIRState *ir = utb_new();

  TCCAbiCallLayout layout;
  memset(&layout, 0xAA, sizeof(layout)); /* poison, so the early-return must overwrite argc/stack_size */
  IROperand *out_args = (IROperand *)0x1; /* poison pointer: must be overwritten to NULL */
  MachineOperand *out_mops = (MachineOperand *)0x1;

  int argc = thumb_build_call_layout_from_ir(ir, /*call_idx=*/0, /*call_id=*/0, /*argc_hint=*/0, &layout, &out_args,
                                             &out_mops);

  UT_ASSERT_EQ(argc, 0);
  UT_ASSERT_EQ(layout.argc, 0);
  UT_ASSERT_EQ(layout.stack_size, 0);
  UT_ASSERT(out_args == NULL);
  UT_ASSERT(out_mops == NULL);

  utb_free(ir);
  return 0;
}

/* argc_hint fast path also works when the caller doesn't want IROperand /
 * MachineOperand arrays back (out_args == NULL, out_mops == NULL passed
 * in) -- both are optional per the function's doc comment. */
UT_TEST(test_build_call_layout_argc_hint_null_out_params_are_optional)
{
  TCCIRState *ir = utb_new();
  const int call_id = 1;

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(99, IROP_BTYPE_INT32), cs_param_marker(call_id, 0));
  int call_idx = ir->next_instruction_index;

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, 1, &layout, NULL, NULL);

  UT_ASSERT_EQ(argc, 1);
  UT_ASSERT_EQ(layout.argc, 1);
  UT_ASSERT_EQ((int)layout.locs[0].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ((int)layout.locs[0].reg_base, 0);

  cs_layout_free(&layout, NULL, NULL);
  utb_free(ir);
  return 0;
}

/* A 64-bit (SCALAR64) argument must land on an even register pair per AAPCS
 * -- exercises the irop_needs_pair() -> TCC_ABI_ARG_SCALAR64 classification
 * path inside thumb_build_call_layout_from_ir, distinct from the plain-int
 * SCALAR32 path above. arg0 is a plain int (r0), arg1 is 64-bit and must
 * skip r1 to land on r2/r3 (the classic AAPCS alignment-gap case, matching
 * test_codegen_call.c's test_aapcs_64bit_param_at_odd_argno_skips_to_even_pair). */
UT_TEST(test_build_call_layout_argc_hint_64bit_arg_uses_even_reg_pair)
{
  TCCIRState *ir = utb_new();
  const int call_id = 9;

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, IROP_BTYPE_INT32), cs_param_marker(call_id, 0));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(2, IROP_BTYPE_INT64), cs_param_marker(call_id, 1));
  int call_idx = ir->next_instruction_index;

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = NULL;

  int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, 2, &layout, &out_args, NULL);

  UT_ASSERT_EQ(argc, 2);
  UT_ASSERT_EQ((int)layout.locs[0].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ((int)layout.locs[0].reg_base, 0);
  UT_ASSERT_EQ((int)layout.locs[0].reg_count, 1);

  UT_ASSERT_EQ((int)layout.locs[1].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ((int)layout.locs[1].reg_base, 2); /* skipped r1 to align the pair */
  UT_ASSERT_EQ((int)layout.locs[1].reg_count, 2);

  cs_layout_free(&layout, out_args, NULL);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------ build_call_layout: legacy scan */

/* argc_hint < 0: legacy backward scan finds argc by tracking the highest
 * param_idx seen for the matching call_id, ignoring FUNCPARAMVALs belonging
 * to OTHER call_ids interleaved in between (proves the call_id filter in the
 * scan, not just the index-max logic). */
UT_TEST(test_build_call_layout_legacy_scan_finds_argc_and_filters_other_call_ids)
{
  TCCIRState *ir = utb_new();
  const int call_id = 2;
  const int other_call_id = 5;

  /* Interleave: other_call_id's args must be skipped by the call_id filter. */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(111, IROP_BTYPE_INT32), cs_param_marker(other_call_id, 0));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, IROP_BTYPE_INT32), cs_param_marker(call_id, 0));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(222, IROP_BTYPE_INT32), cs_param_marker(other_call_id, 1));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(2, IROP_BTYPE_INT32), cs_param_marker(call_id, 1));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(3, IROP_BTYPE_INT32), cs_param_marker(call_id, 2));
  int call_idx = ir->next_instruction_index;

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = NULL;
  MachineOperand *out_mops = NULL;

  int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, /*argc_hint=*/-1, &layout, &out_args, &out_mops);

  UT_ASSERT_EQ(argc, 3); /* max param_idx (2) + 1, from call_id's own args only */
  UT_ASSERT_EQ(layout.argc, 3);
  UT_ASSERT_EQ((int)layout.locs[0].kind, TCC_ABI_LOC_REG);
  UT_ASSERT_EQ((int)layout.locs[0].reg_base, 0);
  UT_ASSERT_EQ((int)layout.locs[1].reg_base, 1);
  UT_ASSERT_EQ((int)layout.locs[2].reg_base, 2);

  UT_ASSERT(out_args != NULL);
  UT_ASSERT_EQ(irop_get_imm32(out_args[0]), 1);
  UT_ASSERT_EQ(irop_get_imm32(out_args[1]), 2);
  UT_ASSERT_EQ(irop_get_imm32(out_args[2]), 3);

  cs_layout_free(&layout, out_args, out_mops);
  utb_free(ir);
  return 0;
}

/* Legacy scan, no FUNCPARAMVAL at all for the requested call_id anywhere in
 * the preceding instructions -> max_arg_index stays -1 -> argc == 0, taking
 * the same early-return shape as the argc_hint==0 test above. */
UT_TEST(test_build_call_layout_legacy_scan_no_matching_funcparamval_yields_argc_zero)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, IROP_BTYPE_INT32), cs_param_marker(4, 0));
  int call_idx = ir->next_instruction_index;

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = (IROperand *)0x1;
  MachineOperand *out_mops = (MachineOperand *)0x1;

  /* Ask for a different call_id (9) than the one FUNCPARAMVAL actually carries (4). */
  int argc = thumb_build_call_layout_from_ir(ir, call_idx, /*call_id=*/9, /*argc_hint=*/-1, &layout, &out_args,
                                             &out_mops);

  UT_ASSERT_EQ(argc, 0);
  UT_ASSERT_EQ(layout.argc, 0);
  UT_ASSERT(out_args == NULL);
  UT_ASSERT(out_mops == NULL);

  utb_free(ir);
  return 0;
}

/* Legacy scan only looks at instructions strictly before call_idx -- a
 * FUNCPARAMVAL placed AT OR AFTER call_idx must not be counted. Two real
 * args precede call_idx; a third (higher-indexed) FUNCPARAMVAL for the same
 * call_id sits at call_idx itself and must be invisible to the scan. */
UT_TEST(test_build_call_layout_legacy_scan_ignores_instructions_at_or_after_call_idx)
{
  TCCIRState *ir = utb_new();
  const int call_id = 6;

  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(1, IROP_BTYPE_INT32), cs_param_marker(call_id, 0));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(2, IROP_BTYPE_INT32), cs_param_marker(call_id, 1));
  int call_idx = ir->next_instruction_index;
  /* This one lands AT call_idx -- out of the backward-scan's j < call_idx range. */
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(3, IROP_BTYPE_INT32), cs_param_marker(call_id, 2));

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = NULL;

  int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, /*argc_hint=*/-1, &layout, &out_args, NULL);

  UT_ASSERT_EQ(argc, 2); /* param_idx 2's FUNCPARAMVAL at call_idx itself is not scanned */
  UT_ASSERT_EQ(irop_get_imm32(out_args[0]), 1);
  UT_ASSERT_EQ(irop_get_imm32(out_args[1]), 2);

  cs_layout_free(&layout, out_args, NULL);
  utb_free(ir);
  return 0;
}

/* call_idx < 0 is the function's own top-level guard -- must return -1
 * without touching layout or the out params at all. */
UT_TEST(test_build_call_layout_negative_call_idx_returns_error)
{
  TCCIRState *ir = utb_new();

  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));
  IROperand *out_args = NULL;
  MachineOperand *out_mops = NULL;

  int argc = thumb_build_call_layout_from_ir(ir, /*call_idx=*/-1, /*call_id=*/0, /*argc_hint=*/-1, &layout,
                                             &out_args, &out_mops);

  UT_ASSERT_EQ(argc, -1);
  UT_ASSERT(out_args == NULL);
  UT_ASSERT(out_mops == NULL);

  utb_free(ir);
  return 0;
}

/* NULL ir is the other half of the same top-level guard. */
UT_TEST(test_build_call_layout_null_ir_returns_error)
{
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  int argc = thumb_build_call_layout_from_ir(NULL, 0, 0, -1, &layout, NULL, NULL);
  UT_ASSERT_EQ(argc, -1);

  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(gen_callsite)
{
  UT_RUN(test_get_or_create_call_site_first_call_grows_to_16);
  UT_RUN(test_get_or_create_call_site_within_first_batch_does_not_regrow);
  UT_RUN(test_get_or_create_call_site_doubles_past_16);
  UT_RUN(test_get_or_create_call_site_large_id_doubles_repeatedly);
  UT_RUN(test_get_or_create_call_site_negative_id_returns_null);
  UT_RUN(test_get_or_create_call_site_idempotent_same_id_returns_same_slot);
  UT_RUN(test_get_call_site_for_id_null_table_returns_null);
  UT_RUN(test_get_call_site_for_id_negative_returns_null);
  UT_RUN(test_get_call_site_for_id_at_or_beyond_size_returns_null);
  UT_RUN(test_get_call_site_for_id_returns_created_slot);
  UT_RUN(test_free_call_sites_resets_table_to_null_and_zero);
  UT_RUN(test_free_call_sites_on_already_empty_state_is_noop);

  UT_RUN(test_build_call_layout_argc_hint_five_int_args_r0_r3_then_stack);
  UT_RUN(test_build_call_layout_argc_hint_zero_short_circuits);
  UT_RUN(test_build_call_layout_argc_hint_null_out_params_are_optional);
  UT_RUN(test_build_call_layout_argc_hint_64bit_arg_uses_even_reg_pair);

  UT_RUN(test_build_call_layout_legacy_scan_finds_argc_and_filters_other_call_ids);
  UT_RUN(test_build_call_layout_legacy_scan_no_matching_funcparamval_yields_argc_zero);
  UT_RUN(test_build_call_layout_legacy_scan_ignores_instructions_at_or_after_call_idx);
  UT_RUN(test_build_call_layout_negative_call_idx_returns_error);
  UT_RUN(test_build_call_layout_null_ir_returns_error);
}
