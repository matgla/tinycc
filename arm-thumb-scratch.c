#include "arm-thumb-scratch.h"

#include <string.h>

#include "arch/arm/thumb/thumb.h"
#include "arch/arm/thumb/thop_block.h"
#include "tccls.h"

/* Provided by arm-thumb-gen.c */
int ot_check(thumb_opcode op);

/* Additional scratch register exclusions (e.g. to protect argument registers
 * while materializing an indirect call target). Applied on top of per-call
 * exclude masks. */
uint32_t scratch_global_exclude = 0;

/* Track registers that were PUSH'ed by get_scratch_reg_with_save() in ORDER.
 * We must POP in reverse order since ARM POP with register lists always pops
 * in register-number order, not stack order.
 */
static int scratch_push_stack[128];
static int scratch_push_count = 0;

ScratchRegAllocs get_scratch_regs_with_save(uint32_t exclude_regs, int count)
{
  ScratchRegAllocs result;
  memset(&result, 0, sizeof(result));
  if (count <= 0)
    return result;
  if (count > (int)(sizeof(result.regs) / sizeof(result.regs[0])))
    tcc_error("compiler_error: requested too many scratch regs (%d)", count);

  TCCIRState *ir = tcc_state->ir;
  uint32_t exclude = exclude_regs | scratch_global_exclude;
  uint32_t regs_to_save = 0;

#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] get_scratch_regs: count=%d input_exclude=0x%x global_exclude=0x%x\n", count, exclude_regs,
          scratch_global_exclude);
#endif

  /* First pass: try to find free registers */
  for (int i = 0; i < count; ++i)
  {
    int reg = PREG_NONE;
    if (ir)
      reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);

    if (reg != PREG_NONE && reg >= 0 && reg < 16)
    {
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> reg[%d]=%d (free)\n", i, reg);
#endif
      result.regs[i] = reg;
      exclude |= (1u << reg);
      if (reg != 11 && reg != 12)
        scratch_global_exclude |= (1u << reg);
      result.count++;
    }
    else
    {
      int reg_to_save = -1;
      /* Prefer R0-R3: 16-bit PUSH/POP and 16-bit ALU encoding */
      for (int r = 0; r <= 3; ++r)
      {
        if (!(exclude & (1u << r)))
        {
          reg_to_save = r;
          break;
        }
      }

      if (reg_to_save < 0 && ir && ir->leaffunc && !(exclude & (1u << R_LR)))
      {
        reg_to_save = R_LR;
      }

      if (reg_to_save < 0 && !(exclude & (1u << R_IP)))
      {
        reg_to_save = R_IP;
      }

      if (reg_to_save < 0)
      {
        for (int r = 4; r <= 10; ++r)
        {
          if (!(exclude & (1u << r)))
          {
            reg_to_save = r;
            break;
          }
        }
      }

      if (reg_to_save < 0)
        tcc_error("compiler_error: no register available for scratch (all 16 registers excluded)");

#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> reg[%d]=%d (will save)\n", i, reg_to_save);
#endif
      result.regs[i] = reg_to_save;
      regs_to_save |= (1u << reg_to_save);
      exclude |= (1u << reg_to_save);
      result.count++;
    }
  }

  if (regs_to_save != 0)
  {
#ifdef ARM_THUMB_DEBUG_SCRATCH
    fprintf(stderr, "[SCRATCH] Pushing registers (mask=0x%x) in single instruction\n", regs_to_save);
#endif
    ot_check(th_push(regs_to_save));
    result.saved_mask = regs_to_save;

    for (int i = 0; i < count; ++i)
    {
      if (regs_to_save & (1u << result.regs[i]))
      {
        if (scratch_push_count < 128)
          scratch_push_stack[scratch_push_count++] = result.regs[i];
        else
          tcc_error("compiler_error: scratch register push stack overflow (>128 pushes without restore)");
      }
    }
  }

  return result;
}

void restore_scratch_regs(ScratchRegAllocs *allocs)
{
  if (!allocs || allocs->count <= 0)
    return;

  if (allocs->saved_mask != 0)
  {
    int can_restore_all = 1;
    int check_count = 0;

    for (int i = allocs->count - 1; i >= 0 && can_restore_all; --i)
    {
      if (allocs->saved_mask & (1u << allocs->regs[i]))
      {
        int stack_idx = scratch_push_count - 1 - check_count;
        if (stack_idx < 0 || scratch_push_stack[stack_idx] != allocs->regs[i])
          can_restore_all = 0;
        check_count++;
      }
    }

    if (can_restore_all && check_count > 0)
    {
      fprintf(stderr, "[SCRATCH] Popping registers (mask=0x%x) in single instruction\n", allocs->saved_mask);
      ot_check(th_pop(allocs->saved_mask));

      scratch_push_count -= check_count;
      for (int i = 0; i < allocs->count; ++i)
      {
        if (allocs->saved_mask & (1u << allocs->regs[i]))
          scratch_global_exclude &= ~(1u << allocs->regs[i]);
      }
      allocs->saved_mask = 0;
    }
    else
    {
      fprintf(stderr, "[SCRATCH] WARNING: restore_scratch_regs out of order; deferring POP\n");
    }
  }

  allocs->count = 0;
}

ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs)
{
  ScratchRegAlloc result = {0};
  TCCIRState *ir = tcc_state->ir;

#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] get_scratch_reg: input_exclude=0x%x global_exclude=0x%x\n", exclude_regs,
          scratch_global_exclude);
