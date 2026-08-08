/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* vector.c -- GCC vector extension: vector ops, subscripting and the constant-recipe cache.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* -------- GCC vector extension helpers -------- */

/* Returns 1 if the type has the VT_VECTOR flag (GCC vector extension). */
int is_vector_type(const CType *type)
{
  return (type->t & VT_VECTOR) != 0;
}

/* Returns number of elements in a vector type. */
int vector_elem_count(const CType *vec)
{
  int align, elem_size;
  elem_size = type_size(&vec->ref->type, &align);
  return vec->ref->c / elem_size;
}

/* Build a vector CType: elem_type elements packed into vector_bytes bytes.
 * Sets *out to the resulting VT_STRUCT | VT_VECTOR type. */
void make_vector_type(CType *out, const CType *elem_type, int vector_bytes)
{
  int elem_align, elem_size;
  Sym *s;

  elem_size = type_size(elem_type, &elem_align);
  if (elem_size <= 0 || vector_bytes % elem_size != 0)
    tcc_error("vector_size %d is not a multiple of element size %d", vector_bytes, elem_size);
  if (!is_integer_btype(elem_type->t & VT_BTYPE) && !is_float(elem_type->t))
    tcc_error("vector element type must be an integer or floating-point type");

  /* Sym for the vector: type = element type, c = total bytes, r = alignment */
  s = sym_push(SYM_FIELD, (CType *)elem_type, 0, vector_bytes);
  s->r = vector_bytes; /* alignment = total size (for 8/16-byte vectors) */
  s->c = vector_bytes; /* total byte size */

  out->t = VT_STRUCT | VT_VECTOR;
  out->ref = s;
}

/* -------- vector constant folding helpers -------- */

unsigned char *find_sv_const_init(const SValue *sv, int min_size)
{
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_LOCAL | VT_LVAL))
    return NULL;
  int addr = (int)sv->c.i;
  Sym *s;
  for (s = local_stack; s; s = s->prev)
  {
    if (s->const_init_data && s->const_init_valid && (int)s->c == addr && s->const_init_size >= min_size)
      return s->const_init_data;
  }
  return NULL;
}

/* Like find_sv_const_init, but only returns data backed by an ANONYMOUS sym
 * (a compound literal or a const-folded vector temp).  A *named* local can
 * carry a stale const_init buffer: when it is initialised from a non-constant
 * expression (e.g. `v4si t = ~a;`) the buffer stays zero-filled yet
 * const_init_valid is left set — init_putv (which clears validity when a
 * non-constant value is stored) only runs for brace-list initialisers, and
 * const_init_in_progress suppresses the store-based invalidation during the
 * initialiser.  Existing callers tolerate this because they only fold when
 * BOTH operands are constant (a named expression-init operand pairs with a
 * non-constant one, so no fold fires).  A single-operand substitution has no
 * such guard, so it must reject named locals.  Anonymous compound literals can
 * only ever be brace lists, so a valid buffer always reflects genuine
 * constants. */
unsigned char *find_sv_vec_literal_init(const SValue *sv, int min_size)
{
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_LOCAL | VT_LVAL))
    return NULL;
  int addr = (int)sv->c.i;
  Sym *s;
  for (s = local_stack; s; s = s->prev)
  {
    if (s->const_init_data && s->const_init_valid && s->v >= SYM_FIRST_ANOM && (int)s->c == addr &&
        s->const_init_size >= min_size)
      return s->const_init_data;
  }
  return NULL;
}

int64_t read_vec_const_elem(const unsigned char *data, int elem_size, int idx, int is_unsigned)
{
  unsigned char *p = (unsigned char *)data + idx * elem_size;
  switch (elem_size)
  {
  case 1:
    return is_unsigned ? (int64_t)(uint8_t)p[0] : (int64_t)(int8_t)p[0];
  case 2:
    return is_unsigned ? (int64_t)(uint16_t)read16le(p) : (int64_t)(int16_t)read16le(p);
  case 4:
    return is_unsigned ? (int64_t)(uint32_t)read32le(p) : (int64_t)(int32_t)read32le(p);
  case 8:
    return (int64_t)read64le(p);
  }
  return 0;
}

