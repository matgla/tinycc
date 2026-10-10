/*
 * TinyCC - Tiny C Compiler
 *
 * Zero-extensions of values that are already zero-extended.
 *
 * A narrow value is widened with an explicit `AND #0xFFFF` / `UBFX #0,#16`
 * wherever the frontend or a pass cannot see that its upper bits are clear:
 *
 *   T5 <-- StackLoc[-8] [LOAD]          ldrh (a u16 error code)
 *   TEST_ZERO T5
 *   JMP to L if "=="
 *   T20 <-- T5 UBFX #512                uxth, in the next block
 *
 * In the Zig compiler built as C that was 36k UXTH/UXTB of a value a LDRH, a
 * UXTH or a phi of such values had produced -- the error union's u16 read in
 * the block after the one that tested it, or at the join of two paths.
 *
 * This pass computes, for every TEMP, a mask of the bits its register image
 * may have set, and turns an extension whose operand has no bit outside the
 * extended width into a copy.  The masks come from what the code generator
 * emits for the defining instruction, never from the operand types alone:
 *
 *   - a narrow unsigned LOAD is LDRB/LDRH (the emitter picks the width and
 *     signedness from the source operand; LOAD_INDEXED / LOAD_POSTINC from the
 *     destination), a MOV plus UXTB/UXTH for a register source;
 *   - AND / UBFX / SHR / SETIF / constants bound the result by their operands;
 *   - a copy (ASSIGN, a STORE into a TEMP) passes its source's bits through.
 *     A spilled TEMP's slot holds its register image as a word, except that a
 *     STORE into a narrow TEMP extends to a word by the destination's type
 *     first: for a signed destination that can set the upper bits, so such a
 *     STORE only keeps the mask when the sign bit is known clear.
 *
 * A TEMP read under a join is the union of its phi's incomings; an undefined
 * incoming (a path that never assigned the variable) holds whatever the
 * register held, so it makes the whole phi unknown.  A TEMP with no definition
 * at all is unknown for the same reason.  Masks start empty and only grow, so
 * loops converge (optimistically) in a few sweeps.
 *
 * The same pass publishes what r0 holds at every return of a static function
 * (FuncAttr.func_ret_zext) for the direct calls compiled after it, drops the
 * second of two identical extensions on one path, removes a BFI that
 * reinserts a field its word already holds, and narrows a word load whose
 * only reader cuts it to a byte or halfword.
 *
 * Runs inside the allocator, after the SSA passes and before phi resolution:
 * the IR is final apart from the copies phi resolution inserts, and the phis
 * are still explicit.  Knobs (TCC_DISABLE_PASS): ra:known_ext (all of it),
 * ra:known_ext_ret / ra:known_ext_call (return classes, published / used),
 * ra:known_ext_cse, ra:known_ext_bfi, ra:known_ext_narrow_load.
 */

#include "ir.h"

#define KE_ALL 0xFFFFFFFFu
#define KE_MAX_SWEEPS 64

typedef struct
{
  TCCIRState *ir;
  uint32_t *bits;   /* [nt]: bits the TEMP's register image may have set */
  uint8_t *defined; /* [nt]: has an instruction or phi definition */
  uint8_t *poison;  /* [nt]: written or read in a way this pass does not model */
  uint8_t *ndefs;   /* [nt]: definitions, saturating at 2 */
  int *defidx;      /* [nt]: the defining instruction of a single-def TEMP, or -1 */
  uint8_t *nuses;   /* [nt]: reads, phi operands included, saturating at 2 */
  int nt;
  /* A direct call of the function itself reads what its returns may hold,
   * assumed as it is being computed: a return's mask built from recursive
   * results within it holds by induction on the recursion depth. */
  int self_ok;
  uint32_t self_ret;
} KnownExt;

static int ke_temp_pos(const KnownExt *ke, IROperand op)
{
  int32_t vr = irop_get_vreg(op);
  if (vr < 0 || TCCIR_DECODE_VREG_TYPE(vr) != TCCIR_VREG_TYPE_TEMP)
    return -1;
  int pos = TCCIR_DECODE_VREG_POSITION(vr);
  return pos < ke->nt ? pos : -1;
}

static int ke_is_scalar32(IROperand op)
{
  int bt = irop_get_btype(op);
  return !op.is_complex && (bt == IROP_BTYPE_INT32 || bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16);
}