#endif

  exclude_regs |= scratch_global_exclude;

  if (ir)
  {
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc);
    if (reg != PREG_NONE && reg >= 0 && reg < 16)
    {
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> returning reg=%d (free) exclude=0x%x\n", reg, exclude_regs);
#endif
      result.reg = reg;
      result.saved = 0;
      if (reg != 11 && reg != 12)
        scratch_global_exclude |= (1u << reg);
      return result;
    }
  }

  int reg_to_save = -1;
  /* Prefer R0-R3: 16-bit PUSH/POP and 16-bit ALU encoding */
  for (int r = 0; r <= 3; ++r)
  {
    if (!(exclude_regs & (1u << r)))
    {
      reg_to_save = r;
      break;
    }
  }

  if (reg_to_save < 0 && ir && ir->leaffunc && !(exclude_regs & (1u << R_LR)))
  {
    reg_to_save = R_LR;
  }

  if (reg_to_save < 0 && !(exclude_regs & (1u << R_IP)))
  {
    reg_to_save = R_IP;
  }

  if (reg_to_save < 0)
  {
    for (int r = 4; r <= 11; ++r)
    {
      if (!(exclude_regs & (1u << r)))
      {
        reg_to_save = r;
        break;
      }
    }
  }

  if (reg_to_save < 0)
    tcc_error("compiler_error: no register available for scratch (all 16 registers excluded)");

#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] WARNING: no free scratch register! Saving r%d to stack\n", reg_to_save);
#endif
  ot_check(th_push(1u << reg_to_save));
  result.reg = reg_to_save;
  result.saved = 1;

  if (scratch_push_count < 128)
    scratch_push_stack[scratch_push_count++] = reg_to_save;
  else
    tcc_error("compiler_error: scratch register push stack overflow (>128 pushes without restore)");

  return result;
}

void restore_scratch_reg(ScratchRegAlloc *alloc)
{
  if (!alloc)
    return;

  if (alloc->saved)
  {
    if (scratch_push_count > 0 && scratch_push_stack[scratch_push_count - 1] == alloc->reg)
    {
      ot_check(th_pop(1u << alloc->reg));
      alloc->saved = 0;
      scratch_push_count--;
      scratch_global_exclude &= ~(1u << alloc->reg);
    }
    else
    {
      if (scratch_push_count > 0)
      {
#ifdef ARM_THUMB_DEBUG_SCRATCH
        fprintf(stderr, "[SCRATCH] WARNING: restore_scratch_reg out of order; deferring POP reg=%d (top=%d)\n",
                alloc->reg, scratch_push_stack[scratch_push_count - 1]);
#endif
      }
      else
      {
#ifdef ARM_THUMB_DEBUG_SCRATCH
        fprintf(stderr, "[SCRATCH] WARNING: restore_scratch_reg with empty push stack; deferring POP reg=%d\n",
                alloc->reg);
#endif
      }
      return;
    }
  }

  scratch_global_exclude &= ~(1u << alloc->reg);
}

static void restore_all_pushed_scratch_regs(void)
{
  for (int i = scratch_push_count - 1; i >= 0; i--)
  {
    int reg = scratch_push_stack[i];
#ifdef ARM_THUMB_DEBUG_SCRATCH
    fprintf(stderr, "[SCRATCH] auto-restoring r%d (push order %d)\n", reg, i);
#endif
    ot_check(th_pop(1u << reg));
  }
  scratch_push_count = 0;
  scratch_global_exclude = 0;
}

ST_FUNC void tcc_machine_acquire_scratch(TCCMachineScratchRegs *scratch, unsigned flags)
{
  if (!scratch)
    return;

  scratch->reg_count = 0;
  scratch->saved_mask = 0;
  scratch->regs[0] = PREG_NONE;
  scratch->regs[1] = PREG_NONE;

  uint32_t exclude_regs = 0;
  const int need_pair = (flags & TCC_MACHINE_SCRATCH_NEEDS_PAIR) != 0;

  if (flags & TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS)
    exclude_regs |= (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);

  if (flags & TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH)
    exclude_regs |= (1u << R11) | (1u << R12);

  ScratchRegAlloc first = get_scratch_reg_with_save(exclude_regs);
  if (first.reg == PREG_NONE)
    tcc_error("compiler_error: unable to allocate scratch register");

  scratch->regs[0] = first.reg;
  scratch->reg_count = 1;
  if (first.saved)
    scratch->saved_mask |= 1u;
  exclude_regs |= (1u << first.reg);
  if (first.reg != 11 && first.reg != 12)
    scratch_global_exclude |= (1u << first.reg);

  if (need_pair)
  {
    ScratchRegAlloc second = get_scratch_reg_with_save(exclude_regs);
    if (second.reg == PREG_NONE)
      tcc_error("compiler_error: unable to allocate scratch register pair");

    scratch->regs[1] = second.reg;
    scratch->reg_count = 2;
    if (second.saved)
      scratch->saved_mask |= 2u;
    if (second.reg != 11 && second.reg != 12)
      scratch_global_exclude |= (1u << second.reg);
  }
}

ST_FUNC void tcc_machine_release_scratch(const TCCMachineScratchRegs *scratch)
{
  if (!scratch)
    return;

  for (int i = scratch->reg_count - 1; i >= 0; --i)
  {
    ScratchRegAlloc alloc = {0};
    alloc.reg = scratch->regs[i];
    alloc.saved = (scratch->saved_mask & (1u << i)) != 0;
    restore_scratch_reg(&alloc);
  }
}

ST_FUNC void tcc_gen_machine_end_instruction(void)
{
  restore_all_pushed_scratch_regs();
}
