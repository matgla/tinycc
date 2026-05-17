/* Check semantics of various constructs to generate renamed symbols.  */

extern int printf(const char *, ...);
void target(void);
void target(void)
{
  printf("in target function\n");
}

/* On ARM Thumb, a pure symbol-alias for a function can be problematic in some
   toolchains (missing Thumb marking / interworking metadata on the alias
   symbol). Use a small wrapper there so calls remain correct. */
#if defined(__thumb__)
void alias_for_target(void)
{
  target();
}
#else
void alias_for_target(void) __attribute__((alias("target")));
#endif

int g_int = 34;
#ifdef __TINYC__
int alias_int __attribute__((alias("g_int")));
#else
extern int alias_int __asm__("g_int");
#endif

#ifdef __leading_underscore
#define _ "_"
#else
#define _
#endif

void asm_for_target(void) __asm__(_ "target");
#ifdef __TINYC__
int asm_int __asm__(_ "g_int");
#else
extern int asm_int __asm__(_ "g_int");
#endif

/* This is not supposed to compile, alias targets must be defined in the
   same unit.  In TCC they even must be defined before the reference
void alias_for_undef(void) __attribute__((alias("undefined")));
*/

extern void inunit2(void);

int main(void)
{
  target();
  alias_for_target();
  asm_for_target();
  printf("g_int = %d\nalias_int = %d\nasm_int = %d\n", g_int, alias_int, asm_int);
  inunit2();
  return 0;
}