/* Bits a value operand may have set when an instruction reads it. */
static uint32_t ke_val(const KnownExt *ke, IROperand op)
{
  if (irop_is_none(op) || op.is_lval || op.is_llocal)
    return KE_ALL;
  int tag = irop_get_tag(op);
  if (tag == IROP_TAG_IMM32 && !op.is_sym && ke_is_scalar32(op))
    return (uint32_t)irop_get_imm32(op);
  if (tag != IROP_TAG_VREG || irop_is_64bit(op))
    return KE_ALL;
  int pos = ke_temp_pos(ke, op);
  if (pos < 0 || ke->poison[pos] || !ke->defined[pos])
    return KE_ALL;
  return ke->bits[pos];
}

static uint32_t ke_width_mask(int btype)
{
  return btype == IROP_BTYPE_INT8 ? 0xFFu : btype == IROP_BTYPE_INT16 ? 0xFFFFu : KE_ALL;
}

/* A narrow load zero-extends when the emitter reads it unsigned. */
static uint32_t ke_load_mask(IROperand typed)
{
  if (typed.is_complex || !typed.is_unsigned)
    return KE_ALL;
  return ke_width_mask(irop_get_btype(typed));
}

/* Every bit at or below the highest one of m. */
static uint32_t ke_smear(uint32_t m)
{
  m |= m >> 1;
  m |= m >> 2;
  m |= m >> 4;
  m |= m >> 8;
  m |= m >> 16;
  return m;
}

static int ke_read_imm(IROperand op, int32_t *out)
{
  if (op.is_lval || op.is_sym || irop_get_tag(op) != IROP_TAG_IMM32 || !ke_is_scalar32(op))
    return 0;
  *out = irop_get_imm32(op);
  return 1;
}

/* FuncAttr.func_ret_zext: what r0 holds at every return of a function. */
static const uint32_t ke_ret_masks[4] = {KE_ALL, 0x1u, 0xFFu, 0xFFFFu};

uint32_t tcc_ir_ret_zext_mask(int ret_class)
{
  return ke_ret_masks[ret_class & 3];
}

static int ke_ret_class(uint32_t m)
{
  for (int c = 1; c < 4; c++)
    if (!(m & ~ke_ret_masks[c]))
      return c;
  return 0;
}

/* The function a call names directly, when this TU's definition of it is the
 * one the call reaches: internal or non-default visibility, not weak.  Its
 * published return class then describes the code the call runs. */
static Sym *ke_direct_callee(TCCIRState *ir, IROperand callee)
{
  if (irop_get_tag(callee) != IROP_TAG_SYMREF || callee.is_lval || tcc_ir_opt_pass_disabled("ra:known_ext_call"))
    return NULL;
  IRPoolSymref *sr = irop_get_symref_ex(ir, callee);
  if (!sr || !sr->sym || sr->addend != 0)
    return NULL;
  Sym *f = sr->sym;
  if ((f->type.t & VT_BTYPE) != VT_FUNC || !f->type.ref || f->a.weak)
    return NULL;
  if (!(f->type.t & VT_STATIC) && f->a.visibility == STV_DEFAULT)
    return NULL;
  return f;
}

/* A BFI's field, when it is a plain 32-bit insert. */
static int ke_bfi_field(TCCIRState *ir, IRQuadCompact *q, int *lsb, int *width)
{
  uint16_t params = tcc_ir_bfi_params_at(ir, q);
  *lsb = params & 0xFF;
  *width = params >> 8;
  if (*width <= 0 || *lsb + *width > 32)
    return 0;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  return !irop_is_64bit(d) && !irop_is_64bit(s1) && !irop_is_64bit(s2);
}

