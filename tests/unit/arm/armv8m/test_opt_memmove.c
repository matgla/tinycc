/*
 *  test_opt_memmove.c - invariant suite for ir/opt.c :: tcc_ir_opt_memmove_to_indexed_stores
 *
 *  The pass folds `memcpy/memmove(dst, &stack_tmp, N<=64)` when the bytes of
 *  `stack_tmp` are fully established by preceding STOREs in the same basic
 *  block: it rewrites those STOREs to target `dst` (direct stack offset) or to
 *  STORE_INDEXED on a `dst` pointer vreg, then drops the call.
 *
 *  These are INVARIANT tests (bug-hunting): they assert what the pass *must*
 *  guarantee from first principles, not merely what it currently does:
 *
 *    INV-A  a folded call is eliminated (FUNCCALL* -> NOP).
 *    INV-B  byte preservation: after a fold, the rewritten stores cover every
 *           byte of [dst_base, dst_base+N) exactly once (no byte lost,
 *           duplicated, or written out of range).
 *    INV-C  the call SURVIVES when the source temp is not fully covered.
 *    INV-D  the call SURVIVES when dst and src ranges overlap.
 *    INV-E  the call SURVIVES when the temp is read/aliased elsewhere.
 *    INV-F  size>Nmax and non-memcpy callees are left untouched.
 *
 *  A failure of any of these is a real correctness bug (lost write, dangling
 *  vreg, or a missed/incorrect fold).
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_opt_memmove_to_indexed_stores(TCCIRState *ir);
int ir_opt_store_btype_size_bytes(int btype); /* ir/opt_alias.c */

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

#define TOK_MEMCPY 30
#define TOK_MEMMOVE 31
#define TOK_FOO 32

/* ---------------------------------------------------------- helpers */

static IROperand utb_callee(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* Emit a 3-arg memcpy/memmove FUNCCALLVOID; returns the call index. */
static int emit_memcpy(TCCIRState *ir, IROperand callee, int call_id,
                       IROperand dst, IROperand src, int size)
{
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, dst,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, src,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(size, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(call_id, 2), I32));
  return utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                  utb_imm((int32_t)TCCIR_ENCODE_CALL(call_id, 3), I32));
}

/* Bitmap of bytes in [base, base+total) written by STORE/STORE_INDEXED whose
 * dest is a local stack offset landing inside the range.  Mirrors the pass's
 * own coverage accounting so a divergence flags a lost/duplicated byte. */
static uint64_t utb_range_coverage(TCCIRState *ir, int base, int total)
{
  uint64_t mask = 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op != TCCIR_OP_STORE && q->op != TCCIR_OP_STORE_INDEXED)
      continue;
    IROperand d = tcc_ir_op_get_dest(ir, q);
    if (irop_get_tag(d) != IROP_TAG_STACKOFF || !d.is_local)
      continue;
    int off = (int)irop_get_imm64_ex(ir, d);
    int sz = ir_opt_store_btype_size_bytes(irop_get_btype(d));
    if (sz <= 0)
      continue;
    for (int b = off; b < off + sz; b++)
      if (b >= base && b < base + total)
        mask |= ((uint64_t)1) << (b - base);
  }
  return mask;
}

static uint64_t want_mask(int total)
{
  return (total >= 64) ? ~(uint64_t)0 : (((uint64_t)1 << total) - 1);
}

/* ============================================ positive folds (INV-A/B) */

