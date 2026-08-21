/* Cross-IR-op frame-slot reload elimination (the strldr cache in
 * source/backend/arch/arm/thumb/arm-thumb-gen.c, reset policy in
 * source/ir/codegen.c).
 *
 * `reload_pair` has more live 64-bit values than registers, so a_lo/a_hi and
 * b_lo/b_hi end up in frame slots and every use materializes them again.  The
 * compare and the subtraction are two IR ops, so before the cache survived an
 * IR boundary the second one reloaded all four halves the first had just
 * loaded -- and a conditional branch sits between them, which is why a plain
 * branch may not invalidate the cache either.
 *
 * `chase` is the negative control that the same cache got WRONG on the first
 * attempt: `ldr r0,[r0]` overwrites its own base, so recording "r0 holds [r0]"
 * collapses a pointer chase into a single dereference.  Both dereferences must
 * survive.  (gcc.c-torture pr103209 and 253_fuzz_ptr_load_cse_addrtaken_alias
 * catch it at runtime; this catches it in the instruction stream.) */

typedef unsigned long long u64;

u64 sink(u64, u64, u64, u64, u64) __attribute__((noinline));

u64 reload_pair(u64 a, u64 b, u64 c, u64 d, u64 e, int s)
{
  u64 r;
  if (a >= b)
    r = a - b;
  else
    r = b - a;
  return r + sink(a, b, c, d, e) + (s ? c : d) + e;
}

int chase(int ***ppp) { return ***ppp; }
