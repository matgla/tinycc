/*
 *  TCC IR - Constant-string evaluators shared by the string-builtin folders
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Address/length evaluation over flat IR: resolve a call argument to a constant
 * string (literal symref or a stack buffer built by preceding STOREs) and to the
 * symref it is based on.  Consumed by source/opt/ssa/string (ssa:const_string_fold). */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

int ir_opt_eval_const_string_operand(TCCIRState *ir, IROperand op, int use_idx, IROperand *out, int depth)
{
  int32_t vr;
  int def_idx;
  IRQuadCompact *q;

  if (!ir || !out || depth > 16)
    return 0;

  if (op.is_lval && op.vreg_type == TCCIR_VREG_TYPE_TEMP)
    return 0;

  if (ir_opt_get_constant_string_from_symref(ir, op))
  {
    *out = op;
    return 1;
  }

  vr = irop_get_vreg(op);
  if (vr < 0)
    return 0;

  if (ir_opt_vreg_address_taken_between(ir, vr, 0, use_idx))
    return 0;

  if (!tcc_ir_vreg_has_single_def(ir, vr))
    return 0;

  def_idx = tcc_ir_find_defining_instruction(ir, vr, use_idx);
  if (def_idx < 0)
    return 0;

  q = &ir->compact_instructions[def_idx];
  switch (q->op)
  {
  case TCCIR_OP_ASSIGN:
  case TCCIR_OP_LOAD:
    return ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, out, depth + 1);
  case TCCIR_OP_ADD:
  {
    IROperand base_op;
    uint64_t addend;
    IRPoolSymref *symref;
    uint32_t new_idx;

    if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src1(ir, q), def_idx, &base_op, depth + 1) ||
        !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src2(ir, q), def_idx, &addend, depth + 1))
    {
      if (!ir_opt_eval_const_string_operand(ir, tcc_ir_op_get_src2(ir, q), def_idx, &base_op, depth + 1) ||
          !ir_opt_eval_const_u64(ir, tcc_ir_op_get_src1(ir, q), def_idx, &addend, depth + 1))
        return 0;
    }

    if (irop_get_tag(base_op) != IROP_TAG_SYMREF)
      return 0;

    symref = irop_get_symref_ex(ir, base_op);
    if (!symref)
      return 0;

    new_idx = tcc_ir_pool_add_symref(ir, symref->sym, symref->addend + (int32_t)addend, symref->flags);
    *out = irop_make_symref(irop_get_vreg(base_op), new_idx, base_op.is_lval, base_op.is_local, base_op.is_const,
                            irop_get_btype(base_op));
    return 1;
  }
  default:
    return 0;
  }
}

int ir_opt_fold_strcmp_result(const char *s1, const char *s2)
{
  while ((unsigned char)*s1 == (unsigned char)*s2)
  {
    if (*s1 == '\0')
      return 0;
    ++s1;
    ++s2;
  }

  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int ir_opt_fold_strncmp_result(const char *s1, const char *s2, uint64_t n)
{
  if (n == 0)
    return 0;

  while (n-- > 0)
  {
    unsigned char c1 = (unsigned char)*s1++;
    unsigned char c2 = (unsigned char)*s2++;
    if (c1 != c2 || c1 == '\0')
      return (int)c1 - (int)c2;
  }

  return 0;
}

int ir_opt_fold_memcmp_result(const char *s1, const char *s2, uint64_t n)
{
  uint64_t i;

  for (i = 0; i < n; ++i)
  {
    unsigned char c1 = (unsigned char)s1[i];
    unsigned char c2 = (unsigned char)s2[i];
    if (c1 != c2)
      return (int)c1 - (int)c2;
  }

  return 0;
}

int ir_opt_fold_memchr_offset(const char *s, unsigned char c, uint64_t n, int *out_offset)
{
  uint64_t i;

  if (!out_offset)
    return 0;

  for (i = 0; i < n; ++i)
  {
    if ((unsigned char)s[i] == c)
    {
      *out_offset = (int)i;
      return 1;
    }
  }

  *out_offset = -1;
  return 1;
}

static int ir_opt_btype_size(int btype)
{
  switch (btype)
  {
  case IROP_BTYPE_INT8:
    return 1;
  case IROP_BTYPE_INT16:
    return 2;
  case IROP_BTYPE_INT64:
  case IROP_BTYPE_FLOAT64:
    return 8;
  case IROP_BTYPE_STRUCT:
    return 0;
  default:
    return 4;
  }
}

static int ir_opt_stack_addr_offset(IROperand op, int *out_off)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) != -1 || op.is_lval || !op.is_local)
    return 0;
  *out_off = (int)irop_get_stack_offset(op);
  return 1;
}

