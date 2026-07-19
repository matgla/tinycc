/*
 *  TCC IR - Pure-function call const folding + switch-func IPCP (flat pass)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Relocated flat pass, Branch A [A*]; see docs/plan_legacy_flat_ir_ssa_retire.md */

#define USING_GLOBALS

#include "ir.h"
#include "opt_engine.h"
#include "opt_utils.h"
#include "opt_du.h"

int tcc_ir_detect_const_result(TCCIRState *ir, int64_t *value, int *btype)
{
  int n = ir->next_instruction_index;
  if (n == 0 || ir->parameters_count > 0)
    return 0;

  int non_nop_count = 0;
  int ret_idx = -1;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;

    non_nop_count++;

    switch (q->op)
    {
    case TCCIR_OP_ASSIGN:
    case TCCIR_OP_RETURNVALUE:
      break;
    default:
      return 0;
    }

    if (q->op == TCCIR_OP_RETURNVALUE)
      ret_idx = i;
  }

  if (ret_idx < 0 || non_nop_count > 4)
    return 0;

  IRQuadCompact *ret_q = &ir->compact_instructions[ret_idx];
  IROperand src1 = tcc_ir_op_get_src1(ir, ret_q);

  if (irop_is_immediate(src1))
  {
    *value = irop_get_imm64_ex(ir, src1);
    *btype = irop_get_btype(src1);
    return 1;
  }

  int32_t ret_vr = irop_get_vreg(src1);
  if (ret_vr < 0)
    return 0;

  for (int i = ret_idx - 1; i >= 0; i--)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (q->op == TCCIR_OP_ASSIGN)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      if (irop_get_vreg(dest) == ret_vr)
      {
        IROperand as1 = tcc_ir_op_get_src1(ir, q);
        if (irop_is_immediate(as1))
        {
          *value = irop_get_imm64_ex(ir, as1);
          *btype = irop_get_btype(as1);
          return 1;
        }
        return 0;
      }
    }
    break;
  }

  return 0;
}

void tcc_ir_cache_const_result(TCCState *s, int func_token, int64_t value, int btype)
{
  if (s->func_const_result_cache_count >= FUNC_CONST_RESULT_CACHE_SIZE)
    return;
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
      return;
  }
  int idx = s->func_const_result_cache_count++;
  s->func_const_result_cache[idx].token = func_token;
  s->func_const_result_cache[idx].value = value;
  s->func_const_result_cache[idx].btype = btype;
}

int tcc_ir_lookup_const_result(TCCState *s, int func_token, int64_t *value, int *btype)
{
  for (int i = 0; i < s->func_const_result_cache_count; i++)
  {
    if (s->func_const_result_cache[i].token == func_token)
    {
      *value = s->func_const_result_cache[i].value;
      *btype = s->func_const_result_cache[i].btype;
      return 1;
    }
  }
  return 0;
}

int tcc_ir_opt_const_call_replace(TCCIRState *ir)
{
  int n = ir->next_instruction_index;
  int changes = 0;

  if (n == 0 || !tcc_state || tcc_state->func_const_result_cache_count == 0)
    return 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand callee_op = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, callee_op);
    if (!callee)
      continue;

    int64_t val;
    int btype;
    if (!tcc_ir_lookup_const_result(tcc_state, callee->v, &val, &btype))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);
    IROperand call_info = tcc_ir_op_get_src2(ir, q);
    int call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, call_info));
    int32_t dest_vr = irop_get_vreg(dest);

    LOG_IR_GEN("OPTIMIZE: IPC replace call to %s with #%lld at i=%d", get_tok_str(callee->v, NULL), (long long)val, i);

    /* No dest vreg: result discarded, call is pure, so NOP it. */
    if (dest_vr < 0)
    {
      q->op = TCCIR_OP_NOP;
    }
    else
    {
      q->op = TCCIR_OP_ASSIGN;
      if (val == (int32_t)val)
        tcc_ir_set_src1(ir, i, irop_make_imm32(-1, (int32_t)val, btype));
      else
      {
        uint32_t pool_idx = tcc_ir_pool_add_i64(ir, val);
        tcc_ir_set_src1(ir, i, irop_make_i64(-1, pool_idx, btype));
      }
      tcc_ir_set_src2(ir, i, IROP_NONE);
      tcc_ir_set_dest(ir, i, dest);
    }

    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op == TCCIR_OP_FUNCPARAMVAL || pq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
        int p_call_id = TCCIR_DECODE_CALL_ID((int)irop_get_imm64_ex(ir, ps2));
        if (p_call_id == call_id)
          pq->op = TCCIR_OP_NOP;
        continue;
      }
      break;
    }

    changes++;
  }

  return changes;
}

