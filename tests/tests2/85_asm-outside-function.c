#ifdef __leading_underscore
#define _ "_"
#else
#define _
#endif

extern int printf(const char *, ...);
extern void vide(void);

#if defined(__arm__) || defined(__thumb__) || defined(__ARMEL__) || defined(__ARM_EABI__)
#if defined(__thumb__)
__asm__(".thumb\n"
        ".globl " _ "vide\n"
        ".thumb_func " _ "vide\n" _ "vide:\n"
        "bx lr\n");
#else
__asm__(".globl " _ "vide\n" _ "vide:\n"
        "bx lr\n");
#endif
#else
__asm__(".globl " _ "vide\n" _ "vide:\n"
        "ret\n");
#endif

int main()
{
  vide();
  printf("okay\n");
  return 0;
}
