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

#ifndef TCC_MACHINE_H
#define TCC_MACHINE_H

#include "tccir_operand.h"

/* ============================================================================
 * Machine Interface - Abstract architecture-dependent operations
 * ============================================================================
 *
 * This module provides an abstraction layer between the architecture-
 * independent IR and the target-specific backend. It defines a contract
 * that all backends must implement.
 *
 * The IR layer should NEVER:
 * - Directly allocate scratch registers
 * - Make assumptions about instruction encoding limits
 * - Know about specific physical registers
 * - Make materialization decisions based on target specifics
 *
 * Instead, the IR layer:
 * - Requests operations through this interface
 * - Lets the backend make target-specific decisions
 * - Works with virtual registers and abstract concepts
 */

/* Forward declarations */
struct TCCIRState;
struct SValue;

/* ============================================================================
 * Scratch Register Management
 * ============================================================================ */

/* Opaque scratch register handle - implementation defined by backend */
typedef struct TCCScratchHandle TCCScratchHandle;

/* Scratch allocation flags - architecture-independent semantics */
typedef enum TCCScratchFlags {
  TCC_SCRATCH_NONE = 0,
  TCC_SCRATCH_NEEDS_PAIR = (1u << 0),        /* Need adjacent register pair (e.g., for 64-bit) */
  TCC_SCRATCH_PREFERS_FLOAT = (1u << 1),     /* Prefer floating-point register if available */
  TCC_SCRATCH_AVOID_CALL_REGS = (1u << 2),   /* Avoid registers clobbered by function calls */
  TCC_SCRATCH_AVOID_PERM_SCRATCH = (1u << 3),/* Avoid "permanent scratch" regs (e.g., IP, FP) */
} TCCScratchFlags;

/* ============================================================================
 * Materialization Requests
 * ============================================================================ */

/* Types of value materialization the IR layer may request */
typedef enum TCCMatType {
  TCC_MAT_LOAD_SPILL,       /* Load value from spill slot */
  TCC_MAT_STORE_SPILL,      /* Store value to spill slot */
  TCC_MAT_ADDR_STACK,       /* Compute address of stack slot */
  TCC_MAT_LOAD_CONST,       /* Load constant to register */
  TCC_MAT_LOAD_CMP,         /* Load comparison result */
  TCC_MAT_LOAD_JMP,         /* Load jump target address */
} TCCMatType;

/* Materialization request context */
typedef struct TCCMatRequest {
  TCCMatType type;
  
  /* Destination register (if pre-allocated) or PREG_REG_NONE */
  int dest_reg;
  
  /* Source information */
  int64_t const_val;        /* For TCC_MAT_LOAD_CONST */
  int frame_offset;         /* For spill/stack operations */
  int is_param;             /* Frame offset is a parameter */
  int is_64bit;             /* Operation is 64-bit */
  int condition_code;       /* For TCC_MAT_LOAD_CMP */
  int jmp_addr;             /* For TCC_MAT_LOAD_JMP */
  int invert_jmp;           /* Invert jump condition */
  
  /* Source SValue (for complex materialization) */
  struct SValue *sv;
} TCCMatRequest;

/* Materialization result */
typedef struct TCCMatResult {
  int success;
  int reg;                  /* Result register */
  int reg_hi;               /* High register for 64-bit */
  TCCScratchHandle *scratch;/* Scratch handle if allocated */
} TCCMatResult;

/* ============================================================================
 * Machine Interface VTable
 * ============================================================================ */

/* Function pointer types for machine interface */
typedef TCCScratchHandle* (*tcc_machine_acquire_scratch_fn)(
    unsigned flags, 
    uint32_t exclude_regs);

typedef void (*tcc_machine_release_scratch_fn)(
    TCCScratchHandle *handle);

typedef int (*tcc_machine_scratch_get_reg_fn)(
    TCCScratchHandle *handle, 
    int idx);

typedef int (*tcc_machine_can_encode_directly_fn)(
    struct TCCIRState *ir,
    const TCCMatRequest *req);

typedef int (*tcc_machine_materialize_fn)(
    struct TCCIRState *ir,
    const TCCMatRequest *req,
    TCCMatResult *result);

typedef int (*tcc_machine_get_spill_offset_fn)(
    struct TCCIRState *ir,
    int vreg);