/* Switch-value function IPCP */

#define SWITCH_FUNC_MAX_OPS 512
#define SWITCH_FUNC_SIM_MAX_VREGS 64
#define SWITCH_FUNC_SIM_MAX_REPLAY 64

/* Compact replayable op; flags: 1=src1 imm, 2=src2 imm, 4=src1 lval-sym load, 8=dest lval-sym store */
typedef struct SwitchSimOp
{
  uint8_t op;
  uint8_t flags;
  int32_t dest_vreg;
  int32_t src1_vreg;
  int64_t src1_imm;
  int32_t src2_vreg;
  int64_t src2_imm;
  int32_t target;
  /* lval-symref payload (flags 4 or 8) */
  struct Sym *sym;
  int32_t sym_addend;
  uint16_t sym_flags;
  int8_t sym_btype;
} SwitchSimOp;

struct TCCFuncSwitchSnapshot
{
  int token;
  int param_vreg;
  int btype;
  int op_count;
  SwitchSimOp *ops;
};

void tcc_ir_switch_func_snapshot_free(TCCFuncSwitchSnapshot *snap)
{
  if (!snap)
    return;
  tcc_free(snap->ops);
  tcc_free(snap);
}

/* Return-type btypes we know how to fold into the caller. */
static int switch_func_is_supported_btype(int btype)
{
  return btype == IROP_BTYPE_INT32 || btype == IROP_BTYPE_INT8 || btype == IROP_BTYPE_INT16;
}

/* Decode operand as immediate or vreg-ref; 0 if anything else. */
static int switch_func_decode_operand(TCCIRState *ir, IROperand op, int32_t *out_vreg, int64_t *out_imm)
{
  if (op.is_lval || op.is_llocal)
    return 0;
  /* Reject 64-bit and FP/complex operands. */
  int btype = irop_get_btype(op);
  if (btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16)
    return 0;
  if (irop_is_immediate(op))
  {
    *out_vreg = -1;
    *out_imm = irop_get_imm64_ex(ir, op);
    return 1;
  }
  int tag = irop_get_tag(op);
  if (tag != IROP_TAG_VREG)
    return 0;
  int32_t v = irop_get_vreg(op);
  if (v < 0)
    return 0;
  *out_vreg = v;
  *out_imm = 0;
  return 1;
}

/* Detect an lval-symref operand (load/store-to-global); fills snap_op sym fields. */
static int switch_func_decode_lval_sym(TCCIRState *ir, IROperand op, SwitchSimOp *snap_op)
{
  if (!op.is_lval || op.is_llocal)
    return 0;
  if (irop_get_tag(op) != IROP_TAG_SYMREF)
    return 0;
  int btype = irop_get_btype(op);
  if (btype != IROP_BTYPE_INT32 && btype != IROP_BTYPE_INT8 && btype != IROP_BTYPE_INT16)
    return 0;
  IRPoolSymref *sr = irop_get_symref_ex(ir, op);
  if (!sr || !sr->sym)
    return 0;
  snap_op->sym = sr->sym;
  snap_op->sym_addend = sr->addend;
  snap_op->sym_flags = (uint16_t)sr->flags;
  snap_op->sym_btype = (int8_t)btype;
  return 1;
}