/* The bits instruction q may leave in its plain TEMP destination. */
static uint32_t ke_transfer(const KnownExt *ke, IRQuadCompact *q)
{
  TCCIRState *ir = ke->ir;
  if (q->op == TCCIR_OP_BFI && !tcc_ir_barrel_shift_at(ir, q))
  {
    int lsb, width;
    if (!ke_bfi_field(ir, q, &lsb, &width))
      return KE_ALL;
    uint32_t field = width == 32 ? KE_ALL : (1u << width) - 1u;
    return (ke_val(ke, tcc_ir_op_get_src1(ir, q)) & ~(field << lsb)) |
           ((ke_val(ke, tcc_ir_op_get_src2(ir, q)) & field) << lsb);
  }
  if (tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q) ||
      tcc_ir_bfi_params_at(ir, q))
    return KE_ALL;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
  IROperand s2 = irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
  int32_t k;
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
    return ke_val(ke, s1);
  case TCCIR_OP_LOAD:
  {
    /* Memory: LDRB/LDRH/LDRSB/LDRSH by the source operand.  A register or
     * spilled vreg read at a narrow type: MOV plus UXTB/UXTH or SXTB/SXTH by
     * the source operand (tcc_gen_machine_load_mop).  An immediate or a
     * symbol address is materialised as it stands. */
    if (irop_is_64bit(s1))
      return KE_ALL;
    if (s1.is_lval)
      return ke_load_mask(s1);
    int bt = irop_get_btype(s1);
    uint32_t v = ke_val(ke, s1);
    if (irop_get_tag(s1) != IROP_TAG_VREG || s1.is_complex || (bt != IROP_BTYPE_INT8 && bt != IROP_BTYPE_INT16))
      return v;
    uint32_t w = ke_width_mask(bt);
    if (s1.is_unsigned)
      return v & w;
    return (v & ~(w >> 1)) ? KE_ALL : v;
  }
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
    return ke_load_mask(d);
  case TCCIR_OP_STORE:
  {
    /* A STORE into a TEMP is a register copy, or LDRB/LDRH by the source for
     * a memory source; spilled, a narrow destination is first extended to a
     * word by its own type (tcc_gen_machine_store_mop). */
    uint32_t m = s1.is_lval ? ke_load_mask(s1) : ke_val(ke, s1);
    int bt = irop_get_btype(d);
    if ((bt == IROP_BTYPE_INT8 || bt == IROP_BTYPE_INT16) && !d.is_unsigned &&
        (m & ~(ke_width_mask(bt) >> 1)) != 0)
      return KE_ALL;
    return m;
  }
  case TCCIR_OP_AND:
    return ke_val(ke, s1) & ke_val(ke, s2);
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
    return ke_val(ke, s1) | ke_val(ke, s2);
  case TCCIR_OP_SELECT:
    return ke_val(ke, s1) | ke_val(ke, s2);
  case TCCIR_OP_SHL:
  case TCCIR_OP_SHR:
  case TCCIR_OP_SAR:
  {
    if (!ke_read_imm(s2, &k) || k < 0 || k > 31)
      return KE_ALL;
    uint32_t m = ke_val(ke, s1);
    if (q->op == TCCIR_OP_SHL)
      return m << k;
    if (q->op == TCCIR_OP_SHR || !(m & 0x80000000u))
      return m >> k;
    return KE_ALL;
  }
  case TCCIR_OP_UDIV:
    return ke_smear(ke_val(ke, s1));
  case TCCIR_OP_UMOD:
    if (ke_read_imm(s2, &k) && k > 0)
      return ke_smear(ke_val(ke, s1)) & ke_smear((uint32_t)k - 1u);
    return ke_smear(ke_val(ke, s1));
  case TCCIR_OP_UBFX:
  {
    if (!ke_read_imm(s2, &k))
      return KE_ALL;
    int width = (k >> 5) & 0x1F;
    return width > 0 ? (1u << width) - 1u : KE_ALL;
  }
  case TCCIR_OP_SETIF:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
    return 1u;
  case TCCIR_OP_CLZ:
    return 0x3Fu;
  case TCCIR_OP_FUNCCALLVAL:
  {
    Sym *f = ke_direct_callee(ir, s1);
    if (f && f == tcc_state->cur_func_sym)
      return ke->self_ok ? ke->self_ret : KE_ALL;
    return f ? ke_ret_masks[f->type.ref->f.func_ret_zext] : KE_ALL;
  }
  default:
    return KE_ALL;
  }
}

/* Slots an instruction may write besides its plain destination: the base of
 * a post-increment access, anything inline asm touches. */
static int ke_writes_operands(int op)
{
  return op == TCCIR_OP_LOAD_POSTINC || op == TCCIR_OP_STORE_POSTINC || op == TCCIR_OP_INLINE_ASM ||
         op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_ASM_OUTPUT;
}

