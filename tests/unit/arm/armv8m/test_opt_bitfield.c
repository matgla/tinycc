/*
 *  test_opt_bitfield.c - suite for ir/opt_bitfield.c (bitfield insert/extract)
 *
 *  Two passes are exercised:
 *
 *    tcc_ir_opt_bitfield_insert_extract  - recognises reading back a bitfield
 *      value that was just inserted into its host word and the word is dead:
 *          L = V SHL n ; R = X AND m ; S = L OR R ; D = S SHR n  ==  D = V
 *      (Shape A, SHR) and the masked low-field variant (Shape B, AND).  The
 *      extract is rewritten to `D = ASSIGN V`.
 *
 *    tcc_ir_opt_bitfield_insert_to_bfi   - the complement: a poke whose result
 *      is observed leaves `(W & ~field) | (V<<lsb)`, rewritten in place to
 *      `BFI dest, W, V` with lsb/width in ir->bfi_params[orig_index]; the AND
 *      (and SHL when lsb>0) are NOPed.
 *
 *  Isolated tests: a hand-built IR sequence is run through the bare pass entry
 *  point and the resulting instructions/operands are inspected directly.  Where
 *  a fold yields a value, the expected value is computed independently and used
 *  as an oracle.
 */

#include "ir_build.h"

#include "ut.h"

/* Pass entry points (declared in ir/opt.h; forward-declared here to avoid
 * pulling in the optimizer engine headers). */
int tcc_ir_opt_bitfield_insert_extract(TCCIRState *ir);
int tcc_ir_opt_bitfield_insert_to_bfi(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

static inline int vreg_temp(int pos)
{
  return TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, pos);
}

/* ==================================================================
 * tcc_ir_opt_bitfield_insert_extract  -  Shape A (SHR) positive folds
 * ================================================================== */

/* Bare SHR extract, field at the top of the word (s_eff == b).
 *
 *   T0 = <undef word W>
 *   T1 = W   AND 0xFF        ; low operand, no bits in field_window (top 24)
 *   T2 = Vimm SHL 8          ; high operand = V<<8, V = 0x123 (< 2^24)
 *   T3 = T2  OR  T1          ; insert
 *   T4 = T3  SHR 8           ; extract  ->  ASSIGN 0x123
 *
 * field_window = ((1<<24)-1) << 8 = 0xFFFFFF00, w = 24, b = 8, outer_shl = 0. */
UT_TEST(test_bf_extract_shr_immediate_value_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_imm(0x123, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_ASSIGN);
  /* V was an immediate -> the ASSIGN source is exactly that immediate. */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ext)), 0x123);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Same shape but V is a TEMP provably bounded (Tv = Tx AND 0xFFFF, 16<=24 bits).
 * The ASSIGN source must be that TEMP vreg, retyped to the dest btype. */
UT_TEST(test_bf_extract_shr_temp_value_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(6, I32), utb_imm(0xFFFF, I32)); /* Tv = Tx & 0xFFFF */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));    /* low */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(5, I32), utb_imm(8, I32));       /* high = Tv<<8 */
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ext)), vreg_temp(5));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* Two-shift form `(S SHL a) SHR b` with b > a, the canonical extract of a field
 * that is not at the top of the word.
 *
 *   field at offset s_eff = b-a, width w = 32-b.  Pick b=12, a=4 -> s_eff=8,
 *   w=20.  field_window = ((1<<20)-1) << 8 = 0x0FFFFF00.
 *   high = V SHL 8 (V=0x55, < 2^20); low = W & 0xFF (no bits in window).
 *
 *   T1 = W   AND 0xFF
 *   T2 = 0x55 SHL 8
 *   T3 = T2  OR  T1          ; insert S
 *   T4 = T3  SHL 4           ; outer shl a=4
 *   T5 = T4  SHR 12          ; extract  ->  ASSIGN 0x55 */
