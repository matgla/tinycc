/*
 * Regression test: argument register clobbered during prologue spill
 * with large stack frame.
 *
 * Bug: When a function parameter is spilled to the stack (e.g. because its
 * address is taken), and the stack frame contains a large local buffer
 * (char buf[1024]), the spill offset exceeds the Thumb STR immediate range.
 * The prologue calls tcc_gen_machine_store_to_stack() which uses
 * th_offset_to_reg_ex() to materialize the large offset into a scratch
 * register. The scratch allocator consults liveness at instruction index 0,
 * which reports R0-R3 as "free" — but they still hold incoming parameters
 * that haven't been saved yet.
 *
 * Result: storing parameter P0 (in R0) to its stack slot uses R1 as scratch
 * to hold the offset constant, destroying the value of P1 (fmt) in R1.
 * Subsequent code that tries to save R1 to a callee-saved register captures
 * the offset constant instead of the original parameter value.
 *
 * Trigger conditions:
 *   1. A register parameter is forced to the stack (address-taken)
 *   2. A large local buffer makes the frame offset > 255 bytes
 *   3. The scratch allocator picks another argument register
 *
 * Taking &fmt forces it onto the stack, and the helper function strstart()
 * modifies it through the pointer (mimicking the real-world pattern from
 * tcc_add_library_internal / strstart in libtcc.c).
 */

#include <stdio.h>
#include <string.h>

/*
 * strstart: check if str starts with val, advance *str past it.
 * This is a common pattern that requires &str (address-of a parameter).
 */
static int strstart(const char **strp, const char *val)
{
  const char *p = *strp;
  const char *q = val;
  while (*q)
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }
  *strp = p;
  return 1;
}

/*
 * The bug-triggering function:
 * - 4 register params: ctx(R0), fmt(R1), name(R2), flags(R3)
 * - 2 stack params: paths, nb_paths
 * - char buf[1024] creates a large frame
 * - &fmt forces fmt to be spilled to stack at a large offset
 * - When the prologue stores ctx(R0) to its large-offset stack slot,
 *   the scratch allocator may pick R1 (fmt) to hold the offset,
 *   destroying fmt before it is saved.
 */
int format_with_prefix(int ctx, const char *fmt, const char *name, int flags, const char **paths, int nb_paths)
{
  char buf[1024];
  int i;

  for (i = 0; i < nb_paths; i++)
  {
    snprintf(buf, sizeof(buf), fmt, paths[i], name);

    /* Taking &fmt forces it to a stack slot. strstart modifies fmt
     * through the pointer — this is the real-world pattern. */
    if (strstart(&fmt, "%s"))
    {
      /* fmt was advanced past "%s" — should not happen for "%s/%s",
       * but the address-of is what matters for triggering the bug. */
    }

    printf("%s\n", buf);
  }

  return ctx + flags;
}

int main(void)
{
  const char *paths[] = {"/usr/lib", "/usr/local/lib"};

  int ret = format_with_prefix(42,           /* ctx:  R0 */
                               "%s/%s",      /* fmt:  R1 — this gets clobbered */
                               "libtest.so", /* name: R2 */
                               7,            /* flags: R3 */
                               paths,        /* paths: stack */
                               2             /* nb_paths: stack */
  );

  printf("ret=%d\n", ret);
  return 0;
}