static IROperand ke_slot(TCCIRState *ir, IRQuadCompact *q, int s)
{
  if (s == 0)
    return irop_config[q->op].has_dest ? tcc_ir_op_get_dest(ir, q) : IROP_NONE;
  if (s == 1)
    return irop_config[q->op].has_src1 ? tcc_ir_op_get_src1(ir, q) : IROP_NONE;
  if (s == 2)
    return irop_config[q->op].has_src2 ? tcc_ir_op_get_src2(ir, q) : IROP_NONE;
  if (tcc_ir_op_is_mac(q->op) || q->op == TCCIR_OP_LOAD_INDEXED || q->op == TCCIR_OP_STORE_INDEXED ||
      q->op == TCCIR_OP_SELECT)
    return ir->iroperand_pool[q->operand_base + 3];
  return IROP_NONE;
}

/* The TEMP a plain (register) destination defines, or -1. */
static int ke_plain_def(const KnownExt *ke, IRQuadCompact *q)
{
  if (!irop_config[q->op].has_dest)
    return -1;
  IROperand d = tcc_ir_op_get_dest(ke->ir, q);
  if (d.is_lval || d.is_llocal || irop_get_tag(d) != IROP_TAG_VREG)
    return -1;
  return ke_temp_pos(ke, d);
}

/* `X AND #m` / `X UBFX #0,#w`: the TEMP operand and the mask it is cut to, or
 * -1 when q is not a plain zero-extension of a TEMP. */
static int ke_ext_operand(const KnownExt *ke, IRQuadCompact *q, IROperand *src, uint32_t *mask)
{
  TCCIRState *ir = ke->ir;
  if (q->op != TCCIR_OP_AND && q->op != TCCIR_OP_UBFX)
    return -1;
  if (tcc_ir_barrel_shift_at(ir, q) || tcc_ir_shift64_dead_half_at(ir, q) || tcc_ir_zero_half64_at(ir, q))
    return -1;
  IROperand d = tcc_ir_op_get_dest(ir, q);
  IROperand s1 = tcc_ir_op_get_src1(ir, q);
  IROperand s2 = tcc_ir_op_get_src2(ir, q);
  if (d.is_lval || irop_get_tag(d) != IROP_TAG_VREG || !ke_is_scalar32(d))
    return -1;
  int32_t k;
  if (q->op == TCCIR_OP_UBFX)
  {
    if (!ke_read_imm(s2, &k) || (k & UBFX_HI_HALF) || (k & 0x1F) != 0 || ((k >> 5) & 0x1F) == 0)
      return -1;
    *mask = (1u << ((k >> 5) & 0x1F)) - 1u;
  }
  else if (ke_read_imm(s2, &k))
    *mask = (uint32_t)k;
  else if (ke_read_imm(s1, &k))
  {
    *mask = (uint32_t)k;
    s1 = s2;
  }
  else
    return -1;
  if (s1.is_lval || s1.is_llocal || irop_get_tag(s1) != IROP_TAG_VREG || irop_is_64bit(s1) || !ke_is_scalar32(s1))
    return -1;
  int pos = ke_temp_pos(ke, s1);
  if (pos < 0)
    return -1;
  *src = s1;
  return pos;
}

/* A function whose r0 a published class may describe: an integer of at most
 * a word, or a composite of at most a word (returned in r0, or in s0 when it
 * is a float, which its RETURNVALUE operand then says). */
static int ke_ret_type_ok(Sym *fs)
{
  if (!fs || !fs->type.ref || fs->a.naked)
    return 0;
  /* Only ke_direct_callee's functions have callers that read the class. */
  if (!(fs->type.t & VT_STATIC) && fs->a.visibility == STV_DEFAULT)
    return 0;
  const CType *rt = &fs->type.ref->type;
  if (rt->t & (VT_VECTOR | VT_COMPLEX))
    return 0;
  int bt = rt->t & VT_BTYPE, align;
  if (bt == VT_BYTE || bt == VT_SHORT || bt == VT_INT || bt == VT_BOOL)
    return 1;
  return bt == VT_STRUCT && type_size(rt, &align) <= 4;
}

