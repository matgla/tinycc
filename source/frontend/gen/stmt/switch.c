/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2001-2004 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* switch.c -- switch/case lowering, including jump tables.
 * Split out of tccgen.c; see docs/plan_tccgen_split.md. */

#include "gen_priv.h"

/* ------------------------------------------------------------------------- */
/* switch/case */

int case_cmp(uint64_t a, uint64_t b)
{
  if (cur_switch->sv.type.t & VT_UNSIGNED)
    return a < b ? -1 : a > b;
  else
    return (int64_t)a<(int64_t)b ? -1 : (int64_t)a>(int64_t) b;
  /* unreachable - all branches above return */
  return 0;
}

static int case_cmp_qs(const void *pa, const void *pb)
{
  return case_cmp((*(struct case_t **)pa)->v1, (*(struct case_t **)pb)->v1);
}

void case_sort(struct switch_t *sw)
{
  struct case_t **p;
  if (sw->n < 2)
    return;
  qsort(sw->p, sw->n, sizeof *sw->p, case_cmp_qs);
  p = sw->p;
  while (p < sw->p + sw->n - 1)
  {
    if (case_cmp(p[0]->v2, p[1]->v1) >= 0)
    {
      int l1 = p[0]->line, l2 = p[1]->line;
      /* using special format "%i:..." to show specific line */
      tcc_error("%i:duplicate case value", l1 > l2 ? l1 : l2);
    }
    else if (p[0]->v2 + 1 == p[1]->v1 && p[0]->ind == p[1]->ind)
    {
      /* treat "case 1: case 2: case 3:" like "case 1 ... 3: */
      p[1]->v1 = p[0]->v1;
      tcc_free(p[0]);
      memmove(p, p + 1, (--sw->n - (p - sw->p)) * sizeof *p);
    }
    else
      ++p;
  }
}

/* ============================================================================
 * Jump Table Switch Optimization
 * ============================================================================
 * For dense switch statements, use a jump table with TBB/TBH instructions
 * instead of linear/binary search for O(1) dispatch.
 */

/* Check if switch is suitable for jump table optimization.
 * Criteria:
 *   - Optimization enabled (-O1 or higher)
 *   - At least 4 cases
 *   - At least 50% density (num_cases / range >= 0.5)
 *   - Range fits in TBH (<= 65535) for TBB/TBH
 *   - No case ranges (v1 == v2 for all cases)
 *   - Not long long type (to simplify initial implementation)
 */
int switch_can_use_jump_table(struct switch_t *sw)
{
  /* Only use jump tables when optimization is enabled */
  if (!tcc_state->optimize)
    return 0;

  if (sw->n < 4)
    return 0; /* Too few cases to justify overhead */

  int64_t min_val = sw->p[0]->v1;
  int64_t max_val = sw->p[sw->n - 1]->v2;
  int64_t range = max_val - min_val + 1;

  /* Check density: must be at least 50% filled */
  if (sw->n * 2 < range)
    return 0;

  /* Check range fits in TBH (halfword indexing, max 65536 entries) */
  if (range > 65536)
    return 0;

  /* Check for case ranges (v1 != v2) - not supported initially */
  for (int i = 0; i < sw->n; i++)
  {
    if (sw->p[i]->v1 != sw->p[i]->v2)
      return 0;
  }

  /* Check integer type (not long long for simplicity) */
  if ((sw->sv.type.t & VT_BTYPE) == VT_LLONG)
    return 0;

  return 1;
}

/* Allocate and populate a switch table for jump table generation.
 * Returns the table_id to be used with TCCIR_OP_SWITCH_TABLE.
 */
static int tcc_ir_add_switch_table(TCCIRState *ir, int64_t min_val, int64_t max_val, int default_target,
                                   struct switch_t *sw)
{
  /* Grow array if needed */
  if (ir->num_switch_tables >= ir->switch_tables_capacity)
  {
    ir->switch_tables_capacity = ir->switch_tables_capacity * 2 + 4;
    ir->switch_tables = tcc_realloc(ir->switch_tables, ir->switch_tables_capacity * sizeof(*ir->switch_tables));
  }

  int id = ir->num_switch_tables++;
  TCCIRSwitchTable *table = &ir->switch_tables[id];

  table->min_val = min_val;
  table->max_val = max_val;
  table->default_target = default_target;
  table->num_entries = (int)(max_val - min_val + 1);
  table->targets = tcc_mallocz(table->num_entries * sizeof(int));
  table->table_code_addr = 0;

  /* Fill with default target initially */
  for (int i = 0; i < table->num_entries; i++)
  {
    table->targets[i] = default_target;
  }

  /* Fill in actual case targets */
  for (int i = 0; i < sw->n; i++)
  {
    int idx = (int)(sw->p[i]->v1 - min_val);
    if (idx >= 0 && idx < table->num_entries)
      table->targets[idx] = sw->p[i]->ind;
  }

  return id;
}

/* Generate jump table for switch statement.
 * Emits:
 *   1. Bounds check: if (index - min > max-min) goto default
 *   2. SWITCH_TABLE instruction with table reference
 *
 * Note: Like gcase(), this function does NOT pop the switch value from vtop.
 * The caller is responsible for vpop() after gcase_jump_table returns.
 */
