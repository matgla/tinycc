/*
 *  test_opt_memory_extra.c - suite for the remaining ir/opt_memory.c passes not
 *  already covered by test_opt_memory.c (sl_forward guard cases) or
 *  test_opt_store_fwd.c (entry_store, byte_store_merge, store_redundant,
 *  dead_static_store, dead_local_slot, dead_temp_local, global_base_share) or
 *  test_opt_global_sl_fwd.c (global_sl_fwd).  (deref_fwd was retired 2026-07-14 —
 *  subsumed by ssa:load_cse.)
 *
 *  This file adds coverage for the remaining bare `int tcc_ir_opt_<name>
 *  (TCCIRState *ir)` entries in ir/opt_memory.c that are called directly from
 *  tccgen.c's IR-generation driver rather than through a PASS_GATED pipeline
 *  entry (so they are NOT in check_pass_coverage.py's ledger -- see
 *  docs/plan_ut_next_steps.md S1 for the "call the legacy entry directly"
 *  contract this still follows):
 *
 *    - addrof_var_fwd            (tcc_ir_opt_addrof_var_fwd)
 *    - invariant_global_load_hoist (tcc_ir_opt_invariant_global_load_hoist)
 *    - invariant_temp_deref_hoist  (tcc_ir_opt_invariant_temp_deref_hoist)
 *    - rmw_byte_clear            (tcc_ir_opt_rmw_byte_clear)
 *    - local_copy_prop           (tcc_ir_opt_local_copy_prop)
 *    - struct_copy_roundtrip_elim (tcc_ir_opt_struct_copy_roundtrip_elim)
 *    - const_memcpy_to_dest      (tcc_ir_opt_const_memcpy_to_dest)
 *
 *  Each gets at least one positive case (the transform fires, asserted with an
 *  oracle fact about the resulting IR) and one negative/guard case (a
 *  legitimate reason the transform must NOT fire).
 *
 *  NOT covered here (documented gaps, not fixed):
 *   - "diamond_store_fwd" and "memmove_global_load_fwd" are also bare,
 *     uncovered entries in this file, but each needs a much larger hand-built
 *     CFG/def-chain shape (a multi-BB diamond for diamond_store_fwd; a
 *     private-snapshot-buffer alias proof for memmove_global_load_fwd) than
 *     the mechanically-clear cases below -- left for a follow-up pass given
 *     this session's breadth-first mandate.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (defined in ir/opt_memory.c; forward-declared here to
 * avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_addrof_var_fwd(TCCIRState *ir);
int tcc_ir_opt_invariant_global_load_hoist(TCCIRState *ir);
int tcc_ir_opt_invariant_temp_deref_hoist(TCCIRState *ir);
int tcc_ir_opt_rmw_byte_clear(TCCIRState *ir);
int tcc_ir_opt_local_copy_prop(TCCIRState *ir);
int tcc_ir_opt_struct_copy_roundtrip_elim(TCCIRState *ir);
int tcc_ir_opt_const_memcpy_to_dest(TCCIRState *ir);
int tcc_ir_opt_deref_operand_cse(TCCIRState *ir);

#define I8  IROP_BTYPE_INT8
#define I32 IROP_BTYPE_INT32

#define TOK_MEMCPY4 70
#define TOK_FOO 71

/* ------------------------------------------------------------------ helpers */

