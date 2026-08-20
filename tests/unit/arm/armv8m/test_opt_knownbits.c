/*
 *  test_opt_knownbits.c - suite for ir/opt_knownbits.c (known-bits propagation)
 *
 *  tcc_ir_opt_known_bits tracks, per TEMP and per stack slot, which bits are
 *  statically known to be 0 or 1 (a kz/ko lattice over 32 bits, single-BB
 *  scope).  When every bit of a TEMP destination becomes known, the defining
 *  op is rewritten to an immediate ASSIGN; it also folds constant stack-slot
 *  reads, narrow LOADs (honoring the load width + signed/unsigned extension),
 *  and a few branch/SETIF patterns.
 *
 *  These are isolated tests: a hand-built IR sequence is run through the bare
 *  pass entry point and the resulting instructions are inspected directly.
 *
 *  Covered:
 *    (a) POSITIVE folds the pass really performs (assert rewritten op + the
 *        exact folded immediate + changes > 0):
 *          - AND with #0  -> ASSIGN #0      (all bits forced to 0)
 *          - OR  with #-1 -> ASSIGN #-1     (all bits forced to 1)
 *          - (param OR #0xFF) SHL #24 -> ASSIGN #0xFF000000  (bitfield-style:
 *            low byte forced to ones, shift makes the whole word known)
 *    (b) Narrow-width LOAD is honored (HISTORICAL BUG guard): a value stored
 *        32-bit wide and read back through a sub-word LOAD must be masked to
 *        the load width and (zero/sign)-extended per dest.is_unsigned, never
 *        carrying the dropped upper bytes.
 *    (c) NEGATIVE: when operands carry no known bits, nothing folds and the
 *        instruction is left unchanged with changes == 0.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry point (declared in ir/opt.h; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_known_bits(TCCIRState *ir);

#define I8  IROP_BTYPE_INT8
#define I16 IROP_BTYPE_INT16
#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

/* Build a direct StackLoc[off] lvalue operand (is_lval=1, no vreg) that the
 * pass recognizes via kb_is_direct_stackoff(). */
static IROperand kb_stack_lval(int32_t off, int btype)
{
  return irop_make_stackoff(-1, off, /*is_lval*/1, /*is_llocal*/0,
                            /*is_param*/0, btype);
}

/* ------------------------------------------------------------------ tests */

/* T1 = param0 AND #0
 * param0 carries no known bits, but AND #0 forces every result bit to 0, so
 * known-bits proves the whole destination is 0 and rewrites the AND into an
 * immediate ASSIGN #0.  (Pos 1 so max_tmp_pos > 0 and the pass runs.) */
UT_TEST(test_knownbits_and_zero_folds)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), 0);

  utb_free(ir);
  return 0;
}

/* T1 = param0 OR #-1
 * OR with all-ones forces every result bit to 1 regardless of param0, so the
 * destination is fully known (0xFFFFFFFF) and the OR folds to ASSIGN #-1. */
UT_TEST(test_knownbits_or_allones_folds)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(-1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), -1);

  utb_free(ir);
  return 0;
}

/* Bitfield-style positive: the low byte is forced to ones, then shifted to the
 * top of the word, leaving every bit determined.
 *   T1 = param0 OR  #0xFF    ; low 8 bits known-one, high 24 unknown
 *   T2 = T1     SHL #24      ; SHL injects 24 known-zero low bits and shifts
 *                              the known-one byte up -> whole word known
 *   -> T2 folds to ASSIGN #0xFF000000.
 * Only the SHL is a full-word fold; the OR is left as-is (high bits unknown),
 * so exactly one rewrite happens. */