UT_TEST(test_memmove_stackoff_two_stores_full_coverage)
{
  /* src tmp @100 (8 bytes), dst @200. Two INT32 stores cover the temp fully.
   * After fold: the stores are relocated to dst (200/204), the call is NOPed,
   * and src range [100,108) is no longer written at all. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(104, 1, 0, 0, I32), utb_imm(2, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 8);

  int changes = tcc_ir_opt_memmove_to_indexed_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP); /* INV-A */
  /* INV-B: dst fully covered, src no longer written. */
  UT_ASSERT_EQ(utb_range_coverage(ir, 200, 8), want_mask(8));
  UT_ASSERT_EQ(utb_range_coverage(ir, 100, 8), (uint64_t)0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_stackoff_single_int64_store)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMMOVE, "memmove");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMMOVE);

  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(40, 1, 0, 0, I64), utb_imm(0, I64), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(80, 0, 0, 0, I32),
                          utb_stackoff(40, 0, 0, 0, I32), 8);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_range_coverage(ir, 80, 8), want_mask(8));
  /* The single store now targets offset 80 with INT64 width. */
  UT_ASSERT_EQ(utb_op(ir, 0), TCCIR_OP_STORE);
  UT_ASSERT_EQ(utb_dest(ir, 0).u.imm32, 80);
  UT_ASSERT_EQ(irop_get_btype(utb_dest(ir, 0)), IROP_BTYPE_INT64);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_vreg_dst_becomes_store_indexed)
{
  /* dst is a pointer vreg (T0, defined by ASSIGN T0=P1). The fold rewrites
   * the contributing STORE to STORE_INDEXED on T0 with byte index 0, scale 0. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(0, I32), utb_param(1, I32), UTB_NONE); /* T0 = P1 */
  int store = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32),
                       utb_imm(7, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_temp(0, I32) /* dst vreg */,
                          utb_stackoff(100, 0, 0, 0, I32), 4);

  int changes = tcc_ir_opt_memmove_to_indexed_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_op(ir, store), TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(utb_vreg(utb_dest(ir, store)),
               irop_get_vreg(utb_temp(0, I32)));
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_src2(ir, store)), 0); /* byte index */
  UT_ASSERT_EQ((int)irop_get_imm64_ex(ir, utb_op4(ir, store)), 0);  /* scale = 0 */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_size64_full_coverage_edge)
{
  /* total_size == 64 -> want_mask == ~0 (the all-ones edge). 8 INT64 stores. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  for (int k = 0; k < 8; k++)
    utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100 + 8 * k, 1, 0, 0, I64),
             utb_imm(k, I64), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(300, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 64);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_range_coverage(ir, 300, 64), want_mask(64)); /* == ~0 */
  utb_free(ir);
  return 0;
}

/* ============================================ survival cases (INV-C/D/E/F) */

UT_TEST(test_memmove_partial_coverage_keeps_call)
{
  /* Only [100,104) stored; [104,108) is missing -> call must survive. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 8);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID); /* INV-C */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_dst_src_overlap_keeps_call)
{
  /* dst @104 overlaps src @100 (size 8). The non-overlap assumption fails. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(104, 1, 0, 0, I32), utb_imm(2, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(104, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 8);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID); /* INV-D */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_temp_read_elsewhere_keeps_call)
{
  /* A LOAD of the temp bytes anywhere makes the prior value observable ->
   * the global aliasing scan must refuse the fold. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_LOAD, utb_temp(5, I32), utb_lval(utb_stackoff(100, 0, 0, 0, I32)), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 4);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID); /* INV-E */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_size_over_cap_keeps_call)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 65); /* > 64 */

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID); /* INV-F */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_non_memcpy_callee_keeps_call)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym foo;
  utb_set_tok_str(TOK_FOO, "foo");
  IROperand callee = utb_callee(ir, &foo, TOK_FOO);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 4);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID); /* INV-F */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_funccallval_with_reader_keeps_call)
{
  /* memcpy returns dst; if the result vreg is read, dropping the call would
   * leave a dangling vreg. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(200, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(100, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(4, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 2), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVAL, utb_temp(0, I32), callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 3), I32));
  /* Reader of the result: */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVAL);
  utb_free(ir);
  return 0;
}

/* ====== indirect contributing stores (trace patterns b/c) — INV-B ====== */