/* The bits r0 may hold at a return of the body, when they are known.  Every
 * way out of the function has to be a RETURNVALUE whose operand the masks
 * describe: inline asm or __builtin_return may set r0 themselves, and a path
 * that falls off the end (a RETURNVOID, a jump to the end not right after a
 * RETURNVALUE, a last instruction that is not a terminator) returns whatever
 * r0 held. */
static int ke_body_returns(const KnownExt *ke, uint32_t *ret)
{
  TCCIRState *ir = ke->ir;
  const int n = ir->next_instruction_index;
  uint32_t m = 0;
  int nret = 0, prev = TCCIR_OP_NOP, last = TCCIR_OP_NOP;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    switch (q->op)
    {
    case TCCIR_OP_NOP:
      continue;
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_BUILTIN_RETURN:
    case TCCIR_OP_RETURNVOID:
      return 0;
    case TCCIR_OP_RETURNVALUE:
      m |= ke_val(ke, tcc_ir_op_get_src1(ir, q));
      nret++;
      break;
    case TCCIR_OP_JUMP:
    case TCCIR_OP_JUMPIF:
      if (tcc_ir_op_dest_imm32(ir, q) >= n && !(q->op == TCCIR_OP_JUMP && prev == TCCIR_OP_RETURNVALUE))
        return 0;
      break;
    default:
      break;
    }
    prev = last = q->op;
  }
  if (!nret || (last != TCCIR_OP_RETURNVALUE && last != TCCIR_OP_JUMP && last != TCCIR_OP_IJUMP &&
                last != TCCIR_OP_TRAP && last != TCCIR_OP_SWITCH_TABLE))
    return 0;
  *ret = m;
  return 1;
}

/* The single-definition TEMP a value is a copy of, through ASSIGNs, or -1. */
static int ke_copy_root(const KnownExt *ke, IROperand op)
{
  for (int guard = 0; guard < 16; guard++)
  {
    if (op.is_lval || op.is_llocal || irop_get_tag(op) != IROP_TAG_VREG || irop_is_64bit(op))
      return -1;
    int pos = ke_temp_pos(ke, op);
    if (pos < 0 || ke->ndefs[pos] != 1)
      return -1;
    int d = ke->defidx[pos];
    if (d < 0 || ke->ir->compact_instructions[d].op != TCCIR_OP_ASSIGN)
      return pos;
    op = tcc_ir_op_get_src1(ke->ir, &ke->ir->compact_instructions[d]);
  }
  return -1;
}

/* `BFI X, V, #lsb, #w` where V's low w bits are X's own field -- the error
 * union word rebuilt from itself after its u16 was tested: `mov r1,r8;
 * bfi r1,r0,#0,#16` with r0 = `uxth r8`.  The insert is a copy of X. */
static int ke_bfi_is_identity(const KnownExt *ke, IRQuadCompact *q)
{
  TCCIRState *ir = ke->ir;
  int lsb, width;
  if (tcc_ir_barrel_shift_at(ir, q) || !ke_bfi_field(ir, q, &lsb, &width))
    return 0;
  int x = ke_copy_root(ke, tcc_ir_op_get_src1(ir, q));
  int v = ke_copy_root(ke, tcc_ir_op_get_src2(ir, q));
  if (x < 0 || v < 0)
    return 0;
  if (v == x)
    return lsb == 0;
  int d = ke->defidx[v];
  if (d < 0)
    return 0;
  IRQuadCompact *vq = &ir->compact_instructions[d];
  if (tcc_ir_barrel_shift_at(ir, vq) || tcc_ir_shift64_dead_half_at(ir, vq) || tcc_ir_zero_half64_at(ir, vq))
    return 0;
  IROperand a = tcc_ir_op_get_src1(ir, vq), b = tcc_ir_op_get_src2(ir, vq);
  uint32_t field = width == 32 ? KE_ALL : (1u << width) - 1u;
  int32_t k;
  switch (vq->op)
  {
  case TCCIR_OP_UBFX:
    /* bits [k&31, +width') of Y, width' covering the field */
    return ke_read_imm(b, &k) && !(k & UBFX_HI_HALF) && (k & 0x1F) == lsb && ((k >> 5) & 0x1F) >= width &&
           ke_copy_root(ke, a) == x;
  case TCCIR_OP_SHR:
    return ke_read_imm(b, &k) && k == lsb && ke_copy_root(ke, a) == x;
  case TCCIR_OP_AND:
    if (lsb != 0)
      return 0;
    if (ke_read_imm(b, &k))
      return ((uint32_t)k & field) == field && ke_copy_root(ke, a) == x;
    if (ke_read_imm(a, &k))
      return ((uint32_t)k & field) == field && ke_copy_root(ke, b) == x;
    return 0;
  default:
    return 0;
  }
}