int tcc_ir_detect_switch_func(TCCIRState *ir, TCCFuncSwitchSnapshot **out)
{
  if (!ir || !out)
    return 0;
  if (ir->parameters_count != 1)
    return 0;

  /* Only accept 32-bit-or-smaller scalar parameters. */
  if (ir->parameters_live_intervals_size < 1 || !ir->parameters_live_intervals)
    return 0;
  const IRLiveInterval *piv = &ir->parameters_live_intervals[0];
  if (piv->is_llong || piv->is_float || piv->is_double || piv->is_complex)
    return 0;
  if (piv->addrtaken)
    return 0;

  int n = ir->next_instruction_index;
  if (n == 0 || n > SWITCH_FUNC_MAX_OPS)
    return 0;

  int param_vreg = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_PARAM, 0);

  SwitchSimOp *ops = tcc_mallocz(n * sizeof(SwitchSimOp));
  int return_btype = -1;
  int has_return = 0;

  for (int i = 0; i < n; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    SwitchSimOp *o = &ops[i];
    o->op = q->op;
    o->dest_vreg = -1;
    o->src1_vreg = -1;
    o->src2_vreg = -1;
    o->target = -1;

    switch (q->op)
    {
    case TCCIR_OP_NOP:
      break;
    case TCCIR_OP_ASSIGN:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s = tcc_ir_op_get_src1(ir, q);
      if (d.is_lval || d.is_llocal)
        goto fail;
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0)
        goto fail;
      o->dest_vreg = dvr;
      /* src1 may be: immediate, plain vreg, or an lval-symref (LOAD form). */
      if (switch_func_decode_lval_sym(ir, s, o))
      {
        o->flags |= 4; /* src1 is lval-sym load */
        break;
      }
      int64_t imm;
      int32_t svr;
      if (!switch_func_decode_operand(ir, s, &svr, &imm))
        goto fail;
      if (svr < 0)
      {
        o->flags |= 1;
        o->src1_imm = imm;
      }
      else
      {
        o->src1_vreg = svr;
      }
      break;
    }
    case TCCIR_OP_STORE:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s = tcc_ir_op_get_src1(ir, q);
      /* STORE to a global: dest must be lval-symref. */
      if (!switch_func_decode_lval_sym(ir, d, o))
        goto fail;
      o->flags |= 8; /* dest is lval-sym store */
      /* Value being stored may be immediate or vreg (must be 32-bit-or-smaller). */
      int64_t imm;
      int32_t svr;
      if (!switch_func_decode_operand(ir, s, &svr, &imm))
        goto fail;
      if (svr < 0)
      {
        o->flags |= 1;
        o->src1_imm = imm;
      }
      else
      {
        o->src1_vreg = svr;
      }
      break;
    }
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      if (d.is_lval || d.is_llocal)
        goto fail;
      int32_t dvr = irop_get_vreg(d);
      if (dvr < 0)
        goto fail;
      o->dest_vreg = dvr;
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      if (!switch_func_decode_operand(ir, s2, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 2; o->src2_imm = imm; } else o->src2_vreg = vr;
      break;
    }
    case TCCIR_OP_CMP:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand s2 = tcc_ir_op_get_src2(ir, q);
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      if (!switch_func_decode_operand(ir, s2, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 2; o->src2_imm = imm; } else o->src2_vreg = vr;
      break;
    }
    case TCCIR_OP_JUMP:
    {
      IROperand d = tcc_ir_op_get_dest(ir, q);
      int64_t t = irop_get_imm64_ex(ir, d);
      if (t < 0 || t >= n)
        goto fail;
      o->target = (int32_t)t;
      break;
    }
    case TCCIR_OP_JUMPIF:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      IROperand d = tcc_ir_op_get_dest(ir, q);
      o->src1_imm = irop_get_imm64_ex(ir, s1); /* condition code (TOK_*) */
      int64_t t = irop_get_imm64_ex(ir, d);
      if (t < 0 || t >= n)
        goto fail;
      o->target = (int32_t)t;
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    {
      IROperand s1 = tcc_ir_op_get_src1(ir, q);
      int64_t imm;
      int32_t vr;
      if (!switch_func_decode_operand(ir, s1, &vr, &imm))
        goto fail;
      if (vr < 0) { o->flags |= 1; o->src1_imm = imm; } else o->src1_vreg = vr;
      int b = irop_get_btype(s1);
      if (return_btype < 0)
        return_btype = b;
      else if (return_btype != b)
        goto fail;
      has_return = 1;
      break;
    }
    default:
      goto fail;
    }
  }

  if (!has_return)
    goto fail;
  if (!switch_func_is_supported_btype(return_btype))
    goto fail;

  TCCFuncSwitchSnapshot *snap = tcc_mallocz(sizeof(*snap));
  snap->token = 0;
  snap->param_vreg = param_vreg;
  snap->btype = return_btype;
  snap->op_count = n;
  snap->ops = ops;
  *out = snap;
  return 1;

fail:
  tcc_free(ops);
  return 0;
}

void tcc_ir_cache_switch_func(TCCState *s, int func_token, TCCFuncSwitchSnapshot *snap)
{
  if (!s || !snap)
  {
    tcc_ir_switch_func_snapshot_free(snap);
    return;
  }
  for (int i = 0; i < s->func_switch_cache_count; i++)
  {
    if (s->func_switch_cache[i] && s->func_switch_cache[i]->token == func_token)
    {
      tcc_ir_switch_func_snapshot_free(snap);
      return;
    }
  }
  if (s->func_switch_cache_count >= FUNC_SWITCH_CACHE_SIZE)
  {
    tcc_ir_switch_func_snapshot_free(snap);
    return;
  }
  snap->token = func_token;
  s->func_switch_cache[s->func_switch_cache_count++] = snap;
}

