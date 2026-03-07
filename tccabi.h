#pragma once

#include <stdint.h>

/*
 * Target-ABI call argument assignment interface.
 *
 * Purpose:
 * - Let target backends describe call argument placement (registers/stack)
 *   without hard-coding ABI rules (e.g. AAPCS) into the IR.
 * - IR can build explicit call sequences (CALLSEQ/CALLARG) by querying this.
 *
 * Notes:
 * - This is intentionally minimal and focused on integer/aggregate calling.
 * - For now it models a single GP register file and stack slots.
 */

typedef enum TCCAbiArgKind
{
  TCC_ABI_ARG_SCALAR32 = 1,
  TCC_ABI_ARG_SCALAR64,
  TCC_ABI_ARG_STRUCT_BYVAL,
} TCCAbiArgKind;

typedef struct TCCAbiArgDesc
{
  TCCAbiArgKind kind;
  uint32_t size;     /* bytes (struct actual size; scalars: 4/8) */
  uint8_t alignment; /* bytes (power of two); use at least 4 */
} TCCAbiArgDesc;

typedef enum TCCAbiLocKind
{
  TCC_ABI_LOC_REG = 1,
  TCC_ABI_LOC_STACK,
  TCC_ABI_LOC_REG_STACK, /* Split: some words in regs, rest on stack */
} TCCAbiLocKind;

typedef struct TCCAbiArgLoc
{
  TCCAbiLocKind kind;
  uint8_t reg_base;    /* first arg register index (0 == R0 on ARM) */
  uint8_t reg_count;   /* number of consecutive arg registers */
  int32_t stack_off;   /* outgoing stack offset in bytes (from outgoing area base) */
  uint32_t size;       /* bytes copied/passed */
  uint32_t stack_size; /* bytes on stack (for REG_STACK split) */
} TCCAbiArgLoc;

typedef struct TCCAbiCallLayout
{
  /* Number of arguments classified/stored in `locs`/`args` (if present). */
  int argc;

  /* Per-argument locations for the last computed layout. Must have >= argc entries.
   * Backends (e.g. arm-thumb-gen.c) write into this array.
   */
  TCCAbiArgLoc *locs;

  /* Incremental classification state (used by tcc_abi_classify_argument).
   * - args_original: the caller-provided description (pre-ABI-lowering)
   * - args_effective: ABI-lowered description used for register/stack accounting
   * - arg_flags: per-arg flags describing ABI lowering decisions
   */
  int capacity;
  TCCAbiArgDesc *args_original;
  TCCAbiArgDesc *args_effective;
  uint8_t *arg_flags;

/* arg_flags bits */
#define TCC_ABI_ARG_FLAG_INVISIBLE_REF 0x01 /* large composite passed as hidden pointer */
  /* Optional per-argument descriptors recorded as classification happens.
   * Useful for debugging and for re-running ABI decisions later. */
  TCCAbiArgDesc *args;

  /* Streaming classification state (target-ABI specific).
   * For ARM AAPCS-like ABIs this tracks the next GP arg register and the
   * next outgoing stack offset.
   */
  uint8_t next_reg;
  int32_t next_stack_off;

  int32_t stack_size;  /* total outgoing argument stack area (bytes), aligned */
  uint8_t stack_align; /* required stack alignment at call boundary */
} TCCAbiCallLayout;

/*
 * The target hook prototype is declared in tcc.h (after ST_FUNC is defined),
 * to avoid mismatched linkage attributes in ONE_SOURCE builds.
 */

TCCAbiArgLoc tcc_abi_classify_argument(TCCAbiCallLayout *layout, int arg_index, const TCCAbiArgDesc *arg_desc);
int tcc_abi_align_up_int(int v, int align);
void tcc_abi_call_layout_ensure_capacity(TCCAbiCallLayout *layout, int needed);
void tcc_abi_call_layout_deinit(TCCAbiCallLayout *layout);