static void write_vec_const_elem(unsigned char *data, int elem_size, int idx, int64_t val)
{
  unsigned char *p = data + idx * elem_size;
  switch (elem_size)
  {
  case 1:
    p[0] = (unsigned char)(val & 0xFF);
    break;
  case 2:
    write16le(p, (uint16_t)(val & 0xFFFF));
    break;
  case 4:
    write32le(p, (uint32_t)(val & 0xFFFFFFFF));
    break;
  case 8:
    write64le(p, (uint64_t)val);
    break;
  }
}

static int64_t eval_vec_const_op(int op, int64_t a, int64_t b, int is_unsigned)
{
  switch (op)
  {
  case '+':
    return a + b;
  case '-':
    return a - b;
  case '*':
    return a * b;
  case '/':
    if (b == 0)
      return 0;
    return is_unsigned ? (int64_t)((uint64_t)a / (uint64_t)b) : a / b;
  case '%':
    if (b == 0)
      return 0;
    return is_unsigned ? (int64_t)((uint64_t)a % (uint64_t)b) : a % b;
  case '^':
    return a ^ b;
  case '|':
    return a | b;
  case '&':
    return a & b;
  case TOK_SHL:
    return a << (b & 63);
  case TOK_SAR:
    return is_unsigned ? (int64_t)((uint64_t)a >> (b & 63)) : a >> (b & 63);
  case TOK_EQ:
    return (a == b) ? (int64_t)-1 : 0;
  case TOK_NE:
    return (a != b) ? (int64_t)-1 : 0;
  case TOK_LT:
    return (a < b) ? (int64_t)-1 : 0;
  case TOK_GT:
    return (a > b) ? (int64_t)-1 : 0;
  case TOK_LE:
    return (a <= b) ? (int64_t)-1 : 0;
  case TOK_GE:
    return (a >= b) ? (int64_t)-1 : 0;
  case TOK_ULT:
    return ((uint64_t)a < (uint64_t)b) ? (int64_t)-1 : 0;
  case TOK_UGT:
    return ((uint64_t)a > (uint64_t)b) ? (int64_t)-1 : 0;
  case TOK_ULE:
    return ((uint64_t)a <= (uint64_t)b) ? (int64_t)-1 : 0;
  case TOK_UGE:
    return ((uint64_t)a >= (uint64_t)b) ? (int64_t)-1 : 0;
  }
  return 0;
}

void attach_const_init_to_temp(int frame_offset, int size, const unsigned char *data)
{
  Sym *s = sym_push2(&local_stack, SYM_FIRST_ANOM, VT_INT, frame_offset);
  s->const_init_data = tcc_malloc(size);
  memcpy(s->const_init_data, data, size);
  s->const_init_size = size;
  s->const_init_valid = 1;
}

/* ---- element-major fusion of chained vector expressions ------------------
 *
 * gen_op_vector lowers one whole-vector op at a time: it evaluates all N
 * elements of `a OP b` into a stack temp before the next op runs.  A chain
 * like `(*p & *r) == (*q & *r)` therefore becomes three phase-major passes
 * over 32 elements, and every intermediate value round-trips through memory
 * (a store in one phase, a reload in the next) because all N results are live
 * at once.  GCC instead interleaves per element, so pressure is O(1) and no
 * intermediate ever reaches memory.
 *
 * Each gen_op_vector call records a "recipe" for the temp slot it produced:
 * enough information to recompute one element of that result from scratch.
 * When a later gen_op_vector consumes such a temp, it recomputes the element
 * inline instead of loading it, and NOPs out the producer's now-dead element
 * loop.  Recomputation is recursive, so an arbitrarily deep expression tree
 * collapses into a single element-major loop.
 *
 * A recipe is only usable while nothing that could change its operands has
 * been emitted since it was recorded.  Rather than hooking every writer, the
 * validity check scans the IR emitted after the recipe: everything must be a
 * pure value computation, a load, or part of another vector element loop
 * (whose stores only ever target its own non-escaping temp slot). */
#define VEC_RECIPE_MAX 24
#define VEC_EMIT_RANGE_MAX 96

