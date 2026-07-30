/*
 *  test_opt_dead_vla.c - suite for ir/opt_dead_vla.c
 *
 *  Three companion passes eliminate observably-dead dynamic-stack-alloc
 *  (VLA / alloca) sequences and forward an alloca-pointer load:
 *
 *    tcc_ir_opt_dead_vla_struct_elim   - VLA_SP_SAVE writes a STACK SLOT; the
 *        slot's only readers are address-arithmetic ops ending in STORE
 *        destinations (no LOAD/escape).  NOPs the alloc + inner save + the
 *        whole derived address+store chain.
 *    tcc_ir_opt_alloca_load_fwd        - VLA_SP_SAVE -> slot immediately
 *        followed by LOAD slot -> vreg, with the slot otherwise dead: retarget
 *        the SAVE's dest to the LOAD's vreg and NOP the LOAD (mov reg, sp).
 *    tcc_ir_opt_dead_alloca_vreg_elim  - same dead-store elimination but for the
 *        VREG-dest VLA_SP_SAVE shape produced after alloca_load_fwd runs.
 *
 *  Isolated tests: a hand-built IR sequence is run through the bare pass entry
 *  point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_dead_vla_struct_elim(TCCIRState *ir);
int tcc_ir_opt_alloca_load_fwd(TCCIRState *ir);
int tcc_ir_opt_dead_alloca_vreg_elim(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* ---------------------------------------------------------------- helpers */

/* A non-lval stack-slot operand (the destination of a VLA_SP_SAVE). */
static inline IROperand slot_dest(int32_t off)
{
  return utb_stackoff(off, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, I32);
}

/* An lval stack-slot operand: a value read of slot `off` (LOAD of the saved
 * SP / address base). */
static inline IROperand slot_read(int32_t off)
{
  return utb_stackoff(off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, I32);
}

static inline int vreg_temp(int pos)
{
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos);
}

/* ============================================================ dead_vla_struct */

/* Canonical dead-VLA-struct pattern (slot-dest save):
 *
 *   VLA_ALLOC  size, align
 *   VLA_SP_SAVE -> StackLoc[S]            (slot)
 *   T0 = StackLoc[S](lval) + #4           (address propagation: tainted T0)
 *   STORE  T0***DEREF*** <- #0            (write through tainted addr: dead)
 *
 * The bytes are never read and the address never escapes, so the whole
 * sequence is dead and must be NOPed. */
UT_TEST(test_dead_vla_struct_basic_elim)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* A plain STORE directly through the slot-read address (no intermediate
 * propagator) is still a kill: dest is the lval-temp... but here we use a
 * single ASSIGN propagator to taint a temp, then STORE.  Two propagation
 * links (ASSIGN then ADD) must all be drained. */
UT_TEST(test_dead_vla_struct_two_link_chain)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(32, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(8), UTB_NONE, UTB_NONE);
  /* T0 = slot (ASSIGN copies the base pointer) */
  int a0 = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), slot_read(8), UTB_NONE);
  /* T1 = T0 + 12 (offset into the struct) */
  int a1 = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  /* STORE through T1 */
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, a0), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, a1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* A LOAD that dereferences a tainted address (the bytes ARE read) means the
 * VLA's contents are observable.  The pass must NOT fire. */
UT_TEST(test_dead_vla_struct_load_through_tainted_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  /* T1 = LOAD T0***DEREF*** : a real read of the VLA bytes. */
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* Escape: the tainted address is itself STORED as a value (the VLA pointer
 * leaks to memory).  The pass must NOT fire. */
UT_TEST(test_dead_vla_struct_address_escape_bails)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  /* STORE  V0***DEREF*** <- T0   : store the tainted VLA address as the VALUE
   * into some unrelated VAR slot.  src1 is the tainted temp -> escape. */
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* A second writer to the save slot means the slot's value can change after the
 * save, so the single-source taint reasoning is unsound.  The pass must bail. */
UT_TEST(test_dead_vla_struct_second_slot_writer_bails)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  /* A STORE writing the SAME slot 16 (dest is the slot lval) -> second writer. */
  int badstore = utb_emit(ir, TCCIR_OP_STORE, slot_read(16), utb_imm(0, I32), UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, badstore), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* Function-wide bail: any IJUMP makes the memory-effect modelling unsound, so
 * the entire pass returns 0 without touching the otherwise-dead VLA. */
UT_TEST(test_dead_vla_struct_ijump_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);
  /* IJUMP anywhere in the function disqualifies the whole pass. */
  utb_emit(ir, TCCIR_OP_IJUMP, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);

  utb_free(ir);
  return 0;
}

