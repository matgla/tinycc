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
#include <limits.h>

/* Debug output for callsite processing - disabled by default
 * Enable with: -DCALLSITE_DEBUG_ENABLED or #define CALLSITE_DEBUG_ENABLED */
#ifdef CALLSITE_DEBUG_ENABLED
#define CALLSITE_DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define CALLSITE_DEBUG(...) ((void)0)
#endif

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
 * out_args: if non-NULL, will be allocated and filled with argument IROperands.
 * out_mops: if non-NULL, will be allocated and filled with MachineOperands.
 * Returns the number of arguments found, or -1 on error.
 */
int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id, int argc_hint, TCCAbiCallLayout *layout,
                                    IROperand **out_args, MachineOperand **out_mops)
{
  CALLSITE_DEBUG("[CALLSITE] thumb_build_call_layout_from_ir: call_idx=%d call_id=%d argc_hint=%d total_insns=%d\n",
          call_idx, call_id, argc_hint, ir ? ir->next_instruction_index : -1);
  if (!ir || !layout || call_idx < 0)
    return -1;

/* Use fixed-size arrays for small argument counts to avoid allocations.
 * Most calls have few arguments, so this is a significant optimization. */
#define MAX_INLINE_ARGS 16
  TCCAbiArgDesc inline_arg_descs[MAX_INLINE_ARGS];
  uint8_t inline_found[MAX_INLINE_ARGS];
  TCCAbiArgDesc *arg_descs = NULL;
  uint8_t *found = NULL;
  IROperand *args = NULL;
  MachineOperand *mops = NULL;

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
        CALLSITE_DEBUG("[CALLSITE]   legacy scan j=%d: FUNCPARAMVAL param_call_id=%d (want %d) param_idx=%d\n",
                j, param_call_id, call_id,
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
    CALLSITE_DEBUG("[CALLSITE]   legacy scan result: max_arg_index=%d argc=%d\n", max_arg_index, argc);
  }

  if (argc <= 0)
  {
    layout->argc = 0;
    layout->stack_size = 0;
    if (out_args)
      *out_args = NULL;
    if (out_mops)
      *out_mops = NULL;
    return 0;
  }

  memset(inline_found, 0, sizeof(inline_found));

  /* Allocate arrays based on argc */
  if (argc <= MAX_INLINE_ARGS)
  {
    /* Fast path: use inline arrays */
    arg_descs = inline_arg_descs;
    found = inline_found;
  }
  else
  {
    /* Slow path: heap allocation needed */
    arg_descs = (TCCAbiArgDesc *)tcc_mallocz(sizeof(TCCAbiArgDesc) * argc);
    found = (uint8_t *)tcc_mallocz(sizeof(uint8_t) * argc);
  }

  /* Allocate args array if caller wants IROperands */
  if (out_args)
  {
    args = (IROperand *)tcc_mallocz(sizeof(IROperand) * argc);
  }

  /* Allocate MachineOperand array if caller wants them */
  if (out_mops)
  {
    mops = (MachineOperand *)tcc_mallocz(sizeof(MachineOperand) * argc);
  }

  CALLSITE_DEBUG("[CALLSITE] scanning backwards from call_idx=%d for call_id=%d argc=%d\n", call_idx, call_id, argc);
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
      CALLSITE_DEBUG("[CALLSITE]   j=%d FUNCPARAMVAL param_call_id=%d param_idx=%d (want call_id=%d)\n",
              j, param_call_id, param_idx_raw, call_id);
      if (param_call_id == call_id)
      {
        const IROperand src1_irop = tcc_ir_get_src1(ir, j);
        int param_idx = TCCIR_DECODE_PARAM_IDX((uint32_t)src2.u.imm32);
        if (param_idx >= 0 && param_idx < argc && !found[param_idx])
        {
          CALLSITE_DEBUG("[CALLSITE]     recording arg[%d] btype=%d is_64bit=%d\n",
                  param_idx, src1_irop.btype, irop_is_64bit(src1_irop));
          /* Collect IROperand if requested */
          if (args)
          {
            args[param_idx] = src1_irop;
          }
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
            arg_descs[param_idx].alignment = (uint8_t)align;
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

          found[param_idx] = 1;
          found_count++;
        }
      }
    }
  }

  CALLSITE_DEBUG("[CALLSITE] scan complete: found_count=%d argc=%d\n", found_count, argc);
  /* Verify all parameters were found */
  for (int i = 0; i < argc; ++i)
  {
    CALLSITE_DEBUG("[CALLSITE]   arg[%d]: found=%d\n", i, found[i]);
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

  /* Return args to caller if requested */
  if (out_args)
  {
    *out_args = args;
  }

  /* Return mops to caller if requested */
  if (out_mops)
  {
    *out_mops = mops;
  }

  /* Free heap-allocated arrays if used */
  if (argc > MAX_INLINE_ARGS)
  {
    tcc_free(arg_descs);
    tcc_free(found);
  }
  return argc;

cleanup_error:
  if (argc > MAX_INLINE_ARGS)
  {
    tcc_free(arg_descs);
    tcc_free(found);
  }
  if (args)
  {
    tcc_free(args);
  }
  if (mops)
  {
    tcc_free(mops);
  }
  if (layout->locs)
  {
    tcc_free(layout->locs);
    layout->locs = NULL;
  }
  return -1;
#undef MAX_INLINE_ARGS
}
