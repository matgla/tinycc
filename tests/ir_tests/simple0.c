#include <stdio.h>

int main(int argc, char *argv[]) {
  if (argc > 2) {
    puts("args more than 2");
  } else {
    puts("args less or equal 2");
  }
}