/* Function-wide bail: a non-zero captured_count (nested-function entanglement)
 * means a VLA address could escape invisibly into a closure.  Bail. */
UT_TEST(test_dead_vla_struct_captured_locals_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  ir->captured_count = 1;

  int changes = tcc_ir_opt_dead_vla_struct_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);

  utb_free(ir);
  return 0;
}

/* Idempotence: after the first elimination the VLA chain is all NOPs; a second
 * run finds no VLA_ALLOC and reports 0. */
UT_TEST(test_dead_vla_struct_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(64, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(16), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(0, I32), slot_read(16), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int first = tcc_ir_opt_dead_vla_struct_elim(ir);
  UT_ASSERT(first > 0);
  int second = tcc_ir_opt_dead_vla_struct_elim(ir);
  UT_ASSERT_EQ(second, 0);

  utb_free(ir);
  return 0;
}

/* ============================================================ alloca_load_fwd */

/* Canonical alloca-pointer forwarding:
 *
 *   VLA_SP_SAVE -> StackLoc[S]
 *   LOAD  T0 <- StackLoc[S]
 *
 * with S otherwise dead -> rewrite the SAVE's dest to T0's vreg and NOP the
 * LOAD (the backend then emits `mov T0, sp`). */
UT_TEST(test_alloca_load_fwd_basic)
{
  TCCIRState *ir = utb_new();

  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(12), UTB_NONE, UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_read(12), UTB_NONE);

  int changes = tcc_ir_opt_alloca_load_fwd(ir);

  UT_ASSERT_EQ(changes, 1);
  /* SAVE still a SAVE, but its dest is now T0's vreg (not a stack slot). */
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, save)), vreg_temp(0));
  /* LOAD folded away. */
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* The LOAD must be the IMMEDIATELY-next non-NOP op.  An intervening real
 * instruction blocks the fold. */
