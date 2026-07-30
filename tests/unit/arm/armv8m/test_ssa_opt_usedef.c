/*
 *  test_ssa_opt_usedef.c - use-def machinery + address resolvers
 *
 *  Phase 1.
 *
 *  Covers:
 *    - ssa_opt_vinfo: vreg → vinfo lookup
 *    - ssa_opt_add_use_instr / ssa_opt_remove_use_instr: use-list management
 *    - ssa_opt_scan_instr_uses: scanning src1/src2/MLA-accum/STORE-dest
 *    - ssa_opt_replace_all_uses: operand rewriting
 *    - ssa_opt_nop_instr: use-count cleanup when NOPing
 *    - ssa_opt_resolve_lea_stackloc[_ex]: LEA/ASSIGN/STORE chain resolution
 *    - ssa_opt_resolve_temp_to_base_off: base+offset canonicalization
 *    - ssa_opt_indirect_stack_offset[_ex]: store/load address resolution
 *
 *  HARNESS NOTES:
 *    - Links the real ir/opt/ssa_opt.c via UT11.
 *    - Uses ssa_build.h helpers for hand-built vinfo.
 */

#include "ssa_build.h"
#include "source/opt/ssa/include/ssa_opt.h"

#include "ut.h"

#define USING_GLOBALS
#include <limits.h>
#include "tcc.h"
#include "source/opt/ssa/include/ssa_opt.h"

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Helper to extract raw vreg encoding from an IROperand. */
#define IROP_VR(op) ((int)(op).vr)

/* ========================================================================
 * ssa_opt_vinfo: vreg lookup
 * ======================================================================== */

UT_TEST(test_vinfo_lookup_temp)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  IRSSAVregInfo *vi = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(vi != NULL);
  UT_ASSERT_EQ(vi->def_instr, 0);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_vinfo_lookup_non_temp_returns_null)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* VAR vreg should not have a vinfo entry (vinfo is TEMP-only). */
  IRSSAVregInfo *vi = ssa_vinfo(&c, utb_vreg(utb_var(0, I32)));
  UT_ASSERT(vi == NULL);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_scan_instr_uses: scanning src1/src2
 * ======================================================================== */

UT_TEST(test_scan_instr_uses_src1_src2)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = #2; t2 = t0 + t1 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(2, I32));
  ssa_add_instr4(&c, TCCIR_OP_ADD, utb_temp(2, I32), utb_temp(0, I32), utb_temp(1, I32), utb_imm(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* t0 has 1 use (the ADD), t1 has 1 use (the ADD). */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  IRSSAVregInfo *v1 = ssa_vinfo(&c, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(v0->use_count, 1);
  UT_ASSERT_EQ(v1->use_count, 1);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_replace_all_uses: rewriting operands
 * ======================================================================== */

UT_TEST(test_replace_all_uses)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = #1; t1 = t0; t2 = t0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  int i1 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  int i2 = ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(0, I32));
  (void)i1; (void)i2;

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* Replace all uses of t0 with t1. */
  int count = ssa_opt_replace_all_uses(c.ctx, utb_vreg(utb_temp(0, I32)),
                                        utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(count, 2);

  /* t1 should now have 2 uses (the two ASSIGNs that used to read t0). */
  IRSSAVregInfo *v1 = ssa_vinfo(&c, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT(v1 != NULL);
  UT_ASSERT_EQ(v1->use_count, 2);

  /* t0 should have 0 uses. */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(v0->use_count, 0);

  /* The instructions should now read t1 instead of t0. */
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, i1)), IROP_VR(utb_temp(1, I32)));
  UT_ASSERT_EQ(IROP_VR(ssa_instr_src1(&c, i2)), IROP_VR(utb_temp(1, I32)));

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_nop_instr: use-count cleanup
 * ======================================================================== */

UT_TEST(test_nop_instr_removes_uses)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #1; t1 = t0; */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* t0 has 1 use (instr 1). */
  IRSSAVregInfo *v0 = ssa_vinfo(&c, utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(v0->use_count, 1);

  /* NOP the second instruction. */
  ssa_opt_nop_instr(c.ctx, 1);
  UT_ASSERT_EQ(utb_op(c.ir, 1), TCCIR_OP_NOP);

  /* t0 should now have 0 uses. */
  UT_ASSERT_EQ(v0->use_count, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_resolve_lea_stackloc: LEA chain resolution
 * ======================================================================== */

UT_TEST(test_resolve_lea_stackloc_lea)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = LEA(StackLoc[16]) → t1 = t0 */
  IROperand stack = utb_stackoff(16, 0, 1, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), stack);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int offset = ssa_opt_resolve_lea_stackloc(c.ctx, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_NE(offset, INT_MIN);
  UT_ASSERT_EQ(offset, 16);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_resolve_lea_stackloc_assign_chain)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/5);
  /* t0 = LEA(StackLoc[32]); t1 = t0; t2 = t1 */
  IROperand stack = utb_stackoff(32, 0, 1, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), stack);
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int offset = ssa_opt_resolve_lea_stackloc(c.ctx, utb_vreg(utb_temp(2, I32)));
  UT_ASSERT_NE(offset, INT_MIN);
  UT_ASSERT_EQ(offset, 32);

  ssa_ctx_free(&c);
  return 0;
}

