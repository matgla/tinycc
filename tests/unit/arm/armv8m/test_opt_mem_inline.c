/*
 *  test_opt_mem_inline.c - suite for tcc_ir_opt_mem_inline():
 *
 *    int tcc_ir_opt_mem_inline(TCCIRState *ir);
 *
 *  Small constant-size memcpy/memset calls become direct LOAD/STORE quads.
 *  When the destination is a stack slot the expansion re-types the slot
 *  operand to the piece's width, and these tests pin that the slot's OFFSET
 *  survives that narrowing.
 *
 *  A STRUCT-typed operand carries its payload in the split `u.s` encoding
 *  (CType index low half, offset in the HIGH half via u.s.aux_data); every
 *  scalar btype reads it from the full-width u.imm32.  A bare
 *  `slot.btype = INT32` therefore reinterpreted offset O as
 *  (O << 16 | ctype_idx), so `memcpy(hdr.magic, "YAFF", 4)` on a struct at
 *  -92 stored to slot -6029312 and the frame allocator sized the prologue to
 *  cover it (tcc_output_yaff: 6 MiB `sub sp`, process stack overflow).
 *
 *  Pinned: memcpy widths 4/2/1 and memset into a STRUCT-encoded slot, plus
 *  scalar-slot controls that must be unaffected.
 */

#include "ir_build.h"

#include "ut.h"

UT_COVERS("mem_inline");

int tcc_ir_opt_mem_inline(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32

#define TOK_MEMCPY 30
#define TOK_MEMSET 31

/* A SYMREF callee operand bound to `sym` whose token is `tok`. */
static IROperand mic_callee(TCCIRState *ir, Sym *sym, int tok)
{
  sym->v = tok;
  uint32_t sidx = tcc_ir_pool_add_symref(ir, sym, 0, 0);
  return irop_make_symref(0, sidx, 0, 0, 0, I32);
}

/* A STACKOFF slot address in the STRUCT split encoding, exactly as
 * tcc_ir_operand_from_svalue() builds it for a struct lvalue: the CType index
 * in u.s.ctype_idx and the slot offset in u.s.aux_data. */
static IROperand mic_struct_slot(int32_t offset, uint16_t ctype_idx)
{
  IROperand op = utb_stackoff(offset, 0, 0, 0, IROP_BTYPE_STRUCT);
  op.u.s.ctype_idx = ctype_idx;
  op.u.s.aux_data = (int16_t)offset;
  return op;
}

/* Emit a 3-arg mem* call in argument order [p0, p1, p2].
 * Returns the FUNCCALLVOID index. */
static int mic_emit_call(TCCIRState *ir, IROperand callee, int call_id,
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

/* Find the single STORE the expansion emitted, or -1. */
static int mic_find_store(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_STORE)
      return i;
  return -1;
}

/* memcpy(<struct slot at -92>, <global>, n) -> LOAD_INDEXED + STORE to the
 * slot.  The STORE dest must still name slot -92, not -92 << 16. */
static int mic_check_memcpy_width(int n, int expect_btype)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc, src;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = mic_callee(ir, &mc, TOK_MEMCPY);
  IROperand srcsym = utb_symref(ir, &src, 0, 0, 1, I32);

  mic_emit_call(ir, callee, 1, mic_struct_slot(-92, 7), srcsym, utb_imm(n, I32));

  UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 1);

  int ist = mic_find_store(ir);
  UT_ASSERT(ist >= 0);
  IROperand dest = utb_dest(ir, ist);
  UT_ASSERT_EQ(irop_get_tag(dest), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_btype(dest), expect_btype);
  UT_ASSERT(dest.is_lval);
  /* The whole point: the offset moved to the field the new btype reads. */
  UT_ASSERT_EQ(irop_get_stack_offset(dest), -92);
  UT_ASSERT_EQ(dest.u.imm32, -92);
  utb_free(ir);
  return 0;
}

