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
 *
 * There are no runtime helpers left: va_start and va_arg are both compiler
 * intrinsics (TOK_builtin_va_start / TOK_builtin_va_arg).
 *
 *   va_start  materializes the address of the first anonymous argument
 *             directly — the backend knows the prologue's push set and the
 *             named-argument byte counts exactly.  __tcc_va_start used to
 *             rediscover it at runtime from three metadata words the prologue
 *             stored at FP-20/-24/-28; those stores are gone, so reviving that
 *             helper would read garbage.
 *   va_arg    emits the pointer bump inline: align up to max(align,4), take the
 *             pointer, advance by (size+3)&~3.  `ap` is invariantly word
 *             aligned, so the align step only survives for 8-byte types.
 *             Inlining it keeps `ap` in a register (its address is no longer
 *             taken) and replaces a 4-instruction call sequence per va_arg.
 */
#endif
