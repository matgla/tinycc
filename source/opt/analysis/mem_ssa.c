/*
 *  TCC IR - Memory SSA: MemoryDef / MemoryUse / MemoryPhi
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS

#include <string.h>

#include "ir.h"
#include "opt.h"
#include "opt_utils.h"
#include "opt_alias.h"
#include "cfg.h"
#include "mem_ssa.h"

/* --- instruction classification ---------------------------------------- */

/* A clobber creates a new memory version (a MemoryDef). */
static int msa_is_clobber(int op)
{
  switch (op) {
  case TCCIR_OP_STORE:
  case TCCIR_OP_STORE_INDEXED:
  case TCCIR_OP_STORE_POSTINC:
  case TCCIR_OP_BLOCK_COPY:
  case TCCIR_OP_FUNCCALLVAL:
  case TCCIR_OP_FUNCCALLVOID:
  case TCCIR_OP_INLINE_ASM:
  case TCCIR_OP_VLA_ALLOC:
  case TCCIR_OP_LOAD_POSTINC: /* modifies the pointer; opaque, matches load_cse */
    return 1;
  default:
    return 0;
  }
}

/* A read observes the current memory version (a MemoryUse). */
static int msa_is_read(TCCIRState *ir, IRQuadCompact *q)
{
  if (q->op == TCCIR_OP_LOAD || q->op == TCCIR_OP_LOAD_INDEXED ||
      q->op == TCCIR_OP_LOAD_POSTINC)
    return 1;
  if (irop_config[q->op].has_src1 && tcc_ir_op_get_src1(ir, q).is_lval)
    return 1;
  if (irop_config[q->op].has_src2 && tcc_ir_op_get_src2(ir, q).is_lval)
    return 1;
  if (q->op == TCCIR_OP_MLA && tcc_ir_op_get_accum(ir, q).is_lval)
    return 1;
  return 0;
}

/* The location a clobber writes: a plain STORE resolves precisely; everything
 * else (indexed/postinc/opaque/call) is UNKNOWN so downstream queries treat it
 * as clobbering all of memory until a mod-ref refinement lands. */
static MemLoc msa_def_loc(TCCIRState *ir, IRQuadCompact *q, int idx)
{
  MemLoc m = {MEMLOC_NONE, NULL, 0, 0};
  if (q->op == TCCIR_OP_STORE) {
    IROperand dest = tcc_ir_op_get_dest(ir, q);
    return memloc_of(ir, dest, idx);
  }
  m.kind = MEMLOC_UNKNOWN;
  return m;
}

/* --- allocation helpers ------------------------------------------------- */

static MemoryDef *msa_new_def(MemSSAState *m, MemDefKind kind, int idx, int block,
                              MemLoc loc, MemoryDef *reaching)
{
  MemoryDef *d = tcc_mallocz(sizeof(MemoryDef));
  d->version = m->num_versions++;
  d->kind = kind;
  d->idx = idx;
  d->block = block;
  d->loc = loc;
  d->reaching = reaching;
  if (m->num_all_defs >= m->cap_all_defs) {
    m->cap_all_defs = m->cap_all_defs ? m->cap_all_defs * 2 : 32;
    m->all_defs = tcc_realloc(m->all_defs, (size_t)m->cap_all_defs * sizeof(MemoryDef *));
  }
  m->all_defs[m->num_all_defs++] = d;
  return d;
}

/* --- construction ------------------------------------------------------- */

