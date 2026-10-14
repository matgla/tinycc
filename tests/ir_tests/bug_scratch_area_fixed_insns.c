/*
 *  TCC IR - scratch save area sized from resolved instructions
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The discovery run saved a scratch at next_clear_bit's `none` stores (-Os),
 * the phase-3 fixup moved the blocker to r8 so the real run found a free
 * register, yet the frame still reserved a save slot for it.  An instruction
 * the fixup vacated enough caller-saved registers at needs no slot (codegen.c). */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

struct bitset { uintptr_t bit_length; uintptr_t *masks; };
struct pool { uintptr_t memory_size; uintptr_t page_count; struct bitset bitmap; uintptr_t used; };
struct opt_usize { uintptr_t payload; bool is_null; };

/* No zero guard: the caller's `bits != 0` already decided it. */
static uint8_t ctz32(uint32_t v)
{
  return (uint8_t)__builtin_ctz(v);
}

/* TmpMemoryPool.next_bit: the first clear bit in [start, end).  At the first
 * `none` store r0 (result), r1 (pool), r2 (start) and r3 (end) are all live. */
__attribute__((noinline)) struct opt_usize next_clear_bit(struct pool *p, uintptr_t start, uintptr_t end)
{
  if (start >= end)
    return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaaul};
  uintptr_t *masks = p->bitmap.masks;
  uintptr_t word = start / 32;
  uintptr_t bits = ~masks[word] & ((uintptr_t)-1 << (start % 32));
  for (;;)
  {
    if (bits != 0)
    {
      uintptr_t index = word * 32 + ctz32(bits);
      if (index < end)
        return (struct opt_usize){.is_null = false, .payload = index};
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaaul};
    }
    word++;
    if (word * 32 >= end)
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaaul};
    bits = ~masks[word];
  }
}

static void show(struct pool *p, uintptr_t start, uintptr_t end)
{
  struct opt_usize r = next_clear_bit(p, start, end);
  if (r.is_null)
    printf("[%u,%u): none\n", (unsigned)start, (unsigned)end);
  else
    printf("[%u,%u): %u\n", (unsigned)start, (unsigned)end, (unsigned)r.payload);
}

int main(void)
{
  uintptr_t masks[4] = {0xffffffffu, 0x0000ffffu, 0xffffffffu, 0x7fffffffu};
  struct pool p = {.memory_size = 4096, .page_count = 128, .bitmap = {128, masks}, .used = 0};
  show(&p, 0, 128);
  show(&p, 5, 128);
  show(&p, 40, 128);
  show(&p, 48, 50);
  show(&p, 48, 48);
  show(&p, 49, 48);
  show(&p, 64, 127);
  show(&p, 64, 128);
  show(&p, 0, 32);
  masks[1] = 0xffffffffu;
  show(&p, 0, 64);
  show(&p, 0, 128);
  return 0;
}