const TCCFuncSwitchSnapshot *tcc_ir_lookup_switch_func(TCCState *s, int func_token)
{
  if (!s)
    return NULL;
  for (int i = 0; i < s->func_switch_cache_count; i++)
  {
    const TCCFuncSwitchSnapshot *snap = s->func_switch_cache[i];
    if (snap && snap->token == func_token)
      return snap;
  }
  return NULL;
}

void tcc_ir_free_switch_func_cache(TCCState *s)
{
  if (!s)
    return;
  for (int i = 0; i < s->func_switch_cache_count; i++)
    tcc_ir_switch_func_snapshot_free(s->func_switch_cache[i]);
  s->func_switch_cache_count = 0;
}

/* Linear-search vreg->value map; values are known (concrete) or tracked-unknown. */
typedef struct SwitchSimEnv
{
  int32_t vregs[SWITCH_FUNC_SIM_MAX_VREGS];
  int64_t values[SWITCH_FUNC_SIM_MAX_VREGS];
  uint8_t known[SWITCH_FUNC_SIM_MAX_VREGS]; /* 1 if `values[i]` is concrete */
  int count;
} SwitchSimEnv;

static int switch_sim_set(SwitchSimEnv *env, int32_t vreg, int64_t value, int known)
{
  for (int i = 0; i < env->count; i++)
  {
    if (env->vregs[i] == vreg)
    {
      env->values[i] = value;
      env->known[i] = (uint8_t)known;
      return 1;
    }
  }
  if (env->count >= SWITCH_FUNC_SIM_MAX_VREGS)
    return 0;
  env->vregs[env->count] = vreg;
  env->values[env->count] = value;
  env->known[env->count] = (uint8_t)known;
  env->count++;
  return 1;
}

/* Get a vreg's value; 1=known (writes *out), 2=tracked-unknown, 0=not in env. */
static int switch_sim_get(const SwitchSimEnv *env, int32_t vreg, int64_t *out)
{
  for (int i = 0; i < env->count; i++)
  {
    if (env->vregs[i] == vreg)
    {
      if (env->known[i])
      {
        *out = env->values[i];
        return 1;
      }
      return 2;
    }
  }
  return 0;
}

/* Read src `which` value from o; 1=known (writes *out), 2=unknown, 0=fail. */
static int switch_sim_read_src(const SwitchSimEnv *env, const SwitchSimOp *o,
                               int which /* 1 or 2 */, int64_t *out)
{
  if (which == 1)
  {
    if (o->flags & 1) { *out = o->src1_imm; return 1; }
    return switch_sim_get(env, o->src1_vreg, out);
  }
  if (o->flags & 2) { *out = o->src2_imm; return 1; }
  return switch_sim_get(env, o->src2_vreg, out);
}

/* Record an op index for replay at the caller; cap protects unbounded growth. */
static int switch_sim_record_replay(int *replay, int *count, int op_idx)
{
  if (*count >= SWITCH_FUNC_SIM_MAX_REPLAY)
    return 0;
  replay[(*count)++] = op_idx;
  return 1;
}

