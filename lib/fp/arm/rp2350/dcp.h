/*
 * dcp.h - RP2350 Double Coprocessor primitives.
 *
 * The DCP is Raspberry Pi's own coprocessor on CP4.  It is not an FPU: it
 * provides primitives that a short instruction sequence composes into an IEEE
 * double operation, and it works on *GPR pairs* through mcrr/mrrc rather than
 * on any floating-point register file.  That is why doubles keep the ordinary
 * soft-float ABI on this target.
 *
 * Instruction names and sequences follow the Pico SDK, which is vendored in
 * this repository at
 *   tests/benchmarks/libs/pico-sdk/src/rp2_common/hardware_dcp/include/hardware/
 *     dcp_instr.inc.S   (mnemonic -> cdp/mcrr/mrrc/mrc encoding)
 *     dcp_canned.inc.S  (the canned sequences)
 * Keeping the same names makes the two directly diffable.
 *
 * IMPORTANT -- why each operation is ONE asm block:
 *
 *   The DCP's internal X/Y/result registers are invisible to the compiler, so
 *   it sees no dependency between separate asm statements and is free to
 *   reorder, duplicate or hoist them out of loops.  Emitting a whole canned
 *   sequence as a single `asm volatile` with all its operands makes the
 *   sequence atomic from the compiler's point of view.  Do not split these
 *   into per-instruction helpers.
 */

#ifndef RP2350_DCP_H
#define RP2350_DCP_H

#include "tcc_stdint.h"

/* ───── bit punning ─────
 *
 * memcpy-free and union-free: lib/fp/soft/soft_common.h records that
 * address-taken 64-bit union locals have been miscompiled on this target.
 * Plain shifts of a uint64_t are safe.
 */

static inline uint64_t dcp_d2b(double d)
{
  uint64_t b;
  __builtin_memcpy(&b, &d, 8);
  return b;
}

static inline double dcp_b2d(uint64_t b)
{
  double d;
  __builtin_memcpy(&d, &b, 8);
  return d;
}

static inline uint32_t dcp_f2b(float f)
{
  uint32_t b;
  __builtin_memcpy(&b, &f, 4);
  return b;
}

static inline float dcp_b2f(uint32_t b)
{
  float f;
  __builtin_memcpy(&f, &b, 4);
  return f;
}

#define DCP_LO(x) ((uint32_t)(x))
#define DCP_HI(x) ((uint32_t)((x) >> 32))
#define DCP_MK64(lo, hi) (((uint64_t)(hi) << 32) | (uint64_t)(lo))

/* ───── reentrancy ─────
 *
 * A DCP sequence is not atomic.  If an interrupt preempts one and the handler
 * also uses the DCP, the interrupted computation is corrupted.  The contract
 * this library implements (matching the Pico SDK) is *preemptor saves*: code
 * that may run on top of an in-flight computation is responsible for saving
 * and restoring the DCP state, and reaching the DCP through these `__aeabi_*`
 * entry points is what makes an interrupt handler safe.
 *
 * The "engaged" flag says a computation is in flight.  PCMP (mrc2, the
 * non-engaging "peek" read) reports it in bit 31 without disturbing it.
 *
 * NOTE: the exact rule for when the engaged flag is set and cleared is taken
 * from the SDK's usage rather than from the datasheet; see RP2350 datasheet
 * section 3.6 before relying on it for anything beyond this save/restore.
 */

typedef struct
{
  uint32_t xl, xh; /* X mantissa/exponent  */
  uint32_t yl, yh; /* Y mantissa/exponent  */
  uint32_t el, eh; /* EFD                  */
  int engaged;
} dcp_state;

/* PCMP: peek at the compare register without engaging the DCP. */
static inline int dcp_is_engaged(void)
{
  uint32_t v;
  __asm__ volatile("mrc2 p4, #0, %0, c0, c0, #1" : "=r"(v));
  return (int)(v >> 31);
}

static inline void dcp_save(dcp_state *s)
{
  uint32_t xl, xh, yl, yh, el, eh;
  __asm__ volatile("mrrc2 p4, #0, %0, %1, c8\n\t" /* PXMD */
                   "mrrc2 p4, #0, %2, %3, c9\n\t" /* PYMD */
                   "mrrc  p4, #0, %4, %5, c10\n\t" /* REFD (engaging: also clears) */
                   : "=r"(xl), "=r"(xh), "=r"(yl), "=r"(yh), "=r"(el), "=r"(eh));
  s->xl = xl;
  s->xh = xh;
  s->yl = yl;
  s->yh = yh;
  s->el = el;
  s->eh = eh;
}

static inline void dcp_restore(const dcp_state *s)
{
  __asm__ volatile("mcrr p4, #0, %0, %1, c0\n\t" /* WXMD */
                   "mcrr p4, #0, %2, %3, c1\n\t" /* WYMD */
                   "mcrr p4, #0, %4, %5, c2\n\t" /* WEFD */
                   :
                   : "r"(s->xl), "r"(s->xh), "r"(s->yl), "r"(s->yh), "r"(s->el), "r"(s->eh));
}

/* Wrap an operation so it is safe to run on top of an interrupted one.  In the
 * common case (nothing in flight) this costs a single mrc2 and a branch. */
#define DCP_ENTER()                                                                                                    \
  dcp_state _dcp_saved;                                                                                                \
  int _dcp_was_engaged = dcp_is_engaged();                                                                             \
  if (_dcp_was_engaged)                                                                                                \
  dcp_save(&_dcp_saved)

#define DCP_LEAVE()                                                                                                    \
  do                                                                                                                   \
  {                                                                                                                    \
    if (_dcp_was_engaged)                                                                                              \
      dcp_restore(&_dcp_saved);                                                                                        \
  } while (0)

void rp2350_dcp_init(void);

#endif /* RP2350_DCP_H */
