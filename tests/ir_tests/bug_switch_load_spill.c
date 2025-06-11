/* Regression: a switch where every case assigns a constant to one common
 * variable is rewritten by the switch->data-table optimization (switch_to_data)
 * into a SWITCH_LOAD whose dest is loaded from a .rodata value table.  The
 * backend (arm-thumb-gen.c tcc_gen_machine_switch_load_mop) required that dest
 * to be a hardware register; under high register pressure the allocator spilled
 * it and codegen aborted with
 *   "internal error: SWITCH_LOAD dest must be in a hardware register".
 *
 * Fixed by resolving the dest through mach_get_dest_reg() (which yields a
 * scratch when spilled) and storing it back with mach_writeback_dest().
 * The many live operands below create the register pressure that spills dest.
 */
#include <stdio.h>

static int ext(int x) { return (int)(((unsigned)x * 2654435761u) >> 28); }

static int compute(int x, int a, int b, int c, int d, int e, int f, int g, int h)
{
  int v;
  switch (x) {
    case 0: v = 11; break;
    case 1: v = 22; break;
    case 2: v = 33; break;
    case 3: v = 44; break;
    case 4: v = 55; break;
    case 5: v = 66; break;
    default: v = 7; break;
  }
  int s = a + b + c + d + e + f + g + h;
  s += ext(a) + ext(b) + ext(c) + ext(d);
  s += ext(e) + ext(f) + ext(g) + ext(h);
  return v + s + a * b + c * d + e * f + g * h;
}

int main(void)
{
  int total = 0;
  for (int x = 0; x <= 6; x++)
    total += compute(x, 1, 2, 3, 4, 5, 6, 7, 8);
  printf("total=%d\n", total);
  return (total == 1638) ? 0 : 1;
}
