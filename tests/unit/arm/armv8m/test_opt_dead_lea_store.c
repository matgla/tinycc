/*
 *  test_opt_dead_lea_store.c - suite for ir/opt_dead_lea_store.c
 *  (dead-store elimination for LEA-deref / direct-stack STOREs)
 *
 *  tcc_ir_opt_dead_lea_store_elim() NOPs a STORE to a local stack slot when no
 *  later instruction reads any byte of that slot.  A slot address may appear
 *  directly (an lval `StackLoc[off]`) or via a single-def TEMP that holds
 *  `Addr[StackLoc[off]]` (the LEA-deref form produced after known_bits collapses
 *  bitfield chains):
 *
 *      T1 <-- LEA  Addr[StackLoc[-4]]      (single-def address temp)
 *      T1***DEREF*** <-- val   [STORE]     (write through the address temp)
 *
 *  A STORE is KEPT alive if any byte it writes is later read (a LOAD lval of the
 *  same slot, a temp-deref read, or a bounded mem* PARAM1).  The pass also bails
 *  wide (returns 0, mutates nothing) when an address escapes — e.g. the address
 *  of a local is itself stored into memory.
 *
 *  Notes used to build the IR (read from the pass + tccir_operand.h):
 *    - A LEA-temp source is a STACKOFF operand with is_local=1, is_lval=0 and
 *      no vreg: irop_make_stackoff(0, off, 0, 0, 0, btype) (arg-0 -> vreg -1).
 *    - A direct slot lval is the same with is_lval=1.
 *    - The pass returns 0 immediately unless there is at least one TEMP dest
 *      (max_tmp > 0), so every fixture defines a tracked address temp.
 *
 *  Isolated tests: a hand-built IR sequence is run through the bare pass entry
 *  point and the resulting instructions are inspected directly.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here to avoid pulling
 * in the optimizer engine headers). */
int tcc_ir_opt_dead_lea_store_elim(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

/* Build a STACKOFF operand for a local slot at byte offset `off`.
 * is_lval selects "the slot itself" (a memory reference) vs. "the address of the
 * slot as a value" (what a LEA computes into a temp). */
static inline IROperand utb_slot(int32_t off, int is_lval)
{
  return irop_make_stackoff(0, off, is_lval, /*is_llocal*/ 0, /*is_param*/ 0, I32);
}

/* ------------------------------------------------------------------ tests */

/* POSITIVE: a STORE to a direct stack slot whose bytes are never read later is
 * dead and gets NOP'd.
 *
 *   T1 = LEA Addr[StackLoc[-8]]     (single-def tracked temp; bumps max_tmp,
 *                                    never used -> stays tame)
 *   StackLoc[-4] <-- #7   [STORE]   (dead: slot -4 is never read)        -> NOP
 *
 * Non-vacuous: if the pass were a no-op this asserts would fail (op stays STORE,
 * changes==0). */
UT_TEST(test_dls_dead_direct_store_removed)
{
  TCCIRState *ir = utb_new();

  /* Tracked address temp for a *different* slot so max_tmp > 0. */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_slot(-8, /*is_lval*/ 0), UTB_NONE);
  int is = utb_emit(ir, TCCIR_OP_STORE, utb_slot(-4, /*is_lval*/ 1), utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_lea_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, is), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* POSITIVE: a STORE through a single-def LEA-deref address temp whose slot is
 * never read later is dead and gets NOP'd.
 *
 *   T1 = LEA Addr[StackLoc[-4]]            (single-def tracked address temp)
 *   T1***DEREF*** <-- #7   [STORE]         (dead deref store)              -> NOP
 *
 * The STORE dest is the temp used as an lval (deref of the address it holds);
 * RESOLVE_LVAL_SLOT maps it back to slot -4 via tmp_addr[]. */
UT_TEST(test_dls_dead_lea_deref_store_removed)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_slot(-4, /*is_lval*/ 0), UTB_NONE);

  /* STORE dest: temp T1 used as an lval (deref). */
  IROperand deref = utb_temp(1, I32);
  deref.is_lval = 1;
  int is = utb_emit(ir, TCCIR_OP_STORE, deref, utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_lea_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, is), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a STORE whose slot is read by a later LOAD is observable and must be
 * KEPT.
 *
 *   T1 = LEA Addr[StackLoc[-8]]      (max_tmp bump; tame)
 *   StackLoc[-4] <-- #7   [STORE]    (slot -4)
 *   T2 = LOAD StackLoc[-4]           (later read of slot -4, overlapping bytes)
 *
 * The LOAD's lval src1 records a read at a position > the store -> alive.
 * Pass returns 0 and leaves the STORE unchanged. */
UT_TEST(test_dls_store_with_later_load_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_slot(-8, /*is_lval*/ 0), UTB_NONE);
  int is = utb_emit(ir, TCCIR_OP_STORE, utb_slot(-4, /*is_lval*/ 1), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(2, I32), utb_slot(-4, /*is_lval*/ 1), UTB_NONE);

  int changes = tcc_ir_opt_dead_lea_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, is), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (escape bail): storing the *address* of a local into memory lets it
 * escape; the pass bails wide (returns 0, mutates nothing) so it never NOPs the
 * otherwise-dead-looking store.
 *
 *   T1 = LEA Addr[StackLoc[-8]]                (max_tmp bump)
 *   T2***DEREF*** <-- Addr[StackLoc[-4]]       [STORE of an address value]
 *
 * The STORE's src1 is a non-lval STACKOFF (address-of-local) -> escape -> bail.
 * Even though no slot is read, nothing is eliminated. */
UT_TEST(test_dls_address_escape_bails)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(1, I32), utb_slot(-8, /*is_lval*/ 0), UTB_NONE);

  /* STORE dest: some deref location (temp T2 used as lval); the *value* stored
   * is the address of local slot -4 (a non-lval STACKOFF) -> escaping. */
  IROperand deref = utb_temp(2, I32);
  deref.is_lval = 1;
  int is = utb_emit(ir, TCCIR_OP_STORE, deref, utb_slot(-4, /*is_lval*/ 0), UTB_NONE);

  int changes = tcc_ir_opt_dead_lea_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, is), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (no temps): with no TEMP dest in the function, max_tmp stays 0 and
 * the pass returns 0 immediately without touching even a plainly dead store.
 * Guards the early-out and documents the "needs a tracked temp" precondition. */
UT_TEST(test_dls_no_temps_early_out)
{
  TCCIRState *ir = utb_new();

  int is = utb_emit(ir, TCCIR_OP_STORE, utb_slot(-4, /*is_lval*/ 1), utb_imm(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_lea_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, is), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_dead_lea_store)
{
  UT_COVERS("dead_lea_store_elim");
  UT_RUN(test_dls_dead_direct_store_removed);
  UT_RUN(test_dls_dead_lea_deref_store_removed);
  UT_RUN(test_dls_store_with_later_load_kept);
  UT_RUN(test_dls_address_escape_bails);
  UT_RUN(test_dls_no_temps_early_out);
}
