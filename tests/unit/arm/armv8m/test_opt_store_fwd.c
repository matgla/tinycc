/*
 *  test_opt_store_fwd.c - suite for the store-forward / redundant-store passes
 *  in ir/opt_memory.c: entry_store, byte_store_merge, store_redundant,
 *  dead_static_store, dead_local_slot, dead_temp_local, plus a guard test for
 *  global_base_share.
 *
 *  This is the P1b half of the store-fwd/DSE cluster (see
 *  docs/plan_ut_next_steps.md) -- the seam the differential fuzzer names as
 *  the dominant optimizer bug-density cluster. Each pass gets a positive case
 *  (the transform fires) and a negative/guard case (a legitimate reason the
 *  transform must NOT fire).
 *
 *  NOT covered here (documented gaps, not fixed):
 *   - "esp_cleanup" (tcc_ir_opt_entry_store_cleanup_ex in ir/opt_pipeline.c)
 *     is `static` (internal linkage) and is a pure compound-orchestration
 *     wrapper around seven already-tested passes with no independent
 *     transformation logic of its own -- it is not reachable or meaningfully
 *     testable as a host-native isolated unit; it needs the golden-IR
 *     (`-dump-ir-passes=`) track instead.
 *   - "global_base_share" needs a real `elfsym()`/section resolution
 *     (SHF_ALLOC|SHF_WRITE section, valid st_shndx) to ever fire; the shared
 *     `elfsym()` stub in stubs.c always returns NULL, so only its "no ELF
 *     state -> never fires" guard path is tested here. A genuine positive
 *     case needs fake-ELF-section stub infrastructure -- out of scope for
 *     this pass alone.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (defined in ir/opt_memory.c; forward-declared here to
 * avoid pulling in the optimizer engine headers). */
