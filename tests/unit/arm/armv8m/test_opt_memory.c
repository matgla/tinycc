/*
 *  test_opt_memory.c - suite for ir/opt_memory.c
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_sl_forward(TCCIRState *ir);
int tcc_ir_opt_memmove_global_load_fwd(TCCIRState *ir);
int tcc_ir_opt_diamond_store_fwd(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define VR_VAR(p) TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, (p))

static IROperand slot_lval(int32_t off)
{
  return irop_make_stackoff(0, off, 1, 0, 0, I32);
}

static IROperand slot_addr(int32_t off)
{
  return irop_make_stackoff(0, off, 0, 0, 0, I32);
}

static IROperand var_slot_lval(int pos, int32_t off)
{
  return irop_make_stackoff(VR_VAR(pos), off, 1, 0, 0, I32);
}

static void utb_alloc_var_intervals(TCCIRState *ir, int count)
{
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->variables_live_intervals_size = count;
}

static void utb_alloc_tmp_intervals(TCCIRState *ir, int count)
{
  ir->temporary_variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * count);
  ir->temporary_variables_live_intervals_size = count;
  ir->next_temporary_variable = count - 1;
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

#define TOK_MEMCPY4 70

/* GUARD (STORE_LVAL VAR namespace): anonymous StackLoc offsets and VAR-backed
 * stack slots can have the same raw offset.  STORE_LVAL forwarding must not
 * replace a read from VAR0's slot with an unrelated anonymous StackLoc value. */
UT_TEST(test_sl_forward_store_lval_var_slot_does_not_alias_stackloc)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_var_intervals(ir, 1);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-88), utb_imm(123, I32), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, slot_lval(-200), var_slot_lval(0, -88), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, store)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, store)), VR_VAR(0));
  UT_ASSERT(utb_src1(ir, store).is_lval);

  utb_free(ir);
  return 0;
}

/* GUARD (FORWARD-HI): a STORE whose source TEMP has stale 64-bit metadata but
 * resolves to a forwarded 32-bit constant must not become an 8-byte store. */
UT_TEST(test_sl_forward_resolves_temp_before_store_width)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);
  ir->temporary_variables_live_intervals[0].is_llong = 1;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(-2385, I32), UTB_NONE);
  IROperand noisy_dest = slot_lval(-56);
  noisy_dest.btype = IROP_BTYPE_INT64;
  utb_emit(ir, TCCIR_OP_STORE, noisy_dest, utb_temp(0, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot_lval(-52), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, load)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(utb_src1(ir, load)), -52);

  utb_free(ir);
  return 0;
}

/* GUARD (LEA-deref store width): a pointer-derived 32-bit store must not be
 * widened by stale/wider source metadata and then forwarded as the low half of
 * an adjacent 64-bit store. */
UT_TEST(test_sl_forward_pointer_store_keeps_deref_width_for_high_half)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 3);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), slot_addr(-56), UTB_NONE);
  uint32_t wide_idx = tcc_ir_pool_add_i64(ir, 0);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)),
           irop_make_i64(-1, wide_idx, IROP_BTYPE_INT64), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(4, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_lval(utb_temp(1, I32)), UTB_NONE);

  (void)tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_vreg_pos(utb_src1(ir, load)), 1);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a plain constant store to an anonymous stack slot is forwarded into
 * a later LOAD from the same slot.  The LOAD becomes an ASSIGN of the constant,
 * and the now-dead store is eliminated. */
UT_TEST(test_sl_forward_basic_imm_store_load)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int store = utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(42, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, load)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, load)), 42);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* POSITIVE: forwarding works when the store and load are mediated by a LEA'd
 * pointer (T0 = &StackLoc[-8]; STORE T0***DEREF*** <- #7; LOAD T1 <- T0***DEREF***). */
UT_TEST(test_sl_forward_lea_pointer_store_load)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), slot_addr(-8), UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(7, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, load)), 7);
  (void)store; /* pointer-based store is forwarded but not DSE'd in this path */

  utb_free(ir);
  return 0;
}

/* POSITIVE: a tracked constant store is forwarded into a later CMP's lval
 * operand, eliminating the implicit memory read. */
UT_TEST(test_sl_forward_cmp_lval_operand_forward)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(5, I32), UTB_NONE);
  int cmp = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(0, I32), slot_lval(-8));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  IROperand s2 = utb_src2(ir, cmp);
  UT_ASSERT(irop_is_immediate(s2));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s2), 5);

  utb_free(ir);
  return 0;
}

/* GUARD: a STORE through an unresolved pointer (TEMP not in the LEA map)
 * conservatively clears all tracked stores, so a later LOAD from a known slot
 * is NOT forwarded. */
UT_TEST(test_sl_forward_unknown_pointer_store_clears_tracking)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(9, I32), UTB_NONE);
  /* Unknown pointer: a TEMP that was never LEA-mapped. */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(1, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, load)), IROP_TAG_STACKOFF);

  utb_free(ir);
  return 0;
}

