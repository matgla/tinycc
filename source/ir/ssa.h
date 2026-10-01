#ifndef TCC_IR_SSA_H
#define TCC_IR_SSA_H

#include "cfg.h"

struct TCCIRState;

typedef struct IRPhiOperand
{
  int32_t vreg;
  int pred_block;
} IRPhiOperand;

typedef struct IRPhiNode
{
  int32_t dest_vreg;
  int32_t orig_vreg;
  IRPhiOperand *operands;
  int num_operands;
  int cap_operands;
  int btype; /* IROP_BTYPE_* of the original variable */
  struct IRPhiNode *next;
} IRPhiNode;

typedef struct IRSSAState
{
  IRCFG *cfg;
  IRPhiNode **block_phis; /* array[num_blocks]: linked list of phis per block */
  int32_t next_ssa_vreg;  /* next available SSA vreg position (TEMP type) */
  uint8_t *is_promotable; /* bitset indexed by VAR position */
  uint8_t *var_store_def_ok; /* per VAR: its widest read in bytes (0xFF for a
                              * non-scalar access).  An INT32 or INT64 slot
                              * STORE at least that wide covers every read, so
                              * rename rewrites it to a fresh-name ASSIGN def;
                              * phi placement consults the same test, so an
                              * upward-exposed (global) var gets phis for its
                              * STORE defs too (ssa_store_slot_def_pos). */
  int num_vars;           /* size of VAR namespace at construct time */
} IRSSAState;

IRSSAState *tcc_ir_ssa_construct(struct TCCIRState *ir, IRCFG *cfg);
void tcc_ir_ssa_rename(struct TCCIRState *ir, IRSSAState *ssa);
void tcc_ir_ssa_free(IRSSAState *ssa);

#endif