static int ir_opt_is_memcpy_like_name(const char *name)
{
  return name &&
         (strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0 ||
          strcmp(name, "__aeabi_memcpy") == 0 || strcmp(name, "__aeabi_memcpy4") == 0 ||
          strcmp(name, "__aeabi_memcpy8") == 0);
}

static int ir_opt_is_memset_like_name(const char *name)
{
  return name && (strcmp(name, "memset") == 0 || strcmp(name, "__aeabi_memset") == 0 ||
                  strcmp(name, "__aeabi_memset4") == 0 || strcmp(name, "__aeabi_memset8") == 0);
}

/* What a library routine does through the pointer arguments it is given.  A name
 * absent from this table is assumed to read AND write through every argument,
 * which is the safe answer.  Names include the __tcc_ redirects that
 * ssa:const_string_calls installs and the EABI aliases the backend emits, since
 * folding runs after both. */
typedef struct StrArgModel
{
  const char *name;
  unsigned read_only_args;  /* bit k: argument k is only read through */
  unsigned write_args;      /* bit k: argument k is written through */
  /* 1 when the routine's destination may not overlap the whole NUL-terminated
   * string it is handed -- C leaves that undefined.  Only then can a write
   * through some other pointer be ruled out of a source we passed it. */
  int src_dst_disjoint;
} StrArgModel;

#define SB_A0 (1u << 0)
#define SB_A1 (1u << 1)

static const StrArgModel str_arg_models[] = {
    {"strcpy", SB_A1, SB_A0, 1},          {"__tcc_strcpy", SB_A1, SB_A0, 1},
    {"stpcpy", SB_A1, SB_A0, 1},          {"strcat", SB_A1, SB_A0, 1},
    {"strncpy", SB_A1, SB_A0, 0},         {"__tcc_strncpy", SB_A1, SB_A0, 0},
    {"strncat", SB_A1, SB_A0, 0},
    {"memcpy", SB_A1, SB_A0, 0},          {"memmove", SB_A1, SB_A0, 0},
    {"__aeabi_memcpy", SB_A1, SB_A0, 0},  {"__aeabi_memcpy4", SB_A1, SB_A0, 0},
    {"__aeabi_memcpy8", SB_A1, SB_A0, 0},
    {"strlen", SB_A0, 0, 0},              {"__tcc_strlen", SB_A0, 0, 0},
    {"strnlen", SB_A0, 0, 0},             {"strchr", SB_A0, 0, 0},
    {"__tcc_strchr", SB_A0, 0, 0},        {"strrchr", SB_A0, 0, 0},
    {"index", SB_A0, 0, 0},               {"rindex", SB_A0, 0, 0},
    {"memchr", SB_A0, 0, 0},              {"__tcc_memchr", SB_A0, 0, 0},
    {"strcmp", SB_A0 | SB_A1, 0, 0},      {"__tcc_strcmp", SB_A0 | SB_A1, 0, 0},
    {"strncmp", SB_A0 | SB_A1, 0, 0},     {"__tcc_strncmp", SB_A0 | SB_A1, 0, 0},
    {"strcoll", SB_A0 | SB_A1, 0, 0},     {"memcmp", SB_A0 | SB_A1, 0, 0},
    {"__tcc_memcmp", SB_A0 | SB_A1, 0, 0},
    {"strstr", SB_A0 | SB_A1, 0, 0},      {"__tcc_strstr", SB_A0 | SB_A1, 0, 0},
    {"strpbrk", SB_A0 | SB_A1, 0, 0},     {"strspn", SB_A0 | SB_A1, 0, 0},
    {"strcspn", SB_A0 | SB_A1, 0, 0},
};

