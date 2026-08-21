/*
 *  TCC IR - Block Lifecycle (alloc/free/reset)
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#define USING_GLOBALS
#include "ir.h"

/* Per-function initial sizes.  tcc_ir_alloc runs once per function and every
 * one of these arrays is allocated with tcc_mallocz, so an oversized default is
 * a memset the target pays on PSRAM at every function.  One shared value of 64
 * for all three interval arrays was far past what real functions use; measured
 * over the 18,518 functions in the tests2 + ir_tests + gcc.c-torture corpus,
 * the share of functions exceeding a given size is:
 *
 *     size:      4      8     16     24     32     48     64    128
 *     vars    2.62%  1.16%  0.48%  0.28%  0.13%  0.05%  0.04%  0.01%
 *     temps  80.79% 71.76% 64.37% 50.25%  4.26%  2.85%  2.25%  1.06%
 *     params  0.91%  0.25%  0.01%  0.01%  0.01%  0.00%  0.00%  0.00%
 *     instrs 91.81% 82.25% 71.24% 66.64% 63.65% 60.75%  3.86%  2.07%
 *
 * Temps and instructions have sharp cliffs (24->32 and 48->64); vars and
 * params never came close to 64.  These values cover 95.7%-99.99% of
 * functions and cut the zeroing from 9,728 to 3,264 bytes per function.  The
 * rest grow by doubling in ir/vreg.c, which zeroes and initialises the new
 * region, so nothing here is a correctness bound. */
#define IR_VAR_INTERVAL_INIT_SIZE 16
#define IR_TEMP_INTERVAL_INIT_SIZE 32
#define IR_PARAM_INTERVAL_INIT_SIZE 8
#define QUADRUPLE_INIT_SIZE 64
/* Initialize all interval start fields to INTERVAL_NOT_STARTED and incoming_reg
 * to -1 */
static void tcc_ir_init_interval_starts(IRLiveInterval *intervals, int count)
{
  for (int i = 0; i < count; ++i)
  {
    intervals[i].start = INTERVAL_NOT_STARTED;
    intervals[i].incoming_reg0 = -1;
    intervals[i].incoming_reg1 = -1;
    intervals[i].stack_slot_index = -1;
    intervals[i].allocation.r0 = PREG_NONE;
    intervals[i].allocation.r1 = PREG_NONE;
    intervals[i].allocation.offset = 0;
  }
}

static void tcc_ir_clear_live_intervals(TCCIRState *ir)
{
  ir->variables_live_intervals_size = IR_VAR_INTERVAL_INIT_SIZE;
  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  ir->variables_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_VAR_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->variables_live_intervals, IR_VAR_INTERVAL_INIT_SIZE);
  ir->next_local_variable = 0;

  ir->temporary_variables_live_intervals_size = IR_TEMP_INTERVAL_INIT_SIZE;
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  ir->temporary_variables_live_intervals =
      (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_TEMP_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->temporary_variables_live_intervals, IR_TEMP_INTERVAL_INIT_SIZE);
  ir->next_temporary_variable = 0;

  ir->parameters_live_intervals_size = IR_PARAM_INTERVAL_INIT_SIZE;
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }

  ir->parameters_live_intervals = (IRLiveInterval *)tcc_mallocz(sizeof(IRLiveInterval) * IR_PARAM_INTERVAL_INIT_SIZE);
  tcc_ir_init_interval_starts(ir->parameters_live_intervals, IR_PARAM_INTERVAL_INIT_SIZE);
  ir->next_parameter = 0;
}