/* Simulate the snapshot for one constant arg_value; replay_indices=NULL bails on any side-effecting function. */
int tcc_ir_simulate_switch_func_ex(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                   int64_t *out_value, int *out_btype,
                                   int *replay_indices, int *replay_count)
{
  if (!snap || !out_value)
    return 0;

  SwitchSimEnv env;
  env.count = 0;
  if (!switch_sim_set(&env, snap->param_vreg, arg_value, 1))
    return 0;

  int local_replay_count = 0;
  if (replay_count)
    *replay_count = 0;

  int64_t flag_lhs = 0, flag_rhs = 0;
  int has_flags = 0;
  int pc = 0;
  int step_limit = 4 * snap->op_count + 64;

  while (pc >= 0 && pc < snap->op_count)
  {
    if (--step_limit < 0)
      return 0;
    const SwitchSimOp *o = &snap->ops[pc];
    switch (o->op)
    {
    case TCCIR_OP_NOP:
      pc++;
      break;
    case TCCIR_OP_ASSIGN:
    {
      /* LOAD form: dest becomes tracked-unknown, op replayed at caller. */
      if (o->flags & 4)
      {
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
        pc++;
        break;
      }
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      if (r == 0)
        return 0;
      if (r == 2)
      {
        /* src1 tracked-unknown → dest tracked-unknown, replay op. */
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
      }
      else
      {
        if (!switch_sim_set(&env, o->dest_vreg, v, 1))
          return 0;
      }
      pc++;
      break;
    }
    case TCCIR_OP_STORE:
    {
      if (!replay_indices)
        return 0;
      /* Verify src1 is readable; bail if it references an unseen vreg. */
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      if (r == 0)
        return 0;
      if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
        return 0;
      pc++;
      break;
    }
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      int64_t l = 0, r1 = 0;
      int rl = switch_sim_read_src(&env, o, 1, &l);
      int rr = switch_sim_read_src(&env, o, 2, &r1);
      if (rl == 0 || rr == 0)
        return 0;
      if (rl == 1 && rr == 1)
      {
        int64_t v = (o->op == TCCIR_OP_ADD) ? (l + r1) : (l - r1);
        if (!switch_sim_set(&env, o->dest_vreg, v, 1))
          return 0;
      }
      else
      {
        if (!replay_indices)
          return 0;
        if (!switch_sim_set(&env, o->dest_vreg, 0, 0))
          return 0;
        if (!switch_sim_record_replay(replay_indices, &local_replay_count, pc))
          return 0;
      }
      pc++;
      break;
    }
    case TCCIR_OP_CMP:
    {
      int64_t l, r;
      int rl = switch_sim_read_src(&env, o, 1, &l);
      int rr = switch_sim_read_src(&env, o, 2, &r);
      /* Both operands must be concrete to decide an upcoming JUMPIF. */
      if (rl != 1 || rr != 1)
        return 0;
      flag_lhs = l;
      flag_rhs = r;
      has_flags = 1;
      pc++;
      break;
    }
    case TCCIR_OP_JUMP:
      pc = o->target;
      break;
    case TCCIR_OP_JUMPIF:
    {
      if (!has_flags)
        return 0;
      int32_t ls = (int32_t)flag_lhs;
      int32_t rs = (int32_t)flag_rhs;
      uint32_t lu = (uint32_t)flag_lhs;
      uint32_t ru = (uint32_t)flag_rhs;
      int taken;
      switch ((int)o->src1_imm)
      {
      case TOK_EQ:  taken = (ls == rs); break;
      case TOK_NE:  taken = (ls != rs); break;
      case TOK_LT:  taken = (ls <  rs); break;
      case TOK_LE:  taken = (ls <= rs); break;
      case TOK_GT:  taken = (ls >  rs); break;
      case TOK_GE:  taken = (ls >= rs); break;
      case TOK_ULT: taken = (lu <  ru); break;
      case TOK_ULE: taken = (lu <= ru); break;
      case TOK_UGT: taken = (lu >  ru); break;
      case TOK_UGE: taken = (lu >= ru); break;
      default: return 0;
      }
      pc = taken ? o->target : pc + 1;
      break;
    }
    case TCCIR_OP_RETURNVALUE:
    {
      int64_t v;
      int r = switch_sim_read_src(&env, o, 1, &v);
      /* Return value must be a concrete constant. */
      if (r != 1)
        return 0;
      *out_value = v;
      if (out_btype)
        *out_btype = snap->btype;
      if (replay_count)
        *replay_count = local_replay_count;
      return 1;
    }
    default:
      return 0;
    }
  }
  return 0;
}

/* Wrapper: pure-fold only, no replay. */
int tcc_ir_simulate_switch_func(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                                int64_t *out_value, int *out_btype)
{
  return tcc_ir_simulate_switch_func_ex(snap, arg_value, out_value, out_btype, NULL, NULL);
}

/* Map callee vreg -> caller tmp vreg for emitting a replayed case body. */
typedef struct VregMap
{
  int32_t callee_vreg[SWITCH_FUNC_SIM_MAX_VREGS];
  int32_t caller_vreg[SWITCH_FUNC_SIM_MAX_VREGS];
  int count;
} VregMap;

static int32_t map_callee_vreg(VregMap *m, TCCIRState *ir, int32_t callee_vr)
{
  for (int k = 0; k < m->count; k++)
    if (m->callee_vreg[k] == callee_vr)
      return m->caller_vreg[k];
  if (m->count >= SWITCH_FUNC_SIM_MAX_VREGS)
    return -1;
  int32_t fresh = TCCIR_ENCODE_VREG(TCCIR_VREG_TYPE_TEMP, ir->next_temporary_variable++);
  m->callee_vreg[m->count] = callee_vr;
  m->caller_vreg[m->count] = fresh;
  m->count++;
  return fresh;
}

