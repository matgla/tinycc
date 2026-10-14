/* try_get_constant_string read the bytes of a `const char *const` pointer
 * object as if they were the string (strlen -> 0).  Fix: an lvalue is only a
 * string when it is the character array itself. */
#include <stdio.h>
#include <string.h>

static const char *const p = "hello";
const char *const tab[] = {"abc", "defg"};
static const char arr[] = "xyz";
static const char *const *pp = tab;

int main(void)
{
  printf("%d %d %d\n", (int)strlen(p), (int)strlen(tab[1]), strcmp(p, "hello"));
  printf("%d %d\n", (int)strlen(arr), (int)strlen("literal"));
  printf("%d %d\n", strcmp(arr, "xyz"), (int)strlen(pp[0]));
  return 0;
}