MemSSAState *tcc_ir_mem_ssa_build(TCCIRState *ir, IRCFG *cfg)
{
  if (!cfg || cfg->num_blocks <= 0)
    return NULL;
  int n = ir->next_instruction_index;
  if (n <= 0)
    return NULL;

  /* Computed goto makes the CFG edges imprecise — memory-SSA would be unsound. */
  for (int i = 0; i < n; i++)
    if (ir->compact_instructions[i].op == TCCIR_OP_IJUMP)
      return NULL;

  int nb = cfg->num_blocks;
  MemSSAState *m = tcc_mallocz(sizeof(MemSSAState));
  m->cfg = cfg;
  m->num_instrs = n;
  m->block_phi = tcc_mallocz((size_t)nb * sizeof(MemoryPhi *));
  m->use_reaching = tcc_mallocz((size_t)n * sizeof(MemoryDef *));
  m->def_at = tcc_mallocz((size_t)n * sizeof(MemoryDef *));
  m->reachable = tcc_mallocz((size_t)((nb + 7) / 8));

  /* Entry (version 0). */
  MemLoc none = {MEMLOC_NONE, NULL, 0, 0};
  int entry = cfg->instr_to_block ? cfg->instr_to_block[0] : 0;
  if (entry < 0 || entry >= nb)
    entry = 0;
  m->entry_def = msa_new_def(m, MEM_DEF_ENTRY, -1, entry, none, NULL);

  /* 1. Def-blocks: blocks containing at least one clobber. */
  int bbytes = (nb + 7) / 8;
  uint8_t *def_blocks = tcc_mallocz((size_t)bbytes);
  for (int i = 0; i < n; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    if (msa_is_clobber(q->op)) {
      int b = cfg->instr_to_block[i];
      if (b >= 0 && b < nb)
        def_blocks[b / 8] |= (uint8_t)(1 << (b % 8));
    }
  }

  /* 2. Place MemoryPhis at the iterated dominance frontier of def-blocks. */
  uint8_t *has_phi = tcc_mallocz((size_t)bbytes);
  uint8_t *in_wl = tcc_mallocz((size_t)bbytes);
  int *wl = tcc_malloc((size_t)nb * sizeof(int));
  int wn = 0;
  for (int b = 0; b < nb; b++)
    if (def_blocks[b / 8] & (1 << (b % 8))) {
      wl[wn++] = b;
      in_wl[b / 8] |= (uint8_t)(1 << (b % 8));
    }
  while (wn > 0) {
    int b = wl[--wn];
    in_wl[b / 8] &= (uint8_t)~(1 << (b % 8));
    IRBasicBlock *bb = &cfg->blocks[b];
    for (int di = 0; di < bb->num_df; di++) {
      int df = bb->dom_frontier[di];
      if (df < 0 || df >= nb)
        continue;
      if (has_phi[df / 8] & (1 << (df % 8)))
        continue;
      has_phi[df / 8] |= (uint8_t)(1 << (df % 8));
      MemoryPhi *phi = tcc_mallocz(sizeof(MemoryPhi));
      phi->block = df;
      phi->num_incoming = cfg->blocks[df].num_preds;
      phi->incoming = tcc_mallocz((size_t)(phi->num_incoming > 0 ? phi->num_incoming : 1) *
                                  sizeof(MemoryDef *));
      phi->def.version = m->num_versions++;
      phi->def.kind = MEM_DEF_PHI;
      phi->def.idx = -1;
      phi->def.block = df;
      phi->def.loc = none;
      phi->def.reaching = NULL;
      m->block_phi[df] = phi;
      /* iterated: the frontier block itself now defines the value */
      if (!(in_wl[df / 8] & (1 << (df % 8)))) {
        wl[wn++] = df;
        in_wl[df / 8] |= (uint8_t)(1 << (df % 8));
      }
    }
  }

  /* 3. Rename: dominator-tree preorder maintaining one reaching-def `cur`. */
  typedef struct { int block; int ci; MemoryDef *saved; } Frame;
  Frame *stk = tcc_malloc((size_t)(nb + 1) * sizeof(Frame));
  int sp = 0;
  MemoryDef *cur = m->entry_def;
  stk[0].block = entry;
  stk[0].ci = 0;
  stk[0].saved = cur;

  while (sp >= 0) {
    Frame *f = &stk[sp];
    int b = f->block;
    IRBasicBlock *bb = &cfg->blocks[b];
    if (f->ci == 0) {
      /* First visit: snapshot, enter phi, process instructions. */
      m->reachable[b / 8] |= (uint8_t)(1 << (b % 8));
      f->saved = cur;
      if (m->block_phi[b])
        cur = &m->block_phi[b]->def;
      for (int i = bb->start_idx; i < bb->end_idx; i++) {
        IRQuadCompact *q = &ir->compact_instructions[i];
        if (q->op == TCCIR_OP_NOP)
          continue;
        if (msa_is_read(ir, q))
          m->use_reaching[i] = cur;
        if (msa_is_clobber(q->op)) {
          MemDefKind k = (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
                          q->op == TCCIR_OP_STORE_POSTINC || q->op == TCCIR_OP_BLOCK_COPY)
                             ? MEM_DEF_STORE
                             : MEM_DEF_CALL;
          MemoryDef *d = msa_new_def(m, k, i, b, msa_def_loc(ir, q, i), cur);
          m->def_at[i] = d;
          cur = d;
        }
      }
      /* Fill each CFG successor's phi operand with b's exit version. */
      for (int si = 0; si < bb->num_succs; si++) {
        int s = bb->succs[si];
        if (s < 0 || s >= nb || !m->block_phi[s])
          continue;
        IRBasicBlock *sbb = &cfg->blocks[s];
        for (int pi = 0; pi < sbb->num_preds; pi++)
          if (sbb->preds[pi] == b) {
            m->block_phi[s]->incoming[pi] = cur;
            break;
          }
      }
    }
    if (f->ci < bb->num_dom_children) {
      int child = bb->dom_children[f->ci];
      f->ci++;
      sp++;
      stk[sp].block = child;
      stk[sp].ci = 0;
      stk[sp].saved = cur;
    } else {
      cur = f->saved; /* leaving the subtree: restore reaching def */
      sp--;
    }
  }

  tcc_free(stk);

  /* Operands for predecessors unreachable from entry are never filled by the
   * dom-tree walk (those preds are not in the dominator tree).  Such a path never
   * executes; default it to the entry version so every phi operand is defined and
   * a downstream reaching-def walk never dereferences a null incoming. */
  for (int b = 0; b < nb; b++) {
    MemoryPhi *phi = m->block_phi[b];
    if (!phi)
      continue;
    for (int pi = 0; pi < phi->num_incoming; pi++)
      if (!phi->incoming[pi])
        phi->incoming[pi] = m->entry_def;
  }

  tcc_free(wl);
  tcc_free(in_wl);
  tcc_free(has_phi);
  tcc_free(def_blocks);
  return m;
}