typedef struct VecRecipe
{
  char live;     /* still findable by slot lookup (cleared when the slot is reused) */
  char consumed; /* emitted element loop already NOPed by a substituting consumer */
  int res_vr, res_loc;
  int op, is_cmp;
  int left_node, right_node; /* recipe index of an operand, or -1 for `*_sv` */
  SValue left_sv, right_sv;
  char scalar_left, scalar_right;
  char subst_left, subst_right;
  char imm_is_unsigned;
  unsigned char *imm_left_data, *imm_right_data;
  CType src_elem_type; /* element type of the operands */
  CType res_elem_type; /* element type of this node's value */
  int elem_size;
  int elem_count;
  int ir_start, ir_end;
} VecRecipe;

static VecRecipe vec_recipes[VEC_RECIPE_MAX];
static int nb_vec_recipes;
static struct
{
  int start, end;
} vec_emit_ranges[VEC_EMIT_RANGE_MAX];
static int nb_vec_emit_ranges;

/* Called at every function start: IR instruction indices are per function. */
ST_FUNC void gen_op_vector_reset(void)
{
  nb_vec_recipes = 0;
  nb_vec_emit_ranges = 0;
}

/* A recycled temp slot no longer holds the value its recipe describes. */
void vec_recipe_kill_slot(int vr)
{
  int k;
  for (k = 0; k < nb_vec_recipes; k++)
    if (vec_recipes[k].live && vec_recipes[k].res_vr == vr)
      vec_recipes[k].live = 0;
}

void vec_emit_range_record(int start, int end)
{
  if (end <= start)
    return;
  if (nb_vec_emit_ranges >= VEC_EMIT_RANGE_MAX)
    return;
  vec_emit_ranges[nb_vec_emit_ranges].start = start;
  vec_emit_ranges[nb_vec_emit_ranges].end = end;
  nb_vec_emit_ranges++;
}

static int vec_ir_in_emit_range(int idx)
{
  int k;
  for (k = 0; k < nb_vec_emit_ranges; k++)
    if (idx >= vec_emit_ranges[k].start && idx < vec_emit_ranges[k].end)
      return 1;
  return 0;
}

/* Whitelist: ops that neither write memory, transfer control, nor call. */
static int vec_ir_op_is_pure(TCCIRState *ir, IRQuadCompact *q)
{
  /* Defining anything but a fresh temp can also change what a recipe reloads:
   * a `p += 1` between the recipe and its use redefines the very pointer its
   * element addresses are derived from. */
  if (irop_config[q->op].has_dest)
  {
    IROperand d = tcc_ir_op_get_dest(ir, q);
    int32_t dvr = irop_get_vreg(d);
    if (d.is_lval || dvr < 0 || TCCIR_DECODE_VREG_TYPE(dvr) != TCCIR_VREG_TYPE_TEMP)
      return 0;
  }

  switch (q->op)
  {
  case TCCIR_OP_ADD:
  case TCCIR_OP_ADC_USE:
  case TCCIR_OP_ADC_GEN:
  case TCCIR_OP_SUB:
  case TCCIR_OP_SUBC_USE:
  case TCCIR_OP_SUBC_GEN:
  case TCCIR_OP_MUL:
  case TCCIR_OP_MLA:
  case TCCIR_OP_UMULL:
  case TCCIR_OP_DIV:
  case TCCIR_OP_UMOD:
  case TCCIR_OP_IMOD:
  case TCCIR_OP_AND:
  case TCCIR_OP_OR:
  case TCCIR_OP_XOR:
  case TCCIR_OP_SHL:
  case TCCIR_OP_SAR:
  case TCCIR_OP_SHR:
  case TCCIR_OP_PDIV:
  case TCCIR_OP_UDIV:
  case TCCIR_OP_CMP:
  case TCCIR_OP_SETIF:
  case TCCIR_OP_TEST_ZERO:
  case TCCIR_OP_SELECT:
  case TCCIR_OP_LOAD:
  case TCCIR_OP_LOAD_INDEXED:
  case TCCIR_OP_LOAD_POSTINC:
  case TCCIR_OP_LEA:
  case TCCIR_OP_UBFX:
  case TCCIR_OP_SBFX:
  case TCCIR_OP_ZEXT:
  case TCCIR_OP_PACK64:
  case TCCIR_OP_BOOL_OR:
  case TCCIR_OP_BOOL_AND:
  case TCCIR_OP_FADD:
  case TCCIR_OP_FSUB:
  case TCCIR_OP_FMUL:
  case TCCIR_OP_FDIV:
  case TCCIR_OP_FNEG:
  case TCCIR_OP_FCMP:
  case TCCIR_OP_CVT_FTOF:
  case TCCIR_OP_CVT_ITOF:
  case TCCIR_OP_CVT_FTOI:
  case TCCIR_OP_ASSIGN: /* dest already proven to be a temp: a register move */
  case TCCIR_OP_NOP:
    return 1;
  default:
    return 0;
  }
}

