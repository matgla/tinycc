/*
 *  test_opt_region_merge_case.c - suite for ir/cross_jump.c:tcc_ir_region_merge,
 *  case bodies that differ only in their own case constant.
 *
 *  Post-RA IR built by hand: a SWITCH_TABLE at the end dispatching on T0
 *  (allocated to r4, live over the whole function) to three bodies
 *  `Tk <-- #k [LOAD]; *P0 <-- Tk [STORE]; JUMP end`.  The first body is kept
 *  and reads T0 in place of its constant; the others become jumps to it.
 */

#include "ir_build.h"

#include "ut.h"

int tcc_ir_region_merge(TCCIRState *ir);

#define I32 IROP_BTYPE_INT32
#define I64 IROP_BTYPE_INT64

enum { NTEMPS = 8 };

typedef struct CaseIR
{
  TCCIRState *ir;
  TCCIRSwitchTable tab;
  int targets[3];
  IRLiveInterval temps[NTEMPS];
  IRLiveInterval params[1];
  int end, sw;
} CaseIR;

static void set_reg(IRLiveInterval *li, int reg, int start, int end)
{
  li->allocation.r0 = (uint16_t)reg;
  li->allocation.r1 = PREG_NONE;
  li->allocation.offset = 0;
  li->start = (uint32_t)start;
  li->end = (uint32_t)end;
}

typedef struct CaseOpts
{
  int delta;      /* constant = case value + delta */
  int goto_mid;   /* the default body jumps to the second case body */
  int clobber;    /* each body redefines the selector first */
  int sel_btype;  /* selector type */
  int body_reg;   /* register of the bodies' temps */
} CaseOpts;

/*  0: JUMP sw
 *  1..: bodies (3 or 4 instructions each)
 *  default: RETURNVOID or JUMP to body 2
 *  sw: SWITCH_TABLE T0, table 0
 *  end: RETURNVOID */
static void build(CaseIR *c, CaseOpts o)
{
  memset(c, 0, sizeof *c);
  TCCIRState *ir = c->ir = utb_new();
  ir->iroperand_pool_capacity = UTB_MAX_OPERANDS;
  int body_len = o.clobber ? 4 : 3;
  c->sw = 1 + 3 * body_len + 1;
  c->end = c->sw + 1;
  utb_emit(ir, TCCIR_OP_JUMP, utb_imm(c->sw, I32), UTB_NONE, UTB_NONE);
  for (int k = 0; k < 3; k++)
  {
    c->targets[k] = ir->next_instruction_index;
    if (o.clobber)
      utb_emit(ir, TCCIR_OP_LOAD, utb_temp(0, I32), utb_imm(9, I32), UTB_NONE);
    utb_emit(ir, TCCIR_OP_LOAD, utb_temp(1 + k, I32), utb_imm(k + o.delta, I32), UTB_NONE);
    utb_emit(ir, TCCIR_OP_STORE, utb_lval(utb_param(0, I32)), utb_temp(1 + k, I32), UTB_NONE);
    utb_emit(ir, TCCIR_OP_JUMP, utb_imm(c->end, I32), UTB_NONE, UTB_NONE);
  }
  if (o.goto_mid)
    utb_emit(ir, TCCIR_OP_JUMP, utb_imm(c->targets[1], I32), UTB_NONE, UTB_NONE);
  else
    utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  int dflt = c->sw - 1;
  utb_emit(ir, TCCIR_OP_SWITCH_TABLE, UTB_NONE, utb_temp(0, o.sel_btype), utb_imm(0, I32));
  utb_emit(ir, TCCIR_OP_RETURNVOID, UTB_NONE, UTB_NONE, UTB_NONE);
  for (int k = 0; k < 3; k++)
    ir->compact_instructions[c->targets[k]].is_jump_target = 1;
  ir->compact_instructions[dflt].is_jump_target = 1;
  ir->compact_instructions[c->sw].is_jump_target = 1;
  ir->compact_instructions[c->end].is_jump_target = 1;

  c->tab.min_val = 0;
  c->tab.max_val = 2;
  c->tab.default_target = dflt;
  c->tab.targets = c->targets;
  c->tab.num_entries = 3;
  ir->switch_tables = &c->tab;
  ir->num_switch_tables = 1;

  set_reg(&c->temps[0], 4, 0, c->sw);
  for (int k = 0; k < 3; k++)
    set_reg(&c->temps[1 + k], o.body_reg, c->targets[k], c->targets[k] + body_len - 1);
  set_reg(&c->params[0], 5, 0, c->sw);
  ir->temporary_variables_live_intervals = c->temps;
  ir->temporary_variables_live_intervals_size = NTEMPS;
  ir->next_temporary_variable = 4;
  ir->parameters_live_intervals = c->params;
  ir->next_parameter = 1;
}