/* Insert new_q before before_idx, fixing up jump targets; 1 on success. */
static int switch_insert_before(TCCIRState *ir, int before_idx, const IRQuadCompact *new_q)
{
  if (ir->next_instruction_index + 1 >= ir->compact_instructions_size)
  {
    int new_size = ir->compact_instructions_size << 1;
    ir->compact_instructions =
        (IRQuadCompact *)tcc_realloc(ir->compact_instructions, sizeof(IRQuadCompact) * new_size);
    if (!ir->compact_instructions)
      return 0;
    ir->compact_instructions_size = new_size;
  }
  for (int k = ir->next_instruction_index; k > before_idx; k--)
    ir->compact_instructions[k] = ir->compact_instructions[k - 1];
  ir->compact_instructions[before_idx] = *new_q;
  ir->next_instruction_index++;
  for (int k = 0; k < ir->next_instruction_index; k++)
  {
    IRQuadCompact *q = &ir->compact_instructions[k];
    if (q->op == TCCIR_OP_JUMP || q->op == TCCIR_OP_JUMPIF)
    {
      IROperand dest = tcc_ir_op_get_dest(ir, q);
      int target = (int)irop_get_imm64_ex(ir, dest);
      if (target >= before_idx && k != before_idx)
      {
        IROperand new_dest = irop_make_imm32(-1, target + 1, IROP_BTYPE_INT32);
        tcc_ir_op_set_dest(ir, q, new_dest);
      }
    }
  }
  return 1;
}

/* Build caller-side operand for a snapshot op's src1; 1 on success. */
static int build_caller_src1(TCCIRState *caller_ir, VregMap *m, const SwitchSimOp *o,
                             const SwitchSimEnv *sim_env, int btype, IROperand *out)
{
  if (o->flags & 1)
  {
    *out = irop_make_imm32(-1, (int32_t)o->src1_imm, btype);
    return 1;
  }
  /* Vreg source: inline known value, else map to a caller tmp. */
  int64_t v;
  if (switch_sim_get(sim_env, o->src1_vreg, &v) == 1)
  {
    *out = irop_make_imm32(-1, (int32_t)v, btype);
    return 1;
  }
  int32_t cvr = map_callee_vreg(m, caller_ir, o->src1_vreg);
  if (cvr < 0)
    return 0;
  *out = irop_make_vreg(cvr, btype);
  return 1;
}

static int build_caller_src2(TCCIRState *caller_ir, VregMap *m, const SwitchSimOp *o,
                             const SwitchSimEnv *sim_env, int btype, IROperand *out)
{
  if (o->flags & 2)
  {
    *out = irop_make_imm32(-1, (int32_t)o->src2_imm, btype);
    return 1;
  }
  int64_t v;
  if (switch_sim_get(sim_env, o->src2_vreg, &v) == 1)
  {
    *out = irop_make_imm32(-1, (int32_t)v, btype);
    return 1;
  }
  int32_t cvr = map_callee_vreg(m, caller_ir, o->src2_vreg);
  if (cvr < 0)
    return 0;
  *out = irop_make_vreg(cvr, btype);
  return 1;
}

/* Emit one replay op before *pcall_idx (bumped by 1 on success). */
static int emit_replay_op(TCCIRState *ir, VregMap *m, const TCCFuncSwitchSnapshot *snap,
                          const SwitchSimEnv *sim_env, int snap_op_idx, int *pcall_idx)
{
  const SwitchSimOp *o = &snap->ops[snap_op_idx];
  IRQuadCompact nq = {0};
  nq.op = o->op;

  switch (o->op)
  {
  case TCCIR_OP_ASSIGN:
  {
    int btype = (o->flags & 4) ? o->sym_btype : IROP_BTYPE_INT32;
    int32_t dvr = map_callee_vreg(m, ir, o->dest_vreg);
    if (dvr < 0) return 0;
    IROperand dest = irop_make_vreg(dvr, btype);
    IROperand src1;
    if (o->flags & 4)
    {
      /* LOAD form: build a fresh symref operand in the caller's pool. */
      uint32_t sidx = tcc_ir_pool_add_symref(ir, o->sym, o->sym_addend, o->sym_flags);
      src1 = irop_make_symref(-1, sidx, 1 /* is_lval */, 0, 0, o->sym_btype);
    }
    else if (!build_caller_src1(ir, m, o, sim_env, btype, &src1))
      return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    break;
  }
  case TCCIR_OP_STORE:
  {
    /* dest is lval-symref; src1 is value (vreg or imm). */
    int btype = o->sym_btype;
    uint32_t sidx = tcc_ir_pool_add_symref(ir, o->sym, o->sym_addend, o->sym_flags);
    IROperand dest = irop_make_symref(-1, sidx, 1, 0, 0, btype);
    IROperand src1;
    if (!build_caller_src1(ir, m, o, sim_env, btype, &src1))
      return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    break;
  }
  case TCCIR_OP_ADD:
  case TCCIR_OP_SUB:
  {
    int btype = IROP_BTYPE_INT32;
    int32_t dvr = map_callee_vreg(m, ir, o->dest_vreg);
    if (dvr < 0) return 0;
    IROperand dest = irop_make_vreg(dvr, btype);
    IROperand src1, src2;
    if (!build_caller_src1(ir, m, o, sim_env, btype, &src1)) return 0;
    if (!build_caller_src2(ir, m, o, sim_env, btype, &src2)) return 0;
    nq.operand_base = tcc_ir_pool_add(ir, dest);
    tcc_ir_pool_add(ir, src1);
    tcc_ir_pool_add(ir, src2);
    break;
  }
  default:
    return 0;
  }

  if (!switch_insert_before(ir, *pcall_idx, &nq))
    return 0;
  (*pcall_idx)++;
  return 1;
}

