extern int printf(const char *, ...);

typedef struct
{
  int x;
  int y;
} TestStruct;

int main()
{
  TestStruct s = {};
  s.x = 10;
  s.y = 20;
  printf("%d\n", s.x);
  printf("%d\n", s.y);

  return 0;
}
