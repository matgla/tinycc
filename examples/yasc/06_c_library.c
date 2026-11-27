/* Chapter 6 -- using a C library from yasc (README.md).
 *
 * counter.h is regular C.  Its plain pointers are unchecked: yasc takes your
 * word for them.  Its attributes are believed: counter_new returns an owned
 * pointer that counter_free takes back, so forgetting counter_free would be a
 * leak warning.  Handing the owned pointer to the other C functions says
 * whether they borrow it (borrow(c)) or keep it (move(c)).  Build: tcc -ccompat -c counter.c && tcc 06_c_library.c counter.o */
#pragma yasc
#include <stdio.h>
#include "counter.h"

int main(void)
{
  counter_t *?owned c = counter_new("clicks"); /* owned: the attribute says so */
  if (!c)
    return 1;
  counter_add(borrow(c), COUNTER_STEP); /* lent: c is still owned */
  counter_add(borrow(c), 2);
  const char *name = unsafe(counter_name(borrow(c))); /* unchecked: unsafe vouches for it */
  printf("%s = %d\n", name, counter_get(borrow(c)));
  counter_free(c); /* the deallocator counter_new named */

  counter_t *?none = COUNTER_NONE; /* a C macro's cast is C too */
  printf("none is %s\n", none ? "set" : "null");
  return 0;
}