UT_TEST(test_bf_extract_two_shift_form_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_imm(0x55, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(3, I32), utb_imm(4, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(5, I32), utb_temp(4, I32), utb_imm(12, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src1(ir, ext)), 0x55);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* s_eff == 0 path: bare SHR by b where the OR's high operand is V itself (no
 * inner SHL).  Field occupies the top w bits already; low operand contributes
 * nothing in the field_window.
 *
 *   b = 16, outer_shl = 0 -> s_eff = 0, w = 16, field_window = 0xFFFF0000.
 *   But for s_eff==0 the field is at bit 0 and the value lives in the HIGH
 *   operand directly; field_window must avoid the LOW operand.  With s_eff==0,
 *   field_window = ((1<<16)-1) << 0 = 0x0000FFFF; high = V (bits <= 16), low =
 *   W & 0xFFFF0000 (no bits in 0x0000FFFF).
 *
 *   T1 = W  AND 0xFFFF0000
 *   T2 = Vt AND 0xFFFF        ; V provably < 2^16, used as high operand directly
 *   T3 = T2 OR T1
 *   T4 = T3 SHR 0 ... no: b must be >= 1.  Use s_eff==0 via b==outer_shl. */
UT_TEST(test_bf_extract_seff_zero_two_shift_folds)
{
  /* Make s_eff == 0 through the two-shift form with a == b.
   *   b = 16, a = 16 -> s_eff = 0, w = 16, field_window = 0x0000FFFF.
   *   high operand of the OR is V directly (s_eff==0), low = W & 0xFFFF0000. */
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm((int32_t)0xFFFF0000, I32)); /* low */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(7, I32), utb_imm(0xFFFF, I32)); /* V = Tv, < 2^16 */
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));      /* insert S */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(3, I32), utb_imm(16, I32));     /* outer shl a=16 */
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(5, I32), utb_temp(4, I32), utb_imm(16, I32)); /* b=16 */

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_ASSIGN);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ext)), vreg_temp(2));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_opt_bitfield_insert_extract  -  Shape B (AND) positive fold
 * ================================================================== */

/* Masked low-field read-back:  (lowpart | other) & m == lowpart, where lowpart
 * has no bits outside m and `other` has no bits inside m.
 *
 *   m = 0x000000FF.
 *   T1 = Wlow AND 0xFF       ; lowpart  (bits within m)
 *   T2 = Whigh AND 0xFFFFFF00; other    (no bits within m)
 *   T3 = T1 OR T2            ; insert
 *   T4 = T3 AND 0xFF         ; extract  ->  ASSIGN T1 (lowpart vreg) */
UT_TEST(test_bf_extract_and_low_field_folds)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(10, I32), utb_imm(0xFF, I32));               /* lowpart */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(11, I32), utb_imm((int32_t)0xFFFFFF00, I32)); /* other */
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(1, I32), utb_temp(2, I32));
  int ext = utb_emit(ir, TCCIR_OP_AND, utb_temp(4, I32), utb_temp(3, I32), utb_imm(0xFF, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_ASSIGN);
  /* lowpart is T1 -> ASSIGN source vreg is temp 1 (retyped to dest btype). */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, ext)), vreg_temp(1));
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_opt_bitfield_insert_extract  -  negative / no-fire cases
 * ================================================================== */

/* The low operand DOES have bits inside the field_window -> the extract is not
 * pure V (the cleared region was not actually cleared) -> must NOT fold. */
UT_TEST(test_bf_extract_low_overlaps_window_no_fold)
{
  TCCIRState *ir = utb_new();

  /* b=8, field_window=0xFFFFFF00.  low = W & 0xFFFFFFFF... use 0xF00 which has
   * bits in 0xFFFFFF00 -> overlap -> no fold. */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xF00, I32)); /* overlaps window */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_imm(0x123, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The shifted value V does NOT provably fit in w bits -> folding could keep
 * bits the round-trip would have dropped -> must NOT fold.  V here is a plain
 * undefined TEMP (no provable bound). */
UT_TEST(test_bf_extract_value_unbounded_no_fold)
{
  TCCIRState *ir = utb_new();

  /* b=8, w=24.  high = V SHL 8 where V = T9 (undefined, no provable bound). */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_temp(9, I32), utb_imm(8, I32)); /* V unbounded */
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_SHR);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* The extracted value S is not defined by an OR (it is a plain AND) -> the
 * insert pattern is absent -> must NOT fold. */
