/*
 * fp_conformance.h - shared types for the floating-point conformance suite.
 *
 * The vector tables in fp_vectors.h are generated on the host by
 * gen_fp_vectors.c; fp_conformance.c walks them on the target and compares
 * results bit-exactly.  The same tables are used for every FP implementation
 * (libsoftfp, the RP2350 DCP, VFP), which is what makes a one-ulp divergence
 * between them a test failure rather than an invisible difference.
 */

#ifndef FP_CONFORMANCE_H
#define FP_CONFORMANCE_H

#include <stdint.h>

/* Operation codes.  MUST stay in sync with the enum in gen_fp_vectors.c. */
enum
{
  FPOP_ADD = 0,
  FPOP_SUB,
  FPOP_MUL,
  FPOP_DIV,
  /* All six relations for one operand pair, packed into the result as
   * bit0=lt bit1=le bit2=eq bit3=ne bit4=ge bit5=gt.  One vector instead of
   * six is a 6x saving on the dominant table term, and it mirrors the DCP,
   * where a single RCMP yields every relation from one compare. */
  FPOP_CMPALL,
  FPOP_NEG,
  FPOP_I2F,
  FPOP_U2F,
  FPOP_L2F,
  FPOP_UL2F,
  FPOP_F2I,
  FPOP_F2U,
  FPOP_F2L,
  FPOP_F2UL,
  FPOP_WIDEN,
  FPOP_NARROW,
};

/* IEEE-754 leaves the payload of a generated NaN unspecified, so vectors whose
 * result is NaN are checked with "is it a NaN" rather than an exact compare. */
#define FPF_NAN_RESULT 1u

/* bit positions inside an FPOP_CMPALL result */
#define FPC_LT 0x01u
#define FPC_LE 0x02u
#define FPC_EQ 0x04u
#define FPC_NE 0x08u
#define FPC_GE 0x10u
#define FPC_GT 0x20u

typedef struct
{
  uint8_t op;
  uint8_t flags;
  uint64_t a;
  uint64_t b;
  uint64_t r;
} fp_vec64;

typedef struct
{
  uint8_t op;
  uint8_t flags;
  uint32_t a;
  uint32_t b;
  uint32_t r;
} fp_vec32;

/* Runs both tables.  Returns the number of failing vectors (0 == pass) and
 * prints a bounded number of failures as raw bit patterns.  Printing uses only
 * integer conversions: formatting a double with %f would itself go through the
 * FP implementation under test. */
int fp_conformance_run(void);

/* Individual halves, for narrowing down a failure. */
int fp_conformance_run_double(void);
int fp_conformance_run_float(void);

#endif /* FP_CONFORMANCE_H */