static const StrArgModel *ir_opt_callee_arg_model(const char *name)
{
  if (!name)
    return NULL;
  for (size_t i = 0; i < sizeof(str_arg_models) / sizeof(str_arg_models[0]); i++)
    if (strcmp(str_arg_models[i].name, name) == 0)
      return &str_arg_models[i];
  return NULL;
}

static int ir_opt_callee_arg_is_read_only(const char *name, int param_idx)
{
  const StrArgModel *m = ir_opt_callee_arg_model(name);
  if (!m || param_idx < 0 || param_idx > 31)
    return 0;
  return (int)((m->read_only_args >> param_idx) & 1u);
}

/* BLOCK_COPY's destination is a frame slot written as an lvalue (see
 * str_strcpy.c), so it does not match the bare-address shape above. */
static int ir_opt_block_copy_dest_offset(IROperand op, int *out_off)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) != -1 || op.is_llocal)
    return 0;
  *out_off = (int)irop_get_stack_offset(op);
  return 1;
}

/* Byte-map state for the tracked buffer.  POISON only arises in a backward scan:
 * it marks a byte a nearer write already clobbered with a value we cannot name,
 * so an earlier write must not be allowed to claim it. */
enum { SB_UNKNOWN = 0, SB_KNOWN = 1, SB_POISON = 2 };
enum { SB_TRACK_MAX = 256 };

static void sb_put(uint8_t *bytes, uint8_t *known, int pos, uint8_t val, int backward)
{
  if (pos < 0 || pos >= SB_TRACK_MAX)
    return;
  if (backward && known[pos] != SB_UNKNOWN)
    return; /* a write nearer the use already decided this byte */
  bytes[pos] = val;
  known[pos] = SB_KNOWN;
}

static void sb_clobber(uint8_t *known, int pos, int backward)
{
  if (pos < 0 || pos >= SB_TRACK_MAX)
    return;
  if (!backward)
  {
    known[pos] = SB_UNKNOWN;
    return;
  }
  if (known[pos] == SB_UNKNOWN)
    known[pos] = SB_POISON;
}

/* The rodata-backed copy that established the bytes at the front of the buffer.
 * Kept so a caller can name the buffer's contents as a constant string rather
 * than only its length; `rel` is where the copy landed relative to the buffer. */
typedef struct SbCover
{
  int valid;
  IROperand src;
  int rel;
  int size;
} SbCover;

static void sb_note_cover(SbCover *cover, TCCIRState *ir, int idx, IROperand src, int rel, int size, int backward)
{
  (void)ir;
  (void)idx;
  if (!cover)
    return;
  /* Only a copy that reaches byte 0 can name the string, and the write nearest
   * the use wins -- which is the last one seen going forward, the first going
   * backward. */
  if (rel > 0 || rel + size <= 0)
    return;
  if (backward && cover->valid)
    return;
  cover->valid = 1;
  cover->src = src;
  cover->rel = rel;
  cover->size = size;
}

/* Fold instruction `idx`'s writes into the byte map for the buffer at base_off.
 * Returns 0 when the instruction may write the buffer in a way the map cannot
 * represent, and the caller must give up. */
