/* An initialized object in a named section: the YAFF writer has no image for
   it, so the link must fail instead of writing a module that reads 0. */
__attribute__((section(".mydata"))) int x = 0x12345678;

void _start(void) { x++; }