int gcase_jump_table(struct switch_t *sw, int dsym)
{
  int64_t min_val = sw->p[0]->v1;
  int64_t max_val = sw->p[sw->n - 1]->v2;
  int range = (int)(max_val - min_val);
  TCCIRState *ir = tcc_state->ir;

  /* We need to preserve the original switch value on vtop for the caller.
   * So we work on a duplicated copy. */

  /* Duplicate the switch value for our manipulation */
  vdup();

  /* Adjust index: index = index - min_val (if min_val != 0) */
  if (min_val != 0)
  {
    vpush64(VT_INT, min_val);
    gen_op('-');
  }

  /* Duplicate adjusted index for bounds check */
  vdup();

  /* Compare: if (index > range) goto default
   * Use unsigned comparison since we just subtracted min */
  vpush64(VT_INT, range);
  gen_op(TOK_UGT); /* Unsigned greater than */

  /* Jump to default if out of bounds */
  int bounds_fail = tcc_ir_codegen_test_gen(ir, 0, dsym);

  /* Allocate switch table */
  int table_id = tcc_ir_add_switch_table(ir, min_val, max_val, dsym, sw);

  /* Emit SWITCH_TABLE instruction.
   * vtop currently holds the adjusted index (0 to range).
   * We'll use src2 to store the table_id. */
  SValue table_ref;
  svalue_init(&table_ref);
  table_ref.r = VT_CONST;
  table_ref.c.i = table_id;
  table_ref.type.t = VT_INT;

  /* src1 = adjusted index (current vtop)
   * src2 = table_id (encoded in an SValue)
   * The backend will handle the actual table emission */
  tcc_ir_put(ir, TCCIR_OP_SWITCH_TABLE, vtop, &table_ref, NULL);

  /* Pop our working copy of the adjusted index.
   * The original switch value remains on the stack below. */
  vpop();

  return bounds_fail; /* Return the jump for potential further use */
}

/* dsym is a jump-chain head (index of a JMP instruction) that will ultimately
 * be patched to the default label or fall-through. Never pass raw -1 here. */
int gcase(struct case_t **base, int len, int dsym)
{
  struct case_t *p;
  SValue dest;
  int t, l2, e;

  t = vtop->type.t & VT_BTYPE;
  if (t != VT_LLONG)
    t = VT_INT;
  while (len)
  {
    /* binary search while len > 8, else linear */
    l2 = len > 8 ? len / 2 : 0;
    p = base[l2];
    vdup(), vpush64(t, p->v2);
    if (l2 == 0 && p->v1 == p->v2)
    {
      int pos = 0;
      gen_op(TOK_EQ); /* jmp to case when equal */
      /* Use -1 (not dsym) as target to avoid corrupting the default chain.
       * tcc_ir_backpatch() follows the jump chain from the target, so passing
       * dsym here would cause it to walk the entire default chain and patch
       * every entry to p->ind, destroying the chain for subsequent cases.
       * With -1, the JUMPIF is independent: on match it is backpatched to
       * p->ind; on mismatch execution falls through to the next case check
       * (or the final JUMP(dsym) at the end of the loop). */
      pos = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
      tcc_ir_backpatch(tcc_state->ir, pos, p->ind);
      // gsym_addr(gvtst(0, 0), p->ind);
    }
    else
    {
      int pos = 0;
      /* case v1 ... v2 */
      gen_op(TOK_GT); /* jmp over when > V2 */
      if (len == 1)   /* last case test jumps to default when false */
      {
        dsym = tcc_ir_codegen_test_gen(tcc_state->ir, 0, dsym);
        e = -1; /* Use -1 so tcc_ir_backpatch_to_here will be a no-op */
      }
      else
      {
        /* Use -1 (not dsym) as target to avoid corrupting the default chain.
         * The e jump will be backpatched independently to fall through.
         * Using -1 ensures backpatching stops at e and doesn't follow any chain. */
        e = tcc_ir_codegen_test_gen(tcc_state->ir, 0, -1);
      }
      vdup(), vpush64(t, p->v1);
      gen_op(TOK_GE); /* jmp to case when >= V1 */
      pos = tcc_ir_codegen_test_gen(tcc_state->ir, 0, p->ind);
      tcc_ir_backpatch(tcc_state->ir, pos, p->ind);
      // gsym_addr(gvtst(0, 0), p->ind);
      dsym = gcase(base, l2, dsym);
      // gsym(e);s
      tcc_ir_backpatch_to_here(tcc_state->ir, e);
    }
    ++l2, base += l2, len -= l2;
  }
  /* jump automagically will suppress more jumps */
  // return gjmp(dsym);
  svalue_init(&dest);
  dest.vr = -1;
  dest.r = VT_CONST; /* Mark as constant so jump target is stored in u.imm32 */
  dest.c.i = dsym;
  return tcc_ir_put(tcc_state->ir, TCCIR_OP_JUMP, NULL, NULL, &dest);
}

void end_switch(void)
{
  struct switch_t *sw = cur_switch;
  dynarray_reset(&sw->p, &sw->n);
  cur_switch = sw->prev;
  tcc_free(sw);
}