static int sb_apply(TCCIRState *ir, int idx, int base_off, uint8_t *bytes, uint8_t *known, int backward,
                    SbCover *cover)
{
  IRQuadCompact *q = &ir->compact_instructions[idx];

  if (q->op == TCCIR_OP_STORE)
  {
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    int dst_off;
    int size;
    int rel;
    uint64_t val;

    if (irop_get_tag(dst) != IROP_TAG_STACKOFF || !dst.is_lval || !dst.is_local || dst.is_llocal)
      return 0;

    dst_off = (int)irop_get_stack_offset(dst);
    size = ir_opt_btype_size(irop_get_btype(dst));
    if (size <= 0)
      return 0;
    rel = dst_off - base_off;
    if (rel + size <= 0 || rel >= SB_TRACK_MAX)
      return 1;

    if (!irop_is_immediate(src))
    {
      for (int b = 0; b < size; b++)
        sb_clobber(known, rel + b, backward);
      return 1;
    }

    val = (uint64_t)irop_get_imm64_ex(ir, src);
    for (int b = 0; b < size; b++)
      sb_put(bytes, known, rel + b, (uint8_t)(val >> (b * 8)), backward);
    return 1;
  }

  /* A folded strcpy / aggregate initialiser: a constant blob copied into a frame
   * slot.  Same information as the memcpy call below, one instruction earlier in
   * the pipeline. */
  if (q->op == TCCIR_OP_BLOCK_COPY)
  {
    IROperand dst = tcc_ir_op_get_dest(ir, q);
    IROperand src = tcc_ir_op_get_src1(ir, q);
    IROperand size_op = tcc_ir_op_get_src2(ir, q);
    const char *str;
    int dst_off;
    int size;
    int rel;

    if (!ir_opt_block_copy_dest_offset(dst, &dst_off) || !irop_is_immediate(size_op))
      return 0;
    size = (int)irop_get_imm64_ex(ir, size_op);
    if (size <= 0)
      return 0;
    if (!ir_opt_eval_const_string(ir, src, idx, &str, 0))
      return 0;
    /* Only the string plus its NUL is readable through this pointer; a blob with
     * embedded NULs (a struct initialiser) would run past what we can name. */
    if ((uint64_t)size > (uint64_t)strlen(str) + 1)
      return 0;

    rel = dst_off - base_off;
    if (rel + size <= 0 || rel >= SB_TRACK_MAX)
      return 1;
    for (int b = 0; b < size; b++)
      sb_put(bytes, known, rel + b, (uint8_t)str[b], backward);
    sb_note_cover(cover, ir, idx, src, rel, size, backward);
    return 1;
  }

  if (q->op == TCCIR_OP_FUNCCALLVAL || q->op == TCCIR_OP_FUNCCALLVOID)
  {
    Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, q));
    const char *name = callee ? get_tok_str(callee->v, NULL) : NULL;
    IROperand dst;
    IROperand src;
    IROperand len_op;
    const char *str;
    uint64_t n;
    int dst_off;
    int rel;

    /* memset with a constant fill: a local array's initialiser zero-fills the
       tail this way, so bailing here would lose every such buffer. */
    if (ir_opt_is_memset_like_name(name))
    {
      uint64_t fill;
      if (!ir_opt_get_call_param_operand(ir, idx, 0, &dst) ||
          !ir_opt_get_call_param_operand(ir, idx, 1, &src) ||
          !ir_opt_get_call_param_operand(ir, idx, 2, &len_op))
        return 0;
      if (!ir_opt_stack_addr_offset(dst, &dst_off) ||
          !ir_opt_eval_const_u64(ir, src, idx, &fill, 0) ||
          !ir_opt_eval_const_u64(ir, len_op, idx, &n, 0))
        return 0;
      if (n > (uint64_t)SB_TRACK_MAX * 2)
        return 0;
      rel = dst_off - base_off;
      for (uint64_t b = 0; b < n; b++)
        sb_put(bytes, known, rel + (int)b, (uint8_t)fill, backward);
      return 1;
    }

    if (!ir_opt_is_memcpy_like_name(name))
      return 0;
    if (!ir_opt_get_call_param_operand(ir, idx, 0, &dst) ||
        !ir_opt_get_call_param_operand(ir, idx, 1, &src) ||
        !ir_opt_get_call_param_operand(ir, idx, 2, &len_op))
      return 0;
    if (!ir_opt_stack_addr_offset(dst, &dst_off) ||
        !ir_opt_eval_const_string(ir, src, idx, &str, 0) ||
        !ir_opt_eval_const_u64(ir, len_op, idx, &n, 0))
      return 0;
    if (n > (uint64_t)strlen(str) + 1)
      return 0;

    rel = dst_off - base_off;
    for (uint64_t b = 0; b < n; b++)
      sb_put(bytes, known, rel + (int)b, (uint8_t)str[b], backward);
    sb_note_cover(cover, ir, idx, src, rel, (int)n, backward);
    return 1;
  }

  if (q->op == TCCIR_OP_STORE_INDEXED || q->op == TCCIR_OP_STORE_POSTINC)
    return 0;

  return 1;
}