/* True when nothing emitted since the recipe was recorded can have changed
 * the values its operands would reload. */
static int vec_recipe_still_valid(const VecRecipe *r)
{
  TCCIRState *ir = tcc_state->ir;
  int idx;
  for (idx = r->ir_end; idx < ir->next_instruction_index; idx++)
  {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->is_jump_target)
      return 0;
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (vec_ir_in_emit_range(idx))
      continue;
    if (!vec_ir_op_is_pure(ir, q))
      return 0;
  }
  return 1;
}

/* The producer's element loop can only be deleted if it is plain straight-line
 * value/memory code with no control flow or calls hanging off it. */
static int vec_range_is_noppable(int start, int end)
{
  TCCIRState *ir = tcc_state->ir;
  int idx;
  if (end > ir->next_instruction_index)
    return 0;
  for (idx = start; idx < end; idx++)
  {
    IRQuadCompact *q = &ir->compact_instructions[idx];
    if (q->is_jump_target)
      return 0;
    if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
        q->op == TCCIR_OP_STORE_POSTINC)
      continue;
    if (!vec_ir_op_is_pure(ir, q))
      return 0;
  }
  return 1;
}

static void vec_range_nop(int start, int end)
{
  TCCIRState *ir = tcc_state->ir;
  int idx;
  for (idx = start; idx < end; idx++)
    ir->compact_instructions[idx].op = TCCIR_OP_NOP;
}

/* Element types wider than a word, floats and sub-int elements are excluded:
 * for the latter the memory round-trip performs the per-element truncation
 * that C's integer promotions would otherwise skip in a fused chain. */
static int vec_recipe_elem_type_ok(const CType *t, int elem_size)
{
  if (elem_size != 4)
    return 0;
  if (is_float(t->t))
    return 0;
  if ((t->t & VT_BTYPE) == VT_LLONG)
    return 0;
  return 1;
}

/* Find the recipe describing `sv`, or -1.  `sv` must be the untouched result
 * SValue a previous gen_op_vector pushed. */