static void done(CaseIR *c)
{
  /* The intervals and the table live in CaseIR. */
  c->ir->temporary_variables_live_intervals = NULL;
  c->ir->parameters_live_intervals = NULL;
  utb_free(c->ir);
}

static int load_src_is(CaseIR *c, int i, int imm)
{
  IROperand s = utb_src1(c->ir, i);
  return s.tag == IROP_TAG_IMM32 && s.u.imm32 == imm;
}

static CaseOpts plain(void)
{
  CaseOpts o = {0, 0, 0, I32, 0};
  return o;
}

UT_TEST(test_region_merge_case_merges_bodies)
{
  CaseIR c;
  build(&c, plain());
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 2);
  int a = c.targets[0];
  UT_ASSERT_EQ(utb_op(c.ir, a), TCCIR_OP_LOAD);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, a)), utb_vreg(utb_temp(0, I32)));
  for (int k = 1; k < 3; k++)
  {
    UT_ASSERT_EQ(utb_op(c.ir, c.targets[k]), TCCIR_OP_JUMP);
    UT_ASSERT_EQ(utb_dest(c.ir, c.targets[k]).u.imm32, a);
  }
  done(&c);
  return 0;
}

UT_TEST(test_region_merge_case_constant_not_case_value)
{
  CaseIR c;
  CaseOpts o = plain();
  o.delta = 1;
  build(&c, o);
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 0);
  for (int k = 0; k < 3; k++)
    UT_ASSERT(load_src_is(&c, c.targets[k], k + 1));
  done(&c);
  return 0;
}

UT_TEST(test_region_merge_case_entry_also_jumped_to)
{
  CaseIR c;
  CaseOpts o = plain();
  o.goto_mid = 1;
  build(&c, o);
  /* Bodies 0 and 2 merge; body 1 is also reached with another selector. */
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 1);
  UT_ASSERT(load_src_is(&c, c.targets[1], 1));
  UT_ASSERT_EQ(utb_op(c.ir, c.targets[2]), TCCIR_OP_JUMP);
  UT_ASSERT_EQ(utb_vreg(utb_src1(c.ir, c.targets[0])), utb_vreg(utb_temp(0, I32)));
  done(&c);
  return 0;
}

UT_TEST(test_region_merge_case_selector_redefined)
{
  CaseIR c;
  CaseOpts o = plain();
  o.clobber = 1;
  build(&c, o);
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 0);
  for (int k = 0; k < 3; k++)
    UT_ASSERT(load_src_is(&c, c.targets[k] + 1, k));
  done(&c);
  return 0;
}

UT_TEST(test_region_merge_case_selector_register_shared)
{
  CaseIR c;
  CaseOpts o = plain();
  o.body_reg = 4; /* the bodies' temps live in the selector's register */
  build(&c, o);
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 0);
  done(&c);
  return 0;
}

UT_TEST(test_region_merge_case_wide_selector)
{
  CaseIR c;
  CaseOpts o = plain();
  o.sel_btype = I64;
  build(&c, o);
  UT_ASSERT_EQ(tcc_ir_region_merge(c.ir), 0);
  done(&c);
  return 0;
}

UT_COVERS("region_merge");
