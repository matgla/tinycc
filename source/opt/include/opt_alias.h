/*
 *  TCC IR - Stack-slot aliasing helpers (pre-SSA optimization)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

#include <stdint.h>

struct TCCIRState;
struct IROperand;

int ir_opt_store_btype_size_bytes(int btype);

int ir_opt_stack_slot_range_for_offset(const struct TCCIRState *ir,
                                       int64_t frame_offset,
                                       int64_t *base_out, int64_t *end_out);

int stackoff_same_slot(IROperand a, IROperand b);

int operand_references_slot(IROperand op, IROperand slot);

int is_stack_address_operand(IROperand op);

int find_deref_use_operand(struct TCCIRState *ir, int consumer_idx,
                           int32_t vreg, int *which_out);

struct Sym;

/* Where the address a TEMP holds points, walked back through its def chain:
 * `T <- Addr[StackLoc[off]]` or `T <- &sym + addend` (LEA or ASSIGN), through
 * `T2 <- T1 ADD #imm` (SUB too with IR_AW_SUB) and plain TEMP copies. */
typedef struct IRAddrKey
{
  const struct Sym *sym; /* the global object, or NULL for the frame */
  int64_t off;           /* byte offset within it */
  int def_idx;           /* the LEA/ASSIGN that materialised the base */
} IRAddrKey;

enum
{
  IR_AW_SUB = 1 << 0,        /* walk through SUB #imm as well as ADD #imm */
  IR_AW_SYMREF = 1 << 1,     /* accept a `&sym [+ addend]` base */
  IR_AW_ANY_VREG = 1 << 2,   /* step through any non-lvalue operand with a vreg, not only a TAG_VREG one */
  IR_AW_LATEST_DEF = 1 << 3, /* the reaching def is the latest before the use (tcc_ir_find_defining_instruction),
                                the walk moving its "before" to each def it passes */
  IR_AW_TEMP_SLOT = 1 << 4,  /* a StackLoc carrying a frontend temporary's negative vreg (`&?NNN`, an
                                anonymous aggregate) is a frame slot too; without it only a vreg-less one is */
};

/* The def model of the pass doing the walk.  Every step must be a TEMP with
 * exactly one value def: def_at names it (-1 none, below -1 several); with
 * IR_AW_LATEST_DEF def_count (saturating at 2) vouches for it and the def is
 * searched before the use. */
typedef struct IRAddrWalk
{
  int max_hops;             /* TEMPs stepped through at most */
  unsigned flags;           /* IR_AW_* */
  const int *def_at;        /* per TEMP position */
  const uint8_t *def_count; /* per TEMP position */
  int n_temps;              /* positions def_at / def_count cover */
} IRAddrWalk;

/* 1 and *out filled when the walk reached a base; 0 when a step was not a
 * single-def TEMP, not an ADD/SUB-imm/LEA/ASSIGN, or ran past max_hops. */
int ir_opt_addr_walk(struct TCCIRState *ir, int32_t vr, int before_idx, const IRAddrWalk *w, IRAddrKey *out);

/* Which frame object a TEMP or VAR may hold the address of (analysis/frame_ptr.c):
 * the facts the dead-store passes share.  Objects are the VARs a LEA takes
 * the address of and the anonymous StackLocs whose address an instruction
 * materialises into a TEMP (one object per such TEMP position).  Read the
 * facts before the pass mutates anything; they are cached per flat-engine
 * generation. */
enum
{
  IR_FP_NONE = -1,
  IR_FP_MANY = -2
};
typedef struct IRFrameObj
{
  int var;              /* VAR position, or -1 for an anonymous StackLoc */
  int tmp;              /* anonymous: the TEMP position its address materialises into */
  int def_idx;          /* the first instruction that materialises the address */
  uint8_t read;         /* read through a pointer, or a pointer use the facts cannot follow */
  uint8_t inexact;      /* some node that may point to it may hold another value too */
  uint8_t many;         /* its address reached a node that may hold another object's address */
  uint8_t has_lea;      /* VAR: a LEA of it lands in a TEMP */
  uint8_t lea_into_var; /* VAR: a LEA of it lands in a VAR */
} IRFrameObj;
typedef struct IRFramePtr
{
  int n;                   /* instructions covered */
  int ntmp, nvar;          /* node = TEMP position, or ntmp + VAR position */
  int *node_obj;           /* IR_FP_NONE, IR_FP_MANY, or an index into objs */
  uint8_t *node_inexact;   /* the node also takes a value that is no tracked address */
  uint8_t *node_defs;      /* how many instructions define the node, capped at 2 */
  IRFrameObj *objs;
  int nobjs, objs_cap;
  int *var_obj;            /* VAR position -> object, or -1 */
  int *tmp_anon_obj;       /* TEMP position -> its anonymous object, or -1 */
  uint8_t *var_direct_read;/* VAR position: read by name (not through a pointer) */
  uint8_t read_all;        /* a MANY node was used: every object may have been read */
} IRFramePtr;
const IRFramePtr *ir_opt_frame_ptr(struct TCCIRState *ir);
void ir_opt_frame_ptr_free(struct TCCIRState *ir);
/* The node of a vreg, or -1. */
int ir_opt_frame_ptr_node(const IRFramePtr *fp, int32_t vr);
/* When q defines its destination from a register copy (ASSIGN, LOAD, STORE)
 * or pointer arithmetic (ADD, SUB) of a vreg value: that source's node, else -1. */
int ir_opt_frame_ptr_copy_src(struct TCCIRState *ir, const IRFramePtr *fp, const struct IRQuadCompact *q);