/* Re-run the simulation to repopulate the env for replay emission. */
static int rebuild_sim_env(const TCCFuncSwitchSnapshot *snap, int64_t arg_value,
                           SwitchSimEnv *env)
{
  env->count = 0;
  if (!switch_sim_set(env, snap->param_vreg, arg_value, 1))
    return 0;
  int64_t flag_lhs = 0, flag_rhs = 0;
  int has_flags = 0;
  int pc = 0;
  int step_limit = 4 * snap->op_count + 64;
  while (pc >= 0 && pc < snap->op_count)
  {
    if (--step_limit < 0) return 0;
    const SwitchSimOp *o = &snap->ops[pc];
    switch (o->op)
    {
    case TCCIR_OP_NOP: pc++; break;
    case TCCIR_OP_ASSIGN:
      if (o->flags & 4)
        switch_sim_set(env, o->dest_vreg, 0, 0);
      else
      {
        int64_t v;
        int r = switch_sim_read_src(env, o, 1, &v);
        if (r == 1) switch_sim_set(env, o->dest_vreg, v, 1);
        else if (r == 2) switch_sim_set(env, o->dest_vreg, 0, 0);
        else return 0;
      }
      pc++;
      break;
    case TCCIR_OP_STORE: pc++; break;
    case TCCIR_OP_ADD:
    case TCCIR_OP_SUB:
    {
      int64_t l = 0, r1 = 0;
      int rl = switch_sim_read_src(env, o, 1, &l);
      int rr = switch_sim_read_src(env, o, 2, &r1);
      if (rl == 0 || rr == 0) return 0;
      if (rl == 1 && rr == 1)
      {
        int64_t v = (o->op == TCCIR_OP_ADD) ? (l + r1) : (l - r1);
        switch_sim_set(env, o->dest_vreg, v, 1);
      }
      else switch_sim_set(env, o->dest_vreg, 0, 0);
      pc++;
      break;
    }
    case TCCIR_OP_CMP:
    {
      int64_t l, r;
      int rl = switch_sim_read_src(env, o, 1, &l);
      int rr = switch_sim_read_src(env, o, 2, &r);
      if (rl != 1 || rr != 1) return 0;
      flag_lhs = l; flag_rhs = r; has_flags = 1; pc++;
      break;
    }
    case TCCIR_OP_JUMP: pc = o->target; break;
    case TCCIR_OP_JUMPIF:
    {
      if (!has_flags) return 0;
      int32_t ls = (int32_t)flag_lhs, rs = (int32_t)flag_rhs;
      uint32_t lu = (uint32_t)flag_lhs, ru = (uint32_t)flag_rhs;
      int taken;
      switch ((int)o->src1_imm)
      {
      case TOK_EQ:  taken = (ls == rs); break;
      case TOK_NE:  taken = (ls != rs); break;
      case TOK_LT:  taken = (ls <  rs); break;
      case TOK_LE:  taken = (ls <= rs); break;
      case TOK_GT:  taken = (ls >  rs); break;
      case TOK_GE:  taken = (ls >= rs); break;
      case TOK_ULT: taken = (lu <  ru); break;
      case TOK_ULE: taken = (lu <= ru); break;
      case TOK_UGT: taken = (lu >  ru); break;
      case TOK_UGE: taken = (lu >= ru); break;
      default: return 0;
      }
      pc = taken ? o->target : pc + 1;
      break;
    }
    case TCCIR_OP_RETURNVALUE: return 1;
    default: return 0;
    }
  }
  return 0;
}

