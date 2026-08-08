/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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
 */

#include "tcc.h"
#include "tccmachine.h"

/* ============================================================================
 * Global Machine Interface Pointer
 * ============================================================================ */

const TCCMachineInterface *tcc_machine = NULL;

/* ============================================================================
 * Backend Registration
 * ============================================================================ */

void tcc_machine_register(const TCCMachineInterface *interface)
{
  tcc_machine = interface;
  
  /* Call initialization if provided */
  if (tcc_machine && tcc_machine->init)
    tcc_machine->init();
}

const TCCMachineInterface* tcc_machine_get(void)
{
  return tcc_machine;
}

/* ============================================================================
 * Legacy Compatibility Layer
 * ============================================================================
 * 
 * These functions bridge the old direct calls with the new interface.
 * They are used during migration and will be removed.
 */

/* Internal structure to wrap legacy scratch allocation */
typedef struct TCCScratchHandleCompat {
  TCCMachineScratchRegs legacy;
  int valid;
} TCCScratchHandleCompat;

/* Fallback implementations using legacy functions if new interface not available */

TCCScratchHandle* tcc_machine_acquire_scratch_compat(unsigned flags, uint32_t exclude_regs)
{
  /* If new interface is available, use it */
  if (tcc_machine && tcc_machine->acquire_scratch) {
    return tcc_machine->acquire_scratch(flags, exclude_regs);
  }
  
  /* Otherwise, allocate a compat wrapper - caller must use legacy functions directly */
  TCCScratchHandleCompat *compat = tcc_malloc(sizeof(*compat));
  compat->valid = 0;
  return (TCCScratchHandle*)compat;
}

void tcc_machine_release_scratch_compat(TCCScratchHandle *handle)
{
  if (!handle)
    return;
    
  if (tcc_machine && tcc_machine->release_scratch) {
    tcc_machine->release_scratch(handle);
  } else {
    tcc_free(handle);
  }
}

/* ============================================================================
 * Materialization Helpers (Legacy Compatibility)
 * ============================================================================ */

int tcc_machine_materialize_spill_compat(
    TCCIRState *ir,
    int frame_offset,
    int is_64bit,
    TCCMatResult *result)
{
  if (!result)
    return 0;
    
  memset(result, 0, sizeof(*result));
  
  /* Build materialization request */
  TCCMatRequest req = {
    .type = TCC_MAT_LOAD_SPILL,
    .dest_reg = PREG_REG_NONE,
    .frame_offset = frame_offset,
    .is_64bit = is_64bit,
  };
  
  /* Try new interface first */
  if (tcc_machine && tcc_machine->materialize) {
    return tcc_machine->materialize(ir, &req, result);
  }
  
  /* Legacy fallback - this will be removed once all backends implement new interface */
  return 0;
}

int tcc_machine_materialize_addr_compat(
    TCCIRState *ir,
    int frame_offset,
    int is_param,
    int dest_reg,
    TCCMatResult *result)
{
  if (!result)
    return 0;
    
  memset(result, 0, sizeof(*result));
  
  /* Build materialization request */
  TCCMatRequest req = {
    .type = TCC_MAT_ADDR_STACK,
    .dest_reg = dest_reg,
    .frame_offset = frame_offset,
    .is_param = is_param,
    .is_64bit = 0,
  };
  
  /* Try new interface first */
  if (tcc_machine && tcc_machine->materialize) {
    return tcc_machine->materialize(ir, &req, result);
  }
  
  /* Legacy fallback */
  return 0;
}

/* ============================================================================
 * Default/Fallback Machine Interface
 * ============================================================================
 * 
 * These are stub implementations used when no backend is registered.
 * They should never be called in normal operation.
 */

static TCCScratchHandle* default_acquire_scratch(unsigned flags, uint32_t exclude_regs)
{
  (void)flags;
  (void)exclude_regs;
  return NULL;
}

static void default_release_scratch(TCCScratchHandle *handle)
{
  (void)handle;
}

static int default_scratch_get_reg(TCCScratchHandle *handle, int idx)
{
  (void)handle;
  (void)idx;
  return PREG_REG_NONE;
}

static int default_can_encode_directly(TCCIRState *ir, const TCCMatRequest *req)
{
  (void)ir;
  (void)req;
  return 0;
}

static int default_materialize(TCCIRState *ir, const TCCMatRequest *req, TCCMatResult *result)
{
  (void)ir;
  (void)req;
  (void)result;
  return 0;
}

static int default_get_spill_offset(TCCIRState *ir, int vreg)
{
  (void)ir;
  (void)vreg;
  return 0;
}

static int default_get_stack_align(void)
{
  return 8;
}

/* Default machine interface - used as fallback */
static const TCCMachineInterface default_machine_interface = {
  .init = NULL,
  .cleanup = NULL,
  .acquire_scratch = default_acquire_scratch,
  .release_scratch = default_release_scratch,
  .scratch_get_reg = default_scratch_get_reg,
  .can_encode_directly = default_can_encode_directly,
  .materialize = default_materialize,
  .get_spill_offset = default_get_spill_offset,
  .get_stack_align = default_get_stack_align,
};

/* Initialize machine interface with defaults */
void tcc_machine_init_defaults(void)
{
  if (!tcc_machine) {
    tcc_machine = &default_machine_interface;
  }
}
