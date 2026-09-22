/*
 * TinyCC - Tiny C Compiler
 *
 * Scalar replacement of small frame objects.
 *
 * A struct local the frontend put in the frame stays there: every field read
 * is a load from the stack and every field write a store, and each call that
 * takes it by value copies it first.  The Zig C backend builds everything out
 * of such locals -- `Type`, `Value`, `InternPool.Index` are one-word structs
 * -- so most of the Zig compiler's stack traffic is to objects whose address
 * never leaves the function.
 *
 * An object qualifies when every reference to it is a 32-bit load or ASSIGN
 * reading one of its words, a 32-bit STORE writing one, or the whole of a
 * four-byte, float-free object passed to or returned from a call -- which the
 * AAPCS moves in a core register exactly like an int.  Each word it uses then
 * becomes a fresh VAR, which SSA renames like any other scalar.  Any other
 * reference -- its address, a narrower or wider access, a block copy --
 * leaves the object in memory.
 *
 * The rule is kept that narrow on purpose.  Letting a word be read or written
 * in any operand, and folding `*(&obj + k)` into direct accesses first so more
 * objects qualify, both made the Zig compiler larger: the extra values mostly
 * end up spilled, and the spill slots sit nearest sp, pushing the frame's hot
 * objects out of the short ldr/str reach.
 *
 * Runs on the flat IR just before SSA construction.
 */

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"

#define SRA_MAX_WORDS 9 /* 32 bytes, plus the guard word frame_record adds */

typedef struct
{
  int lo, hi;  /* the object's extent, from tcc_ir_frame_object_at */
  int refs;    /* every operand a live instruction names it with */
  int ok_refs; /* those a word VAR can replace */
  int32_t var[SRA_MAX_WORDS];
} SraUnit;

/* A frame slot the backend addresses directly, the same test frame.c uses. */
static int sra_concrete(IROperand op, int *off)
{
  if (irop_is_none(op) || irop_get_vreg(op) >= 0 || op.is_param)
    return 0;
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_STACKOFF && !(tag == IROP_TAG_VREG && (op.is_local || op.is_llocal)))
    return 0;
  *off = irop_get_stack_offset(op);
  return 1;
}

static int sra_type_has_float(CType *t, int depth)
{
  if (depth > 8)
    return 1;
  int bt = t->t & VT_BTYPE;
  if (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE)
    return 1;
  if ((t->t & VT_ARRAY) && t->ref)
    return sra_type_has_float(&t->ref->type, depth + 1);
  if (bt == VT_STRUCT && t->ref)
    for (Sym *f = t->ref->next; f; f = f->next)
      if (sra_type_has_float(&f->type, depth + 1))
        return 1;
  return 0;
}

/* A four-byte struct operand that travels in a core register like an int. */
static int sra_struct_is_word(IROperand op)
{
  if (irop_get_btype(op) != IROP_BTYPE_STRUCT || op.is_llocal || op.is_complex)
    return 0;
  CType *ct = irop_get_ctype(op);
  if (!ct)
    return 0;
  int align;
  return type_size(ct, &align) == 4 && align <= 4 && !sra_type_has_float(ct, 0);
}

/* The word of the unit a reference at `off` names, or -1. */
static int sra_word_of(const SraUnit *u, int off)
{
  if (off < u->lo || off >= u->hi || (off - u->lo) & 3 || (off - u->lo) / 4 >= SRA_MAX_WORDS)
    return -1;
  return (off - u->lo) / 4;
}

static int sra_find_unit(SraUnit *units, int nu, int off)
{
  int a = 0, b = nu - 1;
  while (a <= b)
  {
    int m = (a + b) / 2;
    if (off < units[m].lo)
      b = m - 1;
    else if (off >= units[m].hi)
      a = m + 1;
    else
      return m;
  }
  return -1;
}

/* Slot s (0 dest, 1 src1, 2 src2) of an instruction: may a word VAR replace a
 * frame reference there? 1 for a 32-bit word access, 2 for a whole four-byte
 * struct moved like an int. */
static int sra_slot_kind(TCCIRState *ir, TccIrOp op, int s, IROperand o)
{
  if (!o.is_lval || o.is_llocal || o.is_complex || tcc_ir_access_is_volatile(ir, o))
    return 0;
  int bt = irop_get_btype(o);
  if (bt == IROP_BTYPE_INT32 &&
      ((s == 1 && (op == TCCIR_OP_LOAD || op == TCCIR_OP_ASSIGN)) || (s == 0 && op == TCCIR_OP_STORE)))
    return 1;
  if (bt == IROP_BTYPE_STRUCT && sra_struct_is_word(o) &&
      ((s == 1 && (op == TCCIR_OP_FUNCPARAMVAL || op == TCCIR_OP_RETURNVALUE)) ||
       (s == 0 && op == TCCIR_OP_FUNCCALLVAL)))
    return 2;
  return 0;
}

