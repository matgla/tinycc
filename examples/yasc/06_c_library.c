/* Chapter 6 -- using a C library from yasc (README.md).
 *
 * counter.h is regular C.  Its plain pointers are unchecked: yasc takes your
 * word for them.  Its attributes are believed: counter_new returns an owned
 * pointer that counter_free takes back, so forgetting counter_free would be a
 * leak warning.  Build: tcc -ccompat -c counter.c && tcc 06_c_library.c counter.o */
#pragma yasc
#include <stdio.h>
#include "counter.h"

int main(void)
{
  counter_t * owned maybe c = counter_new("clicks"); /* owned: the attribute says so */
  if (!c)
    return 1;
  counter_add(c, COUNTER_STEP); /* a C parameter borrows: c is still owned */
  counter_add(c, 2);
  const char * view name = counter_name(c); /* unchecked: a view is fine */
  printf("%s = %d\n", name, counter_get(c));
  counter_free(c); /* the deallocator counter_new named */

  counter_t * view maybe none = COUNTER_NONE; /* a C macro's cast is C too */
  printf("none is %s\n", none ? "set" : "null");
  return 0;
}