UT_TEST(test_bf_extract_source_not_or_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(3, I32), utb_temp(0, I32), utb_imm((int32_t)0xFFFFFF00, I32)); /* S def is AND */
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_SHR);

  utb_free(ir);
  return 0;
}

/* lval destination on the extract: the pass skips lval dests -> no fold. */
UT_TEST(test_bf_extract_lval_dest_no_fold)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_imm(0x123, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  int ext = utb_emit(ir, TCCIR_OP_SHR, utb_lval(utb_temp(4, I32)), utb_temp(3, I32), utb_imm(8, I32));

  int changes = tcc_ir_opt_bitfield_insert_extract(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, ext), TCCIR_OP_SHR);

  utb_free(ir);
  return 0;
}

/* Idempotence: once folded, a second pass reports no further changes. */
UT_TEST(test_bf_extract_idempotent)
{
  TCCIRState *ir = utb_new();

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(2, I32), utb_imm(0x123, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(3, I32), utb_temp(2, I32), utb_temp(1, I32));
  utb_emit(ir, TCCIR_OP_SHR, utb_temp(4, I32), utb_temp(3, I32), utb_imm(8, I32));

  int total = utb_run_to_fixpoint(ir, tcc_ir_opt_bitfield_insert_extract, 10);
  UT_ASSERT_EQ(total, 1);
  UT_ASSERT_EQ(tcc_ir_opt_bitfield_insert_extract(ir), 0);
  UT_ASSERT_EQ(utb_assert_wellformed(ir, 16), 0);

  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_opt_bitfield_insert_to_bfi  -  positive folds
 * ==================================================================
 *
 * The BFI pass writes ir->bfi_params[orig_index].  In the harness the struct
 * comes back zeroed from utb_new(), so max_orig_index==0 and the side-array
 * would be a single uint16_t -> writes at orig_index==i overflow.  We set
 * max_orig_index to a safe upper bound before running, and free bfi_params
 * ourselves (utb_free does not). */
static void bf_prep_bfi(TCCIRState *ir)
{
  ir->max_orig_index = UTB_MAX_INSTR - 1;
}

static void bf_free_bfi(TCCIRState *ir)
{
  tcc_free(ir->bfi_params);
  ir->bfi_params = NULL;
}

/* lsb>0 insert with an UNencodable clearmask (so the BFI lever fires).
 *
 *   field = bits [8,24): lsb=8, width=16, fieldmask=0x00FFFF00,
 *   clearmask = 0xFF0000FF.  0xFF0000FF is NOT a Thumb-2 modified immediate
 *   (two separated byte runs), so gate 1 passes.
 *
 *   Tword = <undef host word W>            (gate 2: unknown -> assumed needs insert)
 *   Tval  = Tx AND 0xFFFF                  (V provably < 2^16)
 *   Tsh   = Tval SHL 8
 *   Tand  = W   AND 0xFF0000FF
 *   Tor   = Tsh OR Tand   ->  BFI Tor, W, Tval   ;  lsb=8 width=16
 *
 * The AND and the SHL must be single-use and are NOPed. */
UT_TEST(test_bf_to_bfi_lsb_positive_folds)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32));     /* Tval = Tx & 0xFFFF */
  int shl = utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32)); /* Tsh = Tval << 8 */
  int and_idx = utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32),
                         utb_imm((int32_t)0xFF0000FF, I32)); /* Tand = W & clearmask */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_BFI);
  /* src1 = host word W (temp 0); src2 = field value V (temp 2). */
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, orr)), vreg_temp(0));
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, orr)), vreg_temp(2));
  /* lsb (low byte) = 8, width (high byte) = 16. */
  uint16_t p = ir->bfi_params[ir->compact_instructions[orr].orig_index];
  UT_ASSERT_EQ(p & 0xFF, 8);
  UT_ASSERT_EQ((p >> 8) & 0xFF, 16);
  /* The AND and SHL are dropped (NOPed). */
  UT_ASSERT_EQ(utb_op(ir, and_idx), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, shl), TCCIR_OP_NOP);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* lsb==0 insert: the value side is the value itself, no SHL.
 *
 *   field = bits [0,8): lsb=0, width=8, fieldmask=0xFF, clearmask=0xFFFFFF00.
 *   0xFFFFFF00 is NOT a Thumb-2 modified immediate -> gate 1 passes.
 *
 *   Tval = Tx AND 0xFF        (V < 2^8)
 *   Tand = W  AND 0xFFFFFF00
 *   Tor  = Tval OR Tand  ->  BFI Tor, W, Tval ; lsb=0 width=8 */