/* `T <- [p] (word); X <- T AND #0xFF` with T read nowhere else: the load
 * itself can be the narrow one, `ldrb X,[p]` -- a byte or halfword field of a
 * struct copied as words.  Little-endian: the low bits are the first bytes.
 * A volatile word read stays one. */
static int ke_narrow_load(const KnownExt *ke, IRQuadCompact *q, IROperand src, uint32_t mask)
{
  TCCIRState *ir = ke->ir;
  if (mask != 0xFFu && mask != 0xFFFFu)
    return 0;
  int t = ke_temp_pos(ke, src);
  if (t < 0 || ke->ndefs[t] != 1 || ke->nuses[t] != 1 || ke->defidx[t] < 0)
    return 0;
  int j = ke->defidx[t];
  IRQuadCompact *lq = &ir->compact_instructions[j];
  if (lq->op != TCCIR_OP_LOAD || tcc_ir_barrel_shift_at(ir, lq))
    return 0;
  IROperand m = tcc_ir_op_get_src1(ir, lq);
  if (!m.is_lval || m.is_complex || irop_get_btype(m) != IROP_BTYPE_INT32 || tcc_ir_op_dest_btype(ir, lq) != IROP_BTYPE_INT32 ||
      tcc_ir_access_is_volatile(ir, m))
    return 0;
  if (irop_get_tag(m) == IROP_TAG_STACKOFF && m.vreg_type != 0)
    return 0;
  m = irop_retype_scalar(m, mask == 0xFFu ? IROP_BTYPE_INT8 : IROP_BTYPE_INT16);
  m.is_unsigned = 1;
  tcc_ir_set_src1(ir, j, m);
  (void)q;
  return 1;
}

static int ke_cse_extensions(const KnownExt *ke, IRCFG *cfg)
{
  TCCIRState *ir = ke->ir;
  const int n = ir->next_instruction_index;
  int *head = tcc_malloc(sizeof(int) * ke->nt);
  int *next = tcc_malloc(sizeof(int) * n);
  uint32_t *masks = tcc_malloc(sizeof(uint32_t) * n);
  for (int t = 0; t < ke->nt; t++)
    head[t] = -1;
  int changes = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand src;
    uint32_t mask;
    int pos = ke_ext_operand(ke, q, &src, &mask);
    if (pos < 0 || ke->ndefs[pos] != 1)
      continue;
    int dpos = ke_plain_def(ke, q);
    if (dpos < 0 || ke->ndefs[dpos] != 1)
      continue;
    int bi = cfg->instr_to_block[i];
    int found = -1;
    for (int j = head[pos]; j >= 0 && found < 0; j = next[j])
    {
      int bj = cfg->instr_to_block[j];
      if (masks[j] == mask && bi >= 0 && bj >= 0 && tcc_ir_cfg_dominates(cfg, bj, bi))
        found = j;
    }
    if (found < 0)
    {
      masks[i] = mask;
      next[i] = head[pos];
      head[pos] = i;
      continue;
    }
    IROperand prev = tcc_ir_op_get_dest(ir, &ir->compact_instructions[found]);
    if (irop_get_btype(prev) != tcc_ir_op_dest_btype(ir, q))
      prev = irop_retype_scalar(prev, tcc_ir_op_dest_btype(ir, q));
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, prev);
    tcc_ir_set_src2_none(ir, i);
    changes++;
  }
  tcc_free(head);
  tcc_free(next);
  tcc_free(masks);
  return changes;
}