UT_TEST(test_mem_inline_memcpy_struct_slot_word_keeps_offset)
{
  return mic_check_memcpy_width(4, IROP_BTYPE_INT32);
}

UT_TEST(test_mem_inline_memcpy_struct_slot_half_keeps_offset)
{
  return mic_check_memcpy_width(2, IROP_BTYPE_INT16);
}

UT_TEST(test_mem_inline_memcpy_struct_slot_byte_keeps_offset)
{
  return mic_check_memcpy_width(1, IROP_BTYPE_INT8);
}

UT_TEST(test_mem_inline_memset_struct_slot_keeps_offset)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand callee = mic_callee(ir, &ms, TOK_MEMSET);

  mic_emit_call(ir, callee, 1, mic_struct_slot(-92, 7), utb_imm(0, I32), utb_imm(4, I32));

  UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 1);

  int ist = mic_find_store(ir);
  UT_ASSERT(ist >= 0);
  IROperand dest = utb_dest(ir, ist);
  UT_ASSERT_EQ(irop_get_tag(dest), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_btype(dest), IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_get_stack_offset(dest), -92);
  UT_ASSERT_EQ(dest.u.imm32, -92);
  utb_free(ir);
  return 0;
}

/* Control: a slot that was already scalar-typed keeps working unchanged. */
UT_TEST(test_mem_inline_memcpy_scalar_slot_keeps_offset)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym mc, src;
  utb_set_tok_str(TOK_MEMCPY, "memcpy");
  IROperand callee = mic_callee(ir, &mc, TOK_MEMCPY);
  IROperand srcsym = utb_symref(ir, &src, 0, 0, 1, I32);

  mic_emit_call(ir, callee, 1, utb_stackoff(-12, 0, 0, 0, I32), srcsym, utb_imm(4, I32));

  UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 1);

  int ist = mic_find_store(ir);
  UT_ASSERT(ist >= 0);
  IROperand dest = utb_dest(ir, ist);
  UT_ASSERT_EQ(irop_get_tag(dest), IROP_TAG_STACKOFF);
  UT_ASSERT_EQ(irop_get_stack_offset(dest), -12);
  utb_free(ir);
  return 0;
}

/* The retype helper itself: every tag whose payload lives in u.s.aux_data
 * under STRUCT must land in the field its scalar form reads. */
UT_TEST(test_irop_retype_scalar_moves_split_payload)
{
  IROperand slot = mic_struct_slot(-92, 7);
  IROperand narrowed = irop_retype_scalar(slot, IROP_BTYPE_INT32);
  UT_ASSERT_EQ(irop_get_btype(narrowed), IROP_BTYPE_INT32);
  UT_ASSERT_EQ(narrowed.u.imm32, -92);
  UT_ASSERT_EQ(irop_get_stack_offset(narrowed), -92);

  /* already scalar: untouched payload */
  IROperand scalar = utb_stackoff(-12, 0, 0, 0, I32);
  UT_ASSERT_EQ(irop_retype_scalar(scalar, IROP_BTYPE_INT8).u.imm32, -12);

  /* struct vreg: the CType index was the only payload, so it clears */
  IROperand vr = irop_make_vreg(TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_VAR, 3), IROP_BTYPE_STRUCT);
  vr.u.s.ctype_idx = 9;
  UT_ASSERT_EQ(irop_retype_scalar(vr, IROP_BTYPE_INT32).u.imm32, 0);

  /* retyping TO struct is not this helper's job: btype changes, u untouched */
  IROperand keep = irop_retype_scalar(mic_struct_slot(-40, 3), IROP_BTYPE_STRUCT);
  UT_ASSERT_EQ(irop_get_stack_offset(keep), -40);
  return 0;
}