static int sb_first_nul(const uint8_t *bytes, const uint8_t *known, int *out_len)
{
  for (int i = 0; i < SB_TRACK_MAX; i++)
  {
    if (known[i] != SB_KNOWN)
      return 0;
    if (bytes[i] == 0)
    {
      *out_len = i;
      return 1;
    }
  }
  return 0;
}

/* Forward scan from the top of the function, stopping at the first branch (or at
 * the use, whichever comes first).  *out_stop_idx reports where it stopped, so a
 * caller can check what the rest of the function does to the buffer. */
static int sb_scan_forward(TCCIRState *ir, int base_off, int call_idx, int *out_len, SbCover *cover,
                           int *out_stop_idx)
{
  uint8_t bytes[SB_TRACK_MAX];
  uint8_t known[SB_TRACK_MAX];
  int i;

  memset(bytes, 0, sizeof(bytes));
  memset(known, 0, sizeof(known));

  for (i = 0; i < call_idx; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP || q->op == TCCIR_OP_FUNCPARAMVAL || q->op == TCCIR_OP_FUNCPARAMVOID)
      continue;
    if (q->is_jump_target || q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP)
      break;
    if (!sb_apply(ir, i, base_off, bytes, known, 0, cover))
      return 0;
  }

  if (out_stop_idx)
    *out_stop_idx = i;
  return sb_first_nul(bytes, known, out_len);
}

/* Which callee does this call name?  NULL for an indirect call. */
static const char *sb_callee_name(TCCIRState *ir, int call_idx)
{
  Sym *callee = irop_get_sym_ex(ir, tcc_ir_op_get_src1(ir, &ir->compact_instructions[call_idx]));
  return callee ? get_tok_str(callee->v, NULL) : NULL;
}

/* A bare frame address (`&object`), as opposed to an lvalue slot access. */
static int sb_frame_addr_offset(IROperand op, int *out_off)
{
  if (irop_get_tag(op) != IROP_TAG_STACKOFF || irop_get_vreg(op) != -1 || op.is_lval || op.is_llocal)
    return 0;
  *out_off = (int)irop_get_stack_offset(op);
  return 1;
}

/* Can this call leave [base_off, base_off+span) intact?
 *
 * A callee can only write the range through a pointer it is handed.  Of the
 * frame addresses passed here, one at or above base_off+span belongs to an
 * object above ours and cannot reach down into it; one inside the range is our
 * own buffer, which must therefore sit at a read-only argument; and one BELOW
 * base_off might be an object that encloses ours, so it has to be read-only
 * too -- unless our range is the NUL-terminated source of a routine whose
 * destination is not allowed to overlap that source, which is what makes
 * strcpy(other_buffer, ours) harmless. */
static int sb_call_preserves_range(TCCIRState *ir, int call_idx, int base_off, int span)
{
  enum { SB_MAX_ARGS = 8 };
  const char *name = sb_callee_name(ir, call_idx);
  const StrArgModel *m = ir_opt_callee_arg_model(name);
  int ours_read_only = 0;
  int lower_write = 0;

  for (int k = 0; k < SB_MAX_ARGS; k++)
  {
    IROperand p;
    int o;

    if (!ir_opt_get_call_param_operand(ir, call_idx, k, &p))
      continue; /* params are emitted out of order, so a gap is not the end */
    if (!sb_frame_addr_offset(p, &o))
      continue; /* a value, or a pointer in a vreg -- its producer is checked separately */
    if (o >= base_off + span)
      continue;

    if (o >= base_off)
    {
      if (!ir_opt_callee_arg_is_read_only(name, k))
        return 0;
      ours_read_only = 1;
      continue;
    }

    if (!m)
      return 0; /* unknown callee holding a possibly-enclosing address */
    if ((m->read_only_args >> k) & 1u)
      continue;
    lower_write = 1;
  }

  if (lower_write && !(ours_read_only && m->src_dst_disjoint))
    return 0;
  return 1;
}