int tcc_ir_known_ext(TCCIRState *ir, IRSSAState *ssa)
{
  const int n = ir->next_instruction_index;
  const int nt = ir->next_temporary_variable;
  if (n < 2 || nt <= 0)
    return 0;
  KnownExt ke = {.ir = ir, .nt = nt};
  ke.bits = tcc_mallocz(sizeof(uint32_t) * nt);
  ke.defined = tcc_mallocz(nt);
  ke.poison = tcc_mallocz(nt);
  ke.ndefs = tcc_mallocz(nt);
  ke.defidx = tcc_malloc(sizeof(int) * nt);
  ke.nuses = tcc_mallocz(nt);
  for (int t = 0; t < nt; t++)
    ke.defidx[t] = -1;

  /* Which TEMPs have a modelled definition, and which are touched in a way
   * the transfer functions do not describe. */
  int candidates = 0;
  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    for (int s = 0; s < 4; s++)
    {
      IROperand o = ke_slot(ir, q, s);
      int pos = ke_temp_pos(&ke, o);
      if (pos < 0)
        continue;
      if (irop_is_64bit(o) || (!ke_is_scalar32(o) && irop_get_btype(o) != IROP_BTYPE_STRUCT))
        ke.poison[pos] = 1;
      if (s > 0 && ke_writes_operands(q->op))
        ke.poison[pos] = 1;
      if (s > 0 || o.is_lval || o.is_llocal)
        ke.nuses[pos] += ke.nuses[pos] < 2;
    }
    int pos = ke_plain_def(&ke, q);
    if (pos >= 0)
    {
      ke.defined[pos] = 1;
      ke.ndefs[pos] += ke.ndefs[pos] < 2;
      ke.defidx[pos] = i;
    }
    else if (irop_config[q->op].has_dest)
    {
      /* A TEMP named as a non-plain destination (a pinned or stack encoding)
       * is written in a way this pass does not read. */
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int dp = ke_temp_pos(&ke, d);
      if (dp >= 0 && !d.is_lval && !d.is_llocal)
        ke.poison[dp] = 1;
    }
    IROperand src;
    uint32_t mask;
    if (ke_ext_operand(&ke, q, &src, &mask) >= 0)
      candidates++;
  }
  const int nb = ssa && ssa->cfg && ssa->block_phis ? ssa->cfg->num_blocks : 0;
  for (int b = 0; b < nb; b++)
    for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next)
    {
      int32_t dv = phi->dest_vreg;
      if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP ||
          TCCIR_DECODE_VREG_POSITION(dv) >= nt)
        continue;
      int pos = TCCIR_DECODE_VREG_POSITION(dv);
      ke.defined[pos] = 1;
      ke.ndefs[pos] += ke.ndefs[pos] < 2;
      ke.defidx[pos] = -1;
      for (int pi = 0; pi < phi->num_operands; pi++)
      {
        int32_t ov = phi->operands[pi].vreg;
        if (ov >= 0 && TCCIR_DECODE_VREG_TYPE(ov) == TCCIR_VREG_TYPE_TEMP && TCCIR_DECODE_VREG_POSITION(ov) < nt)
          ke.nuses[TCCIR_DECODE_VREG_POSITION(ov)] += ke.nuses[TCCIR_DECODE_VREG_POSITION(ov)] < 2;
      }
      if (phi->btype != IROP_BTYPE_INT32 && phi->btype != IROP_BTYPE_INT8 && phi->btype != IROP_BTYPE_INT16 &&
          phi->btype != IROP_BTYPE_STRUCT)
        ke.poison[pos] = 1;
    }

  int changes = 0;
  const int want_ret = !tcc_ir_opt_pass_disabled("ra:known_ext_ret") && ke_ret_type_ok(tcc_state->cur_func_sym);
  if (candidates == 0 && !want_ret)
    goto out;
  uint32_t ret_bits = 0;
  ke.self_ok = want_ret && ke_body_returns(&ke, &ret_bits);

  /* Optimistic fixpoint: masks only grow. */
  int sweep, grew = 1;
  for (sweep = 0; grew && sweep < KE_MAX_SWEEPS; sweep++)
  {
    grew = 0;
    for (int i = 0; i < n; i++)
    {
      IRQuadCompact *q = &ir->compact_instructions[i];
      if (q->op == TCCIR_OP_NOP)
        continue;
      int pos = ke_plain_def(&ke, q);
      if (pos < 0 || ke.poison[pos])
        continue;
      uint32_t m = ke.bits[pos] | ke_transfer(&ke, q);
      if (m != ke.bits[pos])
      {
        ke.bits[pos] = m;
        grew = 1;
      }
    }
    for (int b = 0; b < nb; b++)
      for (IRPhiNode *phi = ssa->block_phis[b]; phi; phi = phi->next)
      {
        int32_t dv = phi->dest_vreg;
        if (dv < 0 || TCCIR_DECODE_VREG_TYPE(dv) != TCCIR_VREG_TYPE_TEMP ||
            TCCIR_DECODE_VREG_POSITION(dv) >= nt)
          continue;
        int pos = TCCIR_DECODE_VREG_POSITION(dv);
        if (ke.poison[pos])
          continue;
        uint32_t m = ke.bits[pos];
        for (int pi = 0; pi < phi->num_operands && m != KE_ALL; pi++)
        {
          int32_t ov = phi->operands[pi].vreg;
          if (ov < 0 || TCCIR_DECODE_VREG_TYPE(ov) != TCCIR_VREG_TYPE_TEMP ||
              TCCIR_DECODE_VREG_POSITION(ov) >= nt)
          {
            m = KE_ALL;
            break;
          }
          int op = TCCIR_DECODE_VREG_POSITION(ov);
          m |= (ke.poison[op] || !ke.defined[op]) ? KE_ALL : ke.bits[op];
        }
        if (m != ke.bits[pos])
        {
          ke.bits[pos] = m;
          grew = 1;
        }
      }
    if (ke.self_ok)
    {
      uint32_t r = ke.self_ret;
      if (!ke_body_returns(&ke, &r))
        r = KE_ALL;
      if ((r | ke.self_ret) != ke.self_ret)
      {
        ke.self_ret |= r;
        grew = 1;
      }
    }
  }
  if (grew)
    goto out; /* did not converge: no fact is final */
  if (want_ret && ke_body_returns(&ke, &ret_bits))
    ir->ret_zext_class = ke_ret_class(ret_bits);

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    IROperand src;
    uint32_t mask;
    int pos = ke_ext_operand(&ke, q, &src, &mask);
    if (pos < 0)
      continue;
    if (ke.poison[pos] || !ke.defined[pos] || (ke.bits[pos] & ~mask) != 0)
    {
      if (tcc_ir_opt_pass_disabled("ra:known_ext_narrow_load") || !ke_narrow_load(&ke, q, src, mask))
        continue;
    }
    /* A copy of the TEMP.  Its register (or word spill slot) is read whole
     * whatever the operand's type says; type it as the destination. */
    int dbt = tcc_ir_op_dest_btype(ir, q);
    if (irop_get_btype(src) != dbt)
      src = irop_retype_scalar(src, dbt);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, src);
    tcc_ir_set_src2_none(ir, i);
    changes++;
  }

  for (int i = 0; i < n && !tcc_ir_opt_pass_disabled("ra:known_ext_bfi"); i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_BFI || ke_plain_def(&ke, q) < 0 || !ke_bfi_is_identity(&ke, q))
      continue;
    IROperand x = tcc_ir_op_get_src1(ir, q);
    int dbt = tcc_ir_op_dest_btype(ir, q);
    if (irop_get_btype(x) != dbt)
      x = irop_retype_scalar(x, dbt);
    q->op = TCCIR_OP_ASSIGN;
    tcc_ir_set_src1(ir, i, x);
    tcc_ir_set_src2_none(ir, i);
    ir->bfi_params[q->orig_index] = 0;
    changes++;
  }

  /* The same extension of the same value twice, the first on every path to
   * the second: `uxth r1,r0; cbz r1,..; uxth r5,r0` -- an error code tested
   * and then returned, or an AND #0xFFFF and a UBFX #0,#16 of one union word
   * that GVN keeps apart.  The second is a copy of the first.  Both values
   * must have one definition each (SSA names, not a phi's copies). */
  if (ssa && ssa->cfg && ssa->cfg->num_instrs == n && ssa->cfg->instr_to_block &&
      !tcc_ir_opt_pass_disabled("ra:known_ext_cse"))
    changes += ke_cse_extensions(&ke, ssa->cfg);

out:
  tcc_free(ke.bits);
  tcc_free(ke.defined);
  tcc_free(ke.poison);
  tcc_free(ke.ndefs);
  tcc_free(ke.defidx);
  tcc_free(ke.nuses);
  return changes;
}
