#include <stdio.h>

int main()
{
  char *a = "hello";

  // char destarray[10];
  // char *dest = &destarray[0];
  char *src = a;

  return *src != 0;
}