/* Reject unless [base_off, base_off+span) survives from `writes_end` to the end of
 * the function with the contents the byte map recorded.
 *
 * Two things are checked over the WHOLE function.  From `writes_end` on, nothing
 * may write the range.  Everywhere -- including the straight-line prologue whose
 * writes the map already accounts for -- the range's address may not escape
 * anywhere it could be written through later.  Once no pointer outside this scan
 * can name the range, stores through unrelated pointers cannot disturb it, which
 * is what lets a map built before a loop still be trusted at a use inside one. */
static int sb_range_undisturbed(TCCIRState *ir, int base_off, int span, int writes_end)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    int in_prologue = (i < writes_end);

    switch (q->op)
    {
    case TCCIR_OP_NOP:
    case TCCIR_OP_FUNCPARAMVAL:
    case TCCIR_OP_FUNCPARAMVOID:
      continue; /* a param's address use is judged at its call */
    case TCCIR_OP_FUNCCALLVAL:
    case TCCIR_OP_FUNCCALLVOID:
      /* sb_apply only lets a memcpy- or memset-like call through the prologue, and
       * neither retains the pointer it is given, so those need no check here. */
      if (in_prologue)
        continue;
      if (!sb_call_preserves_range(ir, i, base_off, span))
        return 0;
      continue;
    case TCCIR_OP_INLINE_ASM:
    case TCCIR_OP_ASM_INPUT:
    case TCCIR_OP_SETJMP:
    case TCCIR_OP_LONGJMP:
    case TCCIR_OP_IJUMP:
      return 0; /* opaque: assume it can reach anything */
    default:
      break;
    }

    for (int k = 0; k < 4; k++)
    {
      IROperand op;
      int off;

      if (k == 0)
      {
        if (!irop_config[q->op].has_dest)
          continue;
        op = tcc_ir_op_get_dest(ir, q);
      }
      else if (k == 1)
      {
        if (!irop_config[q->op].has_src1)
          continue;
        op = tcc_ir_op_get_src1(ir, q);
      }
      else if (k == 2)
      {
        if (!irop_config[q->op].has_src2)
          continue;
        op = tcc_ir_op_get_src2(ir, q);
      }
      else
      {
        if (q->op != TCCIR_OP_STORE_INDEXED && q->op != TCCIR_OP_LOAD_INDEXED &&
            q->op != TCCIR_OP_STORE_POSTINC)
          continue;
        op = tcc_ir_op_get_scale(ir, q);
      }

      /* A bare frame address at or below the range is either the range itself
       * escaping or an object that might enclose it.  Outside a call argument
       * there is no contract to lean on, so refuse -- in the prologue too, since
       * an address laundered into a vreg there can be written through later. */
      if (sb_frame_addr_offset(op, &off) && off < base_off + span)
        return 0;

      /* A direct lvalue access to the range: reads are fine, writes are not
       * (unless they are the prologue writes the map is built from). */
      if (irop_get_tag(op) == IROP_TAG_STACKOFF && op.is_lval && irop_get_vreg(op) == -1)
      {
        int width = ir_opt_btype_size(irop_get_btype(op));
        if (width <= 0)
          width = 1;
        off = (int)irop_get_stack_offset(op);
        if (off < base_off + span && off + width > base_off)
        {
          int is_write = (k == 0) && (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                                      q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY);
          if (is_write && !in_prologue)
            return 0;
        }
      }
    }
  }
  return 1;
}