UT_TEST(test_mem_inline_pointer_vregs_in_spill_form)
{
  for (int kind = 0; kind < 2; ++kind)
  {
    TCCIRState *ir = utb_new();
    utb_pools_init(ir);
    static Sym mc;
    utb_set_tok_str(TOK_MEMCPY, "memcpy");
    IROperand callee = mic_callee(ir, &mc, TOK_MEMCPY);
    IROperand src = kind ? utb_param(0, I32) : utb_var(0, I32);
    IROperand dst = kind ? utb_param(1, I32) : utb_var(1, I32);
    src.tag = dst.tag = IROP_TAG_STACKOFF;
    src.is_local = dst.is_local = 1;
    src.is_lval = dst.is_lval = 1;
    src.u.imm32 = -20;
    dst.u.imm32 = -24;
    src.aux = dst.aux = IROP_AUX_UNDERALIGN;
    int call = mic_emit_call(ir, callee, 1, dst, src, utb_imm(4, I32));

    UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 1);
    UT_ASSERT_EQ(ir->compact_instructions[call - 1].op, TCCIR_OP_LOAD_INDEXED);
    UT_ASSERT_EQ(ir->compact_instructions[call].op, TCCIR_OP_STORE_INDEXED);
    IROperand load_base = utb_src1(ir, call - 1);
    IROperand store_base = utb_dest(ir, call);
    UT_ASSERT_EQ(irop_get_tag(load_base), IROP_TAG_VREG);
    UT_ASSERT_EQ(irop_get_tag(store_base), IROP_TAG_VREG);
    UT_ASSERT_EQ(irop_get_vreg(load_base), irop_get_vreg(src));
    UT_ASSERT_EQ(irop_get_vreg(store_base), irop_get_vreg(dst));
    UT_ASSERT(!load_base.is_lval && !store_base.is_lval);
    UT_ASSERT(load_base.aux & IROP_AUX_UNDERALIGN);
    UT_ASSERT(store_base.aux & IROP_AUX_UNDERALIGN);
    utb_free(ir);
  }
  return 0;
}

UT_TEST(test_mem_inline_pointer_vreg_memset)
{
  TCCIRState *ir = utb_new();
  utb_pools_init(ir);
  static Sym ms;
  utb_set_tok_str(TOK_MEMSET, "memset");
  IROperand dst = utb_var(0, I32);
  dst.tag = IROP_TAG_STACKOFF;
  dst.is_local = dst.is_lval = 1;
  dst.u.imm32 = -12;
  int call = mic_emit_call(ir, mic_callee(ir, &ms, TOK_MEMSET), 1, dst, utb_imm(0x5a, I32), utb_imm(4, I32));

  UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 1);
  UT_ASSERT_EQ(ir->compact_instructions[call].op, TCCIR_OP_STORE_INDEXED);
  UT_ASSERT_EQ(irop_get_vreg(utb_dest(ir, call)), irop_get_vreg(dst));
  UT_ASSERT_EQ(irop_get_imm64_ex(ir, utb_src1(ir, call)), 0x5a5a5a5a);
  utb_free(ir);
  return 0;
}

UT_TEST(test_mem_inline_real_pointer_dereferences_still_decline)
{
  for (int kind = 0; kind < 3; ++kind)
  {
    TCCIRState *ir = utb_new();
    utb_pools_init(ir);
    static Sym mc;
    utb_set_tok_str(TOK_MEMCPY, "memcpy");
    IROperand src = kind == 0 ? utb_stackoff(-12, 1, 0, 0, I32) : utb_lval(utb_var(0, I32));
    if (kind == 2)
    {
      src.tag = IROP_TAG_STACKOFF;
      src.is_local = src.is_llocal = 1;
    }
    int call = mic_emit_call(ir, mic_callee(ir, &mc, TOK_MEMCPY), 1, utb_temp(0, I32), src, utb_imm(4, I32));
    UT_ASSERT_EQ(tcc_ir_opt_mem_inline(ir), 0);
    UT_ASSERT_EQ(ir->compact_instructions[call].op, TCCIR_OP_FUNCCALLVOID);
    utb_free(ir);
  }
  return 0;
}
