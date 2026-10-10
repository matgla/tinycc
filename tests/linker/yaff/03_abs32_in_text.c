/* Bare-metal (non-PIC) build: the pointer to `x` is an absolute R_ARM_ABS32
   in .text, which the YAFF writer cannot relocate. */
int x = 5;
int *volatile px = &x;

void _start(void) { x = *px + 1; }
