/* Guard: mem_inline must not corrupt a stack-slot operand's offset when it
 * narrows the slot's base type.
 *
 * A small constant-size memcpy/memset whose destination is `&local` (or a
 * local struct's field) is expanded into a direct StackLoc LOAD/STORE.  The
 * expansion re-typed the slot operand with a bare `slot.btype = INT32`, but a
 * STRUCT-typed operand keeps its slot offset in the HIGH half of the split
 * `u.s` encoding (u.s.aux_data) while scalar btypes read it from the full
 * `u.imm32`.  So offset O silently became (O << 16 | ctype_idx):
 *
 *     YaffHeader header;                  // slot at -92
 *     memcpy(header.magic, "YAFF", 4);    // stored to slot -6029312
 *
 * The store missed the struct entirely AND the frame allocator sized the
 * prologue to cover the bogus slot -- tcc_output_yaff got a 6 MiB `sub sp, sp,
 * ip`, so on-device `tcc hello.c -o a.out` died in the prologue with a process
 * stack overflow.
 *
 * Covers the word-width shapes that reach the direct-slot path: a struct
 * field, a plain scalar, a union member, and the memset (fill) side.  The
 * sub-word (1/2 byte) field copies are exercised by the host unit test
 * tests/unit/arm/armv8m/test_opt_mem_inline.c -- the self-hosted compiler
 * still miscompiles those (a separate, pre-existing codegen bug), so keeping
 * them here would make this guard fail for an unrelated reason.
 */

#include <stdio.h>
#include <string.h>

typedef struct
{
  unsigned char magic[4];
  unsigned short half;
  unsigned char tail;
  unsigned int rest;
} Hdr;

typedef union {
  unsigned int u;
  unsigned char b[4];
} Word;

int main(void)
{
  Hdr h;
  Word w;
  unsigned int whole;

  /* field of a local struct, one word */
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, "YAFF", 4);
  h.rest = 0x1234u;
  printf("magic=%c%c%c%c rest=%x\n", h.magic[0], h.magic[1], h.magic[2], h.magic[3], h.rest);

  /* memset into a field (fill side of the same expansion) */
  memset(h.magic, 'x', 4);
  printf("filled=%c%c%c%c\n", h.magic[0], h.magic[1], h.magic[2], h.magic[3]);

  /* whole scalar by address */
  memcpy(&whole, "abcd", 4);
  printf("whole=%08x\n", whole);

  /* union member */
  memcpy(w.b, "1234", 4);
  printf("union=%c%c%c%c u=%08x\n", w.b[0], w.b[1], w.b[2], w.b[3], w.u);

  /* the struct's other fields must be untouched by the field copies */
  printf("rest=%x half=%04x tail=%02x\n", h.rest, (unsigned)h.half, (unsigned)h.tail);
  return 0;
}
