/* The second file of yasc_error_name.c: names only this file uses, and an
 * errno value this file never names (EPERM is not spelled anywhere). */
#pragma yasc
#include <errno.h>

!int other_file(int which)
{
  if (which == 0)
    return error.OnlyInFileB;
  if (which == 1)
    return error_from(EPERM);
  return error.BadPath; /* the same hashed value as in the first file */
}
