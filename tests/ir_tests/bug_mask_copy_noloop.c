/* Minimal test for regalloc bug: struct member load + AND mask + copy
 * under high register pressure. No loops involved.
 * Tests the same pattern as bug_struct_mask_copy.c but with a non-loop
 * popcount to avoid triggering the loop-inline guard. */
#include <stdio.h>

typedef struct {
  int call_id;
  int registers_map;
  int arg_count;
  int used_stack_size;
} CallSite;

volatile int g_flag = 1;
volatile unsigned int g_scratch = 0;

static CallSite g_sites[4];

__attribute__((noinline)) CallSite *get_site(int id)
{
  if (id < 0 || id >= 4) return 0;
  return &g_sites[id];
}

/* Non-loop popcount using bit twiddling (no loop = can be inlined at -O2) */
static int popcount_noloop(unsigned x)
{
  x = x - ((x >> 1) & 0x55555555);
  x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
  x = (x + (x >> 4)) & 0x0F0F0F0F;
  return (x * 0x01010101) >> 24;
}

__attribute__((noinline)) int emit_check_mask(unsigned short regs)
{
  /* Only R0-R12 + LR valid (no SP=bit13, no PC=bit15) */
  if (regs & 0xA000) {
    printf("FAIL: invalid mask 0x%x\n", regs);
    return -1;
  }
  return 0;
}

__attribute__((noinline)) void adjust_sp(int delta) { (void)delta; }
__attribute__((noinline)) void do_call(int target) { (void)target; }
__attribute__((noinline)) void handle_return(int d, int v) { (void)d; (void)v; }

__attribute__((noinline)) int gen_call_op(int func_target, int call_id_packed,
                                          int dest, int drop_value)
{
  int call_id = call_id_packed & 0xFFFF;
  int argc_hint = (call_id_packed >> 16) & 0xFF;

  CallSite *call_site = get_site(call_id);
  if (!call_site) return -1;

  int stack_size = (argc_hint > 0) ? 16 : 0;

  /* THE CRITICAL PATTERN:
   * 1. Load struct member through pointer
   * 2. AND with mask
   * 3. Copy to another local
   * 4. Function call adds register pressure
   * 5. Conditional ORs modify the copy
   * 6. Use the copy later */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  int arg_regs_push_mask = arg_regs_in_use;
  int arg_regs_push_count = popcount_noloop((unsigned)arg_regs_push_mask);

  if (g_flag) {
    arg_regs_push_mask |= (1 << 9);
    arg_regs_push_count++;
  }

  if (arg_regs_push_count & 1) {
    arg_regs_push_mask |= (1 << 12);
    arg_regs_push_count++;
  }

  if (arg_regs_push_mask) {
    if (emit_check_mask((unsigned short)arg_regs_push_mask) < 0) {
      printf("FAIL: mask=0x%x (from registers_map=0x%x)\n",
             arg_regs_push_mask, call_site->registers_map);
      return 1;
    }
    call_site->used_stack_size += arg_regs_push_count * 4;
  }

  stack_size = (stack_size + 7) & ~7;
  if (stack_size > 0) {
    adjust_sp(-stack_size);
    call_site->used_stack_size += stack_size;
  }

  unsigned int saved = g_scratch;
  g_scratch |= 0x0F;
  do_call(func_target);
  g_scratch = saved;

  if (stack_size > 0) {
    adjust_sp(stack_size);
    call_site->used_stack_size -= stack_size;
  }
  if (arg_regs_push_mask) {
    call_site->used_stack_size -= arg_regs_push_count * 4;
  }

  handle_return(dest, drop_value);
  call_site->registers_map &= ~0x0F;
  return 0;
}

int main(void)
{
  int fail = 0;

  g_sites[0].call_id = 0;
  g_sites[0].registers_map = 0xDEAD000B;
  g_sites[0].used_stack_size = 0;
  if (gen_call_op(0x1000, 0x030000, 0, 0)) {
    printf("FAIL test1\n"); fail = 1;
  }

  g_sites[1].call_id = 1;
  g_sites[1].registers_map = 0x12345678;
  g_sites[1].used_stack_size = 0;
  if (gen_call_op(0x2000, 0x020001, 0, 0)) {
    printf("FAIL test2\n"); fail = 1;
  }

  if (!fail) printf("PASS\n");
  return fail;
}