typedef int (*tcc_machine_get_stack_align_fn)(void);

typedef void (*tcc_machine_init_fn)(void);

typedef void (*tcc_machine_cleanup_fn)(void);

/* Machine interface vtable - one per architecture */
typedef struct TCCMachineInterface {
  /* Initialization */
  tcc_machine_init_fn init;
  tcc_machine_cleanup_fn cleanup;
  
  /* Scratch register management */
  tcc_machine_acquire_scratch_fn acquire_scratch;
  tcc_machine_release_scratch_fn release_scratch;
  tcc_machine_scratch_get_reg_fn scratch_get_reg;
  
  /* Value materialization */
  tcc_machine_can_encode_directly_fn can_encode_directly;
  tcc_machine_materialize_fn materialize;
  
  /* Stack frame queries */
  tcc_machine_get_spill_offset_fn get_spill_offset;
  tcc_machine_get_stack_align_fn get_stack_align;
  
} TCCMachineInterface;

/* ============================================================================
 * Global Machine Interface
 * ============================================================================ */

/* Global machine interface pointer - set by backend during initialization */
extern const TCCMachineInterface *tcc_machine;

/* Convenience inline wrappers */
static inline TCCScratchHandle* tcc_machine_acquire_scratch_ex(
    unsigned flags, 
    uint32_t exclude_regs)
{
  if (tcc_machine && tcc_machine->acquire_scratch)
    return tcc_machine->acquire_scratch(flags, exclude_regs);
  return NULL;
}

static inline void tcc_machine_release_scratch_ex(TCCScratchHandle *handle)
{
  if (tcc_machine && tcc_machine->release_scratch && handle)
    tcc_machine->release_scratch(handle);
}

static inline int tcc_machine_scratch_get_reg_ex(TCCScratchHandle *handle, int idx)
{
  if (tcc_machine && tcc_machine->scratch_get_reg && handle)
    return tcc_machine->scratch_get_reg(handle, idx);
  return PREG_REG_NONE;
}

static inline int tcc_machine_can_encode_directly_ex(
    struct TCCIRState *ir,
    const TCCMatRequest *req)
{
  if (tcc_machine && tcc_machine->can_encode_directly)
    return tcc_machine->can_encode_directly(ir, req);
  return 0;
}

static inline int tcc_machine_materialize_ex(
    struct TCCIRState *ir,
    const TCCMatRequest *req,
    TCCMatResult *result)
{
  if (tcc_machine && tcc_machine->materialize)
    return tcc_machine->materialize(ir, req, result);
  return 0;
}

static inline int tcc_machine_get_spill_offset_ex(struct TCCIRState *ir, int vreg)
{
  if (tcc_machine && tcc_machine->get_spill_offset)
    return tcc_machine->get_spill_offset(ir, vreg);
  return 0;
}

static inline int tcc_machine_get_stack_align_ex(void)
{
  if (tcc_machine && tcc_machine->get_stack_align)
    return tcc_machine->get_stack_align();
  return 8; /* Default to 8-byte alignment */
}

/* ============================================================================
 * Legacy Compatibility (During Migration)
 * ============================================================================ */

/* 
 * These wrappers provide compatibility with existing code during migration.
 * They will be removed once all code uses the new interface.
 */

/* Legacy scratch allocation - maps to new interface */
TCCScratchHandle* tcc_machine_acquire_scratch_compat(
    unsigned flags, 
    uint32_t exclude_regs);

void tcc_machine_release_scratch_compat(TCCScratchHandle *handle);

/* Legacy materialization helpers */
int tcc_machine_materialize_spill_compat(
    struct TCCIRState *ir,
    int frame_offset,
    int is_64bit,
    TCCMatResult *result);

int tcc_machine_materialize_addr_compat(
    struct TCCIRState *ir,
    int frame_offset,
    int is_param,
    int dest_reg,
    TCCMatResult *result);

/* ============================================================================
 * Backend Registration
 * ============================================================================ */

/* Register a machine interface implementation */
void tcc_machine_register(const TCCMachineInterface *interface);

/* Get the currently registered machine interface */
const TCCMachineInterface* tcc_machine_get(void);

/* Check if a machine interface is registered */
static inline int tcc_machine_is_registered(void)
{
  return tcc_machine != NULL;
}

#endif /* TCC_MACHINE_H */