/* Fold each FUNCCALLVAL to a switch-value function with a constant arg. */
int tcc_ir_opt_switch_call_replace(TCCIRState *ir)
{
  if (!ir || !tcc_state || tcc_state->func_switch_cache_count == 0)
    return 0;

  int changes = 0;

  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_FUNCCALLVAL)
      continue;

    IROperand callee_op = tcc_ir_op_get_src1(ir, q);
    Sym *callee = irop_get_sym_ex(ir, callee_op);
    if (!callee)
      continue;

    const TCCFuncSwitchSnapshot *snap = tcc_ir_lookup_switch_func(tcc_state, callee->v);
    if (!snap)
      continue;

    IROperand call_info = tcc_ir_op_get_src2(ir, q);
    int encoded = (int)irop_get_imm64_ex(ir, call_info);
    int call_id = TCCIR_DECODE_CALL_ID(encoded);
    int argc = TCCIR_DECODE_CALL_ARGC(encoded);
    if (argc != 1)
      continue;

    /* Locate the FUNCPARAMVAL for this call_id, param 0. */
    int param_idx = -1;
    IROperand arg_val = IROP_NONE;
    for (int j = i - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op != TCCIR_OP_FUNCPARAMVAL && pq->op != TCCIR_OP_FUNCPARAMVOID)
        break;
      IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
      int enc = (int)irop_get_imm64_ex(ir, ps2);
      if (TCCIR_DECODE_CALL_ID(enc) != call_id)
        break;
      if (TCCIR_DECODE_PARAM_IDX(enc) == 0 && pq->op == TCCIR_OP_FUNCPARAMVAL)
      {
        param_idx = j;
        arg_val = tcc_ir_op_get_src1(ir, pq);
        break;
      }
    }

    if (param_idx < 0)
      continue;
    if (!irop_is_immediate(arg_val))
      continue;

    int64_t arg = irop_get_imm64_ex(ir, arg_val);
    int64_t ret_val;
    int ret_btype;
    int replay_indices[SWITCH_FUNC_SIM_MAX_REPLAY];
    int replay_count = 0;
    if (!tcc_ir_simulate_switch_func_ex(snap, arg, &ret_val, &ret_btype,
                                        replay_indices, &replay_count))
      continue;

    IROperand dest = tcc_ir_op_get_dest(ir, q);

    LOG_IR_GEN("OPTIMIZE: switch-IPC fold call %s(%lld) -> #%lld at i=%d (replay=%d)",
               get_tok_str(callee->v, NULL), (long long)arg, (long long)ret_val, i, replay_count);

    /* Emit replay ops just before the call site, in order. */
    int call_idx = i;
    if (replay_count > 0)
    {
      SwitchSimEnv sim_env;
      if (!rebuild_sim_env(snap, arg, &sim_env))
        continue;
      VregMap vmap = {0};
      vmap.count = 0;
      int ok = 1;
      for (int r = 0; r < replay_count; r++)
      {
        if (!emit_replay_op(ir, &vmap, snap, &sim_env, replay_indices[r], &call_idx))
        {
          ok = 0;
          break;
        }
      }
      if (!ok)
      {
        /* Partial emit: bail rather than corrupt the IR. */
        continue;
      }
    }

    /* Rewrite the (now-shifted) FUNCCALLVAL into the result ASSIGN. */
    q = &ir->compact_instructions[call_idx];
    q->op = TCCIR_OP_ASSIGN;
    if (ret_val == (int32_t)ret_val)
      tcc_ir_set_src1(ir, call_idx, irop_make_imm32(-1, (int32_t)ret_val, ret_btype));
    else
    {
      uint32_t pool_idx = tcc_ir_pool_add_i64(ir, ret_val);
      tcc_ir_set_src1(ir, call_idx, irop_make_i64(-1, pool_idx, ret_btype));
    }
    tcc_ir_set_src2(ir, call_idx, IROP_NONE);
    tcc_ir_set_dest(ir, call_idx, dest);

    /* NOP the matching FUNCPARAMVAL(s).  Scan backward from the call. */
    for (int j = call_idx - 1; j >= 0; j--)
    {
      IRQuadCompact *pq = &ir->compact_instructions[j];
      if (pq->op == TCCIR_OP_NOP)
        continue;
      if (pq->op == TCCIR_OP_FUNCPARAMVAL || pq->op == TCCIR_OP_FUNCPARAMVOID)
      {
        IROperand ps2 = tcc_ir_op_get_src2(ir, pq);
        int enc = (int)irop_get_imm64_ex(ir, ps2);
        if (TCCIR_DECODE_CALL_ID(enc) == call_id)
          pq->op = TCCIR_OP_NOP;
        continue;
      }
      break;
    }

    /* Advance past inserted ops + call site. */
    i = call_idx;
    changes++;
  }

  return changes;
}


