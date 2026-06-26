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
#define I32 IROP_BTYPE_INT32

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

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_knownbits)
{
  UT_COVERS("known_bits");
  UT_RUN(test_knownbits_and_zero_folds);
  UT_RUN(test_knownbits_or_allones_folds);
  UT_RUN(test_knownbits_or_then_shl_folds_word);
  UT_RUN(test_knownbits_narrow_unsigned_load_masks_width);
  UT_RUN(test_knownbits_narrow_signed_load_sign_extends);
  UT_RUN(test_knownbits_unknown_operands_no_fold);
  UT_RUN(test_knownbits_partial_known_no_fold);
}