UT_TEST(test_knownbits_or_then_shl_folds_word)
{
  TCCIRState *ir = utb_new();

  int i_or  = utb_emit(ir, TCCIR_OP_OR,  utb_temp(1, I32),
                       utb_param(0, I32), utb_imm(0xFF, I32));
  int i_shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32),
                       utb_temp(1, I32), utb_imm(24, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  /* OR's high bits stay unknown -> not folded. */
  UT_ASSERT_EQ(utb_op(ir, i_or), TCCIR_OP_OR);
  /* SHL becomes a full-word immediate ASSIGN. */
  UT_ASSERT_EQ(utb_op(ir, i_shl), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_shl)));
  /* 0xFF000000 read back as a sign-extended 32-bit immediate. */
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i_shl)),
               (int32_t)0xFF000000);

  utb_free(ir);
  return 0;
}

/* HISTORICAL BUG guard - narrow unsigned LOAD must honor the load width.
 *   *(StackLoc[-4]) = #0x000001F2   ; store a full 32-bit value
 *   T1 = (uint8_t) LOAD StackLoc[-4]; read back as an UNSIGNED byte
 * The slot's known value is 0x1F2, but the byte load sees only 0xF2 with the
 * upper bytes zero-extended.  The fold must produce 0xF2 (242), NOT 0x1F2 and
 * NOT a sign-extended value: the dropped upper byte must not leak into the
 * known bits.  (Low byte 0xF2 != 0xFF, so the all-ones-byte rewrite-suppression
 * does not apply and the LOAD is rewritten to ASSIGN.) */
UT_TEST(test_knownbits_narrow_unsigned_load_masks_width)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-4, I32), utb_imm(0x1F2, I32),
           UTB_NONE);

  /* Destination and the byte-load source are both unsigned 8-bit: the slot
   * read goes through the constant-stack-slot fold (kb_apply_const_width),
   * which reads the SOURCE operand's is_unsigned, while the kb path reads the
   * dest's — mark both so either fold path zero-extends. */
  IROperand dst = utb_temp(1, I8);
  dst.is_unsigned = 1;
  IROperand src = kb_stack_lval(-4, I8);
  src.is_unsigned = 1;
  int i_ld = utb_emit(ir, TCCIR_OP_LOAD, dst, src, UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i_ld), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_ld)));
  /* Exactly the low byte, zero-extended: 0xF2 == 242, not 0x1F2 (498). */
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i_ld)), 0xF2);

  utb_free(ir);
  return 0;
}

/* Companion to the width guard - narrow SIGNED LOAD sign-extends.
 *   *(StackLoc[-8]) = #0x80          ; low byte 0x80, bit 7 set
 *   T1 = (int8_t) LOAD StackLoc[-8]  ; signed byte load
 * A signed byte load of 0x80 must sign-extend to 0xFFFFFF80 == -128, exercising
 * the signed branch of kb_apply_load_width (sign bit known -> upper bytes
 * known-one), distinct from the zero-extend above. */
UT_TEST(test_knownbits_narrow_signed_load_sign_extends)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-8, I32), utb_imm(0x80, I32),
           UTB_NONE);

  IROperand dst = utb_temp(1, I8); /* signed: is_unsigned stays 0 */
  int i_ld = utb_emit(ir, TCCIR_OP_LOAD, dst, kb_stack_lval(-8, I8), UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i_ld), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_ld)));
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i_ld)), -128);

  utb_free(ir);
  return 0;
}

/* GUARD: ASSIGN with an lvalue source is load-shaped even when the source
 * operand is an immediate-like constant.  known-bits may record the value fact,
 * but it must not replace the source with a plain non-lvalue immediate, which
 * would drop the dereference semantics (seed3531). */
UT_TEST(test_knownbits_assign_lval_immediate_keeps_load_shape)
{
  TCCIRState *ir = utb_new();

  IROperand src = utb_imm(1234, I32);
  src.is_lval = 1;
  int i_as = utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), src, UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_as), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)utb_src1(ir, i_as).is_lval, 1);
  /* irop_is_immediate answers NO for an lvalue immediate on purpose — it names
   * an absolute address, not a value — so check the encoding directly. */
  UT_ASSERT(irop_is_lval_imm_addr(utb_src1(ir, i_as)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i_as)), 1234);

  utb_free(ir);
  return 0;
}