static IROperand utb_slot_lval(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

static IROperand utb_slot_addr(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

static IROperand utb_deref_temp(int pos, int btype)
{
  return utb_lval(utb_temp(pos, btype));
}

/* A deref whose access is proven non-volatile.  deref_operand_cse declines
 * anything without the mark, so every positive case must carry it. */
static IROperand utb_deref_temp_nv(int pos, int btype)
{
  IROperand op = utb_lval(utb_temp(pos, btype));
  op.aux |= IROP_AUX_NONVOLATILE;
  return op;
}

static IROperand utb_var_lval(int pos, int btype)
{
  return utb_lval(utb_var(pos, btype));
}

static IROperand utb_callee(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* tcc_ir_opt_invariant_temp_deref_hoist inserts a new ASSIGN (tcc_ir_pool_add
 * / gsym_cse_insert_before) and allocates a fresh TEMP vreg
 * (tcc_ir_vreg_alloc_temp). utb_new() leaves iroperand_pool_capacity,
 * temporary_variables_live_intervals_size and compact_instructions_size at 0;
 * growing any of those from 0 either hangs or silently keeps the backing
 * array zero-sized. Pre-allocate generously, matching test_opt_fusion.c's
 * utb_fusion_new() pattern. */
static TCCIRState *utb_hoist_new(int manual_temp_count)
{
  TCCIRState *ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = manual_temp_count;
  ir->compact_instructions_size = UTB_MAX_INSTR;
  return ir;
}

/* Emit a 3-arg FUNCPARAMVAL x3 + FUNCCALLVOID call; returns the call index. */
static int emit_call3(TCCIRState *ir, IROperand callee, int call_id,
                      IROperand p0, IROperand p1, IROperand p2)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p0,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p1,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, p2,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
}

/* ================================================================ addrof_var_fwd */

/* POSITIVE: V0 <- #77 [ASSIGN]; T0 = LEA &V0; T1 = T0***DEREF*** ADD #1 -- the
 * deref of T0 (which holds &V0) must resolve to the known constant #77. */
UT_TEST(test_addrof_var_fwd_lea_deref_resolves_to_constant)
{
  TCCIRState *ir = utb_new();

  int vset = utb_emit(ir, TCCIR_OP_ASSIGN, utb_var_lval(0, I32), utb_imm(77, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_addrof_var_fwd(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, vset), TCCIR_OP_ASSIGN); /* the original write is untouched */
  IROperand s1 = utb_src1(ir, use);
  UT_ASSERT(irop_is_immediate(s1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 77);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): V0 is redefined between the LEA and the deref use, so the
 * tracked alias must be invalidated -- the deref use is left untouched. */
UT_TEST(test_addrof_var_fwd_redefinition_blocks_forward)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var_lval(0, I32), utb_imm(77, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_var(0, I32), UTB_NONE);
  /* V0 is written again (e.g. by some other statement) before the deref. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var_lval(0, I32), utb_imm(99, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_addrof_var_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s1 = utb_src1(ir, use);
  UT_ASSERT(s1.is_lval);
  UT_ASSERT_EQ(utb_vreg(s1), utb_vreg(utb_temp(0, I32)));

  utb_free(ir);
  return 0;
}

/* ================================================================ invariant_global_load_hoist */

/* POSITIVE: the same non-volatile, never-directly-stored global is loaded
 * twice with only a pure ALU op between -- the second load is replaced by an
 * ASSIGN from the first load's result temp. */
UT_TEST(test_invariant_global_load_hoist_second_load_becomes_assign)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));
  sym_g.type.t = I32;

  uint32_t sidx1 = tcc_ir_pool_add_symref(ir, &sym_g, 0, 0);
  IROperand g1 = irop_make_symref(0, sidx1, 1, 0, 0, I32);
  uint32_t sidx2 = tcc_ir_pool_add_symref(ir, &sym_g, 0, 0);
  IROperand g2 = irop_make_symref(0, sidx2, 1, 0, 0, I32);

  int load1 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), g1, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int load2 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), g2, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_invariant_global_load_hoist(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load1), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, load2);
  UT_ASSERT_EQ(utb_vreg(s1), utb_vreg(utb_temp(0, I32)));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the global is directly STORE-written somewhere in the
 * function, so the two loads may observe different values -- no hoist. */
UT_TEST(test_invariant_global_load_hoist_written_global_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));
  sym_g.type.t = I32;

  uint32_t sidx1 = tcc_ir_pool_add_symref(ir, &sym_g, 0, 0);
  IROperand g1 = irop_make_symref(0, sidx1, 1, 0, 0, I32);
  uint32_t sidx2 = tcc_ir_pool_add_symref(ir, &sym_g, 0, 0);
  IROperand g2 = irop_make_symref(0, sidx2, 1, 0, 0, I32);
  uint32_t sidx3 = tcc_ir_pool_add_symref(ir, &sym_g, 0, 0);
  IROperand g3 = irop_make_symref(0, sidx3, 1, 0, 0, I32);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), g1, UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, g3, utb_imm(9, I32), UTB_NONE);
  int load2 = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), g2, UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_invariant_global_load_hoist(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load2), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* ================================================================ invariant_temp_deref_hoist */

/* POSITIVE: T0 is defined by a LOAD from a stack slot (a "loaded pointer") and
 * dereferenced twice with only a pure ALU op between the two derefs and no
 * clobber -- a fresh hoisted TEMP is inserted right after T0's def, and BOTH
 * deref uses are rewritten to read it instead of re-dereferencing T0.
 * (The insertion shifts every instruction from T0's def onward by one slot,
 * so this test locates use1/use2 post-pass by their distinguishing dest
 * vreg (T1/T2) rather than by a pre-pass instruction index.) */
