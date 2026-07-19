/*
 *  TCC IR - Memory SSA: MemoryDef / MemoryUse / MemoryPhi
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/*
 * A single-variable memory-SSA (LLVM MemorySSA style) built on top of the
 * already-constructed scalar SSA / CFG.  All of memory is modelled as ONE SSA
 * value: every STORE / clobbering call is a MemoryDef, MemoryPhis sit at the
 * dominance frontiers of def-blocks, and every LOAD carries a MemoryUse naming
 * the MemoryDef version that reaches it.  Precision (which def actually clobbers
 * a given load) is recovered at query time with `mem_may_alias` — the def chain
 * is walked, skipping provably-non-aliasing defs, until a must-alias covering
 * store or a barrier is reached.
 *
 * This layer is pure analysis: it reads the IR and CFG and produces the
 * reaching-def annotation.  Nothing here mutates the instruction stream.
 */

#pragma once

#include <stdint.h>

#include "memref.h"

struct TCCIRState;
struct IRCFG;

typedef enum {
  MEM_DEF_ENTRY = 0, /* incoming memory state at function entry (version 0)   */
  MEM_DEF_STORE,     /* STORE / STORE_INDEXED / STORE_POSTINC / BLOCK_COPY    */
  MEM_DEF_CALL,      /* call / INLINE_ASM / VLA_ALLOC / other opaque clobber  */
  MEM_DEF_PHI        /* a MemoryPhi at a control-flow merge                   */
} MemDefKind;

typedef struct MemoryDef {
  int version;              /* dense version id; 0 = entry                     */
  MemDefKind kind;
  int idx;                  /* defining instruction index; -1 for ENTRY / PHI  */
  int block;                /* containing CFG block id                         */
  MemLoc loc;               /* precise written location for a plain STORE;      *
                             * MEMLOC_UNKNOWN for indexed/opaque/call defs      */
  struct MemoryDef *reaching; /* the version this def was chained after         *
                               * (STORE/CALL only); NULL for ENTRY and PHI      */
} MemoryDef;

typedef struct MemoryPhi {
  int block;                /* the merge block this phi heads                  */
  MemoryDef def;            /* the phi IS a def (its own version)              */
  MemoryDef **incoming;     /* array[num_incoming]: reaching def per predecessor*/
  int num_incoming;         /* == cfg->blocks[block].num_preds                 */
} MemoryPhi;

typedef struct MemSSAState {
  struct IRCFG *cfg;
  MemoryDef *entry_def;     /* version 0                                       */
  MemoryPhi **block_phi;    /* array[num_blocks]: the block's MemoryPhi or NULL*/
  MemoryDef **use_reaching; /* array[num_instrs]: reaching def at a LOAD/read;  *
                             * NULL for non-memory-read instructions           */
  MemoryDef **def_at;       /* array[num_instrs]: the MemoryDef a clobber      *
                             * creates; NULL otherwise                         */
  int num_instrs;
  int num_versions;         /* total distinct memory versions (defs + phis)    */
  uint8_t *reachable;       /* bitset[num_blocks]: block visited by the rename  *
                             * walk (== reachable from entry in the dom tree)   */

  /* Ownership bookkeeping (for free). */
  MemoryDef **all_defs;
  int num_all_defs, cap_all_defs;
} MemSSAState;

/* Build memory-SSA over an already-dominator/DF-computed CFG.  Returns NULL for
 * functions memory-SSA cannot model (computed goto, no CFG, etc.). */
MemSSAState *tcc_ir_mem_ssa_build(struct TCCIRState *ir, struct IRCFG *cfg);

/* Check structural invariants (every memory read has a reaching def; every phi
 * operand is filled; versions dense and monotone).  Returns 1 if OK, else 0 and
 * logs the first violation to stderr.  For validation of the construction only. */
int tcc_ir_mem_ssa_verify(struct TCCIRState *ir, MemSSAState *m);

/* Store->load forwarding driven by the reaching-def walk: rewrite a LOAD whose
 * reaching memory version is a dominating store that exactly covers it into an
 * ASSIGN of that store's value.  Survives joins-that-don't-redefine and
 * provably-non-aliasing intervening stores (what dom-spine load_cse cannot).
 * Returns the number of loads rewritten. */
int tcc_ir_mem_ssa_load_fwd(struct TCCIRState *ir, MemSSAState *m);

/* Dump a human-readable form to stderr (gated by callers). */
void tcc_ir_mem_ssa_dump(struct TCCIRState *ir, MemSSAState *m);

void tcc_ir_mem_ssa_free(MemSSAState *m);