/* The ops that keep a fourth operand at operand_base + 3 (frame.c's list). */
static int sra_op_has_slot3(int op)
{
  return op == TCCIR_OP_MLA || op == TCCIR_OP_LOAD_INDEXED || op == TCCIR_OP_STORE_INDEXED || op == TCCIR_OP_SELECT;
}

/* Operand slot s (0-3) of q, or IROP_NONE. */
static IROperand sra_operand(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0 && irop_config[q->op].has_dest)
    return tcc_ir_op_get_dest(ir, q);
  if (s == 1 && irop_config[q->op].has_src1)
    return tcc_ir_op_get_src1(ir, q);
  if (s == 2 && irop_config[q->op].has_src2)
    return tcc_ir_op_get_src2(ir, q);
  if (s == 3 && sra_op_has_slot3(q->op) && q->operand_base + 3 < (uint32_t)ir->iroperand_pool_count)
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

static int sra_function_eligible(TCCIRState *ir)
{
  if (tcc_state->do_debug || tcc_state->do_bounds_check)
    return 0;
  if (ir->has_static_chain || ir->captured_count > 0 || tcc_state->nb_nested_funcs > 0)
    return 0;
  if (tcc_ir_calls_returns_twice(ir))
    return 0;
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    switch (ir->compact_instructions[i].op)
    {
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_OUTPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_NL_SETJMP:
    case TCCIR_OP_VLA_ALLOC:
    case TCCIR_OP_VLA_SP_SAVE:
    case TCCIR_OP_VLA_SP_RESTORE:
      return 0;
    default:
      break;
    }
  }
  return 1;
}

int tcc_ir_opt_sra(TCCIRState *ir)
{
  if (!ir || !tcc_state || tcc_state->optimize <= 0 || ir->frame_relaid || ir->frame_obj_count == 0)
    return 0;
  if (!sra_function_eligible(ir))
    return 0;

  /* Units: the frame objects' merged extents, sorted by start. */
  int lo, hi;
  tcc_ir_frame_object_at(ir, ir->frame_objs[0], &lo, &hi); /* builds ir->frame_index */
  int nu = ir->frame_index_n;
  if (nu <= 0)
    return 0;
  SraUnit *u = tcc_mallocz(sizeof(SraUnit) * nu);
  for (int k = 0; k < nu; k++)
  {
    u[k].lo = ir->frame_index[2 * k];
    u[k].hi = ir->frame_index[2 * k + 1];
    for (int w = 0; w < SRA_MAX_WORDS; w++)
      u[k].var[w] = -1;
  }

  /* Every reference a live instruction makes, the fourth operand slot of
   * MLA / indexed accesses / SELECT included: an object keeps its memory
   * unless all of them can be replaced. */
  const int n = ir->next_instruction_index;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0)
        continue;
      u[k].refs++;
      int kind = s < 3 ? sra_slot_kind(ir, q->op, s, o) : 0;
      if (kind && sra_word_of(&u[k], off) >= 0 && (kind == 1 || off == u[k].lo))
        u[k].ok_refs++;
    }
  }

  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 3; s++)
    {
      IROperand o = sra_operand(ir, q, s);
      int off, k;
      if (!sra_concrete(o, &off) || (k = sra_find_unit(u, nu, off)) < 0 || u[k].ok_refs != u[k].refs)
        continue;
      int w = sra_word_of(&u[k], off);
      if (u[k].var[w] < 0)
        u[k].var[w] = tcc_ir_vreg_alloc_var(ir);
      int32_t v = u[k].var[w];
      IRLiveInterval *iv = tcc_ir_get_live_interval(ir, v);
      IROperand rep = irop_make_stackoff(v, iv ? iv->original_offset : 0, 1, 0, 0, IROP_BTYPE_INT32);
      rep.is_unsigned = irop_get_btype(o) == IROP_BTYPE_STRUCT ? 1 : o.is_unsigned;
      rep.aux = IROP_AUX_NONVOLATILE;
      if (s == 0)
        tcc_ir_set_dest(ir, i, rep);
      else if (s == 1)
        tcc_ir_set_src1(ir, i, rep);
      else
        tcc_ir_set_src2(ir, i, rep);
      changes++;
    }
  }

  tcc_free(u);
  return changes;
}
