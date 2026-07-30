/*
 * ARM Thumb Call Site Management
 *
 * This file is part of TinyCC
 */
#define USING_GLOBALS
#include "arm-thumb-defs.h"
#include "tcc.h"
#include "tccabi.h"
#include "tccir.h"
#include "tcctype.h"
#include "arm-thumb-callsite.h"
#include <limits.h>

TCC_SMALL_SEQUENCE_DEFINE(ThumbAbiArgSequence, TCCAbiArgDesc, THUMB_CALL_INLINE_ARGS)
TCC_SMALL_SEQUENCE_DEFINE(ThumbFoundSequence, uint8_t, THUMB_CALL_INLINE_ARGS)

void thumb_free_call_sites(void)
{
  if (thumb_gen_state.call_sites_by_id)
  {
    for (int i = 0; i < thumb_gen_state.call_sites_by_id_size; ++i)
    {
      ThumbGenCallSite *cs = &thumb_gen_state.call_sites_by_id[i];
      if (cs->function_argument_list)
      {
        tcc_free(cs->function_argument_list);
        cs->function_argument_list = NULL;
      }
    }
    tcc_free(thumb_gen_state.call_sites_by_id);
    thumb_gen_state.call_sites_by_id = NULL;
  }
  thumb_gen_state.call_sites_by_id_size = 0;
}

static void thumb_ensure_call_site_capacity(int call_id)
{
  if (call_id < 0)
    return;

  if (call_id >= thumb_gen_state.call_sites_by_id_size)
  {
    int new_size = thumb_gen_state.call_sites_by_id_size ? thumb_gen_state.call_sites_by_id_size : 16;
    while (new_size <= call_id)
    {
      if (new_size > (INT_MAX >> 1))
        break;
      new_size <<= 1;
    }

    if (new_size > call_id)
    {
      ThumbGenCallSite *new_tab =
          (ThumbGenCallSite *)tcc_realloc(thumb_gen_state.call_sites_by_id, (size_t)new_size * sizeof(*new_tab));
      memset(new_tab + thumb_gen_state.call_sites_by_id_size, 0,
             (size_t)(new_size - thumb_gen_state.call_sites_by_id_size) * sizeof(*new_tab));
      thumb_gen_state.call_sites_by_id = new_tab;
      thumb_gen_state.call_sites_by_id_size = new_size;
    }
  }
}

ThumbGenCallSite *thumb_get_or_create_call_site(int call_id)
{
  if (call_id < 0)
    return NULL;

  thumb_ensure_call_site_capacity(call_id);
  if (call_id >= thumb_gen_state.call_sites_by_id_size)
    return NULL;

  ThumbGenCallSite *cs = &thumb_gen_state.call_sites_by_id[call_id];
  cs->call_id = call_id;
  return cs;
}

ThumbGenCallSite *thumb_get_call_site_for_id(int call_id)
{
  if (call_id >= 0 && call_id < thumb_gen_state.call_sites_by_id_size && thumb_gen_state.call_sites_by_id)
    return &thumb_gen_state.call_sites_by_id[call_id];
  return NULL;
}

/* Build ABI call layout from IR instructions for a given call_id.
 * Scans backwards from call_idx to find all FUNCPARAMVAL operations for this call.
 * argc_hint: if >= 0, use this as the known argument count (from FUNCCALL encoding).
 * out_args: if non-NULL, receives the argument IROperand sequence.
 * out_mops: if non-NULL, receives the MachineOperand sequence.
 * Returns the number of arguments found, or -1 on error.
 */