/* GUARD: if a fully-known result is derived from an lvalue operand, keep the
 * load-bearing instruction shape.  Seed3531 exposed a direct stack-slot SHR
 * being rewritten to an immediate, dropping the read dependency. */
UT_TEST(test_knownbits_lval_shift_keeps_load_shape)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-8, I32), utb_imm(2947349673u, I32),
           UTB_NONE);
  int i_shr = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32),
                       kb_stack_lval(-8, I32), utb_imm(23, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_shr), TCCIR_OP_SHR);
  UT_ASSERT_EQ((int)utb_src1(ir, i_shr).is_lval, 1);

  utb_free(ir);
  return 0;
}

/* GUARD: a stack store before a nested conditional must keep stack-slot facts
 * dirty until the next block boundary.  The old pass cleared that dirty flag on
 * every JUMPIF, so the merge block below inherited StackLoc[-8] = 222 and
 * folded the LOAD even though the branch target can arrive from before that
 * store. */
UT_TEST(test_knownbits_jumpif_after_stack_store_invalidates_merge_slot)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-8, I32), utb_imm(111, I32),
           UTB_NONE);
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_param(0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(0x94, I32),
           UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-8, I32), utb_imm(222, I32),
           UTB_NONE);
  utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_param(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(6, I32), utb_imm(0x94, I32),
           UTB_NONE);
  int i_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I32),
                      kb_stack_lval(-8, I32), UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i_ld), TCCIR_OP_LOAD);
  UT_ASSERT_EQ((int)utb_src1(ir, i_ld).is_lval, 1);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: both operands are unknown params, so no result bit is determined.
 * AND of two unknowns yields no known bits -> the pass must NOT fold and must
 * report zero changes, leaving the AND intact. */
UT_TEST(test_knownbits_unknown_operands_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32),
                    utb_param(0, I32), utb_param(1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_AND);

  utb_free(ir);
  return 0;
}

/* NEGATIVE: a partially-known value is not fully determined, so it is not
 * folded to a constant.
 *   T1 = param0 OR #0xFF   ; low byte known-one, high 24 bits unknown
 * The OR records kb but, because the destination is not fully known, must be
 * left as an OR (not rewritten to ASSIGN) and contribute no change. */
UT_TEST(test_knownbits_partial_known_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0xFF, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* AND with all-ones is the identity: it forces no new bits, so a destination
 * whose other operand is unknown stays unknown and the AND is preserved. */
UT_TEST(test_knownbits_and_allones_identity)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(-1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_AND);

  utb_free(ir);
  return 0;
}

/* A partial AND mask can fully determine a partially-known value.
 *   T1 = param0 OR #0x0F   ; low 4 bits known-one, high 28 unknown
 *   T2 = T1 AND #0x03      ; low 2 bits forced to 11, high 30 forced to 0
 * -> T2 is fully known (#3) and folds to ASSIGN. */
UT_TEST(test_knownbits_partial_and_fully_determines)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
           utb_param(0, I32), utb_imm(0x0F, I32));
  int i1 = utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32),
                    utb_temp(1, I32), utb_imm(0x03, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i1)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i1)), 3);

  utb_free(ir);
  return 0;
}

/* OR with #0 is the identity: no new bits become known, so the OR is preserved
 * when the other operand is unknown. */
UT_TEST(test_knownbits_or_zero_identity)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_OR);

  utb_free(ir);
  return 0;
}

/* XOR of a value with itself is always zero, but the known-bits pass does not
 * model XOR (no known-bits propagation), so it cannot derive that identical
 * operands produce zero.  This is a coverage negative: constprop folds XOR of
 * identical *constants*, but XOR of identical *vregs* is left to a future pass. */
UT_TEST(test_knownbits_xor_self_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32),
           utb_param(0, I32), utb_imm(0xFF, I32));
  int i1 = utb_emit(ir, TCCIR_OP_XOR, utb_temp(2, I32),
                    utb_temp(1, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i1), TCCIR_OP_XOR);

  utb_free(ir);
  return 0;
}