int tcc_ir_opt_entry_store_prop(TCCIRState *ir);
int tcc_ir_opt_byte_store_merge(TCCIRState *ir);
int tcc_ir_opt_dead_static_store_elim(TCCIRState *ir);
int tcc_ir_opt_dead_local_slot_elim(TCCIRState *ir);
int tcc_ir_opt_dead_temp_local_elim(TCCIRState *ir);
int tcc_ir_opt_global_base_share(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I8  IROP_BTYPE_INT8

/* ------------------------------------------------------------------ helpers */

/* A direct stack-slot lvalue: `StackLoc[off]` used as a memory reference. */
static IROperand utb_slot_lval(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 1, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

/* The address of a stack slot (what a LEA computes into a TEMP): not an
 * lvalue, just a value. */
static IROperand utb_slot_addr(int32_t off, int btype)
{
  return irop_make_stackoff(0, off, /*is_lval*/ 0, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

/* A TEMP_LOCAL slot operand (anonymous compiler-generated stack temp),
 * identified by vreg in [-9,-2] rather than the usual vreg encoding. See
 * tccir_operand.h: irop_set_vreg round-trips negative sentinels unchanged. */
static IROperand utb_templocal(int32_t vreg, int32_t off, int is_lval, int btype)
{
  return irop_make_stackoff(vreg, off, is_lval, /*is_llocal*/ 0, /*is_param*/ 0, btype);
}

static IROperand utb_deref_temp(int pos, int btype)
{
  return utb_lval(utb_temp(pos, btype));
}

/* A global symbol reference operand (lval, non-local, non-const). */
static IROperand utb_global(TCCIRState *ir, Sym *sym, int is_lval, int btype)
{
  return utb_symref(ir, sym, is_lval, /*is_local*/ 0, /*is_const*/ 0, btype);
}

/* A global symbol reference with an explicit byte addend -- lets several
 * operands address different bytes of the same underlying symbol. */
static IROperand utb_global_off(TCCIRState *ir, Sym *sym, int32_t addend, int is_lval, int btype)
{
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, addend, 0);
  return irop_make_symref(0, sidx, is_lval, /*is_local*/ 0, /*is_const*/ 0, btype);
}

/* =============================================================== entry_store */

/* POSITIVE: an entry-block constant STORE to StackLoc[-56] is forwarded into a
 * deref reached through LEA(&StackLoc[-68]) + ADD #12 (matches the pass's own
 * motivating comment: entry-BB stores dominate all later code, so their value
 * is valid at any deref that resolves to the same offset).
 *   0: StackLoc[-56] <-- #4            [entry store]
 *   1: JUMP -> 2                        [ends the entry BB]
 *   2: T0 = LEA Addr[StackLoc[-68]]     [jump target; different offset]
 *   3: T1 = T0 + #12                    [-68+12 = -56: resolves to the store]
 *   4: T2 = #0 ADD T1***DEREF***        [forwarded to #0 ADD #4]
 *   5: RETURNVALUE T2
 */
UT_TEST(test_entry_store_forwards_lea_add_deref)
{
  TCCIRState *ir = utb_new();

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-56, I32), utb_imm(4, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-68, I32), UTB_NONE);
  ir->compact_instructions[lea].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(0, I32), utb_deref_temp(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_entry_store_prop(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  IROperand s2 = utb_src2(ir, use);
  UT_ASSERT(irop_is_immediate(s2));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, s2), 4);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): no entry-BB store matches the resolved offset (store is to
 * -60, deref resolves to -56) -- the deref is left untouched. */
UT_TEST(test_entry_store_no_matching_offset_kept)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-60, I32), utb_imm(4, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-68, I32), UTB_NONE);
  ir->compact_instructions[lea].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_temp(0, I32), utb_imm(12, I32));
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(2, I32), utb_imm(0, I32), utb_deref_temp(1, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(2, I32), UTB_NONE);

  int changes = tcc_ir_opt_entry_store_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s2 = utb_src2(ir, use);
  UT_ASSERT(s2.is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the TEMP carrying the address is REDEFINED with something
 * that is not a stack address, so at the deref it only *may* point at the slot
 * -- forwarding the entry store would be unsound.
 *
 * This is the `s = cond ? param : &local;` shape: the LEA is one edge of the
 * phi, the parameter is the other, and both defs land in the same vreg. The
 * LEA map used to be write-only (nothing ever invalidated an entry), so the
 * deref was folded to the stored constant on both paths -- see
 * tests/ir_tests/426_entry_store_phi_alias.c for the C-level repro.
 *   0: StackLoc[-56] <-- #7
 *   1: JUMP -> 2
 *   2: T0 = LEA Addr[StackLoc[-56]]     [jump target]
 *   3: T0 = ASSIGN P0                   [second def: not a stack address]
 *   4: T1 = #0 ADD T0***DEREF***        [must stay a load]
 */
UT_TEST(test_entry_store_redefined_temp_not_forwarded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-56, I32), utb_imm(7, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(2, I32), UTB_NONE, UTB_NONE);
  int lea = utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_slot_addr(-56, I32), UTB_NONE);
  ir->compact_instructions[lea].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(1, I32), utb_imm(0, I32), utb_deref_temp(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(1, I32), UTB_NONE);

  int changes = tcc_ir_opt_entry_store_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s2 = utb_src2(ir, use);
  UT_ASSERT(s2.is_lval);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the shape tcc actually emits for
 *   `for (s = *argv ? argv : fallback; *s; s++) use(*s);`
 * (from `armv8m-tcc -O1 -dump-ir-passes=entry_store`, condensed). The two ends
 * of the ternary write the SAME temp -- one an `Addr[StackLoc]`, one a load of
 * the parameter -- the value flows through a VAR and is then advanced by the
 * `s++`, so the deref resolves to the array's *second* element. Both hazards
 * (may-alias phi and stale-after-increment offset) must block forwarding.
 *   0: StackLoc[-8] <-- #0             [fallback[0] = 0]
 *   1: StackLoc[-4] <-- #0             [fallback[1] = 0  <- the value that leaked]
 *   2: JUMP -> 3
 *   3: T1 <-- Addr[StackLoc[-8]]       [jump target: s = fallback]
 *   4: T1 <-- P0 [LOAD]                [other edge: s = argv]
 *   5: V0 <-- T1
 *   6: T4 <-- V0
 *   7: V0 <-- T4 ADD #4                [s++]
 *   8: T6 <-- V0
 *   9: T7 = #0 ADD T6***DEREF***       [use(*s): must stay a load]
 */