TCCIRState *tcc_ir_alloc(void)
{
  TCCIRState *block = (TCCIRState *)tcc_mallocz(sizeof(TCCIRState));
  if (!block)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory\n");
    exit(1);
  }
  block->parameters_count = 0;
  block->named_arg_reg_bytes = 0;
  block->named_arg_stack_bytes = 0;
  block->active_set = (IRLiveInterval **)tcc_mallocz(sizeof(IRLiveInterval *) * tcc_gen_machine_number_of_registers());
  block->ir_to_code_mapping = NULL;
  block->ir_to_code_mapping_size = 0;
  block->orig_ir_to_code_mapping = NULL;
  block->orig_ir_to_code_mapping_size = 0;

  block->next_instruction_index = 0;
  /* call_id is 0-based and monotonically increasing per function. */
  block->next_call_id = 0;

  block->leaffunc = 1;
  block->processing_if = 0;
  block->basic_block_start = 1;
  block->prevent_coalescing = 0;
  block->func_has_label_addr = 0;

  /* Nested function / static chain fields */
  block->has_static_chain = 0;
  block->static_chain_vreg = 0;
  block->parent_loc = 0;

  /* Nested function tracking (for parent functions) */
  block->nested_funcs = NULL;
  block->nb_nested_funcs = 0;
  block->nested_funcs_capacity = 0;

  tcc_ir_clear_live_intervals(block);

  /* Initialize IROperand pools (i64, f64, symref) */
  tcc_ir_pools_init(block);

  /* Initialize compact instructions array */
  block->compact_instructions_size = QUADRUPLE_INIT_SIZE;
  block->compact_instructions = (IRQuadCompact *)tcc_mallocz(sizeof(IRQuadCompact) * QUADRUPLE_INIT_SIZE);
  if (!block->compact_instructions)
  {
    fprintf(stderr, "tcc_ir_allocate_block: out of memory (compact_instructions)\n");
    exit(1);
  }

  tcc_ls_initialize(&block->ls);
  block->stack_layout.slots = NULL;
  block->stack_layout.slot_capacity = 0;
  block->stack_layout.slot_count = 0;
  block->stack_layout.offset_hash_keys = NULL;
  block->stack_layout.offset_hash_values = NULL;
  block->stack_layout.offset_hash_size = 0;

#ifdef CONFIG_TCC_ASM
  block->inline_asms = NULL;
  block->inline_asm_count = 0;
  block->inline_asm_capacity = 0;
#endif

  /* Initialize optimization module data */
  block->opt_fp_mat_cache = NULL;

  return block;
}