UT_TEST(test_bf_to_bfi_lsb_zero_folds)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFF, I32)); /* Tval = Tx & 0xFF */
  int and_idx = utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32),
                         utb_imm((int32_t)0xFFFFFF00, I32)); /* Tand = W & clearmask */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(2, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_BFI);
  UT_ASSERT_EQ(utb_vreg(utb_src1(ir, orr)), vreg_temp(0));  /* host word */
  UT_ASSERT_EQ(utb_vreg(utb_src2(ir, orr)), vreg_temp(2));  /* value */
  uint16_t p = ir->bfi_params[ir->compact_instructions[orr].orig_index];
  UT_ASSERT_EQ(p & 0xFF, 0);
  UT_ASSERT_EQ((p >> 8) & 0xFF, 8);
  UT_ASSERT_EQ(utb_op(ir, and_idx), TCCIR_OP_NOP);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* ==================================================================
 * tcc_ir_opt_bitfield_insert_to_bfi  -  negative / no-fire cases
 * ================================================================== */

/* Encodable clearmask (Thumb-2 modified immediate) -> gate 1 skips: BFI's
 * two-address mov could regress, so it must NOT fire.
 *
 *   clearmask = 0xFF000000 clears the contiguous field [0,24) (lsb=0,width=24)
 *   and IS a modified immediate (0xXY000000 rotate form), so gate 1 skips even
 *   though the field is contiguous and the value fits.  Everything else here is
 *   a valid BFI candidate, so this isolates the encodable-clearmask gate. */
UT_TEST(test_bf_to_bfi_encodable_clearmask_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFFFF, I32)); /* Tval = Tx & 0xFFFFFF (<2^24) */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32),
           utb_imm((int32_t)0xFF000000, I32)); /* clearmask encodable, field [0,24) */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(2, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* Non-contiguous fieldmask -> not a single bitfield run -> must NOT fold.
 * clearmask = 0xFF00FF00 (unencodable), fieldmask = 0x00FF00FF (two runs). */
UT_TEST(test_bf_to_bfi_noncontiguous_field_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF00FF00, I32)); /* non-contig field */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* lsb of the SHL must equal the field's lsb.  Field is [8,24) (lsb=8) but the
 * value is shifted by 4 -> mismatch -> must NOT fold. */
UT_TEST(test_bf_to_bfi_shl_lsb_mismatch_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(4, I32)); /* shift 4 != lsb 8 */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF0000FF, I32)); /* field [8,24) */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* Host word provably field-clear (gate 2): the cleared region has no bits in W,
 * so the original insert is a single barrel-folded ORR (1 insn); BFI (2) would
 * regress -> must NOT fold.  Here W = T1 = Tx AND 0x000000FF, and the field is
 * [8,24) (clearmask 0xFF0000FF) which W provably never touches. */
UT_TEST(test_bf_to_bfi_word_field_clear_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(1, I32), utb_temp(0, I32), utb_imm(0xFF, I32));   /* W = Tx & 0xFF, no bits in [8,24) */
  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32)); /* Tval */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(1, I32), utb_imm((int32_t)0xFF0000FF, I32)); /* W & clearmask */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* Value does not provably fit in `width` bits -> the ORR would OR extra bits
 * into non-field positions that BFI drops -> must NOT fold.  Field width=16 but
 * V is a plain undefined TEMP (unbounded). */
