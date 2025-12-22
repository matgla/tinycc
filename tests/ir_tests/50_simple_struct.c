extern int printf(const char *, ...);

typedef struct {
  int x;
  int y;
} TestStruct;

int main() {
  TestStruct s = {};
  s.x = 10;
  s.y = 20;
  printf("%d\n", s.x);
  printf("%d\n", s.y);

  return 0;
}

// void aeabi_memset(void *dest, int n, int c) {
//   for (int i = 0; i < n; i++) {
//     ((unsigned char *)dest)[i] = (unsigned char)c;
//   }
// }
