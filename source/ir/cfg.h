#ifndef TCC_IR_CFG_H
#define TCC_IR_CFG_H

struct TCCIRState;

typedef struct IRBasicBlock
{
  int start_idx;
  int end_idx;
  int *succs;
  int num_succs;
  int succs_cap;
  int *preds;
  int num_preds;
  int preds_cap;
  int idom;
  int rpo_number;
  int *dom_frontier;
  int num_df;
  int df_cap;
  int *dom_children;
  int num_dom_children;
  int dom_children_cap;
} IRBasicBlock;

typedef struct IRCFG
{
  IRBasicBlock *blocks;
  int num_blocks;
  int capacity;
  int *rpo_order;
  int rpo_count;
  int *instr_to_block;
  int num_instrs;
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
/* Populate rpo_order/rpo_count only — for consumers that need a dataflow
 * ordering but no dominance (the dominator fixpoint can be quadratic on
 * many-predecessor joins; don't pay it for an ordering). */
void tcc_ir_cfg_compute_rpo(IRCFG *cfg);
void tcc_ir_cfg_compute_dominators(IRCFG *cfg);
void tcc_ir_cfg_compute_dom_frontiers(IRCFG *cfg);
int tcc_ir_cfg_dominates(IRCFG *cfg, int a, int b);

#endif
