/* yasc error_name(), <yasc.h>'s try_errno / try_neg, across two files: every
 * file records the error names it uses in the yasc_errname section, and the
 * file calling error_name also records the errno names its errno.h defines.
 * The records must survive --gc-sections.  docs/plans/yasc_errors.md. */
#pragma yasc
#include <errno.h>
#include <stdio.h>
#include <yasc.h>

error FsError { FileNotFound = ENOENT, BadPath };

!int other_file(int which); /* yasc_error_name+b.c */

static int fake_syscall(int fail)
{
  if (fail)
  {
    errno = EACCES;
    return -1;
  }
  return 7;
}

static int fake_kernel(int fail)
{
  return fail ? -EINVAL : 9;
}

static !int wrapped(int fail)
{
  int a = try_errno(fake_syscall(fail == 1));
  int b = try_neg(fake_kernel(fail == 2));
  return a + b;
}

int main(void)
{
  printf("%s %s %s\n", error_name(error.BadPath), error_name(error.ENOENT), error_name(error.FileNotFound));
  printf("%s\n", error_name(error_from(9999)));
  for (int w = 0; w < 3; w++)
    printf("other %d: %s\n", w, error_name(other_file(w) catch |e| e));
  for (int f = 0; f < 3; f++)
  {
    int v = wrapped(f) catch |e| {
      printf("wrapped %d: %s\n", f, error_name(e));
      continue;
    };
    printf("wrapped %d -> %d\n", f, v);
  }
  return 0;
}