/* --- store->load forwarding --------------------------------------------- */

/* A store S exactly covers a load L: same base and same byte range. */
static int msa_exact_cover(MemLoc s, MemLoc l)
{
  if (s.kind != l.kind)
    return 0;
  if (s.kind == MEMLOC_GLOBAL && s.sym != l.sym)
    return 0;
  return s.off == l.off && s.size == l.size && s.size > 0;
}

/* A value operand is safe to forward across the (dominating) region between the
 * store and the load: an immediate, or a single-assignment SSA TEMP.  A VAR /
 * PARAM / memory lval could be redefined in between. */
static int msa_value_forwardable(IROperand v)
{
  if (v.is_lval)
    return 0;
  if (irop_is_immediate(v))
    return 1;
  int32_t vr = irop_get_vreg(v);
  return vr >= 0 && TCCIR_DECODE_VREG_TYPE(vr) == TCCIR_VREG_TYPE_TEMP;
}

/* Resolve the single store that provides location L's value at memory version
 * `d`, or NULL if none / ambiguous.  Skips provably-disjoint stores; at a phi,
 * every incoming must resolve to the SAME store S — which then provably
 * dominates the phi's block (only a store before the branch is reachable from
 * every predecessor), so S's value vreg is available at any dominated load.
 * `state`/`result` memoize per-version results (state: 0=unset,1=on-stack,
 * 2=done) so diamonds are linear and cycles (loops) resolve conservatively. */