UT_TEST(test_bf_to_bfi_value_unbounded_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  /* value = T9 (undefined), shifted by lsb=8; field [8,24) width 16. */
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(9, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF0000FF, I32));
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* The AND (cleared host word) is used more than once -> NOPing it would drop a
 * live value -> single-use gate fails -> must NOT fold. */
UT_TEST(test_bf_to_bfi_and_multiuse_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF0000FF, I32)); /* Tand */
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));
  /* extra second use of Tand (temp 5) -> not single-use */
  utb_emit(ir, TCCIR_OP_ADD, utb_temp(7, I32), utb_temp(5, I32), utb_imm(1, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* A control-flow edge (jump target) between the AND/SHL reads and the OR splits
 * the re-read window -> unsafe -> must NOT fold. */
UT_TEST(test_bf_to_bfi_jump_target_splits_window_no_fold)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF0000FF, I32));
  /* an instruction marked as a jump target between the AND and the OR */
  int mid = utb_emit(ir, TCCIR_OP_ADD, utb_temp(8, I32), utb_temp(0, I32), utb_imm(0, I32));
  ir->compact_instructions[mid].is_jump_target = 1;
  int orr = utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int changes = tcc_ir_opt_bitfield_insert_to_bfi(ir);

  UT_ASSERT_EQ(changes, 0);
  UT_ASSERT_EQ(utb_op(ir, orr), TCCIR_OP_OR);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* BFI idempotence: after the first fold the OR is now a BFI, so a second pass
 * finds no OR to rewrite and reports zero changes. */
UT_TEST(test_bf_to_bfi_idempotent)
{
  TCCIRState *ir = utb_new();
  bf_prep_bfi(ir);

  utb_emit(ir, TCCIR_OP_AND, utb_temp(2, I32), utb_temp(3, I32), utb_imm(0xFFFF, I32));
  utb_emit(ir, TCCIR_OP_SHL, utb_temp(4, I32), utb_temp(2, I32), utb_imm(8, I32));
  utb_emit(ir, TCCIR_OP_AND, utb_temp(5, I32), utb_temp(0, I32), utb_imm((int32_t)0xFF0000FF, I32));
  utb_emit(ir, TCCIR_OP_OR, utb_temp(6, I32), utb_temp(4, I32), utb_temp(5, I32));

  int first = tcc_ir_opt_bitfield_insert_to_bfi(ir);
  UT_ASSERT_EQ(first, 1);
  int second = tcc_ir_opt_bitfield_insert_to_bfi(ir);
  UT_ASSERT_EQ(second, 0);

  bf_free_bfi(ir);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_bitfield)
{
  UT_COVERS("bitfield_insert_extract");
  UT_COVERS("bitfield_insert_to_bfi");

  UT_RUN(test_bf_extract_shr_immediate_value_folds);
  UT_RUN(test_bf_extract_shr_temp_value_folds);
  UT_RUN(test_bf_extract_two_shift_form_folds);
  UT_RUN(test_bf_extract_seff_zero_two_shift_folds);
  UT_RUN(test_bf_extract_and_low_field_folds);
  UT_RUN(test_bf_extract_low_overlaps_window_no_fold);
  UT_RUN(test_bf_extract_value_unbounded_no_fold);
  UT_RUN(test_bf_extract_source_not_or_no_fold);
  UT_RUN(test_bf_extract_lval_dest_no_fold);
  UT_RUN(test_bf_extract_idempotent);

  UT_RUN(test_bf_to_bfi_lsb_positive_folds);
  UT_RUN(test_bf_to_bfi_lsb_zero_folds);
  UT_RUN(test_bf_to_bfi_encodable_clearmask_no_fold);
  UT_RUN(test_bf_to_bfi_noncontiguous_field_no_fold);
  UT_RUN(test_bf_to_bfi_shl_lsb_mismatch_no_fold);
  UT_RUN(test_bf_to_bfi_word_field_clear_no_fold);
  UT_RUN(test_bf_to_bfi_value_unbounded_no_fold);
  UT_RUN(test_bf_to_bfi_and_multiuse_no_fold);
  UT_RUN(test_bf_to_bfi_jump_target_splits_window_no_fold);
  UT_RUN(test_bf_to_bfi_idempotent);
}