/* XOR with #0 is the identity: it determines no new bits, so the XOR stays. */
UT_TEST(test_knownbits_xor_zero_identity)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_XOR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_XOR);

  utb_free(ir);
  return 0;
}

/* SHL by 0 is the identity for the known-bits lattice: if the shifted value is
 * unknown, the result has the same (empty) known bits and no fold happens. */
UT_TEST(test_knownbits_shl_zero_identity)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_SHL);

  utb_free(ir);
  return 0;
}

/* SHL by 31 of a known 1 -> 0x80000000 (semi-oracle from ARM/C semantics). */
UT_TEST(test_knownbits_shl_31_known_one)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32),
                    utb_imm(1, I32), utb_imm(31, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i0)),
               (int32_t)0x80000000);

  utb_free(ir);
  return 0;
}

/* 32-bit SHL by 32 (and beyond) is defined by the pass as "result is 0";
 * assert this corner case folds to ASSIGN #0. */
UT_TEST(test_knownbits_shl_32_yields_zero)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(32, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), 0);

  utb_free(ir);
  return 0;
}

/* SHR by 30 of a known value -> 1 (semi-oracle).  We avoid 0x80000000 because
 * INT32 immediates are sign-extended internally, which would make a logical
 * shift of the 64-bit representation produce 0xFFFFFFFF instead of 1. */
UT_TEST(test_knownbits_shr_30_logical_shift)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32),
                    utb_imm(0x40000000, I32), utb_imm(30, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), 1);

  utb_free(ir);
  return 0;
}

/* 32-bit SHR by 32 -> 0, matching the pass's >=32 handling for logical shifts. */
UT_TEST(test_knownbits_shr_32_yields_zero)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(32, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), 0);

  utb_free(ir);
  return 0;
}

/* SAR by 0 is the identity: no fold when the shifted value is unknown. */
UT_TEST(test_knownbits_sar_zero_identity)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SAR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_SAR);

  utb_free(ir);
  return 0;
}

/* SAR by 31 of a negative value sign-extends the set sign bit -> -1. */
UT_TEST(test_knownbits_sar_31_negative)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SAR, utb_temp(1, I32),
                    utb_imm(0x80000000, I32), utb_imm(31, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i0)), -1);

  utb_free(ir);
  return 0;
}

/* SAR by 31 of a positive value sign-extends the clear sign bit -> 0. */
UT_TEST(test_knownbits_sar_31_positive)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SAR, utb_temp(1, I32),
                    utb_imm(0x7FFFFFFF, I32), utb_imm(31, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i0)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i0)), 0);

  utb_free(ir);
  return 0;
}

/* SAR by 32 is outside the pass's handled shift range; it must not crash and
 * must leave the instruction untouched. */
UT_TEST(test_knownbits_sar_32_unhandled)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SAR, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(32, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_SAR);

  utb_free(ir);
  return 0;
}

/* Negative shift counts are not folded by known-bits; verify no crash. */
UT_TEST(test_knownbits_shl_negative_count_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(-1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_SHL);

  utb_free(ir);
  return 0;
}

/* 16-bit UNSIGNED load of a value with the sign-bit set must zero-extend.
 *   *(StackLoc[-16]) = #0x1234F2F2
 *   T1 = (uint16_t) LOAD StackLoc[-16]
 * The low 16 bits are 0xF2F2; zero-extension must yield 62194, not a signed
 * value and not the original 0x1234F2F2. */
UT_TEST(test_knownbits_narrow_unsigned16_load_zero_extends)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-16, I32), utb_imm(0x1234F2F2, I32),
           UTB_NONE);

  IROperand dst = utb_unsigned(utb_temp(1, I16));
  IROperand src = utb_unsigned(kb_stack_lval(-16, I16));
  int i_ld = utb_emit(ir, TCCIR_OP_LOAD, dst, src, UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i_ld), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_ld)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i_ld)), 0xF2F2);

  utb_free(ir);
  return 0;
}

/* 16-bit SIGNED load of a value with the sign-bit set must sign-extend.
 *   *(StackLoc[-20]) = #0x12348000
 *   T1 = (int16_t) LOAD StackLoc[-20]
 * The low 16 bits are 0x8000; sign-extension must yield -32768. */
