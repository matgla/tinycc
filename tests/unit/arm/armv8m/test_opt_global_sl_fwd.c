/*
 *  test_opt_global_sl_fwd.c - suite for ir/opt_memory.c global store-load forwarding
 *
 *  Covers tcc_ir_opt_global_sl_fwd: within a single basic block, the value stored
 *  to a GlobalSym is forwarded into later lval (deref) reads of the same global.
 *
 *  Positive cases:
 *    - immediate stored value forwarded into a later arithmetic use
 *    - TEMP stored value forwarded into a later arithmetic use
 *    - LOAD of the stored global becomes an ASSIGN of the forwarded value
 *
 *  Guard cases:
 *    - a FUNCCALL between the store and the use clears tracking
 *    - a STORE through an unknown pointer clears all tracked entries
 *    - a jump target / BB boundary between store and use clears tracking
 *    - a STORE to a different global leaves the first entry intact
 *    - redefinition of the forwarded TEMP before the use drops that entry
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared to avoid pulling in
 * the optimizer engine headers). */
int tcc_ir_opt_global_sl_fwd(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Build an lval symref operand for a global symbol.  The pass only compares the
 * Sym pointer and addend, so a stack-allocated dummy Sym is sufficient. */
static IROperand utb_global(TCCIRState *ir, Sym *sym, int btype)
{
  return utb_symref(ir, sym, 1, 0, 0, btype);
}

/* POSITIVE: immediate store value is forwarded into a later ADD use.
 *   STORE GlobalSym(X) <- #7
 *   T0 = #3 ADD GlobalSym(X)   ->  T0 = #3 ADD #7
 */
UT_TEST(test_global_sl_fwd_imm_store_to_add)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT(changes > 0);
  IROperand s2 = utb_src2(ir, iadd);
  UT_ASSERT(irop_is_immediate(s2));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s2), 7);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: TEMP store value is forwarded into a later ADD use.
 *   STORE GlobalSym(X) <- T1
 *   T0 = #3 ADD GlobalSym(X)   ->  T0 = #3 ADD T1
 */
UT_TEST(test_global_sl_fwd_temp_store_to_add)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_temp(1, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT(changes > 0);
  IROperand s2 = utb_src2(ir, iadd);
  UT_ASSERT_EQ(irop_get_tag(s2), IROP_TAG_VREG);
  UT_ASSERT_EQ(utb_vreg(s2), TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, 1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a LOAD of the stored global becomes an ASSIGN of the value.
 *   STORE GlobalSym(X) <- #7
 *   T0 = LOAD GlobalSym(X)     ->  T0 = ASSIGN #7
 */
UT_TEST(test_global_sl_fwd_load_becomes_assign)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  int iload = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), gx, UTB_NONE);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, iload), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, iload);
  UT_ASSERT(irop_is_immediate(s1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 7);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a FUNCCALL between the store and the use clears all entries.
 *   STORE GlobalSym(X) <- #7
 *   FUNCCALLVOID ...
 *   T0 = #3 ADD GlobalSym(X)   -> unchanged
 */
UT_TEST(test_global_sl_fwd_call_clears_tracking)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src2(ir, iadd)), IROP_TAG_SYMREF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a STORE through an unknown pointer clears all tracked entries.
 *   STORE GlobalSym(X) <- #7
 *   STORE T2 <- #9
 *   T0 = #3 ADD GlobalSym(X)   -> unchanged
 */
UT_TEST(test_global_sl_fwd_unknown_store_clears_tracking)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_temp(2, I32), utb_imm(9, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src2(ir, iadd)), IROP_TAG_SYMREF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a jump target / BB boundary between store and use clears tracking.
 *   STORE GlobalSym(X) <- #7
 * L:
 *   NOP (jump target)
 *   T0 = #3 ADD GlobalSym(X)   -> unchanged
 */
UT_TEST(test_global_sl_fwd_jump_target_clears_tracking)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  int ilab = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[ilab].is_jump_target = 1;
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src2(ir, iadd)), IROP_TAG_SYMREF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: a STORE to a different global does not invalidate the first entry.
 *   STORE GlobalSym(X) <- #7
 *   STORE GlobalSym(Y) <- #9
 *   T0 = #3 ADD GlobalSym(X)   -> T0 = #3 ADD #7
 */
UT_TEST(test_global_sl_fwd_different_global_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x, sym_y;
  IROperand gx = utb_global(ir, &sym_x, I32);
  IROperand gy = utb_global(ir, &sym_y, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, gy, utb_imm(9, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT(changes > 0);
  IROperand s2 = utb_src2(ir, iadd);
  UT_ASSERT(irop_is_immediate(s2));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s2), 7);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

/* GUARD: redefinition of the forwarded TEMP before the use drops that entry.
 *   STORE GlobalSym(X) <- T1
 *   T1 = ASSIGN #9
 *   T0 = #3 ADD GlobalSym(X)   -> unchanged
 */
UT_TEST(test_global_sl_fwd_redef_of_forwarded_temp_drops_entry)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  Sym sym_x;
  IROperand gx = utb_global(ir, &sym_x, I32);

  utb_emit(ir, TCCIR_OP_STORE, gx, utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(9, I32), UTB_NONE);
  int iadd = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_imm(3, I32), gx);

  int changes = tcc_ir_opt_global_sl_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(irop_get_tag(utb_src2(ir, iadd)), IROP_TAG_SYMREF);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 8), 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("global_sl_fwd");
