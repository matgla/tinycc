#ifndef TCC_IR_CFG_H
#define TCC_IR_CFG_H

struct TCCIRState;

typedef struct IRBasicBlock
{
  int start_idx;
  int end_idx;
  int succs[2];
  int num_succs;
  int *preds;
  int num_preds;
  int preds_cap;
  int idom;
  int rpo_number;
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
} IRCFG;

IRCFG *tcc_ir_cfg_build(struct TCCIRState *ir);
void tcc_ir_cfg_free(IRCFG *cfg);
void tcc_ir_cfg_compute_dominators(IRCFG *cfg);
int tcc_ir_cfg_dominates(IRCFG *cfg, int a, int b);

#endif