/* GUARD: taking the address of a stack slot (LEA) and then calling a function
 * invalidates a tracked store to that slot, because the callee may write
 * through the escaped pointer. */
UT_TEST(test_sl_forward_call_invalidates_addrtaken_store)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(6, I32), UTB_NONE);
  /* LEA exposes the address; subsequent CALL may mutate the slot. */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), slot_addr(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* GUARD: a JUMP to the LOAD's basic block means the LOAD can be reached without
 * executing the STORE, so forwarding must not happen. */
UT_TEST(test_sl_forward_jump_target_blocks_forward)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(3, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(-8), UTB_NONE);
  ir->compact_instructions[load].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a STORE_INDEXED through a LEA-mapped base is tracked like a plain
 * STORE, so a later direct LOAD from the resolved offset forwards the value. */
UT_TEST(test_sl_forward_store_indexed_via_lea)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 2);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), slot_addr(-8), UTB_NONE);
  int si = utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_imm(99, I32), utb_imm(0, I32), utb_imm(0, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, load)), 99);
  (void)si;

  utb_free(ir);
  return 0;
}

/* POSITIVE: a call to a known-pure AEABI helper does not invalidate tracked
 * stores to non-addrtaken stack slots, so the later LOAD still forwards. */
UT_TEST(test_sl_forward_pure_aeabi_call_keeps_tracking)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym callee_sym;
  utb_set_tok_str(70, "__aeabi_i2f");
  uint32_t sidx = tcc_ir_pool_add_symref(ir, &callee_sym, 0, 0);
  IROperand callee = irop_make_symref(0, sidx, 0, 0, 0, I32);

  int store = utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(55, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee, utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 0), I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, load)), 55);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a store in the first basic block can forward through a fall-through
 * edge into a second basic block (single predecessor, no jump target in between). */
UT_TEST(test_sl_forward_cross_bb_fallthrough)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  int store = utb_emit(ir, TCCIR_OP_STORE, slot_lval(-8), utb_imm(11, I32), UTB_NONE);
  int target = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[target].is_jump_target = 1;
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(-8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_sl_forward(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, load)), 11);
  (void)store;

  utb_free(ir);
  return 0;
}

/* ========================================================= memmove_global_load_fwd */

/* POSITIVE: __aeabi_memcpy4 from a global into a local stack slot, followed by a
 * direct LOAD from that slot, is forwarded so the load reads the global directly
 * and the memcpy call is removed. */
UT_TEST(test_memmove_global_load_fwd_direct_local_load)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym g;
  memset(&g, 0, sizeof(g));
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  g.v = TOK_MEMCPY4;
  IROperand callee = utb_symref(ir, &g, 0, 0, 0, I32);
  IROperand g_src = utb_symref(ir, &g, 0, 0, 0, I32);

  /* Real codegen materializes the copy destination &y as an LEA temp before
   * passing it to the memcpy; that LEA is what seeds the pass's dest-slot
   * worklist.  The field read stays a direct StackLoc load (the case this
   * test targets). */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), slot_addr(100), UTB_NONE);
  int call = emit_call3(ir, callee, 1,
                        utb_temp(1, I32), g_src, utb_imm(4, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(100), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_memmove_global_load_fwd(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, load)), IROP_TAG_SYMREF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a STORE into the destination slot between the memcpy and the
 * load means the slot's value is no longer the global copy, so forwarding must not
 * happen. */
UT_TEST(test_memmove_global_load_fwd_intervening_store_blocks)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym g;
  memset(&g, 0, sizeof(g));
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  g.v = TOK_MEMCPY4;
  IROperand callee = utb_symref(ir, &g, 0, 0, 0, I32);
  IROperand g_src = utb_symref(ir, &g, 0, 0, 0, I32);

  int call = emit_call3(ir, callee, 1,
                        slot_addr(100), g_src, utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, slot_lval(100), utb_imm(0xAA, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(100), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_memmove_global_load_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(irop_get_tag(utb_src1(ir, load)), IROP_TAG_STACKOFF);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a non-global source operand means the copy cannot be folded
 * into a direct global reference, so the call and load survive. */
UT_TEST(test_memmove_global_load_fwd_non_global_src_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  static Sym g;
  memset(&g, 0, sizeof(g));
  utb_set_tok_str(TOK_MEMCPY4, "__aeabi_memcpy4");
  g.v = TOK_MEMCPY4;
  IROperand callee = utb_symref(ir, &g, 0, 0, 0, I32);

  int call = emit_call3(ir, callee, 1,
                        slot_addr(100), slot_addr(200), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_lval(100), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_memmove_global_load_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);

  utb_free(ir);
  return 0;
}

/* ========================================================= diamond_store_fwd */

/* POSITIVE: both arms of an if/else diamond store the same constant to the same
 * array element (base + (idx << 2)); the post-merge LOAD_INDEXED of that element
 * is folded into an ASSIGN of the constant. */
UT_TEST(test_diamond_store_fwd_both_arms_same_const)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 8);

  int Lelse = 9;
  int Lmerge = 14;

  /* 0: cond = #1 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  /* 1: base = LEA &StackLoc[200] */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), slot_addr(200), UTB_NONE);
  /* 2: idx = #0 */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32), UTB_NONE);
  /* 3: shifted = idx << 2 */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(3, I32), utb_temp(2, I32), utb_imm(2, I32));
  /* 4: JUMPIF Lelse */
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(Lelse, I32), utb_temp(0, I32), UTB_NONE);
  /* 5: addr_then = base + shifted */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(1, I32), utb_temp(3, I32));
  /* 6: STORE addr_then***DEREF*** <- #42 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(4, I32)), utb_imm(42, I32), UTB_NONE);
  /* 7: JUMP Lmerge */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  /* 8: padding NOP */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  /* 9: Lelse */
  int else_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[else_label].is_jump_target = 1;
  /* 10: addr_else = base + shifted */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(1, I32), utb_temp(3, I32));
  /* 11: STORE addr_else***DEREF*** <- #42 */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(5, I32)), utb_imm(42, I32), UTB_NONE);
  /* 12: JUMP Lmerge */
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  /* 13: padding NOP */
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  /* 14: Lmerge */
  int merge_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[merge_label].is_jump_target = 1;
  /* 15: load = LOAD_INDEXED(base, idx, #2) */
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(6, I32), utb_temp(1, I32), utb_temp(2, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32), UTB_NONE);

  int changes = tcc_ir_opt_diamond_store_fwd(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_ASSIGN);
  IROperand s1 = utb_src1(ir, load);
  UT_ASSERT(irop_is_immediate(s1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s1), 42);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the two branches store different constants, so the merge
 * LOAD_INDEXED cannot be resolved to a single value. */
