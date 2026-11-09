/* A static function named like one in 13_spill_gap_b.c: linked from an archive
 * under --gc-sections the two .text.dup sections become one, with alignment
 * between their bytes (10 bytes each, 4-aligned). */
extern unsigned ext_mix(unsigned);

static __attribute__((noinline)) unsigned dup(unsigned x)
{
  return ext_mix(x) + 1;
}

unsigned call_a(unsigned x)
{
  return dup(x);
}
