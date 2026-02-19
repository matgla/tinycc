/*
 * Bug: Register allocator assigns wrong source register for local copy after
 *      struct-member load + AND mask, under high register pressure.
 *
 * Reduced from tcc_gen_machine_func_call_op() which has:
 *   - 6 parameters (3 x 12-byte structs, int, pointer, int)
 *   - A function call returning a struct pointer (call_site)
 *   - Another function call filling a layout struct (7 args, 2 on stack)
 *   - A context struct capturing 5 locals (keeping them all live)
 *   - Then: int in_use = call_site->registers_map & 0x0F;
 *           int push_mask = in_use;    // <-- BUG: gets struct pointer
 *           int push_count = popcount(push_mask);
 *   - Conditional ORs into push_mask
 *   - Both call_site AND push_mask stay live for cleanup at function end
 *
 * The compiler computes AND into r1 but the MOV for push_mask reads r0
 * (stale struct pointer).  push_mask ends up as e.g. 0x3218 with bit 13
 * (SP) set, which no PUSH encoding allows → th_push returns {0,0}.
 */
#include <stdio.h>

/* --- Struct matching ThumbGenCallSite layout --- */
typedef struct
{
  int call_id;
  int registers_map;
  int *arg_list;
  int arg_count;
  int used_stack_size;
} CallSite;

/* --- Struct matching TCCAbiCallLayout --- */
typedef struct
{
  int locs[8];
  int stack_size;
  int reg_count;
} AbiLayout;

/* --- Struct matching CallGenContext (captures many locals, keeping them live) --- */
typedef struct
{
  CallSite *call_site;
  AbiLayout *layout;
  int *args;
  int argc;
  int stack_size;
} CallCtx;

/* --- Opcode struct matching thumb_opcode (8 bytes → returned via sret) --- */
typedef struct
{
  unsigned char size;
  unsigned int opcode;
} Opcode;

/* --- Globals matching text_and_data_separation, scratch_global_exclude --- */
volatile int g_text_data_sep = 1;
volatile unsigned int g_scratch_exclude = 0;

static int popcount_u(unsigned x)
{
  int c = 0;
  while (x)
  {
    c += x & 1;
    x >>= 1;
  }
  return c;
}

/* Simulate thumb_get_call_site_for_id - returns pointer, noinline for call overhead */
static CallSite g_sites[4];

__attribute__((noinline)) CallSite *get_site(int id)
{
  if (id < 0 || id >= 4)
    return 0;
  return &g_sites[id];
}

/* Simulate thumb_build_call_layout_from_ir - many params, 2 on stack like original */
__attribute__((noinline)) int build_layout(int *ir, int call_idx, int call_id, int argc_hint, AbiLayout *layout,
                                           int **out_args)
{
  (void)ir;
  (void)call_idx;
  (void)call_id;
  layout->stack_size = 16;
  layout->reg_count = argc_hint < 4 ? argc_hint : 4;
  *out_args = 0;
  return argc_hint;
}

/* Simulate th_push - returns 8-byte struct (sret), validates register mask */
__attribute__((noinline)) Opcode make_push(unsigned short regs)
{
  /* T1 encoding: R0-R7 + LR only */
  if (!(regs & 0xBF00))
  {
    unsigned short lr = (regs >> 14) & 1;
    return (Opcode){2, 0xB400 | (lr << 8) | (regs & 0xFF)};
  }
  /* T2 encoding: R0-R12 + LR only (no SP=bit13, no PC=bit15) */
  if (!(regs & 0xA000))
  {
    return (Opcode){4, (0xE92DU << 16) | regs};
  }
  /* Invalid register set */
  return (Opcode){0, 0};
}

/* Simulate ot_check */
__attribute__((noinline)) int emit_check(Opcode op)
{
  if (op.size == 0)
  {
    printf("FAIL: emit_check got {0,0} - invalid opcode\n");
    return -1;
  }
  return 0;
}

/* Simulate gadd_sp */
__attribute__((noinline)) void adjust_sp(int delta)
{
  (void)delta;
}

/* Simulate the actual argument emission functions that keep ctx alive */
__attribute__((noinline)) int emit_reg_args(CallCtx *ctx)
{
  return ctx->argc > 4 ? 4 : ctx->argc;
}

__attribute__((noinline)) void emit_stack_args(CallCtx *ctx)
{
  (void)ctx->stack_size;
}

__attribute__((noinline)) void do_call(int target)
{
  (void)target;
}

__attribute__((noinline)) void handle_return(int dest, int drop)
{
  (void)dest;
  (void)drop;
}

/*
 * This function mirrors tcc_gen_machine_func_call_op's structure:
 * - 6 parameters to match the original signature
 * - A struct pointer from function call
 * - Layout build with 6 args (2 on stack)
 * - Context struct capturing 5 locals
 * - The critical: site->registers_map & 0x0F → copy → popcount → OR
 * - Both call_site and push_mask stay live for cleanup at the end
 */
