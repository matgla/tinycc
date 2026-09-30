/* f_a and f_b compile to the same code, and f_b is folded onto f_a
   (identical code folding: its symbol points at f_a's body).  Both read
   `mode`, a static never written, which only the end of the TU can prove, so
   late reopt compiles f_a again: it erases f_a's first body, shifting
   everything after it down, and emits it anew at the end.  f_b's symbol
   stayed at the erased address -- on the range's filler and then on the code
   shifted there -- and f_b returned garbage.  This is Zig's
   link.File.startProgress; the tcc -O2 Zig compiler segfaulted. */
#include <stdio.h>
#include <stdlib.h>

static int mode = 3;

struct File
{
  char pad[64];
  unsigned char tag;
};

__attribute__((noinline)) static int f_a(struct File *f)
{
  if (f->tag == 2)
    abort();
  return f->tag + mode;
}

__attribute__((noinline)) static int f_b(struct File *f)
{
  if (f->tag == 2)
    abort();
  return f->tag + mode;
}

__attribute__((noinline)) static int after(int x) { return x * 7 + 1; }

int main(void)
{
  struct File f = {{0}, 5};
  printf("%d %d %d\n", f_a(&f), f_b(&f), after(2));
  return 0;
}
