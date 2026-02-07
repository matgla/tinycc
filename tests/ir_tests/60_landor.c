extern int printf(const char *, ...);

int nested_operation(int x, int y)
{
  return (x || y) && (y || x);
}

int main()
{
  // Test || and && operators
  int a = 0;
  int b = 1;
  int c = 0;
  int d = 1;

  // Test 1: 0 || 1 = 1
  printf("0 || 1 = %d\n", a || b);

  // Test 2: 1 || 0 = 1 (short-circuit)
  printf("1 || 0 = %d\n", b || c);

  //  Test 3: 0 || 0 = 0
  printf("0 || 0 = %d\n", a || c);

  // Test 4: 1 || 1 = 1
  printf("1 || 1 = %d\n", b || d);

  // Test 5: 0 && 1 = 0 (short-circuit)
  printf("0 && 1 = %d\n", a && b);

  // Test 6: 1 && 0 = 0
  printf("1 && 0 = %d\n", b && c);

  // Test 7: 0 && 0 = 0
  printf("0 && 0 = %d\n", a && c);

  // Test 8: 1 && 1 = 1
  printf("1 && 1 = %d\n", b && d);

  // Test 9: e || e && f where e=0, f=1 => 0 || (0 && 1) = 0 || 0 = 0
  printf("0 || 0 && 1 = %d\n", a || a && b);

  // Test 10: 1 || 0 && 1 => 1 || (0 && 1) = 1 || 0 = 1 (short-circuit)
  printf("1 || 0 && 1 = %d\n", b || a && b);

  printf("Nested operation (0,1): %d\n", nested_operation(a && b, 1));

  return 0;
}
