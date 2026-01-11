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

void thumb_free_call_sites(void)
{
  ThumbGenCallSite *call_site = thumb_gen_state.call_sites;
  while (call_site)
  {
    ThumbGenCallSite *next = call_site->next;
    /* Free the argument list if allocated */
    if (call_site->function_argument_list)
    {
      tcc_free(call_site->function_argument_list);
      call_site->function_argument_list = NULL;
    }
    tcc_free(call_site);
    call_site = next;
  }
  thumb_gen_state.call_sites = NULL;
}

void thumb_append_call_site(ThumbGenCallSite *new_state)
{
  ThumbGenCallSite *next = thumb_gen_state.call_sites;
  if (thumb_gen_state.call_sites == NULL)
  {
    thumb_gen_state.call_sites = new_state;
    return;
  }

  while (next->next)
  {
    next = next->next;
  }
  next->next = new_state;
}

ThumbGenCallSite *thumb_get_call_site_for_id(int call_id)
{
  ThumbGenCallSite *call_state = thumb_gen_state.call_sites;
  while (call_state)
  {
    if (call_state->call_id == call_id)
      return call_state;
    call_state = call_state->next;
  }
  return NULL;
}

/* Build ABI call layout from IR instructions for a given call_id.
 * Scans backwards from call_idx to find all FUNCPARAMVAL operations for this call.
 * Returns the number of arguments found, or -1 on error.
 */
int thumb_build_call_layout_from_ir(TCCIRState *ir, int call_idx, int call_id, TCCAbiCallLayout *layout)
{
  if (!ir || !layout || call_idx < 0)
    return -1;

  /* Scan backwards to find all FUNCPARAMVAL ops for this call_id */
  int max_arg_index = -1;
  for (int j = call_idx - 1; j >= 0; --j)
  {
    const TACQuadruple *p = &ir->instructions[j];
    if (p->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int param_call_id = TCCIR_DECODE_CALL_ID(p->src2.c.i);
      if (param_call_id == call_id)
      {
        int param_idx = TCCIR_DECODE_PARAM_IDX(p->src2.c.i);
        if (param_idx > max_arg_index)
          max_arg_index = param_idx;
      }
    }
  }

  const int argc = max_arg_index + 1;
  if (argc <= 0)
  {
    layout->argc = 0;
    layout->stack_size = 0;
    return 0;
  }

  /* Allocate arrays for argument descriptors and locations */
  TCCAbiArgDesc *arg_descs = (TCCAbiArgDesc *)tcc_mallocz(sizeof(TCCAbiArgDesc) * argc);
  layout->locs = (TCCAbiArgLoc *)tcc_mallocz(sizeof(TCCAbiArgLoc) * argc);
  uint8_t *found = (uint8_t *)tcc_mallocz(sizeof(uint8_t) * argc);

  /* Collect all parameters for this call */
  for (int j = call_idx - 1; j >= 0; --j)
  {
    const TACQuadruple *p = &ir->instructions[j];
    if (p->op == TCCIR_OP_FUNCPARAMVAL)
    {
      int param_call_id = TCCIR_DECODE_CALL_ID(p->src2.c.i);
      if (param_call_id == call_id)
      {
        int param_idx = TCCIR_DECODE_PARAM_IDX(p->src2.c.i);
        if (param_idx < 0 || param_idx >= argc)
        {
          tcc_error("compiler_error: bad FUNCPARAMVAL index %d (argc=%d)", param_idx, argc);
          goto cleanup_error;
        }

        if (!found[param_idx])
        {
          /* Determine argument type and size */
          const int bt = p->src1.type.t & VT_BTYPE;
          int size = 0;
          int align = 0;

          if (bt == VT_STRUCT)
          {
            size = type_size(&p->src1.type, &align);
            if (align < 1)
              align = 1;
            arg_descs[param_idx].kind = TCC_ABI_ARG_STRUCT_BYVAL;
            arg_descs[param_idx].size = (uint16_t)size;
            arg_descs[param_idx].alignment = (uint8_t)align;
          }
          else if (tcc_is_64bit_type(p->src1.type.t))
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
        }
      }
    }
  }

  /* Verify all parameters were found */
  for (int i = 0; i < argc; ++i)
  {
    if (!found[i])
    {
      tcc_error("compiler_error: missing FUNCPARAMVAL for call_id=%d arg=%d", call_id, i);
      goto cleanup_error;
    }
  }

  /* Use target ABI hook to compute register/stack layout */
  if (tcc_gen_machine_abi_assign_call_args(arg_descs, argc, layout) < 0)
  {
    tcc_error("compiler_error: abi_assign_call_args failed");
    goto cleanup_error;
  }

  layout->argc = argc;
  tcc_free(arg_descs);
  tcc_free(found);
  return argc;

cleanup_error:
  tcc_free(arg_descs);
  if (layout->locs)
  {
    tcc_free(layout->locs);
    layout->locs = NULL;
  }
  tcc_free(found);
  return -1;
}