UT_TEST(test_knownbits_narrow_signed16_load_sign_extends)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_STORE, kb_stack_lval(-20, I32), utb_imm(0x12348000, I32),
           UTB_NONE);

  int i_ld = utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1, I16), kb_stack_lval(-20, I16),
                      UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i_ld), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i_ld)));
  UT_ASSERT_EQ((int32_t)irop_get_imm64_ex(ir, utb_src1(ir, i_ld)), -32768);

  utb_free(ir);
  return 0;
}

/* UBFX is not handled by the known-bits pass.  Boundary cases must not crash
 * and the instruction must be left unchanged (no fold). */
UT_TEST(test_knownbits_ubfx_lsb0_width1_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_UBFX, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0 | (1 << 5), I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_UBFX);

  utb_free(ir);
  return 0;
}

UT_TEST(test_knownbits_ubfx_full_width_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_UBFX, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(0 | (32 << 5), I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_UBFX);

  utb_free(ir);
  return 0;
}

UT_TEST(test_knownbits_ubfx_lsb_plus_width_overflow_no_fold)
{
  TCCIRState *ir = utb_new();

  int i0 = utb_emit(ir, TCCIR_OP_UBFX, utb_temp(1, I32),
                    utb_param(0, I32), utb_imm(16 | (17 << 5), I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, i0), TCCIR_OP_UBFX);

  utb_free(ir);
  return 0;
}

/* The pass converges: running it to fixpoint terminates and a subsequent run
 * reports no further changes. */
UT_TEST(test_knownbits_reaches_fixpoint)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32),
           utb_param(0, I32), utb_imm(0, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_known_bits, 5);
  UT_ASSERT(total > 0);

  int more = tcc_ir_opt_known_bits(ir);
  UT_ASSERT_EQ(more, 0);

  utb_free(ir);
  return 0;
}

/* After a control-flow rewrite the IR remains structurally sound.
 * TEST_ZERO of a value with a known-one bit followed by JUMPIF EQ is folded to
 * NOPs (the value is provably non-zero, so the EQ branch is never taken).
 * Uses only TEMP vregs so utb_assert_wellformed's max_vreg bound is meaningful. */
UT_TEST(test_knownbits_test_zero_fold_wellformed)
{
  TCCIRState *ir = utb_new();

  /* T0 = #1, T1 = T0 OR #1  -> T1's low bit is known-one. */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int tz = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(1, I32), UTB_NONE);
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x94, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, tz), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_NOP);
  /* Only TEMP vregs 0 and 1 are used; jump target 0 is in range. */
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 2), 0);

  utb_free(ir);
  return 0;
}

/* TEST_ZERO of a provably non-zero value + JUMPIF NE folds to an unconditional
 * JUMP (the taken branch), complementing the EQ->NOP case above.  Verifies the
 * branch_taken=1 path preserves the JUMPIF's target on the rewritten JUMP. */
UT_TEST(test_knownbits_test_zero_ne_folds_to_jump)
{
  TCCIRState *ir = utb_new();

  /* T0 = #1, T1 = T0 OR #1  -> T1's low bit is known-one (provably non-zero). */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32), utb_temp(0, I32), utb_imm(1, I32));
  int tz = utb_emit(ir, TCCIR_OP_TEST_ZERO, UTB_NONE, utb_temp(1, I32), UTB_NONE);
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x95, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, tz), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_dest(ir, j)), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 2), 0);

  utb_free(ir);
  return 0;
}

/* CMP src1, #0 + JUMPIF sign fold: when src1's sign bit is known-one it is
 * provably negative, so a signed `< 0` (tok 0x9c) branch is always taken.  The
 * CMP is NOPed and the JUMPIF becomes an unconditional JUMP.  Exercises the
 * sign-only fold path (distinct from the fully-known CMP fold). */