UT_TEST(test_invariant_temp_deref_hoist_two_derefs_hoisted)
{
  TCCIRState *ir = utb_hoist_new(3); /* T0..T2 used by hand below */

  int tdef = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_slot_lval(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_invariant_temp_deref_hoist(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, tdef), TCCIR_OP_LOAD); /* the original def is untouched */

  /* Locate the two ADDs post-pass by their (unaffected) dest vreg. */
  int use1 = -1, use2 = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ADD)
      continue;
    int32_t dv = utb_vreg(utb_dest(ir, i));
    if (dv == utb_vreg(utb_temp(1, I32)))
      use1 = i;
    else if (dv == utb_vreg(utb_temp(2, I32)))
      use2 = i;
  }
  UT_ASSERT(use1 >= 0);
  UT_ASSERT(use2 >= 0);

  /* Oracle: neither ADD still dereferences T0 (vreg 0); both read the same
   * non-lval hoisted TEMP instead. */
  IROperand s1_use1 = utb_src1(ir, use1);
  IROperand s1_use2 = utb_src1(ir, use2);
  UT_ASSERT(!s1_use1.is_lval || utb_vreg(s1_use1) != utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(!s1_use2.is_lval || utb_vreg(s1_use2) != utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_vreg(s1_use1), utb_vreg(s1_use2));

  utb_free(ir);
  return 0;
}

UT_TEST(test_invariant_temp_deref_hoist_copy_chain_hoisted)
{
  TCCIRState *ir = utb_hoist_new(5); /* T0..T4 used by hand below */

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_slot_lval(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(3, I32), utb_deref_temp(1, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_deref_temp(2, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(4, I32), UTB_NONE);

  int changes = tcc_ir_opt_invariant_temp_deref_hoist(ir);

  UT_ASSERT(changes > 0);

  int use1 = -1, use2 = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ADD)
      continue;
    int32_t dv = utb_vreg(utb_dest(ir, i));
    if (dv == utb_vreg(utb_temp(3, I32)))
      use1 = i;
    else if (dv == utb_vreg(utb_temp(4, I32)))
      use2 = i;
  }
  UT_ASSERT(use1 >= 0);
  UT_ASSERT(use2 >= 0);

  IROperand s1_use1 = utb_src1(ir, use1);
  IROperand s1_use2 = utb_src1(ir, use2);
  UT_ASSERT(!s1_use1.is_lval);
  UT_ASSERT(!s1_use2.is_lval);
  UT_ASSERT_EQ(utb_vreg(s1_use1), utb_vreg(s1_use2));
  UT_ASSERT(utb_vreg(s1_use1) != utb_vreg(utb_temp(1, I32)));
  UT_ASSERT(utb_vreg(s1_use2) != utb_vreg(utb_temp(2, I32)));

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a FUNCCALLVOID sits between the two derefs -- the callee
 * may write through T0's address, so no hoist may happen across it. */
UT_TEST(test_invariant_temp_deref_hoist_intervening_call_blocks)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir); /* iroperand_pool starts small but tcc_ir_pool_ensure
                        * grows it via realloc, since capacity is nonzero. */
  ir->temporary_variables_live_intervals_size = 64;
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * 64);
  ir->next_temporary_variable = 3; /* T0..T2 used by hand below */
  ir->compact_instructions_size = UTB_MAX_INSTR;

  static Sym callee_sym;
  utb_set_tok_str(TOK_FOO, "foo");
  IROperand callee = utb_callee(ir, &callee_sym, TOK_FOO);

  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_slot_lval(-8, I32), UTB_NONE);
  int use1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee, utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int use2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_invariant_temp_deref_hoist(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, use1)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, use2)), utb_vreg(utb_temp(0, I32)));
  UT_ASSERT(utb_src1(ir, use1).is_lval);
  UT_ASSERT(utb_src1(ir, use2).is_lval);

  utb_free(ir);
  return 0;
}

/* ================================================================ deref_operand_cse */

/* Returns the vreg both reads were rewritten onto, or -1 if either still
 * dereferences the base.  The insert shifts positions, so the two ADDs are
 * located by their dest vreg. */