UT_TEST(test_entry_store_phi_temp_after_increment_not_forwarded)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-4, I32), utb_imm(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(3, I32), UTB_NONE, UTB_NONE);
  int lea = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_slot_addr(-8, I32), UTB_NONE);
  ir->compact_instructions[lea].is_jump_target = 1;
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32), utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_var(0, I32), utb_temp(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(4, I32), utb_var(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ADD, utb_var(0, I32), utb_temp(4, I32), utb_imm(4, I32));
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(6, I32), utb_var(0, I32), UTB_NONE);
  int use = utb_emit(ir, TCCIR_OP_ADD, utb_temp(7, I32), utb_imm(0, I32), utb_deref_temp(6, I32));
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(7, I32), UTB_NONE);

  int changes = tcc_ir_opt_entry_store_prop(ir);

  UT_ASSERT_EQ(changes, 0);
  IROperand s2 = utb_src2(ir, use);
  UT_ASSERT(s2.is_lval);

  utb_free(ir);
  return 0;
}

/* =============================================================== byte_store_merge */

/* POSITIVE: 4 consecutive byte stores at word-aligned addends 0..3 of the same
 * global merge into one INT32 store. (Uses direct SYMREF-deref stores rather
 * than a LEA'd stack TEMP: the TEMP-base resolution path shares file-static
 * def-map state with store_redundant that only that pass populates/frees, so
 * it cannot be exercised reliably in isolation -- the SYMREF path is
 * independent of that state and is the reliable way to drive this pass on
 * its own.) */
UT_TEST(test_byte_store_merge_four_bytes_merged)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));

  int s0 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 0, 1, I8), utb_imm(0x11, I8), UTB_NONE);
  int s1 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 1, 1, I8), utb_imm(0x22, I8), UTB_NONE);
  int s2 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 2, 1, I8), utb_imm(0x33, I8), UTB_NONE);
  int s3 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 3, 1, I8), utb_imm(0x44, I8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_byte_store_merge(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  IROperand merged_src1 = utb_src1(ir, s0);
  UT_ASSERT(irop_is_immediate(merged_src1));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, merged_src1), 0x44332211);
  UT_ASSERT_EQ(utb_op(ir, s1), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, s2), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, s3), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): only 3 of the 4 bytes are present (byte at addend 3 is
 * missing) -- the group never reaches 4 members, so nothing merges. */
UT_TEST(test_byte_store_merge_incomplete_group_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));

  int s0 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 0, 1, I8), utb_imm(0x11, I8), UTB_NONE);
  int s1 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 1, 1, I8), utb_imm(0x22, I8), UTB_NONE);
  int s2 = utb_emit(ir, TCCIR_OP_STORE, utb_global_off(ir, &sym_g, 2, 1, I8), utb_imm(0x33, I8), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_byte_store_merge(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, s1), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, s2), TCCIR_OP_STORE);

  utb_free(ir);
  return 0;
}

/* =============================================================== dead_static_store */

/* POSITIVE: during the end-of-TU late-reopt phase, a STORE to a static global
 * that end-of-TU analysis proved has no readers is dead. */
UT_TEST(test_dead_static_store_unread_global_removed)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  tcc_state->ir_late_reopt_phase = 1;

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));
  sym_g.a.tu_no_readers = 1;

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_global(ir, &sym_g, 1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_dead_static_store_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_NOP);

  tcc_state->ir_late_reopt_phase = 0;
  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): the end-of-TU analysis did NOT prove "no readers" for this
 * global (tu_no_readers unset) -- the store must survive. */