UT_TEST(test_alloca_load_fwd_not_adjacent_no_fold)
{
  TCCIRState *ir = utb_new();

  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(12), UTB_NONE, UTB_NONE);
  /* An unrelated ADD between the save and the load. */
  int mid = utb_emit(ir, TCCIR_OP_ADD, utb_temp(5, I32), utb_imm(1, I32), utb_imm(2, I32));
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_read(12), UTB_NONE);

  int changes = tcc_ir_opt_alloca_load_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  /* SAVE dest remains the stack slot. */
  UT_ASSERT_EQ(irop_get_tag(utb_dest(ir, save)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(utb_op(ir, mid), TCCIR_OP_ADD);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* The slot must be isolated: a second reader of the same slot elsewhere means
 * the value must stay materialized in memory.  No fold. */
UT_TEST(test_alloca_load_fwd_extra_slot_reader_no_fold)
{
  TCCIRState *ir = utb_new();

  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(12), UTB_NONE, UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_read(12), UTB_NONE);
  /* A later VLA_SP_RESTORE reads slot 12 -> not isolated. */
  int restore = utb_emit(ir, TCCIR_OP_VLA_SP_RESTORE, UTB_NONE, slot_read(12), UTB_NONE);

  int changes = tcc_ir_opt_alloca_load_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(irop_get_tag(utb_dest(ir, save)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_op(ir, restore), TCCIR_OP_VLA_SP_RESTORE);

  utb_free(ir);
  return 0;
}

/* The LOAD's dest must be a plain (non-lval) vreg.  A deref-target LOAD dest
 * (the load result is spilled through a pointer) is not foldable. */
UT_TEST(test_alloca_load_fwd_lval_load_dest_no_fold)
{
  TCCIRState *ir = utb_new();

  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(12), UTB_NONE, UTB_NONE);
  /* LOAD dest is an lval temp -> not a plain vreg. */
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_lval(utb_temp(0, I32)), slot_read(12), UTB_NONE);

  int changes = tcc_ir_opt_alloca_load_fwd(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(irop_get_tag(utb_dest(ir, save)), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* Idempotence: a second run after the fold makes no further change (the LOAD
 * is now NOP and the SAVE dest is a vreg, not a slot). */
UT_TEST(test_alloca_load_fwd_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, slot_dest(12), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), slot_read(12), UTB_NONE);

  int first = tcc_ir_opt_alloca_load_fwd(ir);
  UT_ASSERT_EQ(first, 1);
  int second = tcc_ir_opt_alloca_load_fwd(ir);
  UT_ASSERT_EQ(second, 0);

  utb_free(ir);
  return 0;
}

/* ============================================================ dead_alloca_vreg */

/* Canonical VREG-dest dead-alloca pattern (post alloca_load_fwd shape):
 *
 *   VLA_ALLOC  size, align
 *   VLA_SP_SAVE -> T0   (vreg dest = the alloca pointer)
 *   STORE  T0***DEREF*** <- #0   (write through the alloca pointer: dead)
 *
 * The alloca bytes are never read and the pointer never escapes -> NOP all. */
UT_TEST(test_dead_alloca_vreg_basic_elim)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* VREG-dest with a propagation link: T1 = T0 + 8, then STORE through T1.
 * Both the ADD and the STORE drain. */
UT_TEST(test_dead_alloca_vreg_propagation_chain)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  int add = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(8, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(1, I32)), utb_imm(3, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, add), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* A FUNCCALL after the save forces a conservative bail: the alloca pointer
 * might be observed through the call.  Nothing is eliminated. */
UT_TEST(test_dead_alloca_vreg_funccall_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  /* FUNCCALLVOID = {0,1,1}: src1=callee, src2=call_id. */
  int call = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, utb_temp(9, I32), utb_imm(0, I32));
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(utb_op(ir, call), TCCIR_OP_FUNCCALLVOID);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* A LOAD that dereferences the (vreg) alloca pointer reads the bytes -> the
 * deref classification forces a bail.  Nothing is eliminated. */
UT_TEST(test_dead_alloca_vreg_load_through_pointer_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  /* T1 = LOAD T0***DEREF*** : reads the alloca bytes via the tainted temp. */
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_lval(utb_temp(0, I32)), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* Escape via store-as-value: the alloca pointer T0 is STORED into an unrelated
 * VAR slot (its value leaks).  Bail. */
UT_TEST(test_dead_alloca_vreg_pointer_escape_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  int save = utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  /* STORE V0***DEREF*** <- T0 : dest is NOT a tainted addr, src1 IS tainted
   * value -> escape. */
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_var(0, I32)), utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);
  UT_ASSERT_EQ(utb_op(ir, save), TCCIR_OP_VLA_SP_SAVE);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* Function-wide bail: SET_CHAIN (nested-function static chain) disqualifies the
 * whole VREG-dest pass. */
UT_TEST(test_dead_alloca_vreg_set_chain_bails)
{
  TCCIRState *ir = utb_new();

  int alloc = utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);
  /* SET_CHAIN present anywhere -> whole pass bails. */
  utb_emit(ir, TCCIR_OP_SET_CHAIN, UTB_NONE, utb_temp(8, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_alloca_vreg_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, alloc), TCCIR_OP_VLA_ALLOC);

  utb_free(ir);
  return 0;
}

/* Idempotence: after one elimination there is no VLA_ALLOC left; a second run
 * reports 0. */
UT_TEST(test_dead_alloca_vreg_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_VLA_ALLOC, UTB_NONE, utb_imm(16, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_VLA_SP_SAVE, utb_temp(0, I32), UTB_NONE, UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(0, I32), UTB_NONE);

  int first = tcc_ir_opt_dead_alloca_vreg_elim(ir);
  UT_ASSERT(first > 0);
  int second = tcc_ir_opt_dead_alloca_vreg_elim(ir);
  UT_ASSERT_EQ(second, 0);

  utb_free(ir);
  return 0;
}

UT_COVERS("dead_vla_struct_elim");
UT_COVERS("alloca_load_fwd");
UT_COVERS("dead_alloca_vreg_elim");