static int32_t docse_shared_read_vreg(TCCIRState *ir, int dest_a, int dest_b)
{
  int a = -1, b = -1;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    if (utb_op(ir, i) != TCCIR_OP_ADD)
      continue;
    int32_t dv = utb_vreg(utb_dest(ir, i));
    if (dv == utb_vreg(utb_temp(dest_a, I32)))
      a = i;
    else if (dv == utb_vreg(utb_temp(dest_b, I32)))
      b = i;
  }
  if (a < 0 || b < 0)
    return -1;
  IROperand sa = utb_src1(ir, a);
  IROperand sb = utb_src1(ir, b);
  if (sa.is_lval || sb.is_lval || utb_vreg(sa) != utb_vreg(sb))
    return -1;
  return utb_vreg(sa);
}

/* POSITIVE: the same word is read through T0 twice with only a pure ALU op
 * between -- one inserted ASSIGN loads it, both reads become that TEMP. */
UT_TEST(test_deref_operand_cse_two_reads_share_one_load)
{
  TCCIRState *ir = utb_hoist_new(3); /* T0..T2 used by hand below */

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp_nv(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp_nv(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_deref_operand_cse(ir);

  UT_ASSERT(changes > 0);
  int32_t shared = docse_shared_read_vreg(ir, 1, 2);
  UT_ASSERT(shared >= 0);
  UT_ASSERT(shared != utb_vreg(utb_temp(0, I32)));

  /* The inserted ASSIGN is the one and only remaining dereference of T0. */
  int derefs = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IROperand s1 = utb_src1(ir, i);
    if (s1.is_lval && utb_vreg(s1) == utb_vreg(utb_temp(0, I32)))
      derefs++;
  }
  UT_ASSERT_EQ(derefs, 1);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a conditional jump between the two reads does NOT end the region.
 * The second read is only reached by falling through, so the value loaded at
 * the first (dominating) read is still the right one -- this is the shape of
 * `a[i].f == C || a[i].f > x`, mibench_dijkstra's inner loop. */
UT_TEST(test_deref_operand_cse_spans_conditional_jump)
{
  TCCIRState *ir = utb_hoist_new(4); /* T0..T3 used by hand below */

  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp_nv(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(4, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp_nv(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(3, I32), UTB_NONE);

  int changes = tcc_ir_opt_deref_operand_cse(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT(docse_shared_read_vreg(ir, 1, 2) >= 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the reads are not marked non-volatile.  Nothing in the IR
 * can tell a volatile deref from a plain one, so an unmarked operand must be
 * left alone -- two volatile reads are two reads. */
UT_TEST(test_deref_operand_cse_unmarked_read_is_left_alone)
{
  TCCIRState *ir = utb_hoist_new(3);

  int use1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp(0, I32), utb_imm(1, I32));
  int use2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_deref_operand_cse(ir), 0);
  UT_ASSERT(utb_src1(ir, use1).is_lval);
  UT_ASSERT(utb_src1(ir, use2).is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a store to a *stack local* sits between the two reads.
 * The GlobalSym pass treats that as harmless, and here it is not: the address
 * in T0 is opaque and may well be that local's. */
UT_TEST(test_deref_operand_cse_store_to_local_blocks)
{
  TCCIRState *ir = utb_hoist_new(3);

  int use1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp_nv(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(7, I32), UTB_NONE);
  int use2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp_nv(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_deref_operand_cse(ir), 0);
  UT_ASSERT(utb_src1(ir, use1).is_lval);
  UT_ASSERT(utb_src1(ir, use2).is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a call between the reads may write through T0. */
UT_TEST(test_deref_operand_cse_intervening_call_blocks)
{
  TCCIRState *ir = utb_hoist_new(3);

  static Sym docse_callee_sym;
  utb_set_tok_str(TOK_FOO, "foo");
  IROperand callee = utb_callee(ir, &docse_callee_sym, TOK_FOO);

  int use1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp_nv(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee, utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int use2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp_nv(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_deref_operand_cse(ir), 0);
  UT_ASSERT(utb_src1(ir, use1).is_lval);
  UT_ASSERT(utb_src1(ir, use2).is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): T0 is given a new address between the reads.  The IR is
 * de-SSA'd where this pass runs, so this is reachable, and the two reads are
 * of different words. */
UT_TEST(test_deref_operand_cse_base_redefinition_blocks)
{
  TCCIRState *ir = utb_hoist_new(4);

  int use1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_deref_temp_nv(0, I32), utb_imm(1, I32));
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), utb_temp(0, I32), utb_imm(4, I32));
  int use2 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_deref_temp_nv(0, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_deref_operand_cse(ir), 0);
  UT_ASSERT(utb_src1(ir, use1).is_lval);
  UT_ASSERT(utb_src1(ir, use2).is_lval);

  utb_free(ir);
  return 0;
}

/* ================================================================ rmw_byte_clear */

/* POSITIVE (no add-fold): T = P***DEREF*** AND 0xFFFFFF00; STORE P***DEREF***
 * <- T (T single-use, same block) -- becomes a plain byte-0 store, and the
 * AND is NOPed. */
UT_TEST(test_rmw_byte_clear_and_store_becomes_byte_store)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-8, I32), UTB_NONE);
  int and_i = utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_deref_temp(0, I32),
                       utb_imm((int32_t)0xFFFFFF00u, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_rmw_byte_clear(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, and_i), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_dest(ir, store).btype, I8);
  IROperand new_src = utb_src1(ir, store);
  UT_ASSERT(irop_is_immediate(new_src));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, new_src), 0);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the AND result (T1) has a second use besides the STORE,
 * so tcc_ir_vreg_has_single_use fails and the fold must not fire. */
UT_TEST(test_rmw_byte_clear_multi_use_and_result_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-8, I32), UTB_NONE);
  int and_i = utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_deref_temp(0, I32),
                       utb_imm((int32_t)0xFFFFFF00u, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_deref_temp(0, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE); /* extra use */

  int changes = tcc_ir_opt_rmw_byte_clear(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, and_i), TCCIR_OP_AND);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, store)), utb_vreg(utb_temp(1, I32)));

  utb_free(ir);
  return 0;
}

