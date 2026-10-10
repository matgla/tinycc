#ifndef TCC_IR_CFG_H
#define TCC_IR_CFG_H

struct TCCIRState;

/* The edge, dominance-frontier and dominator-child lists of all blocks live
 * in one array per kind, owned by the IRCFG (edge_store, df_store,
 * dom_children_store); each block points at its slice, NULL when empty.  A
 * realloc'd list per block (four per block, each rounded up to four ints)
 * plus their capacity fields was ~40% of a CFG's heap bytes (more on the
 * device, where each small block also pays allocator overhead), and two are
 * alive at once through register allocation.  The CFG is read-only once
 * built: nothing outside cfg.c adds an edge. */
typedef struct IRBasicBlock
{
  int start_idx;
  int end_idx;
  int *succs;
  int *preds;
  int num_succs;
  int num_preds;
  int idom;
  int rpo_number;
  int *dom_frontier;
  int *dom_children;
  int num_df;
  int num_dom_children;
} IRBasicBlock;

typedef struct IRCFG
{
  IRBasicBlock *blocks;
  int num_blocks;
  int *rpo_order;
  int rpo_count;
  int *instr_to_block;
  int num_instrs;
  int *edge_store;         /* every block's succs, then every block's preds */
  int *df_store;           /* every block's dom_frontier */
  int *dom_children_store; /* every block's dom_children */
  /* dominator-tree DFS stamps for O(1) dominance queries; -1 = unreachable */
  int *dom_tin;
  int *dom_tout;
  int dom_dfs_count;
} IRCFG;

IRCFG *tcc_ir_cfg_build(struct TCCIRState *ir);
void tcc_ir_cfg_free(IRCFG *cfg);
/* Cheap flat pre-scan: false only when the function provably has no back-edge
 * (loop passes may then skip CFG+dominator construction entirely). */
int tcc_ir_cfg_flat_has_backedge(struct TCCIRState *ir);
/* True for a CFG cycle or control flow whose successors are unknown. */
int tcc_ir_cfg_has_cycle(struct TCCIRState *ir);
/* Populate rpo_order/rpo_count only — for consumers that need a dataflow
 * ordering but no dominance (the dominator fixpoint can be quadratic on
 * many-predecessor joins; don't pay it for an ordering). */
void tcc_ir_cfg_compute_rpo(IRCFG *cfg);
void tcc_ir_cfg_compute_dominators(IRCFG *cfg);
void tcc_ir_cfg_compute_dom_frontiers(IRCFG *cfg);
int tcc_ir_cfg_dominates(IRCFG *cfg, int a, int b);

#endif