UT_TEST(test_knownbits_cmp_zero_sign_negative_folds_jump)
{
  TCCIRState *ir = utb_new();

  /* T1 = param0 OR #0x80000000 -> bit 31 known-one -> provably negative. */
  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
           utb_param(0, I32), utb_imm((int32_t)0x80000000, I32));
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(0, I32));
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x9c, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 1), 0);

  utb_free(ir);
  return 0;
}

/* CMP src1, #0 + JUMPIF sign fold, non-taken direction: a provably-negative
 * src1 makes a signed `>= 0` (tok 0x9d) branch never taken, so both the CMP and
 * the JUMPIF are NOPed (control falls through). */
UT_TEST(test_knownbits_cmp_zero_sign_negative_drops_ge_branch)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32),
           utb_param(0, I32), utb_imm((int32_t)0x80000000, I32));
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(0, I32));
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x9d, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* INT64 low-32 tracking: a 64-bit SHL by >= 32 makes the low 32 bits provably
 * zero (is_low32 fact).  When that value is used as the shift amount of a
 * 32-bit SHL, the pass narrows the 64-bit operand to the known 32-bit immediate
 * (#0) in place, without folding the outer SHL (its shifted value is unknown). */
UT_TEST(test_knownbits_int64_low32_narrows_shift_amount)
{
  TCCIRState *ir = utb_new();

  /* T1(I64) = param0(I64) SHL #32 -> low 32 bits are all zero. */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(1, I64), utb_param(0, I64),
           utb_imm(32, I32));
  /* T2(I32) = param1(I32) SHL T1(I64) -> the 64-bit amount narrows to #0. */
  int s = utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_param(1, I32),
                   utb_temp(1, I64));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, s), TCCIR_OP_SHL);
  UT_ASSERT(irop_is_immediate(utb_src2(ir, s)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src2(ir, s)), 0);

  utb_free(ir);
  return 0;
}

/* MUL of two fully-known values folds to the product immediate.
 *   T0 = #6, T1 = #7, T2 = T0 * T1  -> T2 = #42. */
UT_TEST(test_knownbits_mul_consts_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_imm(6, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_imm(7, I32), UTB_NONE);
  int i2 = utb_emit(ir, TCCIR_OP_MUL, utb_temp(2, I32),
                    utb_temp(0, I32), utb_temp(1, I32));

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, i2), TCCIR_OP_ASSIGN);
  UT_ASSERT(irop_is_immediate(utb_src1(ir, i2)));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, i2)), 42);

  utb_free(ir);
  return 0;
}

/* CMP x, #C (C != 0) + JUMPIF EQ where a known bit of x contradicts C: the
 * equality is impossible, so the EQ branch is dead (CMP + JUMPIF both NOPed).
 *   T1 = param0 OR #1   ; bit 0 known-one
 *   CMP T1, #4          ; 4 has bit0 = 0 -> T1 != 4 provable
 *   JUMPIF EQ           ; never taken */
UT_TEST(test_knownbits_cmp_const_eq_bit_incompatible_drops_branch)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32), utb_param(0, I32), utb_imm(1, I32));
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(4, I32));
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x94, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_NOP);

  utb_free(ir);
  return 0;
}

/* Companion: same incompatibility with JUMPIF NE makes the branch always taken
 * (CMP NOPed, JUMPIF -> unconditional JUMP). */
UT_TEST(test_knownbits_cmp_const_ne_bit_incompatible_takes_branch)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_OR, utb_temp(1, I32), utb_param(0, I32), utb_imm(1, I32));
  int c = utb_emit(ir, TCCIR_OP_CMP, UTB_NONE, utb_temp(1, I32), utb_imm(4, I32));
  int j = utb_emit(ir, TCCIR_OP_JUMPIF, utb_imm(0, I32), utb_imm(0x95, I32),
                   UTB_NONE);

  int changes = tcc_ir_opt_known_bits(ir);

  UT_ASSERT(changes > 0);
  UT_ASSERT_EQ(utb_op(ir, c), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, j), TCCIR_OP_JUMP);

  utb_free(ir);
  return 0;
}

UT_COVERS("known_bits");