/* ================================================================ local_copy_prop */

/* POSITIVE: 4 consecutive LOAD(A[k*4])+STORE(B[k*4]) pairs (A at 100, B at
 * 200, stride 4) with nothing else touching A -- the pass redirects the
 * writes from A to B and NOPs the whole copy chain. */
UT_TEST(test_local_copy_prop_four_pairs_redirect_writes)
{
  TCCIRState *ir = utb_new();

  int idx[8];
  int k = 0;
  for (int i = 0; i < 4; i++)
  {
    idx[k++] = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(i, I32), utb_slot_lval(100 + i * 4, I32), UTB_NONE);
    idx[k++] = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(200 + i * 4, I32), utb_temp(i, I32), UTB_NONE);
  }
  /* B[0] is read afterwards (the copy's destination is actually used); A is
   * never referenced again outside the chain itself. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_slot_lval(200, I32), UTB_NONE);

  int changes = tcc_ir_opt_local_copy_prop(ir);

  UT_ASSERT_EQ(changes, 4);
  for (int i = 0; i < 8; i++)
    UT_ASSERT_EQ(utb_op(ir, idx[i]), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): only 3 consecutive pairs (below the count>=4 threshold)
 * -- the chain is left completely untouched. */
UT_TEST(test_local_copy_prop_three_pairs_kept)
{
  TCCIRState *ir = utb_new();

  int idx[6];
  int k = 0;
  for (int i = 0; i < 3; i++)
  {
    idx[k++] = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(i, I32), utb_slot_lval(100 + i * 4, I32), UTB_NONE);
    idx[k++] = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(200 + i * 4, I32), utb_temp(i, I32), UTB_NONE);
  }
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_local_copy_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  for (int i = 0; i < 6; i++)
    UT_ASSERT_EQ(utb_op(ir, idx[i]), (i % 2 == 0) ? TCCIR_OP_LOAD : TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* ================================================================ struct_copy_roundtrip_elim */

/* POSITIVE: memcpy(&B, &A, N) immediately followed (straight-line, no writes
 * in between) by memcpy(&A, &B, N), with B referenced nowhere else -- both
 * calls (A:=B:=A round trip through the private buffer B) are dead. */
UT_TEST(test_struct_copy_roundtrip_elim_removes_both_calls)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  IROperand callee1 = utb_callee(ir, &mc, TOK_MEMCPY4);

  /* C1: B(200) := A(100), 16 bytes. */
  int c1 = emit_call3(ir, callee1, 1, utb_slot_addr(200, I32), utb_slot_addr(100, I32), utb_imm(16, I32));

  static Sym mc2;
  IROperand callee2 = utb_callee(ir, &mc2, TOK_MEMCPY4);
  /* C2: A(100) := B(200), 16 bytes -- exact reverse copy, same size. */
  int c2 = emit_call3(ir, callee2, 2, utb_slot_addr(100, I32), utb_slot_addr(200, I32), utb_imm(16, I32));

  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_struct_copy_roundtrip_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a STORE into region A between the two copies invalidates
 * the "A unchanged across the round trip" premise -- neither call is removed. */
UT_TEST(test_struct_copy_roundtrip_elim_intervening_store_blocks)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  IROperand callee1 = utb_callee(ir, &mc, TOK_MEMCPY4);

  int c1 = emit_call3(ir, callee1, 1, utb_slot_addr(200, I32), utb_slot_addr(100, I32), utb_imm(16, I32));
  /* An unrelated store sits between the two calls -- disqualifying. */
  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(300, I32), utb_imm(1, I32), UTB_NONE);

  static Sym mc2;
  IROperand callee2 = utb_callee(ir, &mc2, TOK_MEMCPY4);
  int c2 = emit_call3(ir, callee2, 2, utb_slot_addr(100, I32), utb_slot_addr(200, I32), utb_imm(16, I32));

  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_struct_copy_roundtrip_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, c1), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, c2), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* ================================================================ const_memcpy_to_dest */