UT_TEST(test_dead_static_store_possibly_read_global_kept)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  tcc_state->ir_late_reopt_phase = 1;

  Sym sym_g;
  memset(&sym_g, 0, sizeof(sym_g));
  /* sym_g.a.tu_no_readers left 0: some function may still read this global. */

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_global(ir, &sym_g, 1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_dead_static_store_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);

  tcc_state->ir_late_reopt_phase = 0;
  utb_free(ir);
  return 0;
}

/* =============================================================== dead_local_slot */

/* POSITIVE: a direct write to a plain stack slot that is never read anywhere
 * in the function is dead. */
UT_TEST(test_dead_local_slot_unread_store_removed)
{
  TCCIRState *ir = utb_new();

  int dead = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_local_slot_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a later direct LOAD of the same slot keeps the store
 * alive. */
UT_TEST(test_dead_local_slot_read_store_kept)
{
  TCCIRState *ir = utb_new();

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_slot_lval(-8, I32), utb_imm(5, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_slot_lval(-8, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_local_slot_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* =============================================================== dead_temp_local */

/* POSITIVE: a write to an anonymous TEMP_LOCAL slot (vreg in [-9,-2]) that is
 * never read afterward is dead. */
UT_TEST(test_dead_temp_local_unread_store_removed)
{
  TCCIRState *ir = utb_new();

  int dead = utb_emit(ir, TCCIR_OP_STORE, utb_templocal(-2, 0, 1, I32), utb_imm(5, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_imm(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_temp_local_elim(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* NEGATIVE (guard): a later LOAD of the same TEMP_LOCAL slot keeps the store
 * alive. */
UT_TEST(test_dead_temp_local_read_store_kept)
{
  TCCIRState *ir = utb_new();

  int store = utb_emit(ir, TCCIR_OP_STORE, utb_templocal(-2, 0, 1, I32), utb_imm(5, I32), UTB_NONE);
  int load = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_templocal(-2, 0, 1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVALUE, UTB_NONE, utb_temp(0, I32), UTB_NONE);

  int changes = tcc_ir_opt_dead_temp_local_elim(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, load), TCCIR_OP_LOAD);

  utb_free(ir);
  return 0;
}

/* =============================================================== global_base_share */

/* GUARD ONLY (documented gap; see file header): without real ELF section
 * state, `elfsym()` (stubbed to always return NULL in stubs.c) makes
 * `gbs_get_store_symref` reject every candidate, so even a plausible cluster
 * of consecutive global stores never fires. This pins the current stub-driven
 * behavior; it is not an oracle for the pass's real positive-path logic. */
UT_TEST(test_global_base_share_no_elf_state_never_fires)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  tcc_state->opt_indexed_memory = 1;

  Sym sym_a, sym_b;
  memset(&sym_a, 0, sizeof(sym_a));
  memset(&sym_b, 0, sizeof(sym_b));
  sym_a.type.t = I32;
  sym_b.type.t = I32;

  int s0 = utb_emit(ir, TCCIR_OP_STORE, utb_global(ir, &sym_a, 1, I32), utb_imm(1, I32), UTB_NONE);
  int s1 = utb_emit(ir, TCCIR_OP_STORE, utb_global(ir, &sym_b, 1, I32), utb_imm(2, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);

  int changes = tcc_ir_opt_global_base_share(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, s0), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_op(ir, s1), TCCIR_OP_STORE);

  tcc_state->opt_indexed_memory = 0;
  utb_free(ir);
  return 0;
}

UT_COVERS("entry_store");
UT_COVERS("byte_store_merge");
UT_COVERS("dead_static_store");
UT_COVERS("dead_local_slot");
UT_COVERS("dead_temp_local");
UT_COVERS("global_base_share");