int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id, int argc_hint, TCCAbiCallLayout *layout,
                                    ThumbIROperandSequence *out_args, ThumbMachineOperandSequence *out_mops)
{
  LOG_CALLSITE("thumb_build_call_layout_from_ir: call_idx=%d call_id=%d argc_hint=%d total_insns=%d",
                 call_idx, call_id, argc_hint, ir ? ir->next_instruction_index : -1);
  if (!ir || !layout || call_idx < 0)
    return -1;

  small_sequence(ThumbIROperandSequence) args_owner = {0};
  small_sequence(ThumbMachineOperandSequence) mops_owner = {0};

  /* If argc_hint is provided and valid, use it directly (O(argc) scan only).
   * Otherwise, fall back to scanning to find max_arg_index (O(n) scan). */
  int argc;
  if (argc_hint >= 0)
  {
    argc = argc_hint;
  }
  else
  {
    /* Legacy fallback: scan to find max_arg_index */
    int max_arg_index = -1;
    for (int j = call_idx - 1; j >= 0; --j)
    {
      const IRQuadCompact *p = &ir->compact_instructions[j];
      if (p->op == TCCIR_OP_FUNCPARAMVAL)
      {
        const IROperand src2 = tcc_ir_get_src2(ir, j);
        int param_call_id = irop_is_none(src2) ? -1 : TCCIR_DECODE_CALL_ID((uint32_t)src2.u.imm32);
        LOG_CALLSITE("legacy scan j=%d: FUNCPARAMVAL param_call_id=%d (want %d) param_idx=%d", j,
                       param_call_id, call_id,
                       irop_is_none(src2) ? -1 : (int)TCCIR_DECODE_PARAM_IDX((uint32_t)src2.u.imm32));
        if (param_call_id == call_id)
        {
          int param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)src2.u.imm32);
          if (param_idx > max_arg_index)
            max_arg_index = param_idx;
        }
      }
    }
    argc = max_arg_index + 1;
    LOG_CALLSITE("legacy scan result: max_arg_index=%d argc=%d", max_arg_index, argc);
  }

  if (argc <= 0)
  {
    layout->argc = 0;
    layout->stack_size = 0;
    if (out_args)
      ThumbIROperandSequence_cleanup(out_args);
    if (out_mops)
      ThumbMachineOperandSequence_cleanup(out_mops);
    return 0;
  }

  if ((out_args && ThumbIROperandSequence_init(&args_owner, (size_t)argc) != 0) ||
      (out_mops && ThumbMachineOperandSequence_init(&mops_owner, (size_t)argc) != 0)) {
    return -1;
  }
  IROperand *args = ThumbIROperandSequence_data(&args_owner);
  MachineOperand *mops = out_mops ? ThumbMachineOperandSequence_data(&mops_owner) : NULL;

  small_sequence(ThumbAbiArgSequence) arg_descs_owner = {0};
  small_sequence(ThumbFoundSequence) found_owner = {0};
  if (ThumbAbiArgSequence_init(&arg_descs_owner, (size_t)argc) < 0 ||
      ThumbFoundSequence_init(&found_owner, (size_t)argc) < 0) {
    return -1;
  }
  TCCAbiArgDesc *arg_descs = ThumbAbiArgSequence_data(&arg_descs_owner);
  uint8_t *found = ThumbFoundSequence_data(&found_owner);

  LOG_CALLSITE("scanning backwards from call_idx=%d for call_id=%d argc=%d", call_idx, call_id, argc);
  int found_count = 0;
  for (int j = call_idx - 1; j >= 0 && found_count < argc; --j)
  {
    const IRQuadCompact *p = &ir->compact_instructions[j];
    if (p->op == TCCIR_OP_FUNCPARAMVAL)
    {
      const IROperand src2 = tcc_ir_get_src2(ir, j);
      int param_call_id = !irop_is_none(src2) ? TCCIR_DECODE_CALL_ID((uint32_t)src2.u.imm32) : -1;
      int param_idx_raw = !irop_is_none(src2) ? (int)TCCIR_DECODE_PARAM_IDX((uint32_t)src2.u.imm32) : -1;
      (void)param_idx_raw; /* only used by CALLSITE_DEBUG */
      LOG_CALLSITE("j=%d FUNCPARAMVAL param_call_id=%d param_idx=%d (want call_id=%d)", j,
                     param_call_id, param_idx_raw, call_id);
      if (param_call_id == call_id)
      {
        const IROperand src1_irop = tcc_ir_get_src1(ir, j);
        int param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)src2.u.imm32);
        if (param_idx >= 0 && param_idx < argc && !found[param_idx])
        {
          LOG_CALLSITE("recording arg[%d] btype=%d is_64bit=%d", param_idx, src1_irop.btype,
                         irop_is_64bit(src1_irop));

          if (out_args)
            args[param_idx] = src1_irop;
          /* Collect MachineOperand if requested */
          if (mops)
          {
            mops[param_idx] = machine_op_from_ir(ir, &src1_irop);
          }
          /* Determine argument type and size */
          if (irop_is_none(src1_irop))
          {
            tcc_error("compiler_error: FUNCPARAMVAL missing src1 for call_id=%d arg=%d", call_id, param_idx);
            goto cleanup_error;
          }

          // const int bt = src1_sv->type.t & VT_BTYPE;
          int size = 0;
          int align = 0;

          if (src1_irop.btype == IROP_BTYPE_STRUCT)
          {
            size = irop_type_size_align(src1_irop, &align);
            if (align < 1)
              align = 1;
            arg_descs[param_idx].kind = TCC_ABI_ARG_STRUCT_BYVAL;
            arg_descs[param_idx].size = (uint16_t)size;
            /* Use AAPCS natural alignment (based on member types, not
             * __attribute__((aligned)) on the struct). This determines
             * register alignment (even-register rule for 8-byte aligned). */
            int aapcs_align = irop_aapcs_alignment(src1_irop);
            arg_descs[param_idx].alignment = (uint8_t)(aapcs_align < align ? aapcs_align : align);
          }
          else if (src1_irop.is_complex)
          {
            /* Complex types are passed like composites (AAPCS):
             * complex float = 8 bytes (2 regs), complex double = 16 bytes (4 regs). */
            int elem_size = irop_is_64bit(src1_irop) ? 8 : 4;
            int total_size = elem_size * 2;
            arg_descs[param_idx].kind = TCC_ABI_ARG_STRUCT_BYVAL;
            arg_descs[param_idx].size = (uint16_t)total_size;
            arg_descs[param_idx].alignment = (uint8_t)elem_size;
          }
          else if (irop_needs_pair(src1_irop))
          {
            arg_descs[param_idx].kind = TCC_ABI_ARG_SCALAR64;
            arg_descs[param_idx].size = 8;
            arg_descs[param_idx].alignment = 8;
          }
          else
          {
            arg_descs[param_idx].kind = TCC_ABI_ARG_SCALAR32;
            arg_descs[param_idx].size = 4;
            arg_descs[param_idx].alignment = 4;
          }

          /* Scalar float/double only: complex is passed as a composite, so it
           * keeps the GPR/stack path rather than the VFP one. */
          arg_descs[param_idx].is_float =
              !src1_irop.is_complex && src1_irop.btype != IROP_BTYPE_STRUCT &&
              (src1_irop.btype == IROP_BTYPE_FLOAT32 || src1_irop.btype == IROP_BTYPE_FLOAT64);

          found[param_idx] = 1;
          found_count++;
        }
      }
    }
  }

  LOG_CALLSITE("scan complete: found_count=%d argc=%d", found_count, argc);
  /* Verify all parameters were found */
  for (int i = 0; i < argc; ++i)
  {
    LOG_CALLSITE("arg[%d]: found=%d", i, found[i]);
    if (!found[i])
    {
      tcc_error("compiler_error: missing FUNCPARAMVAL for call_id=%d arg=%d", call_id, i);
      goto cleanup_error;
    }
  }

  /* Allocate layout locations */
  layout->locs = (TCCAbiArgLoc *)tcc_mallocz(sizeof(TCCAbiArgLoc) * argc);

  /* Use target ABI hook to compute register/stack layout */
  if (tcc_gen_machine_abi_assign_call_args(arg_descs, argc, layout) < 0)
  {
    tcc_error("compiler_error: abi_assign_call_args failed");
    goto cleanup_error;
  }

  layout->argc = argc;

  if (out_args)
    ThumbIROperandSequence_move(out_args, &args_owner);

  if (out_mops)
    ThumbMachineOperandSequence_move(out_mops, &mops_owner);

  return argc;

cleanup_error:
  if (layout->locs)
  {
    tcc_free(layout->locs);
    layout->locs = NULL;
  }
  return -1;
}