/* POSITIVE: a stack buffer at offset 100 is fully established by two constant
 * INT32 stores (8 bytes), then __aeabi_memcpy4(dst_param, &buf, 8) copies it
 * to a PARAM-typed pointer with a dead return value -- the call is folded away
 * into STORE_INDEXED writes through dst_param carrying the exact fill bytes.
 * (The pass places the new STORE_INDEXEDs into the LAST n_desc of the
 * available fill/param/call slots, in ascending index order -- with 2 fills +
 * 3 params + 1 call = 6 slots and n_desc=2 descriptors, the call itself ends
 * up rewritten to a STORE_INDEXED rather than NOPed, so this asserts the
 * byte-preservation oracle directly instead of assuming which instruction
 * slot the rewrite lands on.) */
UT_TEST(test_const_memcpy_to_dest_folds_call_away)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY4);

  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(100, I32), utb_imm(0x11111111, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(104, I32), utb_imm(0x22222222, I32), UTB_NONE);

  emit_call3(ir, callee, 1, utb_param(0, I32), utb_slot_addr(100, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_const_memcpy_to_dest(ir);

  UT_ASSERT_EQ(changes, 1);

  /* Oracle: no call to the memcpy callee survives, and the two expected
   * 32-bit words are each written exactly once through dst_param (PARAM0),
   * at relative offsets 0 and 4. */
  int calls_left = 0;
  int found_word0 = 0, found_word4 = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    TccIrOp op = utb_op(ir, i);
    if (op == TCCIR_OP_FUNCCALLVOID || op == TCCIR_OP_FUNCCALLVAL)
      calls_left++;
    if (op != TCCIR_OP_STORE_INDEXED)
      continue;
    IROperand base = utb_dest(ir, i);
    if (utb_vreg(base) != utb_vreg(utb_param(0, I32)))
      continue;
    IROperand idx = utb_src2(ir, i);
    IROperand val = utb_src1(ir, i);
    UT_ASSERT(irop_is_immediate(idx));
    UT_ASSERT(irop_is_immediate(val));
    int64_t rel = irop_get_imm64_ex(ir, idx);
    if (rel == 0)
    {
      UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, val), (uint32_t)0x11111111);
      found_word0 = 1;
    }
    else if (rel == 4)
    {
      UT_ASSERT_EQ((uint32_t)irop_get_imm64_ex(ir, val), (uint32_t)0x22222222);
      found_word4 = 1;
    }
  }
  UT_ASSERT_EQ(calls_left, 0);
  UT_ASSERT(found_word0);
  UT_ASSERT(found_word4);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the source buffer is only PARTIALLY established by
 * constant stores (one of the two words is missing) -- coverage is
 * incomplete, so the call must survive untouched. */
UT_TEST(test_const_memcpy_to_dest_incomplete_coverage_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY4);

  /* Only the first word of the 8-byte source is a constant fill. */
  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(100, I32), utb_imm(0x11111111, I32), UTB_NONE);

  int icall = emit_call3(ir, callee, 1, utb_param(0, I32), utb_slot_addr(100, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_const_memcpy_to_dest(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}
