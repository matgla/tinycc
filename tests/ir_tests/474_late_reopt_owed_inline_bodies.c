/* The end-of-TU analyses must see every body the TU emits.  A `static inline`
   function whose address is taken is not expanded at a call site; its body was
   parsed only when gen_inline_functions emitted it, after the late_reopt
   global_init fold and the dead-static-store analysis had run:
     - a static written only in that body folded to its zero initializer;
     - a store to a static read only in that body was dropped as dead. */
#include <stdio.h>

static int written_in_inline;
static inline void set_written(int v) { written_in_inline = v; }
__attribute__((noinline)) int get_written(void) { return written_in_inline; }

static int read_in_inline;
static inline int get_read(void) { return read_in_inline; }
__attribute__((noinline)) void set_read(int v) { read_in_inline = v; }

void (*volatile set_hook)(int);
int (*volatile get_hook)(void);

int main(void)
{
  set_hook = set_written;
  set_hook(5);
  get_hook = get_read;
  set_read(7);
  printf("%d %d\n", get_written(), get_hook());
  return 0;
}
