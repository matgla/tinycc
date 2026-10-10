/* Loop-invariant parameter passed on the stack (5th+ argument) is reloaded
 * from the incoming argument area every iteration instead of being hoisted
 * into a register — the kernel's mem.findScalarPos shape (its needle is the
 * 5th parameter, so it lives in the caller's argument area at [sp,#arg]).
 *
 * Expected (gcc -O2): the loop body loads buf[i], compares against a needle
 * kept in a register, three instructions per byte:
 *     ldrb rX, [rY, rZ] ; cmp rX, rW ; bne/it ...
 * Actual (armv8m-tcc -O2): the needle is re-loaded from [sp,#N] inside the
 * loop on every iteration:
 *     ldrb r5, [r8, r2] ; ldrb r6, [sp, #32] ; cmp r5, r6 ; ...
 *
 * Build:
 *   armv8m-tcc  -O2 -c loop_stack_param_reload_2026-10-09.c -o /tmp/a.o -B<cross>
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2 -c loop_stack_param_reload_2026-10-09.c -o /tmp/b.o
 *   arm-none-eabi-objdump -d /tmp/a.o /tmp/b.o   (compare <find_scalar> loops)
 */
#include <stdint.h>

struct pos { int64_t p; int ok; };

/* 5 parameters: out (r0), buf (r1), len (r2), start (r3), needle ([sp]) */
__attribute__((noinline))
void find_scalar(struct pos *out, const uint8_t *buf, uint64_t len,
                 uint64_t start, uint8_t needle)
{
    for (uint64_t i = start; i < len; i++) {
        if (buf[i] == needle) {
            out->p = (int64_t)i;
            out->ok = 1;
            return;
        }
    }
    out->p = -1;
    out->ok = 0;
}

/* Same loop with the needle as the 4th (register) parameter, for contrast:
 * tcc compiles this one cleanly — it is specifically the argument-area
 * address that LICM does not treat as a promotable local. */
__attribute__((noinline))
void find_scalar_reg(struct pos *out, const uint8_t *buf, uint64_t len, uint8_t needle)
{
    for (uint64_t i = 0; i < len; i++) {
        if (buf[i] == needle) {
            out->p = (int64_t)i;
            out->ok = 1;
            return;
        }
    }
    out->p = -1;
    out->ok = 0;
}
