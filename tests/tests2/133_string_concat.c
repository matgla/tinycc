extern int printf(const char *, ...);

int main(void)
{
  const char *s = "ab"
                  "cd"
                  "ef";
  printf("%s\n", s);
  return 0;
}
