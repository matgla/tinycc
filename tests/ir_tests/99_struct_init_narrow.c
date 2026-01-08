/* Test 1: Just c[] with a, b - baseline */
#include <stdio.h>

struct S
{
  int x, y;
};

void test1(void)
{
  struct S a = {1, 2}, b = {3, 4}, c[] = {a, b};
  printf("test1 c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
}

/* Test 2: Add d[] with ++i */
void test2(void)
{
  int i = 0;
  struct S a = {1, 2}, b = {3, 4}, c[] = {a, b}, d[] = {++i, ++i, ++i, ++i};
  printf("test2 c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
  printf("test2 d[0]: %d %d, d[1]: %d %d\n", d[0].x, d[0].y, d[1].x, d[1].y);
}

/* Test 3: Add e[] with compound literal */
void test3(void)
{
  struct S a = {1, 2}, b = {3, 4}, c[] = {a, b}, e[] = {b, (struct S){5, 6}};
  printf("test3 c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
  printf("test3 e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

/* Test 4: Full combination like the original */
void test4(void)
{
  int i = 0;
  struct S a = {1, 2}, b = {3, 4}, c[] = {a, b}, d[] = {++i, ++i, ++i, ++i}, e[] = {b, (struct S){5, 6}};
  printf("test4 c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
  printf("test4 d[0]: %d %d, d[1]: %d %d\n", d[0].x, d[0].y, d[1].x, d[1].y);
  printf("test4 e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

/* Test 5: Just compound literal without other complexity */
void test5(void)
{
  struct S e[] = {(struct S){5, 6}, (struct S){7, 8}};
  printf("test5 e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

/* Test 6: Mix struct var and compound literal */
void test6(void)
{
  struct S b = {3, 4};
  struct S e[] = {b, (struct S){5, 6}};
  printf("test6 e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

/* Test 7: Two arrays, second with compound literal */
void test7(void)
{
  struct S a = {1, 2}, b = {3, 4};
  struct S c[] = {a, b};
  struct S e[] = {b, (struct S){5, 6}};
  printf("test7 c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
  printf("test7 e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

int main(void)
{
  test1();
  test2();
  test3();
  test4();
  test5();
  test6();
  test7();
  return 0;
}
