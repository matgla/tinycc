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
  uint16_t size;     /* bytes (struct actual size; scalars: 4/8) */
  uint8_t alignment; /* bytes (power of two); use at least 4 */
} TCCAbiArgDesc;

typedef enum TCCAbiLocKind
{
  TCC_ABI_LOC_REG = 1,
  TCC_ABI_LOC_STACK,
} TCCAbiLocKind;

typedef struct TCCAbiArgLoc
{
  TCCAbiLocKind kind;
  uint8_t reg_base;  /* first arg register index (0 == R0 on ARM) */
  uint8_t reg_count; /* number of consecutive arg registers */
  int32_t stack_off; /* outgoing stack offset in bytes (from outgoing area base) */
  uint16_t size;     /* bytes copied/passed */
} TCCAbiArgLoc;

typedef struct TCCAbiCallLayout
{
  int argc;
  TCCAbiArgLoc *locs;  /* length argc, owned by caller */
  int32_t stack_size;  /* total outgoing argument stack area (bytes), aligned */
  uint8_t stack_align; /* required stack alignment at call boundary */
} TCCAbiCallLayout;

/*
 * The target hook prototype is declared in tcc.h (after ST_FUNC is defined),
 * to avoid mismatched linkage attributes in ONE_SOURCE builds.
 */
