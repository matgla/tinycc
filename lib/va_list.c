/* va_list.c - tinycc support for va_list on X86_64 */

#if defined __x86_64__

/* Avoid include files, they may not be available when cross compiling */
extern void abort(void);

/* This should be in sync with our include/stdarg.h */
enum __va_arg_type
{
  __va_gen_reg,
  __va_float_reg,
  __va_stack
};

/* GCC compatible definition of va_list. */
/*predefined by TCC (tcc_predefs.h):
typedef struct {
    unsigned int gp_offset;
    unsigned int fp_offset;
    union {
        unsigned int overflow_offset;
        char *overflow_arg_area;
    };
    char *reg_save_area;
} __builtin_va_list[1];
*/

extern void *memcpy(void *dest, const void *src, unsigned long n);

void *__va_arg(__builtin_va_list ap, int arg_type, int size, int align)
{
  size = (size + 7) & ~7;
  align = (align + 7) & ~7;
  switch ((enum __va_arg_type)arg_type)
  {
  case __va_gen_reg:
    if (ap->gp_offset + size <= 48)
    {
      ap->gp_offset += size;
      return ap->reg_save_area + ap->gp_offset - size;
    }
    goto use_overflow_area;

  case __va_float_reg:
    if (ap->fp_offset < 128 + 48)
    {
      ap->fp_offset += 16;
      if (size == 8)
        return ap->reg_save_area + ap->fp_offset - 16;
      if (ap->fp_offset < 128 + 48)
      {
        memcpy(ap->reg_save_area + ap->fp_offset - 8, ap->reg_save_area + ap->fp_offset, 8);
        ap->fp_offset += 16;
        return ap->reg_save_area + ap->fp_offset - 32;
      }
    }
    goto use_overflow_area;

  case __va_stack:
  use_overflow_area:
    ap->overflow_arg_area += size;
    ap->overflow_arg_area = (char *)((long long)(ap->overflow_arg_area + align - 1) & -align);
    return ap->overflow_arg_area - size;

  default: /* should never happen */
    abort();
    return 0;
  }
}
#endif

#if defined __arm__
/* ARM EABI va_list: pointer-based (GCC-compatible ABI).
 *
 * va_list is typedef char *__builtin_va_list — a simple pointer that
 * advances through a contiguous area of register-saved + stack arguments.
 *
 * The prologue pushes r0-r3 so they are contiguous with caller stack args.
 * Frame layout at FP:
 *   FP - 20: gr_top   (char*) — end of pushed r0-r3 = start of stack args
 *   FP - 24: reg_bytes (int)  — bytes of named args occupying r0-r3
 *   FP - 28: named_stack_bytes (int) — bytes of named args on stack
 */

void __tcc_va_start(char **ap_ptr, void *fp)
{
  char *frame = (char *)fp;
  char *gr_top = *(char **)(frame - 20);
  int reg_bytes = *(int *)(frame - 24);
  int named_stack_bytes = *(int *)(frame - 28);

  if (reg_bytes < 0)
    reg_bytes = 0;
  if (reg_bytes > 16)
    reg_bytes = 16;

  /* Point ap to the first anonymous argument.
   * gr_top - 16 is the start of the pushed r0-r3 area.
   * Skip past named args in registers and on the stack. */
  *ap_ptr = (gr_top - 16) + reg_bytes + named_stack_bytes;
}

void *__tcc_va_arg(char **ap_ptr, int size, int align)
{
  char *ap = *ap_ptr;

  if (align < 4)
    align = 4;

  /* Align the current pointer */
  ap = (char *)(((unsigned)ap + (unsigned)align - 1u) & ~((unsigned)align - 1u));

  /* Round size up to word boundary */
  int sz = (size + 3) & ~3;

  void *result = ap;
  *ap_ptr = ap + sz;
  return result;
}
#endif