static int vec_recipe_lookup(const SValue *sv, int elem_count, int elem_size)
{
  int k;
  if ((sv->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != (VT_LOCAL | VT_LVAL))
    return -1;
  if (!VR_IS_TEMP_LOCAL(sv->vr))
    return -1;
  for (k = 0; k < nb_vec_recipes; k++)
  {
    VecRecipe *r = &vec_recipes[k];
    if (!r->live || r->consumed)
      continue;
    if (r->res_vr != sv->vr || r->res_loc != (int)sv->c.i)
      continue;
    if (r->elem_count != elem_count || r->elem_size != elem_size)
      continue;
    if (!vec_range_is_noppable(r->ir_start, r->ir_end))
      continue;
    if (!vec_recipe_still_valid(r))
      continue;
    return k;
  }
  return -1;
}

/* Push element [i] of a vector operand as a scalar. */
static void vec_push_operand_elem(const SValue *sv, int scalar, int subst,
                                  unsigned char *imm_data, const CType *elem_type,
                                  int elem_size, int is_unsigned, int i)
{
  if (scalar)
  {
    vpushv((SValue *)sv);
    return;
  }
  if (subst)
  {
    vpush64(elem_type->t & VT_BTYPE,
            (unsigned long long)read_vec_const_elem(imm_data, elem_size, i, is_unsigned));
    return;
  }
  vpushv((SValue *)sv);
  gaddrof();
  vtop->type = char_pointer_type;
  vpushi(i * elem_size);
  gen_op('+');
  vtop->type = *elem_type;
  vtop->r |= VT_LVAL;
}

/* Recompute element [i] of a recipe's result and leave it on the value stack. */
static void vec_emit_node_elem(int node, int i)
{
  VecRecipe *r = &vec_recipes[node];
  CType res_elem_type = r->res_elem_type;

  if (r->left_node >= 0)
    vec_emit_node_elem(r->left_node, i);
  else
    vec_push_operand_elem(&r->left_sv, r->scalar_left, r->subst_left, r->imm_left_data,
                          &r->src_elem_type, r->elem_size, r->imm_is_unsigned, i);

  r = &vec_recipes[node];
  if (r->right_node >= 0)
    vec_emit_node_elem(r->right_node, i);
  else
    vec_push_operand_elem(&r->right_sv, r->scalar_right, r->subst_right, r->imm_right_data,
                          &r->src_elem_type, r->elem_size, r->imm_is_unsigned, i);

  r = &vec_recipes[node];
  gen_op(r->op);
  if (r->is_cmp)
  {
    tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
    vpushi(0);
    vswap();
    gen_op('-'); /* 0 - (0 or 1) = 0 or -1 */
  }
  vtop->type = res_elem_type;
}

/* -------- end vector helpers -------- */

/* Generate element-wise binary vector operation.
 * vtop[-1] = left operand (vector or scalar broadcast),
 * vtop[0]  = right operand (vector or scalar broadcast).
 * At least one must have VT_VECTOR set.  Result is same vector type. */
void gen_op_vector(int op)
{
  CType vec_type, elem_type;
  int elem_size, elem_align, elem_count, vec_size;
  int res_vr, res_loc;
  int i;
  int is_cmp;
  int scalar_left, scalar_right;
  SValue left_sv, right_sv;

  /* Determine which operand carries the vector type */
  if (is_vector_type(&vtop[-1].type))
    vec_type = vtop[-1].type;
  else
    vec_type = vtop[0].type;

  scalar_left = !is_vector_type(&vtop[-1].type);
  scalar_right = !is_vector_type(&vtop[0].type);

  elem_type = vec_type.ref->type;
  elem_size = type_size(&elem_type, &elem_align);
  elem_count = vector_elem_count(&vec_type);
  vec_size = vec_type.ref->c;

  /* Classify op: comparison ops yield -1 (true) or 0 (false) per element */
  is_cmp = (op == TOK_EQ || op == TOK_NE || op == TOK_LT || op == TOK_GE || op == TOK_LE || op == TOK_GT ||
            op == TOK_ULT || op == TOK_UGE || op == TOK_ULE || op == TOK_UGT);

  /* For comparison ops on float vectors, the result is an integer vector
   * of the same total size (GCC vector semantics).  Build the appropriate
   * integer vector type and use its element type for storing results. */
  CType cmp_vec_type = vec_type;
  CType store_elem_type = elem_type;
  if (is_cmp && is_float(elem_type.t))
  {
    CType int_elem;
    int_elem.t = (elem_size == 8) ? VT_LLONG : VT_INT;
    int_elem.ref = NULL;
    make_vector_type(&cmp_vec_type, &int_elem, vec_size);
    store_elem_type = int_elem;
  }

  /* Save both operands and pop them off the value stack */
  right_sv = vtop[0];
  left_sv = vtop[-1];
  vtop -= 2;

  /* Single-element vector fast-path (tiny only): lower as scalar without
   * a temp slot.  Limited to vec_size <= 2 because the rvalue result must
   * be consumable by vstore() and gfunc_return(), which currently have
   * rvalue-vector support only for 1- and 2-byte vectors (see vstore's
   * src_is_vec_rvalue path).  For larger element widths the original
   * temp-slot lowering still kicks in. */
  if (elem_count == 1 && vec_size <= 2)
  {
    /* Load left[0] */
    vpushv(&left_sv);
    if (!scalar_left)
      vtop->type = elem_type;
    /* Load right[0] */
    vpushv(&right_sv);
    if (!scalar_right)
      vtop->type = elem_type;
    /* Apply scalar op */
    gen_op(op);
    /* For comparison ops: convert VT_CMP → -1/0 */
    if (is_cmp)
    {
      tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      vpushi(0);
      vswap();
      gen_op('-'); /* 0 - (0 or 1) = 0 or -1 */
    }
    /* Result is now a scalar rvalue on top; relabel its type as the
     * (possibly cmp-promoted) vector type so callers see a vector. */
    vtop->type = is_cmp ? cmp_vec_type : vec_type;
    return;
  }

  /* Compile-time constant fold: when both operands are fully known at compile
   * time, compute the result in compiler memory and emit constant stores.
   * This cascades: the result gets const_init_data so subsequent vector ops
   * can also fold, collapsing entire chains of vector arithmetic. */
  if (!is_float(elem_type.t) && !NOEVAL_WANTED && vec_size <= 64)
  {
    unsigned char *left_data = NULL, *right_data = NULL;
    int64_t scalar_left_val = 0, scalar_right_val = 0;
    int can_fold = 1;

    if (scalar_left)
    {
      if ((left_sv.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
        scalar_left_val = left_sv.c.i;
      else
        can_fold = 0;
    }
    else
    {
      left_data = find_sv_const_init(&left_sv, vec_size);
      if (!left_data)
        can_fold = 0;
    }

    if (can_fold)
    {
      if (scalar_right)
      {
        if ((right_sv.r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST)
          scalar_right_val = right_sv.c.i;
        else
          can_fold = 0;
      }
      else
      {
        right_data = find_sv_const_init(&right_sv, vec_size);
        if (!right_data)
          can_fold = 0;
      }
    }

    if (can_fold)
    {
      int is_unsigned = (elem_type.t & VT_UNSIGNED) != 0;
      int store_size = is_cmp ? type_size(&store_elem_type, &(int){0}) : elem_size;
      CType *store_type = is_cmp ? &store_elem_type : &elem_type;
      unsigned char result_buf[64];
      memset(result_buf, 0, sizeof(result_buf));

      for (i = 0; i < elem_count; i++)
      {
        int64_t lv = scalar_left ? scalar_left_val : read_vec_const_elem(left_data, elem_size, i, is_unsigned);
        int64_t rv = scalar_right ? scalar_right_val : read_vec_const_elem(right_data, elem_size, i, is_unsigned);
        int64_t res = eval_vec_const_op(op, lv, rv, is_unsigned);
        write_vec_const_elem(result_buf, store_size, i, res);
      }

      res_loc = get_temp_local_var(vec_size, vec_size > 8 ? 8 : vec_size, &res_vr);

      for (i = 0; i < elem_count; i++)
      {
        int offset = i * store_size;
        int64_t val = read_vec_const_elem(result_buf, store_size, i, 0);
        SValue res_base_sv;

        vpush64(store_type->t & VT_BTYPE, (unsigned long long)val);

        memset(&res_base_sv, 0, sizeof(res_base_sv));
        res_base_sv.type = is_cmp ? cmp_vec_type : vec_type;
        res_base_sv.r = VT_LOCAL | VT_LVAL;
        res_base_sv.vr = res_vr;
        res_base_sv.c.i = res_loc;

        vpushv(&res_base_sv);
        gaddrof();
        vtop->type = char_pointer_type;
        vpushi(offset);
        gen_op('+');
        vtop->type = *store_type;
        vtop->r |= VT_LVAL;

        vswap();
        vstore();
        vpop();
      }

      attach_const_init_to_temp(res_loc, vec_size, result_buf);

      {
        SValue result;
        memset(&result, 0, sizeof(result));
        result.type = is_cmp ? cmp_vec_type : vec_type;
        result.r = VT_LOCAL | VT_LVAL;
        result.vr = res_vr;
        result.c.i = res_loc;
        vpushv(&result);
      }
      return;
    }
  }

  /* Element-major fusion: if an operand is the still-valid result of an
   * earlier gen_op_vector, recompute its elements inline here rather than
   * reloading them, and delete the producer's element loop.  Resolved BEFORE
   * the temp slot is allocated, since allocation may recycle (and therefore
   * retire) exactly the slot being consumed. */
  int left_node = -1, right_node = -1;
  if (!nocode_wanted && vec_recipe_elem_type_ok(&elem_type, elem_size))
  {
    if (!scalar_left)
      left_node = vec_recipe_lookup(&left_sv, elem_count, elem_size);
    if (!scalar_right)
      right_node = vec_recipe_lookup(&right_sv, elem_count, elem_size);
  }

  /* Allocate a temp stack slot for the result vector */
  res_loc = get_temp_local_var(vec_size, vec_size > 8 ? 8 : vec_size, &res_vr);

  if (left_node >= 0)
  {
    vec_range_nop(vec_recipes[left_node].ir_start, vec_recipes[left_node].ir_end);
    vec_recipes[left_node].consumed = 1;
    vec_recipes[left_node].live = 0;
  }
  if (right_node >= 0)
  {
    vec_range_nop(vec_recipes[right_node].ir_start, vec_recipes[right_node].ir_end);
    vec_recipes[right_node].consumed = 1;
    vec_recipes[right_node].live = 0;
  }

  /* Constant-operand immediate substitution: for a commutative bitwise op
   * (&, |, ^) where one operand is a vector compound-literal constant (e.g.
   * `*p & (V){1,1,...}`), push each constant element as a scalar immediate
   * rather than dereferencing the in-memory copy.  This turns the per-element
   * `ldr const; and r,r,const` into `and r,r,#imm`, and (when nothing else
   * reads it) lets the compound-literal's materialising memcpy be eliminated —
   * which in turn avoids spilling/reloading the base pointer around that call.
   *
   * Scoped TIGHTLY to bitwise commutative ops on integer elements: for &/|/^
   * the operands are necessarily integer (no float-mask case) and commutative
   * (so substituting either side is value-identical), and the result's low
   * elem_size bytes match the in-memory load regardless of how the immediate is
   * sign-/zero-extended.  Shifts (non-commutative; a const LHS would need
   * mov+lsl) and comparisons (signedness affects codegen) are deliberately
   * excluded — those are exactly the cases an earlier, broader attempt
   * miscompiled. */
  int subst_left = 0, subst_right = 0;
  unsigned char *imm_left_data = NULL, *imm_right_data = NULL;
  int imm_is_unsigned = (elem_type.t & VT_UNSIGNED) != 0;
  if ((op == '&' || op == '|' || op == '^') && !is_cmp && !is_float(elem_type.t))
  {
    if (!scalar_right)
    {
      imm_right_data = find_sv_vec_literal_init(&right_sv, vec_size);
      if (imm_right_data)
        subst_right = 1;
    }
    if (!subst_right && !scalar_left)
    {
      imm_left_data = find_sv_vec_literal_init(&left_sv, vec_size);
      if (imm_left_data)
        subst_left = 1;
    }
  }

  int elem_ir_start = tcc_state->ir ? tcc_state->ir->next_instruction_index : 0;

  /* Emit element-wise operations (unrolled: elem_count is compile-time constant) */
  for (i = 0; i < elem_count; i++)
  {
    int offset = i * elem_size;
    SValue res_base_sv;

    /* ---- Load left element [i] ---- */
    if (left_node >= 0)
      vec_emit_node_elem(left_node, i);
    else
      vec_push_operand_elem(&left_sv, scalar_left, subst_left, imm_left_data,
                            &elem_type, elem_size, imm_is_unsigned, i);

    /* ---- Load right element [i] ---- */
    if (right_node >= 0)
      vec_emit_node_elem(right_node, i);
    else
      vec_push_operand_elem(&right_sv, scalar_right, subst_right, imm_right_data,
                            &elem_type, elem_size, imm_is_unsigned, i);

    /* ---- Apply scalar operation on the two elements ---- */
    gen_op(op);

    /* ---- For comparison ops: convert VT_CMP result to -1/0 integer ---- */
    if (is_cmp)
    {
      /* SETIF materialises VT_CMP as 0 (false) or 1 (true) in a vreg */
      tcc_ir_codegen_cmp_jmp_set(tcc_state->ir);
      /* GCC vector semantics: true → all bits set (-1), false → 0 */
      vpushi(0);
      vswap();
      gen_op('-'); /* 0 - (0 or 1) = 0 or -1 */
    }

    /* ---- Store computed value into result[i] via pointer arithmetic ---- */
    /* Build address of result element using LEA + byte-offset addition */
    memset(&res_base_sv, 0, sizeof(res_base_sv));
    res_base_sv.type = is_cmp ? cmp_vec_type : vec_type;
    res_base_sv.r = VT_LOCAL | VT_LVAL;
    res_base_sv.vr = res_vr;
    res_base_sv.c.i = res_loc;

    vpushv(&res_base_sv); /* push result vector lvalue */
    gaddrof();            /* LEA: result base address in a new vreg */
    vtop->type = char_pointer_type;
    vpushi(offset);
    gen_op('+'); /* char* + byte-offset = element address */
    vtop->type = is_cmp ? store_elem_type : elem_type;
    vtop->r |= VT_LVAL; /* lvalue: *element_address */

    /* Stack is now: vtop[-1] = computed_value, vtop = result[i] lvalue */
    vswap();  /* vtop[-1] = result[i] lvalue, vtop = computed_value */
    vstore(); /* STORE: computed_value → *result[i] */
    vpop();   /* discard the assigned value left on stack */
  }

  /* Record how to recompute one element of this result, so a consumer can
   * fuse it instead of round-tripping the whole vector through the slot. */
  if (!nocode_wanted && tcc_state->ir && res_vr != -1 &&
      nb_vec_recipes < VEC_RECIPE_MAX &&
      vec_recipe_elem_type_ok(&elem_type, elem_size) &&
      !(elem_type.t & VT_VOLATILE) && !(vec_type.t & VT_VOLATILE))
  {
    int elem_ir_end = tcc_state->ir->next_instruction_index;
    VecRecipe *r = &vec_recipes[nb_vec_recipes];
    /* An operand slot that is neither a recipe nor a stable location could be
     * recycled before the recomputation runs. */
    int left_ok = (left_node >= 0) || scalar_left || subst_left || !VR_IS_TEMP_LOCAL(left_sv.vr);
    int right_ok = (right_node >= 0) || scalar_right || subst_right || !VR_IS_TEMP_LOCAL(right_sv.vr);

    if (left_ok && right_ok && vec_range_is_noppable(elem_ir_start, elem_ir_end))
    {
      memset(r, 0, sizeof(*r));
      r->live = 1;
      r->res_vr = res_vr;
      r->res_loc = res_loc;
      r->op = op;
      r->is_cmp = is_cmp;
      r->left_node = left_node;
      r->right_node = right_node;
      r->left_sv = left_sv;
      r->right_sv = right_sv;
      r->scalar_left = (char)scalar_left;
      r->scalar_right = (char)scalar_right;
      r->subst_left = (char)subst_left;
      r->subst_right = (char)subst_right;
      r->imm_is_unsigned = (char)imm_is_unsigned;
      r->imm_left_data = imm_left_data;
      r->imm_right_data = imm_right_data;
      r->src_elem_type = elem_type;
      r->res_elem_type = is_cmp ? store_elem_type : elem_type;
      r->elem_size = elem_size;
      r->elem_count = elem_count;
      r->ir_start = elem_ir_start;
      r->ir_end = elem_ir_end;
      nb_vec_recipes++;
      vec_emit_range_record(elem_ir_start, elem_ir_end);
    }
  }

  /* Push the result vector as a local lvalue */
  {
    SValue result;
    memset(&result, 0, sizeof(result));
    result.type = is_cmp ? cmp_vec_type : vec_type;
    result.r = VT_LOCAL | VT_LVAL;
    result.vr = res_vr;
    result.c.i = res_loc;
    vpushv(&result);
  }
}

/* Generate vector element subscript access: vec[index] → element lvalue.
 * Called from the postfix '[]' handler when the base (vtop[-1]) is a
 * GCC vector type.  vtop[-1] = vector lvalue, vtop[0] = integer index.
 * Replaces both with a scalar lvalue of the vector's element type. */
void gen_vec_subscript(void)
{
  CType elem_type;
  int elem_size, elem_align;

  elem_type = vtop[-1].type.ref->type;
  elem_size = type_size(&elem_type, &elem_align);

  /* Scale index by element size to get a byte offset */
  if (elem_size > 1)
  {
    vpushi(elem_size);
    gen_op('*'); /* vtop[0] = index * elem_size (byte offset) */
  }

  /* Stack: vtop[-1] = vector lvalue, vtop[0] = byte_offset */
  /* Swap so the vector is on top, then take its address */
  vswap();
  gaddrof();                      /* LEA: address of vector base in a vreg */
  vtop->type = char_pointer_type; /* treat as char* for byte arithmetic */
  vswap();                        /* restore: vtop[-1]=char*, vtop[0]=byte_offset */

  gen_op('+'); /* char* + byte_offset = element address */

  /* Change pointer to element-type lvalue (dereferences the address) */
  vtop->type = elem_type;
  vtop->r |= VT_LVAL;
}