/* Block-local backward scan: walks back from the use to the top of its basic
 * block.  This is what sees a buffer rebuilt on every trip of a loop, where the
 * forward scan cannot get past the loop header.  Sound without dominance
 * information because a length is only reported when the bytes up to the NUL
 * were all written by instructions inside this block, and every instruction
 * between those writes and the use has been examined. */
static int sb_scan_block_backward(TCCIRState *ir, int base_off, int call_idx, int *out_len)
{
  uint8_t bytes[SB_TRACK_MAX];
  uint8_t known[SB_TRACK_MAX];

  memset(bytes, 0, sizeof(bytes));
  memset(known, 0, sizeof(known));

  for (int i = call_idx - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF || q->op == TCCIR_OP_IJUMP)
      return 0; /* walked off the top of the block */
    if (q->op != TCCIR_OP_NOP && q->op != TCCIR_OP_FUNCPARAMVAL && q->op != TCCIR_OP_FUNCPARAMVOID)
    {
      if (!sb_apply(ir, i, base_off, bytes, known, 1, NULL))
        return 0;
    }
    if (q->is_jump_target)
      break; /* this instruction opens the block; nothing above it is ours */
  }

  return sb_first_nul(bytes, known, out_len);
}

int ir_opt_eval_stack_strlen(TCCIRState *ir, IROperand arg, int call_idx, int *out_len)
{
  int base_off;
  int stop_idx = 0;

  if (!ir || !out_len || !ir_opt_stack_addr_offset(arg, &base_off))
    return 0;

  if (sb_scan_forward(ir, base_off, call_idx, out_len, NULL, &stop_idx))
  {
    /* The scan reached the use without meeting a branch: the map is exact. */
    if (stop_idx >= call_idx && sb_range_undisturbed(ir, base_off, *out_len + 1, stop_idx))
      return 1;
    /* It stopped early.  The map still describes the buffer at the use provided
     * nothing from there on can write the bytes up to the NUL. */
    if (stop_idx < call_idx && sb_range_undisturbed(ir, base_off, *out_len + 1, stop_idx))
      return 1;
  }

  return sb_scan_block_backward(ir, base_off, call_idx, out_len);
}

int ir_opt_eval_stack_const_string(TCCIRState *ir, IROperand arg, int use_idx, IROperand *out_sym, int *out_len)
{
  SbCover cover;
  IRPoolSymref *sr;
  IROperand cand;
  const char *str;
  uint32_t pool_idx;
  int base_off;
  int stop_idx = 0;
  int len = 0;
  int32_t addend;

  if (!ir || !out_sym || !out_len || !ir_opt_stack_addr_offset(arg, &base_off))
    return 0;

  memset(&cover, 0, sizeof(cover));
  if (!sb_scan_forward(ir, base_off, use_idx, &len, &cover, &stop_idx))
    return 0;
  if (!cover.valid)
    return 0;
  if (!sb_range_undisturbed(ir, base_off, len + 1, stop_idx))
    return 0;

  /* The covering copy put source byte (p - cover.rel) at buffer byte p, so the
   * string starts cover.rel bytes into the copy's source. */
  sr = irop_get_symref_ex(ir, cover.src);
  if (!sr || !sr->sym)
    return 0;
  addend = sr->addend - cover.rel;
  if (addend < 0)
    return 0;
  pool_idx = tcc_ir_pool_add_symref(ir, sr->sym, addend, sr->flags);
  cand = irop_make_symref(-1, pool_idx, 0, 0, 1, IROP_BTYPE_INT32);

  /* Confirm against the rodata itself: the candidate must be a string of exactly
   * the length the byte map found.  A later write inside the buffer, or a copy
   * that did not reach as far as the NUL, fails here. */
  str = ir_opt_get_constant_string_from_symref(ir, cand);
  if (!str || (int)strlen(str) != len)
    return 0;
  if (cover.rel + cover.size < len + 1)
    return 0;

  *out_sym = cand;
  *out_len = len;
  return 1;
}
