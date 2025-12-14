
int simple0() { return 12312; }

int simple01() { return 0xdeadbeef; }

int simple02(int x) {
  int y = 0xdeadbeef;
  return x + y;
}

int simple1(int x) { return 42 + x * x; }

int simple_stack(int x) {
  int a = x + 123;
  return a;
}

int simple2(int x, int y) { return x + y; }

int simple3(int x, int y, int z) { return x * y + z; }

int simple4(int x, int y, int z, int w) { return x + y + z + w; }

int simple5(int x, int y, int z, int w, int u, int i) {
  return x * y + z * w + u + i;
}
