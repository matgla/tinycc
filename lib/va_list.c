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
/* ARM EABI va_list support (soft-float / integer regs only). */
extern void abort(void);

static inline char *tcc_align_ptr(char *p, int align)
{
  if (align < 4)
    align = 4;
  return (char *)(((unsigned)p + (unsigned)align - 1u) & ~((unsigned)align - 1u));
}

void __tcc_va_start(__builtin_va_list ap, void *last, int size, int align, void *fp)
{
  char *frame = (char *)fp;
  char *reg_save = frame - 16;               /* r0-r3 saved at FP-16..FP-4 */
  char *stack_base = *(char **)(frame - 20); /* stored by prolog */
  int reg_bytes = *(int *)(frame - 24);      /* bytes of named args in r0-r3 */

  ap->__gr_top = reg_save + 16;
  if (reg_bytes < 0)
    reg_bytes = 0;
  if (reg_bytes > 16)
    reg_bytes = 16;
  ap->__gr_offs = reg_bytes;
  ap->__stack = stack_base ? stack_base : frame;
}

void *__va_arg(__builtin_va_list ap, int size, int align)
{
  int sz = size;
  if (align > 4)
    sz = (sz + align - 1) & ~(align - 1);
  else
    sz = (sz + 3) & ~3;

  int reg_align = align;
  if (reg_align < 4)
    reg_align = 4;
  int reg_offs = (ap->__gr_offs + reg_align - 1) & ~(reg_align - 1);

  if (reg_offs + sz <= 16)
  {
    char *p = ap->__gr_top + reg_offs - 16;
    ap->__gr_offs = reg_offs + sz;
    return p;
  }

  ap->__stack = tcc_align_ptr(ap->__stack, align);
  void *res = ap->__stack;
  ap->__stack += sz;
  return res;
}
#endif