void tcc_ir_free(TCCIRState *ir)
{
  if (!ir)
  {
    fprintf(stderr, "tcc_ir_release_block: NULL ir block\n");
    exit(1);
  }

  if (ir->active_set != NULL)
  {
    tcc_free(ir->active_set);
  }

  if (ir->ir_to_code_mapping)
  {
    tcc_free(ir->ir_to_code_mapping);
    ir->ir_to_code_mapping = NULL;
    ir->ir_to_code_mapping_size = 0;
  }

  if (ir->orig_ir_to_code_mapping)
  {
    tcc_free(ir->orig_ir_to_code_mapping);
    ir->orig_ir_to_code_mapping = NULL;
    ir->orig_ir_to_code_mapping_size = 0;
  }

  /* Free IROperand pools (i64, f64, symref, ctype) */

  /* Free IROperand pools */
  tcc_ir_pools_free(ir);

  /* Free compact instructions array */
  if (ir->compact_instructions)
  {
    tcc_free(ir->compact_instructions);
    ir->compact_instructions = NULL;
    ir->compact_instructions_size = 0;
  }

#ifdef CONFIG_TCC_ASM
  if (ir->inline_asms)
  {
    for (int i = 0; i < ir->inline_asm_count; ++i)
    {
      TCCIRInlineAsm *ia = &ir->inline_asms[i];
      if (ia->asm_str)
        tcc_free(ia->asm_str);
      ia->asm_str = NULL;
      if (ia->operands)
        tcc_free(ia->operands);
      ia->operands = NULL;
      if (ia->values)
        tcc_free(ia->values);
      ia->values = NULL;
    }
    tcc_free(ir->inline_asms);
  }
  ir->inline_asms = NULL;
  ir->inline_asm_count = 0;
  ir->inline_asm_capacity = 0;
#endif

  if (ir->variables_live_intervals != NULL)
  {
    tcc_free(ir->variables_live_intervals);
  }
  if (ir->temporary_variables_live_intervals != NULL)
  {
    tcc_free(ir->temporary_variables_live_intervals);
  }
  if (ir->parameters_live_intervals != NULL)
  {
    tcc_free(ir->parameters_live_intervals);
  }

  if (ir->barrel_shifts)
  {
    tcc_free(ir->barrel_shifts);
    ir->barrel_shifts = NULL;
    ir->barrel_shifts_len = 0;
  }
  if (ir->zero_half64)
  {
    tcc_free(ir->zero_half64);
    ir->zero_half64 = NULL;
    ir->zero_half64_len = 0;
  }
  if (ir->shift64_dead_half)
  {
    tcc_free(ir->shift64_dead_half);
    ir->shift64_dead_half = NULL;
    ir->shift64_dead_half_len = 0;
  }
  if (ir->bfi_params)
  {
    tcc_free(ir->bfi_params);
    ir->bfi_params = NULL;
    ir->bfi_params_len = 0;
  }

  tcc_free(ir->codegen_return_jump_addrs);
  ir->codegen_return_jump_addrs = NULL;
  tcc_free(ir->codegen_dry_insn_scratch);
  ir->codegen_dry_insn_scratch = NULL;
  tcc_free(ir->codegen_dry_insn_saves);
  ir->codegen_dry_insn_saves = NULL;
  tcc_free(ir->codegen_mop_cache);
  ir->codegen_mop_cache = NULL;
  tcc_free(ir->codegen_cbz_dry_mapping);
  ir->codegen_cbz_dry_mapping = NULL;
  ir->codegen_rehearsal_end = 0;
  tcc_free(ir->codegen_dry_pool_entries);
  ir->codegen_dry_pool_entries = NULL;
  tcc_free(ir->codegen_branch_target_reset);
  ir->codegen_branch_target_reset = NULL;

  if (ir->stack_layout.slots != NULL)
  {
    tcc_free(ir->stack_layout.slots);
    ir->stack_layout.slots = NULL;
    ir->stack_layout.slot_capacity = 0;
    ir->stack_layout.slot_count = 0;
  }

  if (ir->stack_layout.offset_hash_keys)
  {
    tcc_free(ir->stack_layout.offset_hash_keys);
    ir->stack_layout.offset_hash_keys = NULL;
  }
  if (ir->stack_layout.offset_hash_values)
  {
    tcc_free(ir->stack_layout.offset_hash_values);
    ir->stack_layout.offset_hash_values = NULL;
  }
  ir->stack_layout.offset_hash_size = 0;

  tcc_ls_deinitialize(&ir->ls);

  /* Free optimization module data */
  tcc_ir_opt_fp_cache_free(ir);

  /* Free switch tables */
  if (ir->switch_tables)
  {
    for (int i = 0; i < ir->num_switch_tables; i++)
      tcc_free(ir->switch_tables[i].targets);
    tcc_free(ir->switch_tables);
    ir->switch_tables = NULL;
    ir->num_switch_tables = 0;
    ir->switch_tables_capacity = 0;
  }

  /* Free switch value tables (SWITCH_LOAD lookup data) */
  if (ir->switch_value_tables)
  {
    for (int i = 0; i < ir->num_switch_value_tables; i++)
      tcc_free(ir->switch_value_tables[i].values);
    tcc_free(ir->switch_value_tables);
    ir->switch_value_tables = NULL;
    ir->num_switch_value_tables = 0;
    ir->switch_value_tables_capacity = 0;
  }

  /* Free nested_funcs array (note: NestedFunc structs themselves are owned by TCCState) */
  if (ir->nested_funcs)
  {
    tcc_free(ir->nested_funcs);
    ir->nested_funcs = NULL;
    ir->nb_nested_funcs = 0;
    ir->nested_funcs_capacity = 0;
  }

  tcc_free(ir);
}

void tcc_ir_reset(TCCIRState *ir)
{
  /* TODO: Implement IR reset for reuse */
  (void)ir;
}