static MemoryDef *msa_resolve_cover(MemSSAState *m, MemoryDef *d, MemLoc L,
                                    uint8_t *state, MemoryDef **result)
{
  while (d && d->kind == MEM_DEF_STORE) {
    if (!mem_may_alias(L, d->loc)) {
      d = d->reaching; /* disjoint store: L's value is unchanged across it */
      continue;
    }
    return msa_exact_cover(d->loc, L) ? d : NULL; /* cover, or may-alias-not-cover */
  }
  if (!d || d->kind != MEM_DEF_PHI)
    return NULL; /* entry / opaque call */
  int v = d->version;
  if (state[v] == 1)
    return NULL; /* back-edge cycle — conservatively unresolved */
  if (state[v] == 2)
    return result[v];
  state[v] = 1;
  MemoryPhi *phi = m->block_phi[d->block];
  MemoryDef *common = NULL;
  int ok = phi != NULL;
  for (int pi = 0; ok && pi < phi->num_incoming; pi++) {
    MemoryDef *r = msa_resolve_cover(m, phi->incoming[pi], L, state, result);
    if (!r)
      ok = 0;
    else if (!common)
      common = r;
    else if (common != r)
      ok = 0;
  }
  MemoryDef *res = ok ? common : NULL;
  state[v] = 2;
  result[v] = res;
  return res;
}

int tcc_ir_mem_ssa_load_fwd(TCCIRState *ir, MemSSAState *m)
{
  if (!m || m->num_versions <= 0)
    return 0;
  int changes = 0;
  uint8_t *state = tcc_malloc((size_t)m->num_versions);
  MemoryDef **result = tcc_malloc((size_t)m->num_versions * sizeof(MemoryDef *));

  for (int i = 0; i < m->num_instrs; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op != TCCIR_OP_LOAD)
      continue;
    IROperand addr = tcc_ir_op_get_src1(ir, q);
    if (!addr.is_lval)
      continue;
    MemLoc L = memloc_of(ir, addr, i);
    if (L.kind != MEMLOC_GLOBAL && L.kind != MEMLOC_FRAME)
      continue;
    /* Only full-register-width accesses (4 or 8 bytes).  A sub-word store
     * (`short s = intval`) truncates its register value IN the store — tccgen
     * emits no cast temp holding the narrowed bits — so the raw stored operand
     * is wider than what a later load reads back.  (The bitfield idiom in
     * 20040709-* forwards such truncated shorts too, but is only correct because
     * its consumer re-masks; that is not sound in general — see pr78477.)  Word
     * and dword stores carry a value tccgen already cast to the access width, so
     * no truncation or extension is elided by forwarding the operand directly. */
    if (L.size != 4 && L.size != 8)
      continue;
    MemoryDef *d = m->use_reaching[i];
    if (!d)
      continue;

    memset(state, 0, (size_t)m->num_versions);
    MemoryDef *cov = msa_resolve_cover(m, d, L, state, result);
    if (!cov || cov->kind != MEM_DEF_STORE)
      continue;

    IRQuadCompact *sq = &ir->compact_instructions[cov->idx];
    IROperand v = tcc_ir_op_get_src1(ir, sq);
    /* Require NO width change on either side: the bytes in memory are the store
     * value truncated to the store's access width, and the load result is the
     * loaded bytes extended to the load's result width.  Forwarding the raw
     * store value is only correct when store-value, store-access, load-access
     * and load-result all have the same btype (so no truncation / extension is
     * elided).  A truncating store (e.g. `short s = (long)t`) or widening load
     * would otherwise forward the wrong value. */
    int bt = irop_get_btype(addr);
    if (irop_get_btype(tcc_ir_op_get_dest(ir, sq)) != bt ||
        irop_get_btype(v) != bt ||
        irop_get_btype(tcc_ir_op_get_dest(ir, q)) != bt)
      continue;
    if (!msa_value_forwardable(v))
      continue;

    tcc_ir_set_src1(ir, i, v);
    q->op = TCCIR_OP_ASSIGN;
    changes++;
  }

  tcc_free(state);
  tcc_free(result);
  return changes;
}

/* --- verification ------------------------------------------------------- */