UT_TEST(test_diamond_store_fwd_different_const_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 8);

  int Lelse = 9;
  int Lmerge = 14;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), slot_addr(200), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(3, I32), utb_temp(2, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(Lelse, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(4, I32)), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int else_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[else_label].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(1, I32), utb_temp(3, I32));
  /* Different constant than the then arm. */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(5, I32)), utb_imm(43, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int merge_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[merge_label].is_jump_target = 1;
  int load = utb_emit4(ir, TCCIR_OP_LOAD_INDEXED, utb_temp(6, I32), utb_temp(1, I32), utb_temp(2, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(6, I32), UTB_NONE);

  int changes = tcc_ir_opt_diamond_store_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD_INDEXED);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): no post-merge LOAD_INDEXED reads the stored slot, so the
 * pass has no use to forward to and makes no change. */
UT_TEST(test_diamond_store_fwd_no_load_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  utb_alloc_tmp_intervals(ir, 8);

  int Lelse = 9;
  int Lmerge = 14;

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), slot_addr(200), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(2, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(3, I32), utb_temp(2, I32), utb_imm(2, I32));
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(Lelse, I32), utb_temp(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(4, I32), utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(4, I32)), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int else_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[else_label].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_temp(1, I32), utb_temp(3, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(5, I32)), utb_imm(42, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(Lmerge, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  int merge_label = utb_emit(ir, TCCIR_OP_NOP, UTB_NONE, UTB_NONE, UTB_NONE);
  ir->compact_instructions[merge_label].is_jump_target = 1;
  /* No LOAD_INDEXED after the merge. */
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_diamond_store_fwd(ir);

  UT_ASSERT_EQ(changes, 0);

  utb_free(ir);
  return 0;
}

UT_SUITE(opt_memory)
{
  UT_RUN(test_sl_forward_store_lval_var_slot_does_not_alias_stackloc);
  UT_RUN(test_sl_forward_resolves_temp_before_store_width);
  UT_RUN(test_sl_forward_pointer_store_keeps_deref_width_for_high_half);
  UT_RUN(test_sl_forward_basic_imm_store_load);
  UT_RUN(test_sl_forward_lea_pointer_store_load);
  UT_RUN(test_sl_forward_cmp_lval_operand_forward);
  UT_RUN(test_sl_forward_unknown_pointer_store_clears_tracking);
  UT_RUN(test_sl_forward_call_invalidates_addrtaken_store);
  UT_RUN(test_sl_forward_jump_target_blocks_forward);
  UT_RUN(test_sl_forward_cross_bb_fallthrough);
  UT_RUN(test_sl_forward_store_indexed_via_lea);
  UT_RUN(test_sl_forward_pure_aeabi_call_keeps_tracking);

  UT_RUN(test_memmove_global_load_fwd_direct_local_load);
  UT_RUN(test_memmove_global_load_fwd_intervening_store_blocks);
  UT_RUN(test_memmove_global_load_fwd_non_global_src_kept);

  UT_RUN(test_diamond_store_fwd_both_arms_same_const);
  UT_RUN(test_diamond_store_fwd_different_const_kept);
  UT_RUN(test_diamond_store_fwd_no_load_kept);
}
