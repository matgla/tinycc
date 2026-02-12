extern int printf(const char *, ...);

void check2(char **hum)
{
  printf("str: %s\n", *hum);
}

/* Minimal test 1: cleanup with goto in same scope */
void test_basic_cleanup(void)
{
  int chk = 0;
  {
    char *__attribute__((cleanup(check2))) stop_that = "plop";
    {
    label1:
      printf("---- %d\n", chk);
    }
    if (!chk)
    {
      chk = 1;
      goto label1;
    }
  }
  printf("test_basic_cleanup done\n");
}

/* Minimal test 2: cleanup with forward goto */
void test_forward_goto(void)
{
  char *__attribute__((cleanup(check2))) outer = "outer";
  {
    char *__attribute__((cleanup(check2))) inner = "tata !";
    goto out;
    inner = "titi";
  }
out:
  printf("test_forward_goto done\n");
}

/* Minimal test 3: for loop with cleanup */
void cl(int *ip)
{
  printf("%d\n", *ip);
}

void test_loop_cleanup(void)
{
  printf("-- loop --\n");
  for (__attribute__((cleanup(cl))) int i = 0; i < 3; ++i)
  {
    __attribute__((cleanup(cl))) int j = 100;
  }
  printf("test_loop_cleanup done\n");
}

int main(void)
{
  printf("=== test 1 ===\n");
  test_basic_cleanup();
  printf("=== test 2 ===\n");
  test_forward_goto();
  printf("=== test 3 ===\n");
  test_loop_cleanup();
  printf("=== ALL DONE ===\n");
  return 0;
}