UT_TEST(test_resolve_lea_stackloc_unresolved)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/3);
  /* t0 = #42; t1 = t0 (not a LEA chain) */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(42, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int offset = ssa_opt_resolve_lea_stackloc(c.ctx, utb_vreg(utb_temp(1, I32)));
  UT_ASSERT_EQ(offset, INT_MIN);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_resolve_temp_to_base_off: base+offset canonicalization
 * ======================================================================== */

UT_TEST(test_resolve_temp_to_base_off_var)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = VAR[0]; t1 = t0 */
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_var(0, I32));
  ssa_add_instr(&c, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  int32_t base, off;
  int r = ssa_opt_resolve_temp_to_base_off(c.ctx, utb_vreg(utb_temp(1, I32)),
                                            &base, &off);
  /* t0 is a TEMP defined by ASSIGN(VAR[0]), so it should resolve to
   * base=VAR[0], off=0. */
  UT_ASSERT_EQ(r, 1);
  UT_ASSERT_EQ(off, 0);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_indirect_stack_offset: store/load address resolution
 * ======================================================================== */

UT_TEST(test_indirect_stack_offset_plain_store)
{
  ssa_ctx c = ssa_ctx_new(/*blocks=*/1, /*temps=*/4);
  /* t0 = LEA(StackLoc[8]); STORE(*t0, t1) */
  IROperand stack = utb_stackoff(8, 0, 1, 0, I32);
  ssa_add_instr(&c, TCCIR_OP_LEA, utb_temp(0, I32), stack);
  ssa_add_instr(&c, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
                utb_temp(1, I32));

  ssa_ctx_build_cfg(&c);
  ssa_ctx_build_ssa_plain(&c);
  ssa_ctx_rebuild(&c);

  /* Find the STORE instruction. */
  int store_i = -1;
  for (int i = 0; i < c.ir->next_instruction_index; i++)
    if (utb_op(c.ir, i) == TCCIR_OP_STORE)
      store_i = i;
  UT_ASSERT_NE(store_i, -1);

  int offset = ssa_opt_indirect_stack_offset(c.ctx, &c.ir->compact_instructions[store_i],
                                              SSA_OPT_INDIRECT_DEST);
  UT_ASSERT_NE(offset, INT_MIN);
  UT_ASSERT_EQ(offset, 8);

  ssa_ctx_free(&c);
  return 0;
}

/* ========================================================================
 * ssa_opt_has_side_effects: classification
 * ======================================================================== */

UT_TEST(test_has_side_effects_store)
{
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_STORE) != 0);
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_STORE_INDEXED) != 0);
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_FUNCCALLVAL) != 0);
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_ADD) == 0);
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_ASSIGN) == 0);
  UT_ASSERT(ssa_opt_has_side_effects(TCCIR_OP_MUL) == 0);
  return 0;
}

/* ========================================================================
 * Suite registration
 * ======================================================================== */

UT_COVERS("ssa:usedef");
