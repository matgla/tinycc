#include <stdio.h>
#include <string.h>

int main(void)
{
  char buf[64];
  int strcmp_sum = 0;
  int strlen_sum = 0;

  for (int i = 0; i < 8; i++) {
    strcpy(buf, "tinycc-armv8m");
    strlen_sum += (int)strlen(buf);
    strcmp_sum += strcmp(buf, "tinycc-armv8m");
  }

  printf("buf = %s\n", buf);
  printf("strlen_sum = %d\n", strlen_sum);
  printf("strcmp_sum = %d\n", strcmp_sum);

  if (strlen_sum == 104 && strcmp_sum == 0) {
    printf("PASS\n");
    return 0;
  }

  printf("FAIL\n");
  return 1;
}