UT_TEST(test_memmove_indirect_store_through_vreg_preserves_bytes)
{
  /* Contributing store is `STORE [T0] = #1` where T0 = LEA StackLoc[100]
  * (pattern b).  The pass must trace T0 -> offset 100, then rebuild the
  * store as a plain STORE at dst_base+0.  Byte coverage must be exact. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(100, 0, 0, 0, I32), UTB_NONE); /* T0 = &tmp */
  utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_temp(0, I32)), utb_imm(1, I32), UTB_NONE);     /* [T0]=1 */
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 4);

  int changes = tcc_ir_opt_memmove_to_indexed_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_range_coverage(ir, 200, 4), want_mask(4)); /* INV-B */
  UT_ASSERT_EQ(utb_range_coverage(ir, 100, 4), (uint64_t)0);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_store_indexed_source_preserves_bytes)
{
  /* Contributing store is STORE_INDEXED [T0, idx=4, scale=0] where T0 =
  * LEA StackLoc[100] (pattern c).  Effective offset = 100+4 = 104; after
  * fold it must land at dst_base+(104-100) = dst_base+4. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  /* T0 = &tmp[0];  cover [100..104) directly and [104..108) via STORE_INDEXED */
  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(100, 0, 0, 0, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  /* STORE_INDEXED T0[4]=2, scale 0  (dest=T0 base, src1=val, src2=idx, op4=scale) */
  utb_emit4(ir, TCCIR_OP_STORE_INDEXED, utb_temp(0, I32), utb_imm(2, I32),
            utb_imm(4, I32), utb_imm(0, I32));
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 8);

  int changes = tcc_ir_opt_memmove_to_indexed_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_range_coverage(ir, 200, 8), want_mask(8)); /* bytes 0-7 at dst */
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_dead_pre_store_is_noped)
{
  /* A STORE into the src range strictly before the earliest contributing
  * store is dead (overwritten by the contributing stores). The pass must
  * NOP it during the rewrite — otherwise it would still write the stale
  * value into the now-relocated slot. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  int dead = utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(99, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  int icall = emit_memcpy(ir, callee, 1, utb_stackoff(200, 0, 0, 0, I32),
                          utb_stackoff(100, 0, 0, 0, I32), 4);

  int changes = tcc_ir_opt_memmove_to_indexed_stores(ir);

  UT_ASSERT_EQ(changes, 1);
  UT_ASSERT_EQ(utb_op(ir, dead), TCCIR_OP_NOP);          /* stale store eliminated */
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_NOP);
  UT_ASSERT_EQ(utb_range_coverage(ir, 200, 4), want_mask(4));
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_lea_addr_reused_elsewhere_keeps_call)
{
  /* src address is taken into a vreg (LEA) that is ALSO used by another
  * instruction.  Folding would relocate writes the other reader could
  * observe -> the LEA single-use check must refuse. */
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = utb_callee(ir, &mc, TOK_MEMCPY);

  utb_emit(ir, TCCIR_OP_LEA, utb_temp(0, I32), utb_stackoff(100, 0, 0, 0, I32), UTB_NONE); /* &tmp */
  utb_emit(ir, TCCIR_OP_STORE, utb_stackoff(100, 1, 0, 0, I32), utb_imm(1, I32), UTB_NONE);
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_stackoff(200, 0, 0, 0, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 0), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_temp(0, I32) /* src via LEA vreg */,
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 1), I32));
  utb_emit(ir, TCCIR_OP_FUNCPARAMVAL, UTB_NONE, utb_imm(4, I32),
           utb_imm((int32_t)TCCIR_ENCODE_PARAM(1, 2), I32));
  int icall = utb_emit(ir, TCCIR_OP_FUNCCALLVOID, UTB_NONE, callee,
                       utb_imm((int32_t)TCCIR_ENCODE_CALL(1, 3), I32));
  /* A second consumer of the LEA's result vreg (not the memcpy PARAM1): */
  utb_emit(ir, TCCIR_OP_ASSIGN, utb_temp(1, I32), utb_temp(0, I32), UTB_NONE);

  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  UT_ASSERT_EQ(utb_op(ir, icall), TCCIR_OP_FUNCCALLVOID);
  utb_free(ir);
  return 0;
}

UT_TEST(test_memmove_empty_ir_no_crash)
{
  TCCIRState *ir = utb_new();
  UT_ASSERT_EQ(tcc_ir_opt_memmove_to_indexed_stores(ir), 0);
  utb_free(ir);
  return 0;
}

/* ------------------------------------------------------------------ suite */

UT_SUITE(opt_memmove)
{
  UT_COVERS("memmove_to_indexed_stores");
  UT_RUN(test_memmove_stackoff_two_stores_full_coverage);
  UT_RUN(test_memmove_stackoff_single_int64_store);
  UT_RUN(test_memmove_vreg_dst_becomes_store_indexed);
  UT_RUN(test_memmove_size64_full_coverage_edge);
  UT_RUN(test_memmove_partial_coverage_keeps_call);
  UT_RUN(test_memmove_dst_src_overlap_keeps_call);
  UT_RUN(test_memmove_temp_read_elsewhere_keeps_call);
  UT_RUN(test_memmove_size_over_cap_keeps_call);
  UT_RUN(test_memmove_non_memcpy_callee_keeps_call);
  UT_RUN(test_memmove_funccallval_with_reader_keeps_call);
  UT_RUN(test_memmove_indirect_store_through_vreg_preserves_bytes);
  UT_RUN(test_memmove_store_indexed_source_preserves_bytes);
  UT_RUN(test_memmove_dead_pre_store_is_noped);
  UT_RUN(test_memmove_lea_addr_reused_elsewhere_keeps_call);
  UT_RUN(test_memmove_empty_ir_no_crash);
}