int tcc_ir_mem_ssa_verify(TCCIRState *ir, MemSSAState *m)
{
  if (!m)
    return 1;
  int nb = m->cfg->num_blocks;
  /* Every memory read in a reachable block has a reaching def.  (Reads in
   * unreachable blocks are never renamed and never queried — skip them.) */
  for (int i = 0; i < m->num_instrs; i++) {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_NOP)
      continue;
    int b = m->cfg->instr_to_block[i];
    if (b < 0 || b >= nb || !(m->reachable[b / 8] & (1 << (b % 8))))
      continue;
    if (msa_is_read(ir, q) && !m->use_reaching[i]) {
      fprintf(stderr, "[mem-ssa] verify FAIL: read at i=%d has no reaching def\n", i);
      return 0;
    }
  }
  /* Every phi operand is filled. */
  for (int b = 0; b < nb; b++) {
    MemoryPhi *phi = m->block_phi[b];
    if (!phi)
      continue;
    if (phi->num_incoming != m->cfg->blocks[b].num_preds) {
      fprintf(stderr, "[mem-ssa] verify FAIL: phi@blk%d arity %d != preds %d\n",
              b, phi->num_incoming, m->cfg->blocks[b].num_preds);
      return 0;
    }
    for (int pi = 0; pi < phi->num_incoming; pi++)
      if (!phi->incoming[pi]) {
        fprintf(stderr, "[mem-ssa] verify FAIL: phi@blk%d operand %d unfilled\n", b, pi);
        return 0;
      }
  }
  return 1;
}

/* --- dump --------------------------------------------------------------- */

static void msa_ver(MemoryDef *d, char *buf, int n)
{
  if (!d) {
    snprintf(buf, n, "<null>");
    return;
  }
  const char *k = d->kind == MEM_DEF_ENTRY ? "entry"
                  : d->kind == MEM_DEF_PHI ? "phi"
                  : d->kind == MEM_DEF_STORE ? "store"
                                             : "call";
  snprintf(buf, n, "m%d(%s)", d->version, k);
}

void tcc_ir_mem_ssa_dump(TCCIRState *ir, MemSSAState *m)
{
  if (!m) {
    fprintf(stderr, "[mem-ssa] (none)\n");
    return;
  }
  char b1[32], b2[32];
  fprintf(stderr, "[mem-ssa] versions=%d blocks=%d\n", m->num_versions,
          m->cfg->num_blocks);
  for (int b = 0; b < m->cfg->num_blocks; b++) {
    MemoryPhi *phi = m->block_phi[b];
    if (!phi)
      continue;
    msa_ver(&phi->def, b1, sizeof b1);
    fprintf(stderr, "  blk%d: %s = phi(", b, b1);
    for (int pi = 0; pi < phi->num_incoming; pi++) {
      msa_ver(phi->incoming[pi], b2, sizeof b2);
      fprintf(stderr, "%s%s", pi ? ", " : "", b2);
    }
    fprintf(stderr, ")\n");
  }
  for (int i = 0; i < m->num_instrs; i++) {
    if (m->def_at[i]) {
      msa_ver(m->def_at[i], b1, sizeof b1);
      msa_ver(m->def_at[i]->reaching, b2, sizeof b2);
      fprintf(stderr, "  i=%d DEF %s <- %s\n", i, b1, b2);
    }
    if (m->use_reaching[i]) {
      msa_ver(m->use_reaching[i], b1, sizeof b1);
      fprintf(stderr, "  i=%d USE reaches %s\n", i, b1);
    }
  }
}

/* --- free --------------------------------------------------------------- */

void tcc_ir_mem_ssa_free(MemSSAState *m)
{
  if (!m)
    return;
  for (int i = 0; i < m->num_all_defs; i++)
    tcc_free(m->all_defs[i]);
  tcc_free(m->all_defs);
  if (m->block_phi)
    for (int b = 0; b < m->cfg->num_blocks; b++)
      if (m->block_phi[b]) {
        tcc_free(m->block_phi[b]->incoming);
        tcc_free(m->block_phi[b]);
      }
  tcc_free(m->block_phi);
  tcc_free(m->use_reaching);
  tcc_free(m->def_at);
  tcc_free(m->reachable);
  tcc_free(m);
}