__attribute__((noinline)) int gen_call_op(int func_target, int call_id_packed, int dest, int drop_value, int *ir,
                                          int call_idx)
{
  int call_id = call_id_packed & 0xFFFF;
  int argc_hint = (call_id_packed >> 16) & 0xFF;

  CallSite *call_site = get_site(call_id);
  if (!call_site)
    return -1;

  /* Build ABI layout (many args → register pressure like original) */
  AbiLayout layout;
  int *args = 0;
  int argc = build_layout(ir, call_idx, call_id, argc_hint, &layout, &args);
  if (argc < 0)
    return -1;

  int stack_size = (argc > 0) ? layout.stack_size : 0;

  /* Context struct keeps call_site, &layout, args, argc, stack_size all live */
  CallCtx ctx;
  ctx.call_site = call_site;
  ctx.layout = &layout;
  ctx.args = args;
  ctx.argc = argc;
  ctx.stack_size = stack_size;

  /* ====== THE CRITICAL SECTION ======
   * This is the pattern that triggers the bug:
   * 1. Load struct member through pointer
   * 2. AND with mask
   * 3. Copy to another local
   * 4. Function call (popcount) adds register pressure
   * 5. Conditional ORs modify the copy
   * 6. Use the copy in a function call returning a struct (sret)
   */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  int arg_regs_push_mask = arg_regs_in_use;
  int arg_regs_push_count = popcount_u((unsigned)arg_regs_push_mask);

  /* Conditional OR (like text_and_data_separation check) */
  if (g_text_data_sep)
  {
    arg_regs_push_mask |= (1 << 9); /* R9 */
    arg_regs_push_count++;
  }

  /* Alignment pad */
  if (arg_regs_push_count & 1)
  {
    arg_regs_push_mask |= (1 << 12); /* R12 */
    arg_regs_push_count++;
  }

  if (arg_regs_push_mask)
  {
    Opcode op = make_push((unsigned short)arg_regs_push_mask);
    if (emit_check(op) < 0)
    {
      printf("FAIL: arg_regs_push_mask=0x%x (from registers_map=0x%x)\n", arg_regs_push_mask, call_site->registers_map);
      printf("  Expected mask <= 0x%x, got 0x%x\n", 0x120F, arg_regs_push_mask);
      return 1;
    }
    call_site->used_stack_size += arg_regs_push_count * 4;
  }

  /* Reserve stack space */
  stack_size = (stack_size + 7) & ~7;
  if (stack_size > 0)
  {
    adjust_sp(-stack_size);
    call_site->used_stack_size += stack_size;
  }

  /* Block R0-R3 (like scratch_global_exclude |= 0x0F) */
  unsigned int saved_exclude = g_scratch_exclude;
  g_scratch_exclude |= 0x0F;

  /* Emit argument moves and call (keeps ctx alive) */
  emit_reg_args(&ctx);
  emit_stack_args(&ctx);
  do_call(func_target);

  g_scratch_exclude = saved_exclude;

  /* Cleanup - uses stack_size, arg_regs_push_mask, call_site again */
  if (stack_size > 0)
  {
    adjust_sp(stack_size);
    call_site->used_stack_size -= stack_size;
  }
  if (arg_regs_push_mask)
  {
    call_site->used_stack_size -= arg_regs_push_count * 4;
  }

  handle_return(dest, drop_value);
  call_site->registers_map &= ~0x0F;

  return 0;
}

int main(void)
{
  int fail = 0;
  int dummy_ir[4] = {0};

  /* Setup call sites with various registers_map values.
   * The high bits are set to large values so if the struct pointer
   * leaks into push_mask, it will have bits > 0x0F set. */

  /* Test 1: registers_map = 0xDEAD000B → low nibble 0xB = {R0,R1,R3} */
  g_sites[0].call_id = 0;
  g_sites[0].registers_map = 0xDEAD000B;
  g_sites[0].used_stack_size = 0;
  if (gen_call_op(0x1000, 0x030000, 0, 0, dummy_ir, 0))
  {
    printf("FAIL test1\n");
    fail = 1;
  }

  /* Test 2: registers_map = 0x12345678 → low nibble 0x8 = {R3} */
  g_sites[1].call_id = 1;
  g_sites[1].registers_map = 0x12345678;
  g_sites[1].used_stack_size = 0;
  if (gen_call_op(0x2000, 0x020001, 0, 0, dummy_ir, 1))
  {
    printf("FAIL test2\n");
    fail = 1;
  }

  /* Test 3: registers_map = 0xFFFF0000 → low nibble 0 */
  g_sites[2].call_id = 2;
  g_sites[2].registers_map = 0xFFFF0000;
  g_sites[2].used_stack_size = 0;
  if (gen_call_op(0x3000, 0x010002, 0, 0, dummy_ir, 2))
  {
    printf("FAIL test3\n");
    fail = 1;
  }

  /* Test 4: registers_map = 0x8000000F → low nibble 0xF = {R0,R1,R2,R3} */
  g_sites[3].call_id = 3;
  g_sites[3].registers_map = 0x8000000F;
  g_sites[3].used_stack_size = 0;
  if (gen_call_op(0x4000, 0x050003, 0, 0, dummy_ir, 3))
  {
    printf("FAIL test4\n");
    fail = 1;
  }

  if (!fail)
    printf("PASS\n");

  return fail;
}
